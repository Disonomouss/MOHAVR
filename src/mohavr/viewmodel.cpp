#include "viewmodel.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
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
} g_state{};

void __fastcall Hook_ViewModelTransform(std::uint8_t* proxy, void* /*edx*/, void* view, M4* outL2W, M4* outW2L) {
    const auto comp = *reinterpret_cast<const std::uintptr_t*>(proxy + addr::kProxyComponent);
    const float fov = comp ? *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov) : 0.0f;
    if (fov != 0.0f) NotePart(comp);
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

namespace {

// The weapon in the player's hands: the first-person part drawn lately whose Outer isn't the pawn (the arms'
// is). Its Outer is the weapon actor; the key is that actor's class name. Published to the host when it changes.
void UpdateWeaponKey(shared::Header* hdr) {
    static std::string current;
    static DWORD nextCheck = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextCheck) < 0) return;
    nextCheck = now + 250;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    std::uintptr_t gun = 0;
    DWORD newest = 0;
    for (auto& p : g_parts) {
        const std::uintptr_t comp = p.comp.load(std::memory_order_relaxed);
        const DWORD tick = p.tick.load(std::memory_order_relaxed);
        if (!comp || now - tick > 500) continue;
        const std::uintptr_t outer = names::Outer(comp);
        if (!outer || outer == pawn) continue;
        if (!gun || static_cast<LONG>(tick - newest) > 0) {
            gun = comp;
            newest = tick;
        }
    }
    const std::string key = gun ? names::ClassName(names::Outer(gun)) : std::string();
    if (key == current) return;
    MLOG("viewmodel: weapon in hand: '%s' (part %s of %s)", key.c_str(), gun ? names::Name(gun).c_str() : "-",
         gun ? names::Name(names::Outer(gun)).c_str() : "-");
    current = key;
    if (!hdr) return;
    const size_t n = key.size() < sizeof(hdr->weaponKey) - 1 ? key.size() : sizeof(hdr->weaponKey) - 1;
    std::memcpy(hdr->weaponKey, key.c_str(), n);
    hdr->weaponKey[n] = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->weaponSeq));
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
    g_line.valid = false;
    if (!g_installed) return;
    shared::Header* hdr = bridge::SharedHeader();
    UpdateWeaponKey(hdr);
    float camLoc[3], pitch = 0.0f, yaw = 0.0f;
    if (!view::GameCamera(camLoc, pitch, yaw)) return;
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float cx[3] = {cp * cy, cp * sy, sp}, cyv[3] = {-sy, cy, 0.0f}, cz[3] = {-sp * cy, -sp * sy, cp};
    const M4 cam = Frame(cx, cyv, cz, camLoc);
    const M4 camInv = RigidInverse(cam);

    M4 d{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, dInv = d;  // ViewModel=1: where the game put it
    if (g_cfg.viewModel == 2) {
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
        const shared::GunFit fit = CurrentFit(hdr);
        // The gun's frame: the controller's, pitched by the fit's angle about its right axis (+ = muzzle up).
        const float a = fit.angle * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
        float gf[3], gr[3], gu[3];
        for (int i = 0; i < 3; ++i) {
            gf[i] = axes[0][i] * ca + axes[2][i] * sa;
            gr[i] = axes[1][i];
            gu[i] = axes[2][i] * ca - axes[0][i] * sa;
        }
        // The camera-frame grip point lands on the controller: the gun frame's origin is moved back by it.
        float t[3];
        for (int i = 0; i < 3; ++i) t[i] = pos[i] - (fit.grip[0] * gf[i] + fit.grip[1] * gr[i] + fit.grip[2] * gu[i]);
        const M4 gunFrame = Frame(gf, gr, gu, t);
        d = Mul(camInv, gunFrame);
        dInv = Mul(RigidInverse(gunFrame), cam);
        // The aim line along the barrel: from the controller, offset by the fit's ray (cm), along the gun.
        const float up = fit.rayUp * upm / 100.0f, right = fit.rayRight * upm / 100.0f;
        for (int i = 0; i < 3; ++i) {
            g_line.pos[i] = pos[i] + gu[i] * up + gr[i] * right;
            g_line.dir[i] = gf[i];
        }
        g_line.upm = upm;
        g_line.valid = true;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state.valid = true;
    g_state.tick = GetTickCount();
    g_state.d = d;
    g_state.dInv = dInv;
    g_state.camInv = camInv;
    ReleaseSRWLockExclusive(&g_lock);
}

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
