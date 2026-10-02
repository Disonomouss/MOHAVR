#include "viewmodel.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "offhand.hpp"
#include "patch.hpp"
#include "vr_view.hpp"

namespace mohavr::viewmodel {
namespace {

// Unreal's FMatrix: row vectors (v' = v * M), rows = X/Y/Z axes, then the origin.
struct M4 {
    float m[4][4];
};

M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// A rigid frame: axes (unit, orthogonal) as rows, then the origin.
M4 Frame(const float (&x)[3], const float (&y)[3], const float (&z)[3], const float (&t)[3]) {
    return M4{{{x[0], x[1], x[2], 0.0f}, {y[0], y[1], y[2], 0.0f}, {z[0], z[1], z[2], 0.0f}, {t[0], t[1], t[2], 1.0f}}};
}
constexpr float kAxisX[3] = {1.0f, 0.0f, 0.0f}, kAxisY[3] = {0.0f, 1.0f, 0.0f}, kAxisZ[3] = {0.0f, 0.0f, 1.0f};

// The reflection across the plane through p with unit normal n (row vectors: x' = x - 2((x - p).n) n); its own inverse.
M4 Reflection(const float (&p)[3], const float (&n)[3]) {
    M4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = (i == j ? 1.0f : 0.0f) - 2.0f * n[i] * n[j];
    const float pn = p[0] * n[0] + p[1] * n[1] + p[2] * n[2];
    for (int j = 0; j < 3; ++j) r.m[3][j] = 2.0f * pn * n[j];
    r.m[3][3] = 1.0f;
    return r;
}
// A frame seen in the mirror, kept right-handed: its own Y axis flipped, then the world reflected.
M4 MirrorFrame(const M4& f, const M4& r) {
    M4 flipped = f;
    for (int j = 0; j < 4; ++j) flipped.m[1][j] = -flipped.m[1][j];
    return Mul(flipped, r);
}

M4 RigidInverse(const M4& a) {
    M4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int j = 0; j < 3; ++j)
        r.m[3][j] = -(a.m[3][0] * r.m[0][j] + a.m[3][1] * r.m[1][j] + a.m[3][2] * r.m[2][j]);
    r.m[3][3] = 1.0f;
    return r;
}

Config           g_cfg;
SafetyHookInline g_hook;
bool             g_installed = false;

// Render thread -> game thread: the first-person parts drawn lately (the arms and the gun), to name the gun.
struct PartSeen {
    std::atomic<std::uintptr_t> comp{0};
    std::atomic<DWORD>          tick{0};
};
PartSeen g_parts[4];

void NotePart(std::uintptr_t comp) {
    const DWORD now = GetTickCount();
    int oldest = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_parts[i].comp.load(std::memory_order_relaxed) == comp) {
            g_parts[i].tick.store(now, std::memory_order_relaxed);
            return;
        }
        if (now - g_parts[i].tick.load(std::memory_order_relaxed) > now - g_parts[oldest].tick.load(std::memory_order_relaxed))
            oldest = i;
    }
    g_parts[oldest].comp.store(comp, std::memory_order_relaxed);
    g_parts[oldest].tick.store(now, std::memory_order_relaxed);
}

// Game thread: this frame's gun-in-hand aim line (GunRay).
struct GunLine {
    bool  valid;
    float pos[3], dir[3], upm;
} g_line{};

// Game -> render thread: the move for the arms/gun (the latest player view).
SRWLOCK g_lock = SRWLOCK_INIT;
struct State {
    bool  valid;
    DWORD tick;
    M4    d, dInv;  // LocalToWorld' = LocalToWorld * d; WorldToLocal' = dInv * WorldToLocal
    M4    camInv;   // the game camera's inverse (calibration log)
    // Arm IK: the gun's frame and the other (off) controller's frame in the world (rows: forward, right, up, origin).
    M4    gunFrame, offFrame;
    bool  offValid, twoHanded;
    // Weapon.LeftHandMirror: with the gun in the left hand everything above is in a mirror world (reflected across the
    // body's centre plane: the left controller acts as a right hand), and the parts are drawn through `mirror`.
    bool  mirrored;
    M4    mirror;
    // The player's pawn and its frame (location and yaw; the arms' LocalToWorld moves with it) at this view: the next
    // tick's bake carries the move and the hand frames along with the body's move since (Weapon.CatchUp).
    std::uintptr_t pawn;
    M4    pawnFrame;
    // D21 manual reload: the host's flags, pull, held magazine and rack, read with the hands (RELOAD-DESIGN X3), and the
    // held magazine's grab-point frame mapped like the off hand's (mirrored with it).
    bool  reloadValid, magValid;
    shared::ReloadView reload;
    M4    magFrame;
    float upm;
    // DrawWithoutHands: the parts drawn with d, no hands (HandFrames, CurrentMove ... report none).
    bool  noHands;
} g_state{};

M4 PawnFrame(std::uintptr_t pawn) {
    const float* loc = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    const float yaw = static_cast<float>(*reinterpret_cast<const int*>(pawn + addr::kActorRotation + 4)) * (3.14159265f / 32768.0f);
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float x[3] = {c, s, 0.0f}, y[3] = {-s, c, 0.0f}, z[3] = {0.0f, 0.0f, 1.0f}, t[3] = {loc[0], loc[1], loc[2]};
    return Frame(x, y, z, t);
}

bool g_cullOk = false;  // the determinant's readers hold the pinned bytes (Install)

// A mirrored part draws inside-out unless its proxy's LocalToWorldDeterminant is negative (addresses.hpp): set its sign
// (never flip it -- this runs for both eyes and every pass), for first-person parts only.
void SetDeterminantSign(std::uint8_t* proxy, bool negative) {
    if (!g_cullOk) return;
    float& det = *reinterpret_cast<float*>(proxy + addr::kProxyLocalToWorldDeterminant);
    const float mag = std::fabs(det) > 1e-6f ? std::fabs(det) : 1.0f;
    det = negative ? -mag : mag;
}

void __fastcall Hook_ViewModelTransform(std::uint8_t* proxy, void* /*edx*/, void* view, M4* outL2W, M4* outW2L) {
    const auto comp = *reinterpret_cast<const std::uintptr_t*>(proxy + addr::kProxyComponent);
    const float fov = comp ? *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov) : 0.0f;
    if (fov != 0.0f) NotePart(comp);
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    const bool mirrorNow = fov != 0.0f && s.mirrored && s.valid && GetTickCount() - s.tick <= 250;
    if (fov != 0.0f) SetDeterminantSign(proxy, mirrorNow);
    const M4& l2w = *reinterpret_cast<const M4*>(proxy + addr::kProxyLocalToWorld);
    const M4& w2l = *reinterpret_cast<const M4*>(proxy + addr::kProxyWorldToLocal);
    if (fov != 0.0f && armsik::IsBaked(comp)) {
        // The move (and the arms' IK) is already in this part's bone matrices, from the same frame for the arms and
        // the gun: drawn where it is, without the flat-screen FOV trick -- through the mirror in left-hand mode.
        if (mirrorNow) {
            *outL2W = Mul(l2w, s.mirror);
            *outW2L = Mul(s.mirror, w2l);
        } else {
            *outL2W = l2w;
            *outW2L = w2l;
        }
        return;
    }
    if (fov == 0.0f || !s.valid || GetTickCount() - s.tick > 250) {
        g_hook.thiscall<void>(proxy, view, outL2W, outW2L);  // not the first-person model, or not the player's view
        return;
    }
    *outL2W = mirrorNow ? Mul(Mul(l2w, s.d), s.mirror) : Mul(l2w, s.d);
    *outW2L = mirrorNow ? Mul(Mul(s.mirror, s.dInv), w2l) : Mul(s.dInv, w2l);

    // Calibration: where the first-person parts sit in the game camera's frame (Weapon.GripX/Y/Z).
    static DWORD nextLog = 0;
    static int partsThisLog = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextLog) >= 0) {
        nextLog = now + 10000;
        partsThisLog = 0;
    }
    if (partsThisLog < 4) {
        ++partsThisLog;
        const M4 local = Mul(l2w, s.camInv);
        MLOG("viewmodel: part %08X (FOV %.0f) origin in the camera frame: fwd %.1f right %.1f up %.1f",
             static_cast<unsigned>(comp), fov, local.m[3][0], local.m[3][1], local.m[3][2]);
    }
}

SafetyHookMid g_activityHook;
// The first-person part drawn isn't a weapon's (the parachute's harness) and no weapon's is: the game draws them.
bool g_noGunDrawn = false;
std::uintptr_t g_gunComp = 0;  // the weapon's first-person part (UpdateWeaponKey)
unsigned      g_sprintSwaps = 0, g_walkSwaps = 0, g_jumpSwaps = 0;

// Weapon.SprintArms (round 24; rounds 22-23: the sprint animation swung the gun out of the hand, and a speed-detected
// lock on top of it jittered and missed sprints): the first-person arms' activity node ticks with EAX = the pawn's
// CurrentActivity (addresses.hpp). For the local player's own arms, sprint (2) becomes idle (0) or walk (1), so they
// never blend into the <weapon>_sprint loop; the gun (on the arms' RightGun socket) and the game camera (FPArms' Cam
// socket) follow. The pawn still sprints -- speed, zoom, blur and sound come from its Stand_Sprint state, not this node.
// Weapon.WalkArms (round 25: "slight jitter in general movement ... a slight rubber banding feeling"): walking (1) and
// crouch-walking (3) become idle too. The walk and run loops sway the Cam socket -- the game camera, which the VR view is
// built on -- up to 3 cm side to side, 1.6 cm up and down and 2 cm fore and aft (ENGINE-NOTES 5ah), and the gun 3-9 cm in
// the hand. Crouch-idle is activity 0 as well; footsteps run on a timer, not on the animation.
// Weapon.JumpArms (round 26: "a similar animation to sprint happens when walking over rough terrain or falling a small
// distance"): jump start, falling and the soft and hard landings (26-29) become idle too -- a drop of over 75 units
// plays them (CheckForAnimatedJumpTransition), and they move the camera 18-24 units and up to 17 deg. The JumpStart,
// JumpIdle and JumpEnd states run on timers and Landed, not on the animations ending, so nothing waits for them.
void OnActivityTick(SafetyHookContext& ctx) {
    const bool sprint = ctx.eax == 2 && g_cfg.sprintArms != 0;               // PLAYER_ACTIVITY_STAND_SPRINT
    const bool walk = (ctx.eax == 1 || ctx.eax == 3) && g_cfg.walkArms;  // PLAYER_ACTIVITY_STAND_WALK, _CROUCH_WALK
    const bool jump = ctx.eax >= 26 && ctx.eax <= 29 && g_cfg.jumpArms;  // _JUMP_START, _JUMP_IDLE, _JUMP_END_SOFT/_HARD
    if (!sprint && !walk && !jump) return;
    const std::uintptr_t pawn = ctx.edi;
    if (!pawn || pawn != aim::LocalPlayerPawn()) return;
    const int ao = names::PropertyOffset(pawn, "FPArms");
    if (ao < 0 || names::ReadPointer(pawn + ao) != ctx.ebx) return;  // the first-person arms' tree only
    const std::uintptr_t to = sprint && g_cfg.sprintArms == 1 ? 1u : 0u;
    unsigned& swaps = sprint ? g_sprintSwaps : walk ? g_walkSwaps : g_jumpSwaps;
    if (*reinterpret_cast<const int*>(ctx.esi + addr::kActivityNodeActiveChild) != static_cast<int>(to) && swaps < 20) {
        ++swaps;  // a sprint, walk or jump starting (the node is about to blend)
        MLOG("viewmodel: %s -- the arms play %s instead of the %s animation (Weapon.%s)",
             sprint ? "sprinting" : walk ? "walking" : "jumping or landing", to ? "walk" : "idle",
             sprint ? "sprint" : walk ? "walk" : "jump", sprint ? "SprintArms" : walk ? "WalkArms" : "JumpArms");
    }
    ctx.eax = to;
}

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
    // Weapon.SprintArms, WalkArms and JumpArms are their own switches: they work whatever Weapon.ViewModel is (the game
    // camera's sprint shake, walk sway and landing dip come from the arms too).
    if (cfg.sprintArms != 0 || cfg.walkArms || cfg.jumpArms) {
        // Standing rule 4: the load of CurrentActivity and the compare the MidHook replaces.
        if (patch::BytesMatch(addr::kActivityTickLoad, addr::kActivityTickLoadBytes, sizeof(addr::kActivityTickLoadBytes)) &&
            patch::BytesMatch(addr::kActivityTickCmp, addr::kActivityTickCmpBytes, sizeof(addr::kActivityTickCmpBytes))) {
            auto mid = safetyhook::MidHook::create(reinterpret_cast<void*>(addr::kActivityTickCmp), OnActivityTick);
            if (mid) {
                g_activityHook = std::move(*mid);
                MLOG("viewmodel: Weapon.SprintArms=%s WalkArms=%s JumpArms=%s -- the arms' activity tick hooked at 0x%08X",
                     cfg.sprintArms == 0 ? "game" : cfg.sprintArms == 1 ? "walk" : "idle", cfg.walkArms ? "idle" : "game",
                     cfg.jumpArms ? "idle" : "game", static_cast<unsigned>(addr::kActivityTickCmp));
            } else {
                MLOG("viewmodel: activity tick hook failed (error %d) -- the game's sprint, walk and jump animations stay",
                     static_cast<int>(mid.error().type));
            }
        } else {
            MLOG("viewmodel: activity tick bytes differ -- the game's sprint, walk and jump animations stay");
        }
    }
    if (cfg.viewModel == 0) {
        MLOG("viewmodel: Weapon.ViewModel=0 -- the game's own first-person gun (flat-screen FOV)");
        return false;
    }
    if (!patch::BytesMatch(addr::kViewModelTransform, addr::kViewModelTransformBytes, sizeof(addr::kViewModelTransformBytes))) {
        MLOG("viewmodel: proxy transform bytes differ -- standing down (the game's own gun)");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kViewModelTransform),
                                              reinterpret_cast<void*>(&Hook_ViewModelTransform));
    if (!res) {
        MLOG("viewmodel: inline hook failed (error %d) -- the game's own gun", static_cast<int>(res.error().type));
        return false;
    }
    g_hook = std::move(*res);
    g_installed = true;
    if (g_cfg.leftHandMirror) {
        // Standing rule 4: the three reads of the proxy's determinant the mirror's culling depends on.
        g_cullOk = patch::BytesMatch(addr::kProxyDetReaderDraw, addr::kProxyDetReaderBytes, sizeof(addr::kProxyDetReaderBytes)) &&
                   patch::BytesMatch(addr::kProxyDetReaderDecalA, addr::kProxyDetReaderBytes, sizeof(addr::kProxyDetReaderBytes)) &&
                   patch::BytesMatch(addr::kProxyDetReaderDecalB, addr::kProxyDetReaderBytes, sizeof(addr::kProxyDetReaderBytes));
        if (!g_cullOk) g_cfg.leftHandMirror = false;
        MLOG("viewmodel: Weapon.LeftHandMirror=1 -- %s", g_cullOk ? "with the gun in the left hand, the arms and gun are drawn mirrored"
                                                                   : "the culling reads differ: no mirroring");
    }
    MLOG("viewmodel: Weapon.ViewModel=%d (%s) -- proxy transform hooked at 0x%08X; grip %.1f %.1f %.1f", cfg.viewModel,
         cfg.viewModel == 1 ? "true 3D" : "in the aiming hand", static_cast<unsigned>(addr::kViewModelTransform), cfg.gripX,
         cfg.gripY, cfg.gripZ);
    return true;
}

namespace {

// Research (Debug.Reflect, arm IK): the arms component's bone arrays and its mesh's skeleton layout, logged once.
void ProbeArms(std::uintptr_t comp) {
    const int sbo = names::PropertyOffset(comp, "SpaceBases"), lao = names::PropertyOffset(comp, "LocalAtoms"),
              smo = names::PropertyOffset(comp, "SkeletalMesh");
    const std::uintptr_t sbData = sbo >= 0 ? names::ReadPointer(comp + sbo) : 0;
    const int bones = sbo >= 0 ? static_cast<int>(names::ReadPointer(comp + sbo + 4)) : 0;
    const std::uintptr_t mesh = smo >= 0 ? names::ReadPointer(comp + smo) : 0;
    MLOG("arms: %s SpaceBases +0x%X (%d bones, data %08X) LocalAtoms +0x%X, mesh +0x%X = %s", names::Name(comp).c_str(), sbo,
         bones, static_cast<unsigned>(sbData), lao, smo, names::Name(mesh).c_str());
    const std::uintptr_t vt = names::ReadPointer(comp);
    std::string slots;
    for (int i = 0; i < 96; ++i) {
        char b[16];
        sprintf_s(b, " %X", static_cast<unsigned>(names::ReadPointer(vt + 4 * static_cast<std::uintptr_t>(i))));
        slots += b;
        if (i % 16 == 15) {
            MLOG("arms: vtable %08X [%d..%d]%s", static_cast<unsigned>(vt), i - 15, i, slots.c_str());
            slots.clear();
        }
    }
    if (!mesh || bones <= 0 || bones > 512) return;
    // Arrays in the mesh with exactly `bones` elements whose elements start with bone names.
    for (std::uintptr_t off = 0x3C; off < 0x400; off += 4) {
        const std::uintptr_t data = names::ReadPointer(mesh + off);
        const int num = static_cast<int>(names::ReadPointer(mesh + off + 4));
        if (num != bones || !data) continue;
        for (int stride = 8; stride <= 256; stride += 4) {
            // Every element must start with a plausible, distinct bone name.
            bool all = true;
            std::string prev;
            for (int i = 0; i < num && all; ++i) {
                const std::string n = names::NameAt(data + static_cast<std::uintptr_t>(i) * stride);
                all = !n.empty() && n.rfind("None", 0) != 0 && n.find("Property") == std::string::npos && n != prev;
                prev = n;
            }
            if (!all) continue;
            const std::string n0 = names::NameAt(data), n1 = names::NameAt(data + stride),
                              n2 = names::NameAt(data + 2 * static_cast<std::uintptr_t>(stride));
            MLOG("arms: mesh +0x%X: %d elements, stride %d, names %s %s %s ...", static_cast<unsigned>(off), num, stride,
                 n0.c_str(), n1.c_str(), n2.c_str());
            // The parent index: an int field that is < i for every bone i > 0.
            for (int k = 8; k < stride; k += 4) {
                bool ok = true;
                for (int i = 1; i < num && ok; ++i) {
                    const int v = static_cast<int>(names::ReadPointer(data + static_cast<std::uintptr_t>(i) * stride + k));
                    ok = v >= 0 && v < i;
                }
                if (ok) MLOG("arms:   parent index candidate at element +%d", k);
            }
            for (int i = 0; i < num; ++i) {
                const std::uintptr_t e = data + static_cast<std::uintptr_t>(i) * stride;
                MLOG("arms:   bone %2d %-28s words %X %X %X %X %X %X", i, names::NameAt(e).c_str(),
                     static_cast<unsigned>(names::ReadPointer(e + stride - 24)), static_cast<unsigned>(names::ReadPointer(e + stride - 20)),
                     static_cast<unsigned>(names::ReadPointer(e + stride - 16)), static_cast<unsigned>(names::ReadPointer(e + stride - 12)),
                     static_cast<unsigned>(names::ReadPointer(e + stride - 8)), static_cast<unsigned>(names::ReadPointer(e + stride - 4)));
            }
            return;
        }
    }
    MLOG("arms: no bone-name array of %d found in the mesh", bones);
}

// The weapon in the player's hands: the first-person part drawn lately whose Outer isn't the pawn (the arms'
// is). Its Outer is the weapon actor; the key is that actor's class name. Published to the host when it changes.
void UpdateWeaponKey(shared::Header* hdr) {
    static std::string current;
    static DWORD nextCheck = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextCheck) < 0) return;
    nextCheck = now + 250;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    static bool probed = false;
    if (g_cfg.debugReflect && pawn && !probed) {
        probed = true;
        names::ProbeReflection(pawn);
    }
    std::uintptr_t gun = 0;
    DWORD newest = 0;
    bool notGun = false;
    for (auto& p : g_parts) {
        const std::uintptr_t comp = p.comp.load(std::memory_order_relaxed);
        const DWORD tick = p.tick.load(std::memory_order_relaxed);
        if (!comp || now - tick > 500) continue;
        if (comp == offhand::CarrierComponent()) continue;  // the off-hand grenade: neither the gun nor "no gun drawn"
        const std::uintptr_t outer = names::Outer(comp);
        static bool armsProbed = false;
        if (g_cfg.debugReflect && outer && outer == pawn && !armsProbed) {
            armsProbed = true;
            ProbeArms(comp);
        }
        if (!outer || outer == pawn) continue;
        // (The player, 2026-10-01: "the chest of the character is held like a gun in the right hand" -- while parachuting
        // the newest part is the MOHAParachuteActor's harness. Only a weapon's attachment goes in the hand.)
        if (!names::IsA(outer, "WeaponAttachment")) {
            notGun = true;
            continue;
        }
        if (!gun || static_cast<LONG>(tick - newest) > 0) {
            gun = comp;
            newest = tick;
        }
    }
    // Something drawn first-person that isn't a weapon (the parachute) and no weapon: the game's own drawing (OnPlayerView).
    g_noGunDrawn = notGun && !gun;
    g_gunComp = gun;
    const std::string key = gun ? names::ClassName(names::Outer(gun)) : std::string();
    // What it is, by the weapon's class chain: a grenade (EALAGrenade), a pistol (MOHAPistol), else a long gun.
    static std::uint32_t currentKind = 0;
    std::uint32_t kind = 0;
    const int wo = pawn ? names::PropertyOffset(pawn, "Weapon") : -1;
    const std::uintptr_t weapon = wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
    if (weapon && names::IsA(weapon, "EALAGrenade")) kind = 2;
    else if (weapon && names::IsA(weapon, "MOHAPistol")) kind = 1;
    if (key == current && kind == currentKind) return;
    static const char* kKinds[] = {"long gun", "pistol", "grenade"};
    MLOG("viewmodel: weapon in hand: '%s' -- %s (part %s of %s; weapon %s)", key.c_str(), kKinds[kind],
         gun ? names::Name(gun).c_str() : "-", gun ? names::Name(names::Outer(gun)).c_str() : "-",
         weapon ? names::ClassName(weapon).c_str() : "-");
    current = key;
    currentKind = kind;
    if (!hdr) return;
    hdr->weaponKind = kind;
    const size_t n = key.size() < sizeof(hdr->weaponKey) - 1 ? key.size() : sizeof(hdr->weaponKey) - 1;
    std::memcpy(hdr->weaponKey, key.c_str(), n);
    hdr->weaponKey[n] = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->weaponSeq));
}

// [BarrelDir] (round 40, the Panzerschreck: "the further away I aim the more to the right the red dot goes"): the game's
// own pose points a gun's barrel (its mesh +Z) along the camera's forward -- all but the Panzerschreck, whose tube on the
// right shoulder is turned 12.9 deg left and 3.8 deg up, towards the flat screen's crosshair. In the hand the aim line
// runs along the controller, so the two parted with distance. A gun listed (its barrel's direction in the camera frame,
// forward right up) is drawn turned about the fit's grip point so the barrel runs along the controller. Per weapon key,
// read once.
M4 BarrelTurn(const char* key) {
    struct Cached {
        std::string key;
        M4          r;
    };
    static Cached cache[24];
    static int used = 0;
    for (int i = 0; i < used; ++i)
        if (cache[i].key == key) return cache[i].r;
    M4 r{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    wchar_t wkey[64], buf[96];
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    GetPrivateProfileStringW(L"BarrelDir", wkey, L"", buf, 96, g_cfg.iniPath.c_str());
    float a[3] = {0, 0, 0};
    if (swscanf_s(buf, L"%f %f %f", &a[0], &a[1], &a[2]) == 3) {
        const float n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (n > 0.5f) {
            for (float& x : a) x /= n;
            // The rotation (row vectors: v' = v * r) taking the barrel a onto forward b = (1, 0, 0), the shortest way.
            const float k[3] = {0.0f, a[2], -a[1]};  // a x b
            const float sn = std::sqrt(k[1] * k[1] + k[2] * k[2]), cs = a[0];
            if (sn > 1e-6f) {
                const float u[3] = {0.0f, k[1] / sn, k[2] / sn};
                for (int i = 0; i < 3; ++i) {
                    const float v[3] = {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
                    const float uv[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                    for (int j = 0; j < 3; ++j) r.m[i][j] = v[j] * cs + uv[j] * sn + u[j] * u[i] * (1.0f - cs);
                }
            }
            MLOG("viewmodel: %s's barrel (%.3f %.3f %.3f in the camera frame) turned onto the controller's forward ([BarrelDir]: "
                 "%.1f deg)", key, a[0], a[1], a[2], std::acos(cs > 1.0f ? 1.0f : cs) * 57.2958f);
        }
    }
    if (used < 24) cache[used++] = {key, r};
    return r;
}

// The fit for the weapon in hand: the host's (the menu, per weapon) when it's for this weapon, else the ini's.
shared::GunFit CurrentFit(const shared::Header* hdr) {
    shared::GunFit fit{{g_cfg.gripX, g_cfg.gripY, g_cfg.gripZ}, 0.0f, g_cfg.aimRayUp, 0.0f};
    shared::GunFit host{};
    char key[48];
    if (hdr && shared::ReadFit(hdr, host, key) && hdr->weaponKey[0] && !std::strncmp(key, hdr->weaponKey, 47)) fit = host;
    return fit;
}

}  // namespace

void OnPlayerView() {
    if (!g_installed) return;
    shared::Header* hdr = bridge::SharedHeader();
    UpdateWeaponKey(hdr);
    float camLoc[3], pitch = 0.0f, yaw = 0.0f;
    if (!view::GameCamera(camLoc, pitch, yaw)) {
        g_line.valid = false;
        return;
    }
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float cx[3] = {cp * cy, cp * sy, sp}, cyv[3] = {-sy, cy, 0.0f}, cz[3] = {-sp * cy, -sp * sy, cp};
    const M4 cam = Frame(cx, cyv, cz, camLoc);
    const M4 camInv = RigidInverse(cam);
    {
        // Research (round 40, the Panzerschreck's aim): the gun mesh's axes and origin in the game camera's frame (forward,
        // right, up; the game's own pose), once per weapon after 2 s in hand (settled) -- how the game points the barrel
        // against the view.
        static std::uintptr_t logged = 0, seen = 0;
        static DWORD seenAt = 0;
        const std::uintptr_t comp = g_gunComp;
        if (comp != seen) {
            seen = comp;
            seenAt = GetTickCount();
        }
        const int lo = comp ? names::PropertyOffset(comp, "LocalToWorld") : -1;
        if (comp && comp != logged && lo >= 0 && GetTickCount() - seenAt > 2000) {
            logged = comp;
            M4 l2w;
            std::memcpy(l2w.m, reinterpret_cast<const void*>(comp + lo), sizeof(l2w.m));
            const M4 local = Mul(l2w, camInv);
            MLOG("viewmodel: %s's mesh in the camera frame (fwd right up): X %.3f %.3f %.3f, Y %.3f %.3f %.3f, Z %.3f %.3f %.3f, "
                 "origin %.1f %.1f %.1f", names::ClassName(names::Outer(comp)).c_str(), local.m[0][0], local.m[0][1],
                 local.m[0][2], local.m[1][0], local.m[1][1], local.m[1][2], local.m[2][0], local.m[2][1], local.m[2][2],
                 local.m[3][0], local.m[3][1], local.m[3][2]);
        }
    }

    M4 d{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, dInv = d;  // ViewModel=1: where the game put it
    M4 gunFrameNow = d, offFrameNow = d, mirror = d, magFrameNow = d;
    bool offValidNow = false, twoHandedNow = false, mirroredNow = false, reloadValidNow = false, magValidNow = false;
    shared::ReloadView reloadNow{};
    float upmNow = 100.0f;
    if (g_cfg.viewModel != 2) g_line.valid = false;
    if (g_cfg.viewModel == 2) {
        // The host works the gun out from both hands (hands.cpp: the gun hand, the foregrip, the fit's angle and aim
        // line); here it is only mapped into the world like the eyes, and the fit's grip point put on it.
        shared::Pose gun, ray;
        std::uint32_t flags = 0xFFFFFFFFu;
        if (!hdr) return;
        if (g_noGunDrawn) {  // the parachute: where the game puts it, in true 3D
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            DrawWithoutHands(kIdentity);
            return;
        }
        if (!shared::ReadGun(hdr, gun, ray, flags)) {
            if (flags == 0xFFFFFFFFu) return;  // mid-write: keep last frame's
            g_line.valid = false;
            AcquireSRWLockExclusive(&g_lock);
            g_state.valid = false;  // no gun hand this frame: the game's own drawing
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        float pos[3], axes[3][3], upm = 100.0f;
        if (!view::PoseFrameToWorld(gun, pos, axes, upm)) return;
        upmNow = upm;
        const shared::GunFit fit = CurrentFit(hdr);
        const float (&gf)[3] = axes[0], (&gr)[3] = axes[1], (&gu)[3] = axes[2];
        // The gun hand's controller frame (the host's gun pose: the controller, pitched by the fit's angle).
        M4 ctrlFrame = Frame(gf, gr, gu, pos);
        // Weapon.LeftHandMirror (round 25: in left-hand mode the right arm reached across the chest to the gun): with the
        // gun in the left hand, work in a mirror world -- reflected across the body's centre plane (through the head,
        // across the body's right axis) -- where the left controller is a right hand and the game's right-handed
        // animations and the arm IK apply as they are; the parts are drawn back through the mirror (the proxy hook).
        float head[3], hyaw = 0.0f, hupm = 100.0f;
        if (g_cfg.leftHandMirror && (flags & 4u) && view::HeadInWorld(head, hyaw, hupm)) {
            const float n[3] = {-sy, cy, 0.0f};  // the body's right axis (the game camera's yaw)
            mirror = Reflection(head, n);
            ctrlFrame = MirrorFrame(ctrlFrame, mirror);
            mirroredNow = true;
        }
        // The camera-frame grip point lands on the controller: the gun frame is the controller's, its origin moved back by
        // the grip -- in the mirror world for the left hand, where the fit (tuned on the right hand) applies as it is and
        // comes out mirrored (round 26: put on before the mirror, its sideways part landed on the wrong side, 2 x 11 cm).
        const float back[3] = {-fit.grip[0], -fit.grip[1], -fit.grip[2]};
        const M4 gunFrame = Mul(Mul(Frame(kAxisX, kAxisY, kAxisZ, back), BarrelTurn(hdr->weaponKey)), ctrlFrame);
        d = Mul(camInv, gunFrame);
        dInv = Mul(RigidInverse(gunFrame), cam);
        // The aim line: the host's, mapped the same way.
        float rpos[3], rdir[3], rupm = 100.0f;
        g_line.valid = false;
        if (view::PoseToWorld(ray, rpos, rdir, rupm)) {
            for (int i = 0; i < 3; ++i) {
                g_line.pos[i] = rpos[i];
                g_line.dir[i] = rdir[i];
            }
            g_line.upm = rupm;
            g_line.valid = true;
            // Research (round 40): once per weapon, settled 3 s, the drawn barrel (mesh +Z through the move) against the
            // aim line: the angle between them ([BarrelDir] should bring it to ~0).
            static std::uintptr_t checked = 0, seen = 0;
            static DWORD seenAt = 0;
            const std::uintptr_t comp = g_gunComp;
            if (comp != seen) {
                seen = comp;
                seenAt = GetTickCount();
            }
            const int lo = comp ? names::PropertyOffset(comp, "LocalToWorld") : -1;
            if (comp && comp != checked && lo >= 0 && GetTickCount() - seenAt > 3000) {
                checked = comp;
                M4 l2w;
                std::memcpy(l2w.m, reinterpret_cast<const void*>(comp + lo), sizeof(l2w.m));
                const M4 drawn = Mul(l2w, d);
                float z[3] = {drawn.m[2][0], drawn.m[2][1], drawn.m[2][2]};
                const float zn = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
                const float c = (z[0] * rdir[0] + z[1] * rdir[1] + z[2] * rdir[2]) / (zn > 1e-6f ? zn : 1.0f);
                MLOG("viewmodel: %s's drawn barrel is %.2f deg off the aim line", hdr->weaponKey,
                     std::acos(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c) * 57.2958f);
            }
        }
        static std::uint32_t seenFlags = 0;
        if ((flags & 6u) != (seenFlags & 6u)) {
            MLOG("viewmodel: gun in the %s hand%s", (flags & 4u) ? "left" : "right", (flags & 2u) ? ", two-handed" : "");
            seenFlags = flags;
        }
        // For the arm IK: the gun's frame, and the other controller's (the free hand follows it off the foregrip).
        gunFrameNow = ctrlFrame;  // the gun hand's controller frame (its origin at the controller; mirrored with the gun)
        twoHandedNow = (flags & 2u) != 0;
        shared::Pose hand[2];
        std::uint32_t hv = 0;
        const int o = (flags & 4u) ? 1 : 0;  // the gun in the left hand -> the right one is free
        float opos[3], oaxes[3][3], oupm = 100.0f;
        const bool handsRead = shared::ReadHands(hdr, hand, hv, &reloadNow);
        if (handsRead && (hv & (1u << o)) && view::PoseFrameToWorld(hand[o], opos, oaxes, oupm)) {
            offFrameNow = Frame(oaxes[0], oaxes[1], oaxes[2], opos);
            if (mirroredNow) offFrameNow = MirrorFrame(offFrameNow, mirror);
            offValidNow = true;
        }
        // The magazine in the off hand (reloadFlags bits 1-2 = 2): its frame from the same host frame as that hand.
        reloadValidNow = handsRead;
        float mpos[3], maxes[3][3], mupm = 100.0f;
        if (handsRead && ((reloadNow.flags >> 1) & 3u) == 2u && view::PoseFrameToWorld(reloadNow.magPose, mpos, maxes, mupm)) {
            magFrameNow = Frame(maxes[0], maxes[1], maxes[2], mpos);
            if (mirroredNow) magFrameNow = MirrorFrame(magFrameNow, mirror);
            magValidNow = true;
        }
    }
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const M4 pawnFrame = pawn ? PawnFrame(pawn) : d;
    AcquireSRWLockExclusive(&g_lock);
    g_state.gunFrame = gunFrameNow;
    g_state.offFrame = offFrameNow;
    g_state.offValid = offValidNow;
    g_state.twoHanded = twoHandedNow;
    g_state.mirrored = mirroredNow;
    g_state.mirror = mirror;
    g_state.reloadValid = reloadValidNow;
    g_state.reload = reloadNow;
    g_state.magValid = magValidNow;
    g_state.magFrame = magFrameNow;
    g_state.upm = upmNow;
    g_state.pawn = pawn;
    g_state.pawnFrame = pawnFrame;
    g_state.valid = true;
    g_state.tick = GetTickCount();
    g_state.d = d;
    g_state.dInv = dInv;
    g_state.camInv = camInv;
    g_state.noHands = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void DrawWithoutHands(const float (&d)[16]) {
    if (!g_installed) return;
    g_line.valid = false;
    M4 m;
    std::memcpy(m.m, d, sizeof(m.m));
    const M4 inv = RigidInverse(m);
    AcquireSRWLockExclusive(&g_lock);
    g_state.valid = true;
    g_state.noHands = true;
    g_state.tick = GetTickCount();
    g_state.d = m;
    g_state.dInv = inv;
    g_state.mirrored = false;
    g_state.offValid = g_state.twoHanded = g_state.reloadValid = g_state.magValid = false;
    g_state.pawn = 0;
    ReleaseSRWLockExclusive(&g_lock);
    static bool logged = false;
    if (!logged) {
        logged = true;
        MLOG("viewmodel: first-person parts with no gun in the hand drawn in true 3D (the parachute, the landing)");
    }
}

bool HandFrames(float (&gun)[16], float (&off)[16], bool& offValid, bool& twoHanded) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.valid || s.noHands || GetTickCount() - s.tick > 250) return false;
    std::memcpy(gun, s.gunFrame.m, sizeof(gun));
    std::memcpy(off, s.offFrame.m, sizeof(off));
    offValid = s.offValid;
    twoHanded = s.twoHanded;
    return true;
}

bool ReloadInputs(ReloadFrame& out) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.valid || s.noHands || !s.reloadValid || GetTickCount() - s.tick > 250) return false;
    out.view = s.reload;
    std::memcpy(out.magFrame, s.magFrame.m, sizeof(out.magFrame));
    out.magValid = s.magValid;
    out.mirrored = s.mirrored;
    out.upm = s.upm;
    return true;
}

bool CurrentMove(float (&d)[16], float (&dInv)[16]) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.valid || s.noHands || GetTickCount() - s.tick > 250) return false;
    std::memcpy(d, s.d.m, sizeof(d));
    std::memcpy(dInv, s.dInv.m, sizeof(dInv));
    return true;
}

bool DrawMirror(float (&r)[16]) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    // As the proxy hook decides it for the first-person parts.
    if (!s.mirrored || !s.valid || s.noHands || GetTickCount() - s.tick > 250) return false;
    std::memcpy(r, s.mirror.m, sizeof(r));
    return true;
}

bool BodyMoveSinceView(float (&w)[16]) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!s.valid || s.noHands || !s.pawn || pawn != s.pawn || GetTickCount() - s.tick > 250) return false;
    const M4 now = PawnFrame(pawn);
    // More than a frame's walk isn't one (a teleport, a respawn at the same pawn): no catch-up.
    const float dx = now.m[3][0] - s.pawnFrame.m[3][0], dy = now.m[3][1] - s.pawnFrame.m[3][1], dz = now.m[3][2] - s.pawnFrame.m[3][2];
    if (dx * dx + dy * dy + dz * dz > 100.0f * 100.0f) return false;
    const M4 m = Mul(RigidInverse(s.pawnFrame), now);
    std::memcpy(w, m.m, sizeof(w));
    return true;
}

bool NoGunDrawn() { return g_noGunDrawn; }

bool GunRay(float (&pos)[3], float (&dir)[3], float& unitsPerMeter) {
    if (!g_line.valid) return false;
    for (int i = 0; i < 3; ++i) {
        pos[i] = g_line.pos[i];
        dir[i] = g_line.dir[i];
    }
    unitsPerMeter = g_line.upm;
    return true;
}

}  // namespace mohavr::viewmodel
