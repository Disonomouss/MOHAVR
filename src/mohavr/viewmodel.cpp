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
    // Arm IK: the gun's frame and the other (off) controller's frame in the world (rows: forward, right, up, origin).
    M4    gunFrame, offFrame;
    bool  offValid, twoHanded;
} g_state{};

void __fastcall Hook_ViewModelTransform(std::uint8_t* proxy, void* /*edx*/, void* view, M4* outL2W, M4* outW2L) {
    const auto comp = *reinterpret_cast<const std::uintptr_t*>(proxy + addr::kProxyComponent);
    const float fov = comp ? *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov) : 0.0f;
    if (fov != 0.0f) NotePart(comp);
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    const M4& l2w = *reinterpret_cast<const M4*>(proxy + addr::kProxyLocalToWorld);
    const M4& w2l = *reinterpret_cast<const M4*>(proxy + addr::kProxyWorldToLocal);
    if (fov != 0.0f && armsik::IsBaked(comp)) {
        // The move (and the arms' IK) is already in this part's bone matrices, from the same frame for the arms and
        // the gun: drawn where it is, without the flat-screen FOV trick.
        *outL2W = l2w;
        *outW2L = w2l;
        return;
    }
    if (fov == 0.0f || !s.valid || GetTickCount() - s.tick > 250) {
        g_hook.thiscall<void>(proxy, view, outL2W, outW2L);  // not the first-person model, or not the player's view
        return;
    }
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
    for (auto& p : g_parts) {
        const std::uintptr_t comp = p.comp.load(std::memory_order_relaxed);
        const DWORD tick = p.tick.load(std::memory_order_relaxed);
        if (!comp || now - tick > 500) continue;
        const std::uintptr_t outer = names::Outer(comp);
        static bool armsProbed = false;
        if (g_cfg.debugReflect && outer && outer == pawn && !armsProbed) {
            armsProbed = true;
            ProbeArms(comp);
        }
        if (!outer || outer == pawn) continue;
        if (!gun || static_cast<LONG>(tick - newest) > 0) {
            gun = comp;
            newest = tick;
        }
    }
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

    M4 d{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, dInv = d;  // ViewModel=1: where the game put it
    M4 gunFrameNow = d, offFrameNow = d;
    bool offValidNow = false, twoHandedNow = false;
    if (g_cfg.viewModel != 2) g_line.valid = false;
    if (g_cfg.viewModel == 2) {
        // The host works the gun out from both hands (hands.cpp: the gun hand, the foregrip, the fit's angle and aim
        // line); here it is only mapped into the world like the eyes, and the fit's grip point put on it.
        shared::Pose gun, ray;
        std::uint32_t flags = 0xFFFFFFFFu;
        if (!hdr) return;
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
        const shared::GunFit fit = CurrentFit(hdr);
        const float (&gf)[3] = axes[0], (&gr)[3] = axes[1], (&gu)[3] = axes[2];
        // The camera-frame grip point lands on the controller: the gun frame's origin is moved back by it.
        float t[3];
        for (int i = 0; i < 3; ++i) t[i] = pos[i] - (fit.grip[0] * gf[i] + fit.grip[1] * gr[i] + fit.grip[2] * gu[i]);
        const M4 gunFrame = Frame(gf, gr, gu, t);
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
        }
        static std::uint32_t seenFlags = 0;
        if ((flags & 6u) != (seenFlags & 6u)) {
            MLOG("viewmodel: gun in the %s hand%s", (flags & 4u) ? "left" : "right", (flags & 2u) ? ", two-handed" : "");
            seenFlags = flags;
        }
        // For the arm IK: the gun's frame, and the other controller's (the free hand follows it off the foregrip).
        gunFrameNow = Frame(gf, gr, gu, pos);  // the gun hand's controller frame (its origin at the controller)
        twoHandedNow = (flags & 2u) != 0;
        shared::Pose hand[2];
        std::uint32_t hv = 0;
        const int o = (flags & 4u) ? 1 : 0;  // the gun in the left hand -> the right one is free
        float opos[3], oaxes[3][3], oupm = 100.0f;
        if (shared::ReadHands(hdr, hand, hv) && (hv & (1u << o)) && view::PoseFrameToWorld(hand[o], opos, oaxes, oupm)) {
            offFrameNow = Frame(oaxes[0], oaxes[1], oaxes[2], opos);
            offValidNow = true;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state.gunFrame = gunFrameNow;
    g_state.offFrame = offFrameNow;
    g_state.offValid = offValidNow;
    g_state.twoHanded = twoHandedNow;
    g_state.valid = true;
    g_state.tick = GetTickCount();
    g_state.d = d;
    g_state.dInv = dInv;
    g_state.camInv = camInv;
    ReleaseSRWLockExclusive(&g_lock);
}
bool HandFrames(float (&gun)[16], float (&off)[16], bool& offValid, bool& twoHanded) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.valid || GetTickCount() - s.tick > 250) return false;
    std::memcpy(gun, s.gunFrame.m, sizeof(gun));
    std::memcpy(off, s.offFrame.m, sizeof(off));
    offValid = s.offValid;
    twoHanded = s.twoHanded;
    return true;
}

bool CurrentMove(float (&d)[16], float (&dInv)[16]) {
    if (!g_installed || g_cfg.viewModel != 2) return false;
    State s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.valid || GetTickCount() - s.tick > 250) return false;
    std::memcpy(d, s.d.m, sizeof(d));
    std::memcpy(dInv, s.dInv.m, sizeof(dInv));
    return true;
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
