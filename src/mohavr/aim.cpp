#include "aim.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstring>

#include "addresses.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "patch.hpp"
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
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        MLOG("aim: pawn %08X base aim P%d Y%d -> P%d Y%d (aim point %.0f %.0f %.0f from %.0f %.0f %.0f)",
             static_cast<unsigned>(self), before[0] & 0xFFFF, before[1] & 0xFFFF, result[0], result[1], g_frame.point[0],
             g_frame.point[1], g_frame.point[2], g_frame.start[0], g_frame.start[1], g_frame.start[2]);
    }
}

}  // namespace

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
    static const char* kNames[] = {"game", "head", "left hand", "right hand"};
    MLOG("aim: Aim.Mode=%d (%s) -- execGetBaseAimRotation hooked at 0x%08X", cfg.aimMode, kNames[cfg.aimMode],
         static_cast<unsigned>(addr::kExecGetBaseAimRotation));
    return true;
}

void OnPlayerView(std::uintptr_t ctrl, const float (&shotStart)[3]) {
    if (!g_installed) return;
    g_frame.valid = false;
    const shared::Header* hdr = bridge::SharedHeader();
    const std::uintptr_t pawn = LocalPawn(ctrl);
    if (!hdr || !pawn) {
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
        if (!shared::ReadHands(hdr, hand, valid)) return;
        const int h = g_cfg.aimMode - 2;
        if (!(valid & (1u << h))) {
            Publish(0.0f, 0);
            return;
        }
        pose = hand[h];
    }
    float pos[3], axes[3][3], upm = 100.0f;
    if (!view::PoseFrameToWorld(pose, pos, axes, upm)) return;
    const float (&fwd)[3] = axes[0];
    // With the gun drawn in the hand (Weapon.ViewModel=2) the ray runs along its barrel, Aim.RayUp above the
    // controller's aim pose (headset round 12: the shots were ~8 cm below the barrel).
    if (g_cfg.aimMode >= 2 && g_cfg.viewModel == 2) {
        const float lift = g_cfg.aimRayUp * upm / 100.0f;
        for (int i = 0; i < 3; ++i) pos[i] += axes[2][i] * lift;
    }
    const float reach = kTraceMeters * upm;
    const float end[3] = {pos[0] + fwd[0] * reach, pos[1] + fwd[1] * reach, pos[2] + fwd[2] * reach};
    float point[3];
    const bool hit = Trace(pawn, pos, end, point);
    g_frame.valid = true;
    g_frame.tick = GetTickCount();
    std::memcpy(g_frame.start, shotStart, sizeof(g_frame.start));
    std::memcpy(g_frame.point, point, sizeof(g_frame.point));
    const float dx = point[0] - pos[0], dy = point[1] - pos[1], dz = point[2] - pos[2];
    Publish(std::sqrt(dx * dx + dy * dy + dz * dz) / upm, static_cast<std::uint32_t>(g_cfg.aimMode));
    static DWORD nextLog = 0;
    if (static_cast<LONG>(g_frame.tick - nextLog) >= 0) {
        nextLog = g_frame.tick + 5000;
        MLOG("aim: ray from %.0f %.0f %.0f dir %.2f %.2f %.2f -> %s at %.0f %.0f %.0f (%.1f m)", pos[0], pos[1], pos[2],
             fwd[0], fwd[1], fwd[2], hit ? "hit" : "nothing", point[0], point[1], point[2],
             std::sqrt(dx * dx + dy * dy + dz * dz) / upm);
    }
}

}  // namespace mohavr::aim
