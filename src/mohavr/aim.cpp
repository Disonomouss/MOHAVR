#include "aim.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "addresses.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::aim {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTraceMeters = 300.0f;  // how far the aim ray is traced (a miss aims at its end)

Config           g_cfg;
SafetyHookInline g_hook;
bool             g_installed = false;

// The aim point of the last player view (game thread only).
struct AimFrame {
    bool  valid;
    DWORD tick;
    float start[3];  // the game's shot start (its untracked eye)
    float point[3];  // where the aim ray hits (or its end)
    float from[3];   // where the aim ray was traced from (the gun; past anything it started inside)
    float dir[3];    // the aim ray's direction
    float upm;       // units per metre
    std::uintptr_t actor;  // what it hit (0: nothing)
};
AimFrame g_frame{};

// The player's shot: armed by GetBaseAimRotation, then taken up by the bullet's trace (game thread only).
struct ShotState {
    bool           armed, active, followOn, fromGun;
    DWORD          tick;
    float          aim[3];                     // the base aim given (unit)
    float          eye[3], start[3], end[3];   // the game's start; the trace's start and end
    float          dot[3], dotDist;            // the red dot's point when the shot went (and its distance, m)
    float          turn;                       // degrees between the aim given and the game's shot line
    DWORD          frameAge;                   // ms since the aim frame
    std::uintptr_t source, hitPtr, blocker;    // blocker: 1 = something unnamed
    unsigned       count, logged;
    bool           layoutBad;
};
ShotState     g_shot{};
SafetyHookMid g_bulletPre, g_bulletPost;

// UWorld::SingleLineCheck through its LTCG convention (addresses.hpp): stack (this, Hit, Source, End, Start,
// Extent), EAX = flags, ECX = 0, callee pops. Returns nonzero when nothing was hit.
__declspec(naked) int __stdcall CallSingleLineCheck(void* /*world*/, void* /*hit*/, void* /*source*/, const float* /*end*/,
                                                    const float* /*start*/, const float* /*extent*/, unsigned /*flags*/,
                                                    std::uintptr_t /*fn*/) {
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi
        push dword ptr [ebp + 28]  // extent
        push dword ptr [ebp + 24]  // start
        push dword ptr [ebp + 20]  // end
        push dword ptr [ebp + 16]  // source
        push dword ptr [ebp + 12]  // hit
        push dword ptr [ebp + 8]   // this = GWorld
        mov  eax, dword ptr [ebp + 32]
        xor  ecx, ecx
        call dword ptr [ebp + 36]
        lea  esp, [ebp - 12]       // whoever popped what, back to our saved registers
        pop  edi
        pop  esi
        pop  ebx
        pop  ebp
        ret  32
    }
}

// The trace, with the player's bullets' flags (per-poly collision; addresses.hpp); false (and `hit` = end) when
// nothing is in the way.
bool Trace(std::uintptr_t source, const float (&start)[3], const float (&end)[3], float (&hit)[3],
           std::uintptr_t* actor = nullptr) {
    void* world = *reinterpret_cast<void**>(addr::kGWorld);
    std::memcpy(hit, end, sizeof(hit));
    if (!world) return false;
    alignas(16) std::uint8_t result[0x80] = {};
    *reinterpret_cast<float*>(result + addr::kCheckResultTime) = 1.0f;
    *reinterpret_cast<int*>(result + addr::kCheckResultItem) = -1;
    const float extent[3] = {0.0f, 0.0f, 0.0f};
    const int clear = CallSingleLineCheck(world, result, reinterpret_cast<void*>(source), end, start, extent,
                                          addr::kTraceFlagsBullet, addr::kSingleLineCheck);
    if (clear) return false;
    std::memcpy(hit, result + addr::kCheckResultLocation, sizeof(hit));
    if (actor) *actor = *reinterpret_cast<const std::uintptr_t*>(result + addr::kCheckResultActor);
    return true;
}

// The local player's pawn: PlayerController.Pawn, used only if that pawn's Controller points back.
std::uintptr_t LocalPawn(std::uintptr_t ctrl) {
    if (!ctrl) return 0;
    const auto pawn = *reinterpret_cast<const std::uintptr_t*>(ctrl + addr::kControllerPawn);
    if (!pawn) return 0;
    if (*reinterpret_cast<const std::uintptr_t*>(pawn + addr::kPawnController) == ctrl) return pawn;
    static int logged = 0;
    if (logged++ < 3)
        MLOG("aim: controller %08X +0x%X -> %08X, whose +0x%X is not the controller -- no aim this frame",
             static_cast<unsigned>(ctrl), static_cast<unsigned>(addr::kControllerPawn), static_cast<unsigned>(pawn),
             static_cast<unsigned>(addr::kPawnController));
    return 0;
}

std::uintptr_t LocalController() {
    const auto engine = *reinterpret_cast<const std::uintptr_t*>(addr::kGEngine);
    const auto* arr = engine ? reinterpret_cast<const std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
    if (!arr || arr[1] < 1 || !arr[0]) return 0;
    const auto player = *reinterpret_cast<const std::uintptr_t*>(arr[0]);
    return player ? *reinterpret_cast<const std::uintptr_t*>(player + addr::kLocalPlayerActor) : 0;
}

// Clears a script bool of `object` if set; true if it was.
bool ClearBool(std::uintptr_t object, const char* flag) {
    int off = -1;
    std::uint32_t mask = 0;
    if (!object || !names::BoolProperty(object, flag, off, mask)) return false;
    auto* word = reinterpret_cast<volatile std::uint32_t*>(object + static_cast<std::uintptr_t>(off));
    if (!(*word & mask)) return false;
    *word = *word & ~mask;
    return true;
}

// The game's HUD bits that point where the gun doesn't (cleared each view, before the HUD draws):
// - HUD.Crosshair=0: MOHAHUD.hud_cursor (a MOHAHUDCursor) -- its own CursorRenderingEnabled (read by the native Render
//   alone; the game sets it again every frame) and the element's bRender (what EnableElement sets);
// - HUD.HitMarker=0: MOHAHUD.hud_weaponHitNotify, the red cross OnNotifyWeaponHit shows for a moment on a hit (round 22).
void HideHudBits(std::uintptr_t ctrl) {
    const int ho = names::PropertyOffset(ctrl, "myHUD");
    const std::uintptr_t hud = ho >= 0 ? names::ReadPointer(ctrl + ho) : 0;
    if (!hud) return;
    if (!g_cfg.hudCrosshair) {
        const int co = names::PropertyOffset(hud, "hud_cursor");
        const std::uintptr_t cursor = co >= 0 ? names::ReadPointer(hud + co) : 0;
        for (const char* flag : {"bRender", "CursorRenderingEnabled"}) {
            static int logged = 0;
            if (ClearBool(cursor, flag) && logged < 4) {
                ++logged;
                MLOG("aim: the game's crosshair hidden (%s.%s off; HUD.Crosshair=0)", names::Name(cursor).c_str(), flag);
            }
        }
    }
    if (!g_cfg.hudHitMarker) {
        const int mo = names::PropertyOffset(hud, "hud_weaponHitNotify");
        const std::uintptr_t marker = mo >= 0 ? names::ReadPointer(hud + mo) : 0;
        static int logged = 0;
        if (ClearBool(marker, "bRender") && logged < 3) {
            ++logged;
            MLOG("aim: the hit marker hidden (%s.bRender off; HUD.HitMarker=0)", names::Name(marker).c_str());
        }
    }
}

// Weapon.Tracers=0: the player's tracers start at the attachment mesh's BarrelTip socket (SmallArmsAttachment.TurnOnTracer)
// -- the first-person gun, but in the game's own pose in front of the face, not the gun drawn in your hand (round 22;
// round 26 corrected: the local player has no separate third-person gun). Its
// CreateTracers[fire mode] = 0 makes UpdateTracerData return before spawning one.
void HideTracers(std::uintptr_t pawn) {
    const int ao = names::PropertyOffset(pawn, "CurrentWeaponAttachment");
    const std::uintptr_t att = ao >= 0 ? names::ReadPointer(pawn + ao) : 0;
    const int to = att ? names::PropertyOffset(att, "CreateTracers") : -1;
    if (to < 0) return;
    auto* create = reinterpret_cast<volatile std::uint8_t*>(att + static_cast<std::uintptr_t>(to));
    if (!create[0] && !create[1]) return;
    static int logged = 0;
    if (logged < 4) {
        ++logged;
        MLOG("aim: tracers off for %s (CreateTracers %u %u -> 0 0; Weapon.Tracers=0)", names::Name(att).c_str(), create[0], create[1]);
    }
    create[0] = 0;
    create[1] = 0;
}

void Publish(float distance, std::uint32_t source) {
    if (shared::Header* hdr = bridge::SharedHeader()) {
        hdr->aimDistance = distance;
        hdr->aimSource = source;
    }
}

// Frames since the last ray log, and those whose hand read met the host mid-write (kept the last aim).
unsigned g_frames = 0, g_torn = 0, g_near = 0, g_triggers = 0;

bool IsPassThrough(std::uintptr_t actor) {
    return actor && (names::IsA(actor, "Trigger") || names::IsA(actor, "TriggerVolume"));
}

// A trace the way the game's bullets go (0x10F0CF10): through Triggers and TriggerVolumes. Each one hit has its
// bProjTarget cleared and the trace goes on; all of them are set back at the end (round 21: setting each back before
// the next trace made two overlapping triggers hit in turn until the loop gave up, and the aim point sat at the gun).
bool TraceThrough(std::uintptr_t source, const float (&start)[3], const float (&end)[3], float (&hit)[3],
                  std::uintptr_t* actorOut) {
    struct Cleared {
        volatile std::uint32_t* word;
        std::uint32_t           mask;
    };
    Cleared cleared[16];
    int n = 0;
    std::uintptr_t actor = 0;
    bool h = Trace(source, start, end, hit, &actor);
    while (h && n < 16 && IsPassThrough(actor)) {
        int off = -1;
        std::uint32_t mask = 0;
        if (!names::BoolProperty(actor, "bProjTarget", off, mask)) break;
        auto* word = reinterpret_cast<volatile std::uint32_t*>(actor + static_cast<std::uintptr_t>(off));
        if (!(*word & mask)) break;  // hit with it already off: take the hit rather than loop
        *word = *word & ~mask;
        cleared[n++] = {word, mask};
        h = Trace(source, start, end, hit, &actor);
    }
    for (int i = n - 1; i >= 0; --i) *cleared[i].word = *cleared[i].word | cleared[i].mask;
    g_triggers += static_cast<unsigned>(n);
    if (actorOut) *actorOut = h ? actor : 0;
    return h;
}

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// The last base aim this hook gave the player (for AddSpread).
int   g_lastAim[3] = {0, 0, 0};
DWORD g_lastAimTick = 0;
SafetyHookInline g_spreadHook;

// WeaponAccuracyComponent.AddSpread(BaseAim) (native, 0x10E4E310): the player's shots get Aim.Spread of the game's
// spread around our aim (0 = none: in VR the hand is the spread; the game's hip-fire and "turning" penalties -- the
// head moves all the time -- scattered shots far from the red dot at distance, headset round 17).
void __fastcall Hook_AddSpread(std::uintptr_t self, void* /*edx*/, void* stack, int* result) {
    g_spreadHook.thiscall<void>(self, stack, result);
    if (!result || GetTickCount() - g_lastAimTick > 100) return;
    const int wo = names::PropertyOffset(self, "mWeapon");
    const std::uintptr_t weapon = wo >= 0 ? names::ReadPointer(self + wo) : 0;
    const int io = weapon ? names::PropertyOffset(weapon, "Instigator") : -1;
    if (io < 0 || names::ReadPointer(weapon + io) != LocalPawn(LocalController())) return;
    const int spread[3] = {result[0], result[1], result[2]};
    for (int i = 0; i < 2; ++i) {
        const int delta = static_cast<std::int16_t>(static_cast<std::uint16_t>((spread[i] - g_lastAim[i]) & 0xFFFF));
        result[i] = (g_lastAim[i] + static_cast<int>(std::lround(delta * g_cfg.aimSpread))) & 0xFFFF;
    }
    static int logged = 0;
    if (logged < 6) {
        ++logged;
        MLOG("aim: spread P%d Y%d -> P%d Y%d (x%.2f of the game's)", spread[0] & 0xFFFF, spread[1] & 0xFFFF, result[0], result[1],
             g_cfg.aimSpread);
    }
}

// Pawn.GetBaseAimRotation() -- the player's pawn aims at this frame's aim point from where its shot starts.
void __fastcall Hook_GetBaseAimRotation(std::uintptr_t self, void* /*edx*/, void* stack, int* result) {
    g_hook.thiscall<void>(self, stack, result);
    if (!result || !g_frame.valid || GetTickCount() - g_frame.tick > 250) return;
    const std::uintptr_t pawn = LocalPawn(LocalController());
    if (!pawn || pawn != self) return;
    const float vx = g_frame.point[0] - g_frame.start[0], vy = g_frame.point[1] - g_frame.start[1],
                vz = g_frame.point[2] - g_frame.start[2];
    if (vx * vx + vy * vy + vz * vz < 1.0f) return;  // the aim point at the eye: leave the game's aim
    const int before[3] = {result[0], result[1], result[2]};
    const float toUnr = 32768.0f / kPi;
    result[0] = static_cast<int>(std::lround(std::atan2(vz, std::sqrt(vx * vx + vy * vy)) * toUnr)) & 0xFFFF;
    result[1] = static_cast<int>(std::lround(std::atan2(vy, vx) * toUnr)) & 0xFFFF;
    result[2] = 0;
    for (int i = 0; i < 3; ++i) g_lastAim[i] = result[i];
    g_lastAimTick = GetTickCount();
    // A shot follows (PerformWeaponTrace: GetAdjustedAim, then CalcWeaponFire): arm the bullet-trace hook.
    const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
    g_shot.armed = true;
    g_shot.tick = g_lastAimTick;
    g_shot.aim[0] = vx / len;
    g_shot.aim[1] = vy / len;
    g_shot.aim[2] = vz / len;
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        MLOG("aim: pawn %08X base aim P%d Y%d -> P%d Y%d (aim point %.0f %.0f %.0f from %.0f %.0f %.0f)",
             static_cast<unsigned>(self), before[0] & 0xFFFF, before[1] & 0xFFFF, result[0], result[1], g_frame.point[0],
             g_frame.point[1], g_frame.point[2], g_frame.start[0], g_frame.start[1], g_frame.start[2]);
    }
}

// v turned by the rotation that takes unit a onto unit b (the shortest one).
void TurnLike(const float (&a)[3], const float (&b)[3], float (&v)[3]) {
    const float k[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float s = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (s < 1e-6f) return;
    const float u[3] = {k[0] / s, k[1] / s, k[2] / s};
    const float uv[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float ud = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    for (int i = 0; i < 3; ++i) v[i] = v[i] * c + uv[i] * s + u[i] * ud * (1.0f - c);
}

// The bullet's SingleLineCheck in 0x10F0CDE0 (addresses.hpp). Aim.ShotFromGun: the player's shot starts at the gun
// and runs along the red dot's ray -- the same ray, start and flags as the dot, so they can't disagree (round 21: the
// shot from the eye towards the dot's point met other things on the way) -- unless something stands between the eye
// and the gun (a hand through a wall). Aim.ShotLog: where each shot went.
void OnBulletTrace(SafetyHookContext& ctx) {
    if ((!g_shot.armed && !g_shot.followOn) || g_shot.layoutBad) return;
    const std::uintptr_t esp = ctx.esp;
    // The call's arguments as addresses.hpp has them: [esp] GWorld, [esp+8] Source (pushed from EBX).
    if (*reinterpret_cast<const std::uintptr_t*>(esp) != *reinterpret_cast<const std::uintptr_t*>(addr::kGWorld) ||
        *reinterpret_cast<const std::uintptr_t*>(esp + 8) != ctx.ebx) {
        g_shot.layoutBad = true;
        MLOG("aim: the bullet trace's stack isn't as expected -- Aim.ShotFromGun / ShotLog stand down");
        return;
    }
    const std::uintptr_t source = ctx.ebx;
    float* endP = *reinterpret_cast<float**>(esp + 0xC);
    float* startP = *reinterpret_cast<float**>(esp + 0x10);
    const float* extP = *reinterpret_cast<const float**>(esp + 0x14);
    if (!source || !endP || !startP) return;
    // The same shot going on past a trigger (0x10F0CF10 recurses from the hit, towards the same end).
    if (g_shot.followOn) {
        if (source == g_shot.source && std::fabs(endP[0] - g_shot.end[0]) < 1.0f && std::fabs(endP[1] - g_shot.end[1]) < 1.0f &&
            std::fabs(endP[2] - g_shot.end[2]) < 1.0f) {
            g_shot.active = true;
            g_shot.hitPtr = *reinterpret_cast<const std::uintptr_t*>(esp + 4);
            return;
        }
        g_shot.followOn = false;
    }
    if (!g_shot.armed || GetTickCount() - g_shot.tick > 100) return;
    const std::uintptr_t pawn = LocalPawn(LocalController());
    if (source != pawn) return;
    if (extP && (extP[0] != 0.0f || extP[1] != 0.0f || extP[2] != 0.0f)) return;  // a box trace (melee)
    float d[3] = {endP[0] - startP[0], endP[1] - startP[1], endP[2] - startP[2]};
    const float range = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (range < 1.0f) return;
    for (float& x : d) x /= range;
    if (d[0] * g_shot.aim[0] + d[1] * g_shot.aim[1] + d[2] * g_shot.aim[2] < 0.95f) return;  // not along this shot's aim
    g_shot.armed = false;
    ++g_shot.count;
    g_shot.source = source;
    g_shot.hitPtr = *reinterpret_cast<const std::uintptr_t*>(esp + 4);
    g_shot.active = true;
    g_shot.fromGun = false;
    g_shot.blocker = 0;
    for (int i = 0; i < 3; ++i) {
        g_shot.eye[i] = startP[i];
        g_shot.dot[i] = g_frame.point[i];
    }
    g_shot.dotDist = Dist(g_frame.from, g_frame.point) / (g_frame.upm > 1.0f ? g_frame.upm : 100.0f);
    g_shot.frameAge = GetTickCount() - g_frame.tick;
    {
        const float c = d[0] * g_shot.aim[0] + d[1] * g_shot.aim[1] + d[2] * g_shot.aim[2];
        g_shot.turn = std::acos(std::fmin(1.0f, std::fmax(-1.0f, c))) * 180.0f / kPi;
    }
    if (g_cfg.aimShotFromGun && g_frame.valid && GetTickCount() - g_frame.tick < 250) {
        // The dot's ray, turned by whatever the game's spread did to the base aim (none with Aim.Spread=0).
        float dir[3] = {g_frame.dir[0], g_frame.dir[1], g_frame.dir[2]};
        TurnLike(g_shot.aim, d, dir);
        float at[3];
        std::uintptr_t blocker = 0;
        if (TraceThrough(source, g_shot.eye, g_frame.from, at, &blocker)) {
            g_shot.blocker = blocker ? blocker : 1;
        } else {
            for (int i = 0; i < 3; ++i) {
                startP[i] = g_frame.from[i];
                endP[i] = g_frame.from[i] + dir[i] * range;
            }
            g_shot.fromGun = true;
        }
    }
    for (int i = 0; i < 3; ++i) {
        g_shot.start[i] = startP[i];
        g_shot.end[i] = endP[i];
    }
}

// [Aim] LauncherFromGun (the player, round 38: "the Panzerschreck's missile comes from beside the player"): a projectile
// weapon's EALAWeapon.ProjectileFire spawns at GetPhysicalFireStartLoc -- the Panzerschreck's and M18's GetBarrelPosition,
// the barrel of the game's own first-person gun, beside the head -- unless PhysicalStartFireOverride is set (it reads and
// clears it per shot). Set every frame to where the aim ray starts: the game's trace (CalcWeaponFire, from the gun along
// the ray with ShotFromGun) then gives AimDir = the ray itself. Not when something stands between the eye and the gun.
void SetLauncherStart(std::uintptr_t pawn, bool barrel) {
    static std::uintptr_t lastWeapon = 0;
    static bool lastSet = false;
    static unsigned taken = 0;
    const int wo = names::PropertyOffset(pawn, "Weapon");
    const std::uintptr_t weapon = wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
    if (!weapon || names::IsA(weapon, "EALAGrenade")) return;
    const int so = names::PropertyOffset(weapon, "PhysicalStartFireOverride"), fo = names::PropertyOffset(weapon, "WeaponFireTypes");
    if (so < 0 || fo < 0) return;
    // WeaponFireTypes: array<EWeaponFireType> (TArray: data, count) -- fire mode 0 is EWFT_Projectile (1).
    const std::uintptr_t data = names::ReadPointer(weapon + fo);
    const int count = *reinterpret_cast<const int*>(weapon + fo + 4);
    if (!data || count < 1 || *reinterpret_cast<const std::uint8_t*>(data) != 1) return;
    float* over = reinterpret_cast<float*>(weapon + so);
    if (weapon == lastWeapon && lastSet && over[0] == 0.0f && over[1] == 0.0f && over[2] == 0.0f && taken < 20) {
        ++taken;  // the game read and cleared it: a projectile left from the aim line
        MLOG("aim: %s's projectile started on the aim line (%.0f %.0f %.0f, %.2f m from the eye)", names::ClassName(weapon).c_str(),
             g_frame.from[0], g_frame.from[1], g_frame.from[2], Dist(g_frame.start, g_frame.from) / g_frame.upm);
    }
    float at[3];
    const bool clear = barrel && g_frame.valid && !TraceThrough(pawn, g_frame.start, g_frame.from, at, nullptr);
    if (weapon != lastWeapon) {
        lastWeapon = weapon;
        MLOG("aim: %s fires projectiles -- %s", names::ClassName(weapon).c_str(),
             clear ? "from the aim line (Aim.LauncherFromGun)" : "from the game's barrel for now (no clear aim line)");
    }
    for (int i = 0; i < 3; ++i) over[i] = clear ? g_frame.from[i] : 0.0f;
    lastSet = clear;
}

// Right after it: where the bullet went (the Hit at esp+0x14).
void OnBulletTraceDone(SafetyHookContext& ctx) {
    if (!g_shot.active) return;
    g_shot.active = false;
    const std::uintptr_t hit = ctx.esp + 0x14;
    if (hit != g_shot.hitPtr || ctx.ebx != g_shot.source) return;  // not the Hit we saw go in
    const std::uintptr_t actor = *reinterpret_cast<const std::uintptr_t*>(hit + addr::kCheckResultActor);
    const float* loc = reinterpret_cast<const float*>(hit + addr::kCheckResultLocation);
    if (actor && IsPassThrough(actor)) {  // the game goes on through it: log where it ends up
        g_shot.followOn = true;
        return;
    }
    g_shot.followOn = false;
    if (!g_cfg.aimShotLog || g_shot.logged >= 400) return;
    ++g_shot.logged;
    const float upm = g_frame.upm > 1.0f ? g_frame.upm : 100.0f;
    const float at[3] = {actor ? loc[0] : g_shot.end[0], actor ? loc[1] : g_shot.end[1], actor ? loc[2] : g_shot.end[2]};
    char from[160];
    if (g_shot.fromGun)
        sprintf_s(from, "from the gun (%.2f m from the eye)", Dist(g_shot.eye, g_shot.start) / upm);
    else if (g_shot.blocker)
        sprintf_s(from, "from the eye -- %s between the eye and the gun",
                  g_shot.blocker > 1 ? names::Name(g_shot.blocker).c_str() : "something");
    else
        sprintf_s(from, "from the eye");
    MLOG("shot %u: %s; hit %s at %.0f %.0f %.0f (%.1f m) -- %.0f cm from the red dot's point (%.1f m; the game's line %.2f deg "
         "off the aim; aim frame %u ms old)",
         g_shot.count, from, actor ? names::Name(actor).c_str() : "nothing", at[0], at[1], at[2], Dist(g_shot.start, at) / upm,
         Dist(at, g_shot.dot) * 100.0f / upm, g_shot.dotDist, g_shot.turn, g_shot.frameAge);
}

}  // namespace

std::uintptr_t LocalPlayerPawn() { return LocalPawn(LocalController()); }

bool WorldTrace(std::uintptr_t source, const float (&start)[3], const float (&end)[3], float (&hit)[3], std::uintptr_t* actor) {
    const unsigned triggers = g_triggers;  // (the aim's own statistic)
    const bool h = TraceThrough(source, start, end, hit, actor);
    g_triggers = triggers;
    return h;
}

bool Install(const Config& cfg) {
    g_cfg = cfg;
    if (cfg.aimMode == 0) {
        MLOG("aim: Aim.Mode=0 -- the game's own aim (body yaw%s)", cfg.aimHeadPitch ? ", head pitch" : "");
        return false;
    }
    // Standing rule 4 (the build check verified them too): both sites must hold the pinned bytes.
    if (!patch::BytesMatch(addr::kExecGetBaseAimRotation, addr::kExecGetBaseAimRotationBytes,
                           sizeof(addr::kExecGetBaseAimRotationBytes)) ||
        !patch::BytesMatch(addr::kSingleLineCheck, addr::kSingleLineCheckBytes, sizeof(addr::kSingleLineCheckBytes))) {
        MLOG("aim: execGetBaseAimRotation / SingleLineCheck bytes differ -- standing down (the game's own aim)");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecGetBaseAimRotation),
                                              reinterpret_cast<void*>(&Hook_GetBaseAimRotation));
    if (!res) {
        MLOG("aim: inline hook on execGetBaseAimRotation failed (error %d) -- the game's own aim",
             static_cast<int>(res.error().type));
        return false;
    }
    g_hook = std::move(*res);
    g_installed = true;
    if (cfg.aimSpread < 0.999f) {
        if (patch::BytesMatch(addr::kExecAddSpread, addr::kExecAddSpreadBytes, sizeof(addr::kExecAddSpreadBytes))) {
            auto sp = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecAddSpread),
                                                     reinterpret_cast<void*>(&Hook_AddSpread));
            if (sp) {
                g_spreadHook = std::move(*sp);
                MLOG("aim: Aim.Spread=%.2f -- the player's spread scaled (AddSpread hooked at 0x%08X)", cfg.aimSpread,
                     static_cast<unsigned>(addr::kExecAddSpread));
            }
        } else {
            MLOG("aim: AddSpread bytes differ -- the game's spread stays");
        }
    }
    if (cfg.aimShotFromGun || cfg.aimShotLog) {
        // Standing rule 4: the flags the bullet traces with, its call and the instruction after it.
        if (patch::BytesMatch(addr::kBulletTraceFlagsSite, addr::kBulletTraceFlagsSiteBytes, sizeof(addr::kBulletTraceFlagsSiteBytes)) &&
            patch::BytesMatch(addr::kBulletTraceCall, addr::kBulletTraceCallBytes, sizeof(addr::kBulletTraceCallBytes)) &&
            patch::BytesMatch(addr::kBulletTraceAfter, addr::kBulletTraceAfterBytes, sizeof(addr::kBulletTraceAfterBytes))) {
            auto pre = safetyhook::MidHook::create(reinterpret_cast<void*>(addr::kBulletTraceCall), OnBulletTrace);
            auto post = safetyhook::MidHook::create(reinterpret_cast<void*>(addr::kBulletTraceAfter), OnBulletTraceDone);
            if (pre && post) {
                g_bulletPre = std::move(*pre);
                g_bulletPost = std::move(*post);
                MLOG("aim: bullet trace hooked at 0x%08X/0x%08X (Aim.ShotFromGun=%d: %s; Aim.ShotLog=%d)",
                     static_cast<unsigned>(addr::kBulletTraceCall), static_cast<unsigned>(addr::kBulletTraceAfter),
                     cfg.aimShotFromGun, cfg.aimShotFromGun ? "shots start at the gun, along the red dot's ray" : "shots from the eye",
                     cfg.aimShotLog);
            } else {
                MLOG("aim: bullet trace hooks failed -- shots start at the eye, no shot log");
            }
        } else {
            MLOG("aim: bullet trace bytes differ -- shots start at the eye, no shot log");
        }
    }
    static const char* kNames[] = {"game", "head", "left hand", "right hand"};
    MLOG("aim: Aim.Mode=%d (%s) -- execGetBaseAimRotation hooked at 0x%08X", cfg.aimMode, kNames[cfg.aimMode],
         static_cast<unsigned>(addr::kExecGetBaseAimRotation));
    return true;
}

void OnPlayerView(std::uintptr_t ctrl, const float (&shotStart)[3]) {
    if (!g_installed) return;
    const shared::Header* hdr = bridge::SharedHeader();
    const std::uintptr_t pawn = LocalPawn(ctrl);
    if (ctrl && (!g_cfg.hudCrosshair || !g_cfg.hudHitMarker)) HideHudBits(ctrl);
    if (pawn && !g_cfg.weaponTracers) HideTracers(pawn);
    if (!hdr || !pawn) {
        g_frame.valid = false;
        Publish(0.0f, 0);
        return;
    }
    // The aiming pose: the head, or a controller when the host has it this frame.
    shared::Pose pose{};
    if (g_cfg.aimMode == 1) {
        shared::Pose eye[2];
        shared::Fov fov[2];
        if (!shared::ReadViews(hdr, pose, eye, fov)) return;  // mid-write: keep last frame's reticle
    } else {
        shared::Pose hand[2];
        std::uint32_t valid = 0;
        if (!shared::ReadHands(hdr, hand, valid)) {  // still mid-write: keep last frame's aim
            ++g_torn;
            return;
        }
        const int h = g_cfg.aimMode - 2;
        if (!(valid & (1u << h))) {
            g_frame.valid = false;
            Publish(0.0f, 0);
            return;
        }
        pose = hand[h];
    }
    ++g_frames;
    float pos[3], fwd[3], upm = 100.0f;
    // With the gun drawn in the aiming hand (Weapon.ViewModel=2) the ray runs along its barrel -- the gun's own
    // frame and aim-line offset, per weapon (the menu's Gun fit; headset round 12: the shots were ~8 cm low).
    const bool barrel = g_cfg.aimMode >= 2 && viewmodel::GunRay(pos, fwd, upm);
    if (!barrel && !view::PoseToWorld(pose, pos, fwd, upm)) return;
    const float reach = kTraceMeters * upm;
    const float end[3] = {pos[0] + fwd[0] * reach, pos[1] + fwd[1] * reach, pos[2] + fwd[2] * reach};
    float point[3];
    std::uintptr_t actor = 0;
    float from[3] = {pos[0], pos[1], pos[2]};
    bool hit = TraceThrough(pawn, from, end, point, &actor);
    // A hit right at the start means the ray began inside something (headset rounds 18-19: runs of 0.0 / 0.2 m hits
    // while walking -- the aim then pointed from the eye at the hand). Step along the ray past it, 20 cm at a time, up
    // to 1 m.
    const float step = 20.0f * upm / 100.0f;
    for (int stepsOn = 1; hit && stepsOn <= 5; ++stepsOn) {
        if (Dist(point, from) >= step) break;
        ++g_near;
        static int loggedNear = 0;
        static std::uintptr_t seenNear = 0;
        if (actor != seenNear && loggedNear < 12) {
            ++loggedNear;
            seenNear = actor;
            MLOG("aim: the ray starts inside %s (%s)%s -- traced again from %d cm on", actor ? names::Name(actor).c_str() : "?",
                 actor ? names::ClassName(actor).c_str() : "?", actor == pawn ? " = the player" : "", stepsOn * 20);
        }
        for (int k = 0; k < 3; ++k) from[k] = pos[k] + fwd[k] * step * static_cast<float>(stepsOn);
        hit = TraceThrough(pawn, from, end, point, &actor);
    }
    for (int k = 0; k < 3; ++k) {
        g_frame.from[k] = from[k];
        g_frame.dir[k] = fwd[k];
    }
    g_frame.upm = upm;
    g_frame.actor = hit ? actor : 0;
    g_frame.valid = true;
    g_frame.tick = GetTickCount();
    std::memcpy(g_frame.start, shotStart, sizeof(g_frame.start));
    std::memcpy(g_frame.point, point, sizeof(g_frame.point));
    if (g_cfg.aimShotFromGun && g_cfg.aimLauncherFromGun) SetLauncherStart(pawn, barrel);
    const float dx = point[0] - pos[0], dy = point[1] - pos[1], dz = point[2] - pos[2];
    Publish(std::sqrt(dx * dx + dy * dy + dz * dz) / upm, static_cast<std::uint32_t>(g_cfg.aimMode));
    static DWORD nextLog = GetTickCount();
    if (static_cast<LONG>(g_frame.tick - nextLog) >= 0) {
        nextLog = g_frame.tick + 5000;
        MLOG("aim: ray from %.0f %.0f %.0f dir %.2f %.2f %.2f -> %s at %.0f %.0f %.0f (%.1f m; %s; %u of %u frames torn, %u started inside, %u through triggers)",
             pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2], hit ? "hit" : "nothing", point[0], point[1], point[2],
             std::sqrt(dx * dx + dy * dy + dz * dz) / upm, barrel ? "barrel" : "controller", g_torn, g_frames, g_near, g_triggers);
        g_torn = g_frames = g_near = g_triggers = 0;
    }
}

}  // namespace mohavr::aim
