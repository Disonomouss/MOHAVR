#include "aim.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
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
};
AimFrame g_frame{};

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

// The trace; false (and `hit` = end) when nothing is in the way.
bool Trace(std::uintptr_t source, const float (&start)[3], const float (&end)[3], float (&hit)[3]) {
    void* world = *reinterpret_cast<void**>(addr::kGWorld);
    std::memcpy(hit, end, sizeof(hit));
    if (!world) return false;
    alignas(16) std::uint8_t result[0x80] = {};
    *reinterpret_cast<float*>(result + addr::kCheckResultTime) = 1.0f;
    *reinterpret_cast<int*>(result + addr::kCheckResultItem) = -1;
    const float extent[3] = {0.0f, 0.0f, 0.0f};
    const int clear = CallSingleLineCheck(world, result, reinterpret_cast<void*>(source), end, start, extent,
                                          addr::kTraceFlagsActors, addr::kSingleLineCheck);
    if (clear) return false;
    std::memcpy(hit, result + addr::kCheckResultLocation, sizeof(hit));
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

void Publish(float distance, std::uint32_t source) {
    if (shared::Header* hdr = bridge::SharedHeader()) {
        hdr->aimDistance = distance;
        hdr->aimSource = source;
    }
}

// Frames since the last ray log, and those whose hand read met the host mid-write (kept the last aim).
unsigned g_frames = 0, g_torn = 0;

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
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        MLOG("aim: pawn %08X base aim P%d Y%d -> P%d Y%d (aim point %.0f %.0f %.0f from %.0f %.0f %.0f)",
             static_cast<unsigned>(self), before[0] & 0xFFFF, before[1] & 0xFFFF, result[0], result[1], g_frame.point[0],
             g_frame.point[1], g_frame.point[2], g_frame.start[0], g_frame.start[1], g_frame.start[2]);
    }
}

}  // namespace

std::uintptr_t LocalPlayerPawn() { return LocalPawn(LocalController()); }

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
    static const char* kNames[] = {"game", "head", "left hand", "right hand"};
    MLOG("aim: Aim.Mode=%d (%s) -- execGetBaseAimRotation hooked at 0x%08X", cfg.aimMode, kNames[cfg.aimMode],
         static_cast<unsigned>(addr::kExecGetBaseAimRotation));
    return true;
}

void OnPlayerView(std::uintptr_t ctrl, const float (&shotStart)[3]) {
    if (!g_installed) return;
    const shared::Header* hdr = bridge::SharedHeader();
    const std::uintptr_t pawn = LocalPawn(ctrl);
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
    bool hit = Trace(pawn, pos, end, point);
    // A hit right at the start means the ray began inside something (headset round 18: 6 of 37 rays at 0.0 m while
    // moving along walls -- the aim then pointed from the eye at the hand). Trace again from just past it.
    const float skip = 20.0f * upm / 100.0f;
    const float dx0 = point[0] - pos[0], dy0 = point[1] - pos[1], dz0 = point[2] - pos[2];
    if (hit && dx0 * dx0 + dy0 * dy0 + dz0 * dz0 < skip * skip) {
        const float from[3] = {pos[0] + fwd[0] * skip, pos[1] + fwd[1] * skip, pos[2] + fwd[2] * skip};
        hit = Trace(pawn, from, end, point);
    }
    g_frame.valid = true;
    g_frame.tick = GetTickCount();
    std::memcpy(g_frame.start, shotStart, sizeof(g_frame.start));
    std::memcpy(g_frame.point, point, sizeof(g_frame.point));
    const float dx = point[0] - pos[0], dy = point[1] - pos[1], dz = point[2] - pos[2];
    Publish(std::sqrt(dx * dx + dy * dy + dz * dz) / upm, static_cast<std::uint32_t>(g_cfg.aimMode));
    static DWORD nextLog = 0;
    if (static_cast<LONG>(g_frame.tick - nextLog) >= 0) {
        nextLog = g_frame.tick + 5000;
        MLOG("aim: ray from %.0f %.0f %.0f dir %.2f %.2f %.2f -> %s at %.0f %.0f %.0f (%.1f m; %s; %u of %u frames torn)",
             pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2], hit ? "hit" : "nothing", point[0], point[1], point[2],
             std::sqrt(dx * dx + dy * dy + dz * dz) / upm, barrel ? "barrel" : "controller", g_torn, g_frames);
        g_torn = g_frames = 0;
    }
}

}  // namespace mohavr::aim
