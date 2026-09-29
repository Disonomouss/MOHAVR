#include "muzzle.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "config.hpp"
#include "game_exec.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::muzzle {
namespace {

constexpr float kUnrToRad = 3.14159265f / 32768.0f;

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

// FRotationTranslationMatrix (rotator in Unreal units).
M4 RotationTranslation(const int (&rot)[3], const float (&t)[3]) {
    const float sp = std::sin(rot[0] * kUnrToRad), cp = std::cos(rot[0] * kUnrToRad);
    const float sy = std::sin(rot[1] * kUnrToRad), cy = std::cos(rot[1] * kUnrToRad);
    const float sr = std::sin(rot[2] * kUnrToRad), cr = std::cos(rot[2] * kUnrToRad);
    return M4{{{cp * cy, cp * sy, sp, 0.0f},
               {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp, 0.0f},
               {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp, 0.0f},
               {t[0], t[1], t[2], 1.0f}}};
}

// FMatrix::Rotator: pitch and yaw from X, roll from Z and Y against the unrolled Y axis.
void ToRotator(const M4& m, int (&rot)[3]) {
    const float(&x)[4] = m.m[0], (&y)[4] = m.m[1], (&z)[4] = m.m[2];
    const float pitch = std::atan2(x[2], std::sqrt(x[0] * x[0] + x[1] * x[1]));
    const float yaw = std::atan2(x[1], x[0]);
    const float syx = -std::sin(yaw), syy = std::cos(yaw);
    const float roll = std::atan2(z[0] * syx + z[1] * syy, y[0] * syx + y[1] * syy);
    rot[0] = static_cast<int>(std::lround(pitch / kUnrToRad));
    rot[1] = static_cast<int>(std::lround(yaw / kUnrToRad));
    rot[2] = static_cast<int>(std::lround(roll / kUnrToRad));
}

float Det3(const M4& a) {
    const float (&m)[4][4] = a.m;
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Config           g_cfg;
SafetyHookInline g_hook;
int              g_logged = 0, g_checked = 0;
int              g_freezeIn = -1;    // Debug.MuzzleFreeze: Draws left until the world is paused (-1: not armed)
std::uintptr_t   g_frozenFlash = 0;  // the flash the freeze logs

enum Kind { kNone, kFlash, kBrass };
const char* const kKindName[] = {"", "muzzle flash", "brass"};

// The local player's weapon attachment's flash or brass component, and the gun its sockets are on.
Kind Classify(std::uintptr_t psc, std::uintptr_t& gun) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn || !psc) return kNone;
    const int ao = names::PropertyOffset(pawn, "CurrentWeaponAttachment");
    const std::uintptr_t att = ao >= 0 ? names::ReadPointer(pawn + ao) : 0;
    if (!att) return kNone;
    const int fo = names::PropertyOffset(att, "MuzzleFlashPSComponent"), bo = names::PropertyOffset(att, "ShellEjectPSComponent");
    Kind k = kNone;
    if (fo >= 0 && names::ReadPointer(att + fo) == psc) k = kFlash;
    else if (bo >= 0 && names::ReadPointer(att + bo) == psc) k = kBrass;
    if (k == kNone) return kNone;
    const int mo = names::PropertyOffset(att, "Mesh");  // WeaponAttachment.Mesh: the first-person gun for the player
    gun = mo >= 0 ? names::ReadPointer(att + mo) : 0;
    return k;
}

// The brass through the left hand's mirror (round 27: "the brass comes out of the wrong side of the gun and flies off in
// the wrong direction"): its emitters are world-space casing meshes whose start velocity and offset go through the
// component's full LocalToWorld at spawn, in the socket's frame (BAR: +Y 150 sideways, +Z 100 up; the Garand +200 along
// X too), so a rotator alone throws them unmirrored. A negative Scale3D.Y on top of the rotation (its Y flipped back)
// makes LocalToWorld the exact mirrored frame (ActivateSystem applies it; the casing meshes, sized by Scale x Scale3D,
// draw as solid mirror images). Positive otherwise, as the game creates it (SetScale 1).
void SetBrassMirrored(std::uintptr_t psc, bool mirrored) {
    const int so = names::PropertyOffset(psc, "Scale3D");
    if (so < 0) return;
    float& y = reinterpret_cast<float*>(psc + so)[1];
    y = mirrored ? -std::fabs(y) : std::fabs(y);
}

// barrel: the component's pending transform (the game's socket) x the gun's drawn move (and the left hand's mirror).
void MoveToDrawnGun(std::uintptr_t psc, Kind kind, std::uintptr_t gun) {
    if (kind == kBrass) SetBrassMirrored(psc, false);
    float d[16];
    if (!gun || !armsik::BakedMove(gun, d)) return;  // the gun isn't drawn moved: leave it where the game put it
    const int to = names::PropertyOffset(psc, "Translation"), ro = names::PropertyOffset(psc, "Rotation");
    int uo = -1;
    std::uint32_t um = 0;
    if (to < 0 || ro < 0 || !names::BoolProperty(psc, "bNeedsUpdateTransform", uo, um)) return;
    float* t = reinterpret_cast<float*>(psc + to);
    int* r = reinterpret_cast<int*>(psc + ro);
    const float t0[3] = {t[0], t[1], t[2]};
    const int r0[3] = {r[0], r[1], r[2]};
    M4 move;
    std::memcpy(move.m, d, sizeof(move.m));
    float mirror[16];
    if (viewmodel::DrawMirror(mirror)) {
        M4 m;
        std::memcpy(m.m, mirror, sizeof(m.m));
        move = Mul(move, m);
    }
    M4 s = Mul(RotationTranslation(r0, t0), move);
    const bool reflected = Det3(s) < 0.0f;
    if (reflected)  // through the mirror: a rotator can't reflect -- its own Y (across the barrel) flipped back
        for (int j = 0; j < 3; ++j) s.m[1][j] = -s.m[1][j];
    if (kind == kBrass && g_cfg.brassMirror) SetBrassMirrored(psc, reflected);
    int r1[3];
    ToRotator(s, r1);
    for (int i = 0; i < 3; ++i) {
        t[i] = s.m[3][i];
        r[i] = r1[i];
    }
    *reinterpret_cast<std::uint32_t*>(psc + uo) |= um;  // as SetTranslation leaves it: ActivateSystem applies it
    if (g_logged < 8) {
        ++g_logged;
        const float dx = t[0] - t0[0], dy = t[1] - t0[1], dz = t[2] - t0[2];
        MLOG("muzzle: the %s moved to the drawn gun -- from %.1f %.1f %.1f to %.1f %.1f %.1f (%.1f cm), rotation %d %d %d -> "
             "%d %d %d%s", kKindName[kind], t0[0], t0[1], t0[2], t[0], t[1], t[2], std::sqrt(dx * dx + dy * dy + dz * dz), r0[0],
             r0[1], r0[2], r[0], r[1], r[2],
             !reflected ? "" : kind == kBrass && g_cfg.brassMirror ? " (mirrored: Scale3D.Y negative)" : " (mirrored: its Y flipped back)");
    }
}

// Weapon.MuzzleFlash / Brass: 0 hide, 1 the game's, 2 on the drawn gun.
int ModeOf(Kind kind) { return kind == kFlash ? g_cfg.muzzleFlash : kind == kBrass ? g_cfg.brass : 1; }

void __fastcall Hook_ExecActivateSystem(std::uintptr_t psc, void* /*edx*/, void* stack, void* result) {
    std::uintptr_t gun = 0;
    const Kind kind = Classify(psc, gun);
    const int mode = ModeOf(kind);
    float want[3] = {};
    const bool move = kind != kNone && mode == 2;
    if (move) {
        MoveToDrawnGun(psc, kind, gun);
        const int to = names::PropertyOffset(psc, "Translation");
        if (to >= 0) std::memcpy(want, reinterpret_cast<const void*>(psc + to), sizeof(want));
    }
    g_hook.thiscall<void>(psc, stack, result);
    if (kind == kNone) return;
    static bool frozeOnce = false;
    if (kind == kFlash && g_cfg.debugMuzzleFreeze > 0 && !frozeOnce) {
        frozeOnce = true;
        g_freezeIn = g_cfg.debugMuzzleFreeze - 1;  // 1: at the first Draw after the activation
        g_frozenFlash = psc;
    }
    if (mode == 0) {
        // hide: ActivateSystem cleared it; with it set the emitters skip spawning (bursts too) until the next shot.
        int so = -1;
        std::uint32_t sm = 0;
        if (names::BoolProperty(psc, "bSuppressSpawning", so, sm)) *reinterpret_cast<std::uint32_t*>(psc + so) |= sm;
        if (g_logged < 4) {
            ++g_logged;
            MLOG("muzzle: the %s hidden (Weapon.%s=hide)", kKindName[kind], kind == kFlash ? "MuzzleFlash" : "Brass");
        }
        return;
    }
    if (move && g_checked < 4) {
        // [S] check: the transform ActivateSystem applied is the moved one.
        const int lo = names::PropertyOffset(psc, "LocalToWorld");
        if (lo >= 0) {
            ++g_checked;
            const float* l2w = reinterpret_cast<const float*>(psc + lo);
            MLOG("muzzle: the %s's LocalToWorld origin after ActivateSystem %.1f %.1f %.1f (wanted %.1f %.1f %.1f), its Y axis "
                 "%.2f %.2f %.2f",
                 kKindName[kind], l2w[12], l2w[13], l2w[14], want[0], want[1], want[2], l2w[4], l2w[5], l2w[6]);
        }
    }
}

// Debug.MuzzleFreeze: the flash each Draw until the freeze -- its transform, its particles' bounds (zero once they are
// all dead) and when it was last rendered (the renderer sets it: advancing = drawn).
void TraceFlash(int draw) {
    const std::uintptr_t psc = g_frozenFlash;
    if (!psc) return;
    auto off = [](std::uintptr_t obj, const char* n) { return obj ? names::PropertyOffset(obj, n) : -1; };
    const int lo = off(psc, "LocalToWorld"), bo = off(psc, "Bounds"), ro = off(psc, "LastRenderTime"), oo = off(psc, "Owner");
    const std::uintptr_t owner = oo >= 0 ? names::ReadPointer(psc + oo) : 0;
    const int wo = off(owner, "WorldInfo"), aro = off(owner, "LastRenderTime");
    const std::uintptr_t wi = wo >= 0 ? names::ReadPointer(owner + wo) : 0;
    const int tso = off(wi, "TimeSeconds");
    int ao = -1, so = -1;
    std::uint32_t am = 0, sm = 0;
    const int active = names::BoolProperty(psc, "bIsActive", ao, am) ? ((*reinterpret_cast<const std::uint32_t*>(psc + ao) & am) ? 1 : 0) : -1;
    const int suppress =
        names::BoolProperty(psc, "bSuppressSpawning", so, sm) ? ((*reinterpret_cast<const std::uint32_t*>(psc + so) & sm) ? 1 : 0) : -1;
    const float* l2w = lo >= 0 ? reinterpret_cast<const float*>(psc + lo) : nullptr;
    const float* b = bo >= 0 ? reinterpret_cast<const float*>(psc + bo) : nullptr;  // Origin, BoxExtent, SphereRadius
    auto f = [](std::uintptr_t obj, int o) { return obj && o >= 0 ? *reinterpret_cast<const float*>(obj + o) : -1.0f; };
    MLOG("muzzle: trace Draw %d -- L2W %.1f %.1f %.1f, bounds %.1f %.1f %.1f extent %.1f %.1f %.1f r %.1f, rendered %.4f (owner %.4f, "
         "world %.4f), active %d suppress %d",
         draw, l2w ? l2w[12] : 0.0f, l2w ? l2w[13] : 0.0f, l2w ? l2w[14] : 0.0f, b ? b[0] : 0.0f, b ? b[1] : 0.0f, b ? b[2] : 0.0f,
         b ? b[3] : 0.0f, b ? b[4] : 0.0f, b ? b[5] : 0.0f, b ? b[6] : 0.0f, f(psc, ro), f(owner, aro), f(wi, tso), active, suppress);
}

}  // namespace

// Debug.MuzzleFreeze: the flash's flame shows for about one frame (the game's own too; in slow motion as well, where its
// smoke lives ~15 Draws), too short for a capture to catch; paused, it stays where it was drawn, and the log gives its
// transform and the left eye's view to project it.
void OnDraw(std::uintptr_t localPlayer) {
    if (g_freezeIn < 0 || !localPlayer) return;
    TraceFlash(g_cfg.debugMuzzleFreeze - g_freezeIn);
    if (g_freezeIn-- > 0) return;
    // Needs the cheat manager (EnableCheats); the world stops ticking, the flash's particles stay as they are.
    const bool ok = gexec::Run(localPlayer, L"FreezeFrame 0");
    MLOG("muzzle: Debug.MuzzleFreeze -- the world paused %d Draws after the flash (%s)", g_cfg.debugMuzzleFreeze,
         ok ? "FreezeFrame handled" : "FreezeFrame not handled: EnableCheats first");
    float eye[3], fov[4];
    int rot[3];
    const int lo = g_frozenFlash ? names::PropertyOffset(g_frozenFlash, "LocalToWorld") : -1;
    if (lo >= 0 && view::LastEye0(eye, rot, fov)) {
        const float* l2w = reinterpret_cast<const float*>(g_frozenFlash + lo);
        MLOG("muzzle: freeze -- eye0 at %.2f %.2f %.2f rot %d %d %d fov L%.4f R%.4f U%.4f D%.4f; flash origin %.2f %.2f %.2f "
             "x-axis %.3f %.3f %.3f y-axis %.3f %.3f %.3f z-axis %.3f %.3f %.3f", eye[0], eye[1], eye[2], rot[0], rot[1], rot[2],
             fov[0], fov[1], fov[2], fov[3], l2w[12], l2w[13], l2w[14], l2w[0], l2w[1], l2w[2], l2w[4], l2w[5], l2w[6], l2w[8],
             l2w[9], l2w[10]);
    }
}

bool Install(const Config& cfg) {
    g_cfg = cfg;
    auto mode = [](int v, const char* moved) { return v == 0 ? "hide" : v == 1 ? "game" : moved; };
    if (cfg.muzzleFlash == 1 && cfg.brass == 1) {
        MLOG("muzzle: Weapon.MuzzleFlash=game Brass=game -- the flash and the brass where the game puts them");
        return false;
    }
    // Standing rule 4: the exec's bytes, through its call to UParticleSystemComponent::ActivateSystem.
    if (!patch::BytesMatch(addr::kExecActivateSystem, addr::kExecActivateSystemBytes, sizeof(addr::kExecActivateSystemBytes))) {
        MLOG("muzzle: execActivateSystem bytes differ -- standing down (the game's flash)");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecActivateSystem),
                                              reinterpret_cast<void*>(&Hook_ExecActivateSystem));
    if (!res) {
        MLOG("muzzle: inline hook failed (error %d) -- the game's flash", static_cast<int>(res.error().type));
        return false;
    }
    g_hook = std::move(*res);
    MLOG("muzzle: Weapon.MuzzleFlash=%s Brass=%s BrassMirror=%d -- execActivateSystem hooked at 0x%08X",
         mode(cfg.muzzleFlash, "barrel"), mode(cfg.brass, "gun"), cfg.brassMirror ? 1 : 0,
         static_cast<unsigned>(addr::kExecActivateSystem));
    return true;
}

}  // namespace mohavr::muzzle
