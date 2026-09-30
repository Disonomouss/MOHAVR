#include "reload.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"

namespace mohavr::reload {
namespace {

Config           g_cfg;
SafetyHookInline g_hook;

// --- M0 probe state (game thread) ---
std::vector<std::uintptr_t> g_meshSeen;        // meshes already logged
std::uintptr_t   g_bakeMesh = 0;               // the mesh of the gun baking now, and how often
int              g_bakeCount = 0;
int              g_bakesThisDraw = 0;
struct BakeStats { long draws = 0, bakes = 0; int max = 0; } g_still, g_moving;
DWORD            g_nextStats = 0, g_nextWeaponLog = 0;
std::uintptr_t   g_lastWeapon = 0;
// Hook counters (game thread: the script VM)
long             g_callsWeapon = 0, g_callsOther = 0, g_resultTrue = 0;
std::uintptr_t   g_otherClasses[8] = {};
long             g_otherCounts[8] = {};

float Det3(const float* m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
float RowNorm(const float* m, int r) { return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]); }

std::uintptr_t PawnWeapon(std::uintptr_t pawn) {
    const int wo = pawn ? names::PropertyOffset(pawn, "Weapon") : -1;
    return wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
}

// UEALAWeapon::execHasReserveAmmo -- folded with AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent (the same
// code; RELOAD-DESIGN 2.5): `self` may be the GameInfo. M0 counts only; nothing is read through reflection on `self`.
void __fastcall Hook_ExecHasReserveAmmo(std::uintptr_t self, void* /*edx*/, void* stack, void* result) {
    g_hook.thiscall<void>(self, stack, result);
    const std::uintptr_t w = PawnWeapon(aim::LocalPlayerPawn());
    if (self && self == w) {
        ++g_callsWeapon;
        if (result && *static_cast<const std::uint32_t*>(result)) ++g_resultTrue;
        return;
    }
    ++g_callsOther;
    const std::uintptr_t cls = self ? names::ReadPointer(self + addr::kObjectClass) : 0;
    for (int i = 0; i < 8; ++i) {
        if (g_otherClasses[i] == cls || !g_otherClasses[i]) {
            if (!g_otherClasses[i]) {
                g_otherClasses[i] = cls;
                MLOG("reload: probe -- execHasReserveAmmo called for a %s (not the pawn's weapon; result %u, left as is)",
                     cls ? names::Name(cls).c_str() : "null", result ? *static_cast<const std::uint32_t*>(result) : 0u);
            }
            ++g_otherCounts[i];
            break;
        }
    }
}

std::string StateName(std::uintptr_t obj) {
    const std::uintptr_t frame = names::ReadPointer(obj + 0x18);
    const std::uintptr_t node = frame ? names::ReadPointer(frame + 0x2C) : 0;
    if (!node) return "?";
    if (node == names::ReadPointer(obj + addr::kObjectClass)) return "(none)";
    return names::NameAt(node + 0x2C);
}

// Whether class `c` is `want` or derives from it (the native reserve read's subclass match).
bool Derives(std::uintptr_t c, std::uintptr_t want) {
    for (int depth = 0; c && depth < 64; c = names::ReadPointer(c + addr::kFieldSuper), ++depth)
        if (c == want) return true;
    return false;
}

void LogWeapon(std::uintptr_t pawn, std::uintptr_t w) {
    auto off = [&](const char* n) { return names::PropertyOffset(w, n); };
    const int ac = off("AmmoCount"), mc = off("MaxAmmoCount"), cl = off("AmmoClass"), at = off("AttachmentClass"),
              up = off("CurrentUpgradeLevel"), tm = off("TapedMagMode");
    int ao = -1, io = -1;
    std::uint32_t am = 0, im = 0;
    const bool haveAlt = names::BoolProperty(w, "bAlternateFireMode", ao, am), haveInf = names::BoolProperty(w, "bInfiniteAmmo", io, im);
    const int* ammo = ac >= 0 ? reinterpret_cast<const int*>(w + ac) : nullptr;
    const int* maxa = mc >= 0 ? reinterpret_cast<const int*>(w + mc) : nullptr;
    const std::uintptr_t ammoClass = cl >= 0 ? names::ReadPointer(w + cl) : 0;
    MLOG("reload: probe -- weapon %s (%s), attachment %s, state %s; AmmoCount %d %d %d (at 0x%X, static 0x2D4), MaxAmmoCount %d %d %d "
         "(0x%X, 0x2E0), alt %d / infinite %d (bits at 0x%X mask 0x%X / 0x%X mask 0x%X, static 0x2EC 0x400 / 0x1), AmmoClass %s "
         "(0x%X, 0x2FC), CurrentUpgradeLevel %d, TapedMagMode %d",
         names::Name(w).c_str(), names::ClassName(w).c_str(), at >= 0 ? names::Name(names::ReadPointer(w + at)).c_str() : "?",
         StateName(w).c_str(), ammo ? ammo[0] : -1, ammo ? ammo[1] : -1, ammo ? ammo[2] : -1, ac, maxa ? maxa[0] : -1,
         maxa ? maxa[1] : -1, maxa ? maxa[2] : -1, mc,
         haveAlt ? ((*reinterpret_cast<const std::uint32_t*>(w + ao) & am) ? 1 : 0) : -1,
         haveInf ? ((*reinterpret_cast<const std::uint32_t*>(w + io) & im) ? 1 : 0) : -1, ao, am, io, im,
         ammoClass ? names::Name(ammoClass).c_str() : "none", cl, up >= 0 ? *reinterpret_cast<const int*>(w + up) : -99,
         tm >= 0 ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(w + tm)) : -1);
    const int io2 = names::PropertyOffset(pawn, "InvManager");
    const std::uintptr_t inv = io2 >= 0 ? names::ReadPointer(pawn + io2) : 0;
    if (!inv) return;
    const int so = names::PropertyOffset(inv, "AmmoStorage"), no = names::PropertyOffset(inv, "NumAmmoClasses");
    const int n = no >= 0 ? *reinterpret_cast<const int*>(inv + no) : -1;
    MLOG("reload: probe -- inventory %s: AmmoStorage at 0x%X (static 0x228), NumAmmoClasses %d at 0x%X (static 0x2A0)",
         names::Name(inv).c_str(), so, n, no);
    if (so < 0 || n < 0 || n > 10) return;
    int exact = -1, sub = -1;
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t e = inv + so + 12 * i;
        const std::uintptr_t c = names::ReadPointer(e);
        const int amount = *reinterpret_cast<const int*>(e + 4), cap = *reinterpret_cast<const int*>(e + 8);
        if (c == ammoClass && exact < 0) exact = i;
        if (sub < 0 && ammoClass && Derives(c, ammoClass)) sub = i;
        MLOG("reload: probe --   reserve[%d] %s amount %d cap %d", i, c ? names::Name(c).c_str() : "none", amount, cap);
    }
    MLOG("reload: probe --   the weapon's AmmoClass: exact match [%d], subclass match (the native read) [%d]%s", exact, sub,
         exact == sub ? "" : " -- DIFFERENT");
}

void LogBones(std::uintptr_t comp, std::uintptr_t mesh, const float* saved, int num, const float* l2w, const float* a) {
    const std::uintptr_t data = names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton);
    const int refNum = static_cast<int>(names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton + 4));
    MLOG("reload: probe -- gun %s (%s of %s): %d bones (RefSkeleton %d); L2W rows %.4f %.4f %.4f, A rows %.4f %.4f %.4f",
         names::Name(mesh).c_str(), names::Name(comp).c_str(), names::Name(names::Outer(comp)).c_str(), num, refNum,
         RowNorm(l2w, 0), RowNorm(l2w, 1), RowNorm(l2w, 2), RowNorm(a, 0), RowNorm(a, 1), RowNorm(a, 2));
    for (int i = 0; i < num && i < refNum && data; ++i) {
        const std::uintptr_t b = data + i * addr::kMeshBoneStride;
        const int parent = *reinterpret_cast<const int*>(b + 56);
        const float* ref = reinterpret_cast<const float*>(b + 28);
        const float* m = saved + 16 * i;
        MLOG("reload: probe --   [%2d] %-28s parent %2d  ref %8.3f %8.3f %8.3f  pose %8.3f %8.3f %8.3f  |det| %.3f", i,
             names::NameAt(b).c_str(), parent, ref[0], ref[1], ref[2], m[12], m[13], m[14], std::fabs(Det3(m)));
    }
}

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
    if (!cfg.debugReloadProbe) return false;
    // Standing rule 4: the 59 bytes through the result's store (RELOAD-DESIGN 2.5).
    if (!patch::BytesMatch(addr::kExecHasReserveAmmo, addr::kExecHasReserveAmmoBytes, sizeof(addr::kExecHasReserveAmmoBytes))) {
        MLOG("reload: execHasReserveAmmo bytes differ -- the probe runs without its hook");
        return true;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecHasReserveAmmo),
                                              reinterpret_cast<void*>(&Hook_ExecHasReserveAmmo));
    if (!res) {
        MLOG("reload: inline hook failed (error %d) -- the probe runs without its hook", static_cast<int>(res.error().type));
        return true;
    }
    g_hook = std::move(*res);
    MLOG("reload: Debug.ReloadProbe=1 -- logging only; execHasReserveAmmo hooked count-only at 0x%08X",
         static_cast<unsigned>(addr::kExecHasReserveAmmo));
    return true;
}

void OnGunBake(std::uintptr_t comp, const float* saved, int num, const float* l2w, const float* a) {
    if (!g_cfg.debugReloadProbe) return;
    ++g_bakesThisDraw;
    const int smo = names::PropertyOffset(comp, "SkeletalMesh");
    const std::uintptr_t mesh = smo >= 0 ? names::ReadPointer(comp + smo) : 0;
    if (!mesh) return;
    if (mesh != g_bakeMesh) {
        g_bakeMesh = mesh;
        g_bakeCount = 0;
    }
    if (++g_bakeCount != 10) return;
    for (std::uintptr_t m : g_meshSeen)
        if (m == mesh) return;
    g_meshSeen.push_back(mesh);
    LogBones(comp, mesh, saved, num, l2w, a);
}

void OnDraw() {
    if (!g_cfg.debugReloadProbe) return;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    float speed = 0.0f;
    if (pawn) {
        float v[3];
        const int vo = names::PropertyOffset(pawn, "Velocity");
        if (vo >= 0 && names::ReadVector(pawn + vo, v)) speed = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    }
    BakeStats& s = speed > 50.0f ? g_moving : g_still;
    ++s.draws;
    s.bakes += g_bakesThisDraw;
    if (g_bakesThisDraw > s.max) s.max = g_bakesThisDraw;
    g_bakesThisDraw = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - g_nextStats) >= 0) {
        g_nextStats = now + 10000;
        if (g_still.draws || g_moving.draws)
            MLOG("reload: probe -- gun bakes per Draw: standing %.2f (max %d, %ld Draws), moving %.2f (max %d, %ld Draws); "
                 "execHasReserveAmmo: %ld for the pawn's weapon (%ld true), %ld for other objects",
                 g_still.draws ? static_cast<double>(g_still.bakes) / g_still.draws : 0.0, g_still.max, g_still.draws,
                 g_moving.draws ? static_cast<double>(g_moving.bakes) / g_moving.draws : 0.0, g_moving.max, g_moving.draws,
                 g_callsWeapon, g_resultTrue, g_callsOther);
        g_still = g_moving = BakeStats{};
    }
    const std::uintptr_t w = PawnWeapon(pawn);
    if (w && (w != g_lastWeapon || static_cast<LONG>(now - g_nextWeaponLog) >= 0)) {
        g_lastWeapon = w;
        g_nextWeaponLog = now + 15000;
        LogWeapon(pawn, w);
    }
}

}  // namespace mohavr::reload
