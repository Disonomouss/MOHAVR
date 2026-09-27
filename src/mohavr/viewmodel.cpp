#include "viewmodel.hpp"

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

// Game thread -> render thread: the move for the arms/gun (the latest player view).
SRWLOCK g_lock = SRWLOCK_INIT;
struct State {
    bool  valid;
    DWORD tick;
    M4    d, dInv;  // LocalToWorld' = LocalToWorld * d; WorldToLocal' = dInv * WorldToLocal
    M4    camInv;   // the game camera's inverse (calibration log)
} g_state{};

void __fastcall Hook_ViewModelTransform(std::uint8_t* proxy, void* /*edx*/, void* view, M4* outL2W, M4* outW2L) {
    const auto comp = *reinterpret_cast<const std::uintptr_t*>(proxy + addr::kProxyComponent);
    const float fov = comp ? *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov) : 0.0f;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (fov == 0.0f || !s.valid || GetTickCount() - s.tick > 250) {
        g_hook.thiscall<void>(proxy, view, outL2W, outW2L);  // not the first-person model, or not the player's view
        return;
    }
    const M4& l2w = *reinterpret_cast<const M4*>(proxy + addr::kProxyLocalToWorld);
    const M4& w2l = *reinterpret_cast<const M4*>(proxy + addr::kProxyWorldToLocal);
    *outL2W = Mul(l2w, s.d);
    *outW2L = Mul(s.dInv, w2l);

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

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
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
    MLOG("viewmodel: Weapon.ViewModel=%d (%s) -- proxy transform hooked at 0x%08X; grip %.1f %.1f %.1f", cfg.viewModel,
         cfg.viewModel == 1 ? "true 3D" : "in the aiming hand", static_cast<unsigned>(addr::kViewModelTransform), cfg.gripX,
         cfg.gripY, cfg.gripZ);
    return true;
}

void OnPlayerView() {
    if (!g_installed) return;
    float camLoc[3], pitch = 0.0f, yaw = 0.0f;
    if (!view::GameCamera(camLoc, pitch, yaw)) return;
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float cx[3] = {cp * cy, cp * sy, sp}, cyv[3] = {-sy, cy, 0.0f}, cz[3] = {-sp * cy, -sp * sy, cp};
    const M4 cam = Frame(cx, cyv, cz, camLoc);
    const M4 camInv = RigidInverse(cam);

    M4 d{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, dInv = d;  // ViewModel=1: where the game put it
    if (g_cfg.viewModel == 2) {
        const shared::Header* hdr = bridge::SharedHeader();
        shared::Pose hand[2];
        std::uint32_t valid = 0;
        const int h = g_cfg.aimMode == 2 ? 0 : 1;  // the aiming hand (right unless Aim.Mode=2)
        if (!hdr || !shared::ReadHands(hdr, hand, valid)) return;  // mid-write: keep the last one
        if (!(valid & (1u << h))) {
            AcquireSRWLockExclusive(&g_lock);
            g_state.valid = false;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        float pos[3], axes[3][3], upm = 100.0f;
        if (!view::PoseFrameToWorld(hand[h], pos, axes, upm)) return;
        // The camera-frame grip point lands on the controller: the hand frame's origin is moved back by it.
        float t[3];
        for (int i = 0; i < 3; ++i)
            t[i] = pos[i] - (g_cfg.gripX * axes[0][i] + g_cfg.gripY * axes[1][i] + g_cfg.gripZ * axes[2][i]);
        const M4 handFrame = Frame(axes[0], axes[1], axes[2], t);
        d = Mul(camInv, handFrame);
        dInv = Mul(RigidInverse(handFrame), cam);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state.valid = true;
    g_state.tick = GetTickCount();
    g_state.d = d;
    g_state.dInv = dInv;
    g_state.camInv = camInv;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace mohavr::viewmodel
