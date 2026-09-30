#include "reload.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "aim.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"

namespace mohavr::reload {
namespace {

Config           g_cfg;
SafetyHookInline g_hook;
bool             g_hooked = false;        // execHasReserveAmmo hooked (count-only for the probe, or blocking)
bool             g_blockAllowed = false;  // Weapon.ManualReload + [ManualReload] Hook + the pipeline's hooks installed

// --- the per-gun lines ([ManualReload] Attachment_<Gun>=Action=open|closed MinUpgrade=N Mag=... ; RELOAD-DESIGN 1.3) ---
struct GunLine {
    std::string key;
    bool        open = false;
    int         minUpgrade = 0;
};
std::vector<GunLine> g_lines;

const GunLine* LineFor(const std::string& key) {
    for (const GunLine& l : g_lines)
        if (l.key == key) return &l;
    return nullptr;
}

std::string Token(const std::string& line, const char* name) {
    const std::string k = std::string(name) + "=";
    size_t p = 0;
    while ((p = line.find(k, p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ' ') {
            const size_t e = line.find(' ', p);
            return line.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
        }
        p += k.size();
    }
    return {};
}

void ParseLines(const std::wstring& ini) {
    std::vector<wchar_t> keys(8192);
    const DWORD n = GetPrivateProfileStringW(L"ManualReload", nullptr, L"", keys.data(), static_cast<DWORD>(keys.size()), ini.c_str());
    for (const wchar_t* k = keys.data(); k < keys.data() + n && *k; k += wcslen(k) + 1) {
        const std::wstring wk(k);
        if (wk.rfind(L"Attachment_", 0) != 0 || wk.find(L'.') != std::wstring::npos) continue;
        wchar_t v[1024] = L"";
        GetPrivateProfileStringW(L"ManualReload", k, L"", v, 1024, ini.c_str());
        std::string line;
        for (const wchar_t* c = v; *c; ++c) line += static_cast<char>(*c);
        GunLine g;
        for (wchar_t ch : wk) g.key += static_cast<char>(ch);  // the class names are ASCII
        g.open = Token(line, "Action") == "open";
        const std::string mu = Token(line, "MinUpgrade");
        g.minUpgrade = mu.empty() ? 0 : atoi(mu.c_str());
        g_lines.push_back(g);
    }
}

// --- the game-side model (RELOAD-DESIGN 2.1) ---
struct WState {
    std::uintptr_t w = 0;
    int            index = -1;       // UObject Index: a new weapon at a freed one's address is a new state
    bool           magIn = true, pending = false, cocked = true;
    int            lastClip = -1;    // the clip at the end of the last Draw
    int            heldRounds = 0;   // the magazine in the hand (visual only)
};
std::vector<WState> g_ws;
struct Owed {
    std::uintptr_t cls;
    int            n;
};
std::vector<Owed> g_owed;           // per pawn: rounds an EJECT could not put back (the reserve at its cap)
std::uintptr_t   g_pawn = 0;
DWORD            g_lastDraw = 0, g_lastGunBake = 0;
std::uint32_t    g_flags = 0;       // hdr->reloadFlags as read this Draw
std::uint32_t    g_seen = 0;
bool             g_seenInit = false;
std::uint32_t    g_pawnSeq = 0;
std::string      g_lastState;       // Debug.ReloadTrace: the weapon's state, logged on change

int ObjIndex(std::uintptr_t o) { return o ? *reinterpret_cast<const int*>(o + 4) : -1; }

std::uintptr_t PawnWeapon(std::uintptr_t pawn) {
    const int wo = pawn ? names::PropertyOffset(pawn, "Weapon") : -1;
    return wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
}

// The weapon's fields (reflection; the static offsets are in ENGINE-NOTES 5am).
int* Field(std::uintptr_t w, const char* n) {
    const int o = names::PropertyOffset(w, n);
    return o >= 0 ? reinterpret_cast<int*>(w + o) : nullptr;
}
bool Bit(std::uintptr_t w, const char* n) {
    int o = -1;
    std::uint32_t m = 0;
    return names::BoolProperty(w, n, o, m) && (*reinterpret_cast<const std::uint32_t*>(w + o) & m) != 0;
}
std::string AttachKey(std::uintptr_t w) {  // the class object's own name (e.g. Attachment_Stg44), RELOAD-DESIGN X4
    const int o = names::PropertyOffset(w, "AttachmentClass");
    const std::uintptr_t cls = o >= 0 ? names::ReadPointer(w + o) : 0;
    return cls ? names::Name(cls) : std::string();
}
std::uintptr_t AmmoClassOf(std::uintptr_t w) {
    const int o = names::PropertyOffset(w, "AmmoClass");
    return o >= 0 ? names::ReadPointer(w + o) : 0;
}

// The converted line for this weapon now, or null (no line, below MinUpgrade, alt mode).
const GunLine* Converted(std::uintptr_t w) {
    const GunLine* l = LineFor(AttachKey(w));
    if (!l) return nullptr;
    const int* up = Field(w, "CurrentUpgradeLevel");
    if (up && *up < l->minUpgrade) return nullptr;
    if (Bit(w, "bAlternateFireMode")) return nullptr;
    return l;
}

WState& StateFor(std::uintptr_t w) {
    const int idx = ObjIndex(w);
    for (WState& s : g_ws)
        if (s.w == w && s.index == idx) return s;
    if (g_ws.size() >= 16) g_ws.erase(g_ws.begin());
    WState s;
    s.w = w;
    s.index = idx;
    g_ws.push_back(s);
    return g_ws.back();
}

int& OwedFor(std::uintptr_t cls) {
    for (Owed& o : g_owed)
        if (o.cls == cls) return o.n;
    g_owed.push_back({cls, 0});
    return g_owed.back().n;
}

// The reserve entry for the weapon's ammo class (the exact match, as AddReserveAmmo writes; M0: the native's subclass
// match picks the same entry); null if none.
int* ReserveOf(std::uintptr_t pawn, std::uintptr_t cls, int& cap) {
    const int io = names::PropertyOffset(pawn, "InvManager");
    const std::uintptr_t inv = io >= 0 ? names::ReadPointer(pawn + io) : 0;
    if (!inv || !cls) return nullptr;
    const int so = names::PropertyOffset(inv, "AmmoStorage"), no = names::PropertyOffset(inv, "NumAmmoClasses");
    if (so < 0 || no < 0) return nullptr;
    const int n = *reinterpret_cast<const int*>(inv + no);
    for (int i = 0; i < n && i < 10; ++i) {
        const std::uintptr_t e = inv + so + 12 * i;
        if (names::ReadPointer(e) != cls) continue;
        cap = *reinterpret_cast<const int*>(e + 8);
        return reinterpret_cast<int*>(e + 4);
    }
    return nullptr;
}

// RELOAD-DESIGN 2.2: no round is ever lost (the overflow at the cap is carried as `owed`).
void ToReserve(std::uintptr_t pawn, std::uintptr_t w, int k) {
    if (k <= 0 || Bit(w, "bInfiniteAmmo")) return;
    const std::uintptr_t cls = AmmoClassOf(w);
    int cap = 0;
    int* r = ReserveOf(pawn, cls, cap);
    int put = 0;
    if (r) {
        put = std::min(k, std::max(cap - *r, 0));
        *r += put;
    }
    OwedFor(cls) += k - put;
}
int FromReserve(std::uintptr_t pawn, std::uintptr_t w, int n) {
    if (n <= 0) return 0;
    if (Bit(w, "bInfiniteAmmo")) return n;
    const std::uintptr_t cls = AmmoClassOf(w);
    int& o = OwedFor(cls);
    const int a = std::min(n, o);
    o -= a;
    int cap = 0;
    int* r = ReserveOf(pawn, cls, cap);
    const int b = r ? std::min(n - a, *r) : 0;
    if (r) *r -= b;
    return a + b;
}
int ReserveAvailable(std::uintptr_t pawn, std::uintptr_t w) {
    if (Bit(w, "bInfiniteAmmo")) return 9999;
    const std::uintptr_t cls = AmmoClassOf(w);
    int cap = 0;
    const int* r = ReserveOf(pawn, cls, cap);
    return (r ? *r : 0) + OwedFor(cls);
}

bool Ready(const WState& s, const GunLine& l, int clip) { return l.open ? s.cocked : clip >= 1; }

const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP"};

void Apply(std::uintptr_t pawn, std::uintptr_t w, WState& s, const GunLine& l, std::uint32_t type) {
    int* clipP = Field(w, "AmmoCount");
    const int* maxP = Field(w, "MaxAmmoCount");
    if (!clipP || !maxP) return;
    const int c0 = *clipP, m = maxP[0];
    const int r0 = ReserveAvailable(pawn, w);
    int& c = *clipP;
    switch (type) {
    case shared::kReloadEject: {
        if (!s.magIn) break;
        const int keep = (!l.open && g_cfg.keepChambered) ? std::min(c, 1) : 0;
        const int k = c;
        c = keep;
        ToReserve(pawn, w, k - keep);
        s.magIn = false;
        s.pending = false;
        s.heldRounds = k - keep;
        break;
    }
    case shared::kReloadTake:
        if (s.magIn) break;
        s.heldRounds = Bit(w, "bInfiniteAmmo") ? m : std::min(m, ReserveAvailable(pawn, w));
        break;
    case shared::kReloadInsert:
        if (s.magIn) break;
        s.magIn = true;
        if (Ready(s, l, c)) {
            c += FromReserve(pawn, w, std::max(m - c, 0));
            s.pending = false;
        } else {
            s.pending = Bit(w, "bInfiniteAmmo") || ReserveAvailable(pawn, w) > 0;
        }
        s.heldRounds = 0;
        break;
    case shared::kReloadRack:
        if (!l.open) {
            if (c == 0 && s.magIn && s.pending) {
                c = FromReserve(pawn, w, m);
                s.pending = false;
            }
        } else if (!s.cocked) {
            s.cocked = true;
            if (s.magIn && s.pending) {
                c = FromReserve(pawn, w, m);
                s.pending = false;
            }
        }
        break;
    case shared::kReloadDrop:
        s.heldRounds = 0;
        break;
    default:
        break;
    }
    MLOG("reload: %s %s: clip %d -> %d, reserve %d -> %d (owed %d); magazine %s%s%s", type < 6 ? kEventName[type] : "?",
         l.key.c_str(), c0, c, r0, ReserveAvailable(pawn, w), OwedFor(AmmoClassOf(w)), s.magIn ? "in" : "out",
         s.pending ? ", pending (rack to feed)" : "", l.open ? (s.cocked ? ", cocked" : ", bolt forward") : "");
}

// RELOAD-DESIGN 2.5: whether the game's own reload is blocked for `self` right now.
bool Blocking(std::uintptr_t self) {
    if (!g_blockAllowed || !self) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn || self != PawnWeapon(pawn)) return false;  // pointer first: the folded GameInfo call never gets further
    if (!(g_flags & 1u) || !(g_flags & 16u)) return false;  // the player's toggle, and the host engaged
    const DWORD now = GetTickCount();
    if (now - g_lastDraw > 250 || now - g_lastGunBake > 250) return false;  // the pipeline alive
    if (!Converted(self)) return false;
    float gf[16], of[16];
    bool ov = false, th = false;
    return viewmodel::HandFrames(gf, of, ov, th);
}

// --- M0 probe state (game thread) ---
std::vector<std::uintptr_t> g_meshSeen;
std::uintptr_t   g_bakeMesh = 0;
int              g_bakeCount = 0;
int              g_bakesThisDraw = 0;
struct BakeStats { long draws = 0, bakes = 0; int max = 0; } g_still, g_moving;
DWORD            g_nextStats = 0, g_nextWeaponLog = 0;
std::uintptr_t   g_lastWeapon = 0;
long             g_callsWeapon = 0, g_callsOther = 0, g_resultTrue = 0, g_blocks = 0;
std::uintptr_t   g_otherClasses[8] = {};
long             g_otherCounts[8] = {};

// UEALAWeapon::execHasReserveAmmo -- folded with AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent (the same
// code; RELOAD-DESIGN 2.5): `self` may be the GameInfo. Only the pawn's own weapon is ever looked at further.
void __fastcall Hook_ExecHasReserveAmmo(std::uintptr_t self, void* /*edx*/, void* stack, void* result) {
    g_hook.thiscall<void>(self, stack, result);
    if (Blocking(self)) {
        if (result && *static_cast<std::uint32_t*>(result)) {
            *static_cast<std::uint32_t*>(result) = 0;
            ++g_blocks;
        }
        return;
    }
    if (!g_cfg.debugReloadProbe) return;
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

bool Derives(std::uintptr_t c, std::uintptr_t want) {
    for (int depth = 0; c && depth < 64; c = names::ReadPointer(c + addr::kFieldSuper), ++depth)
        if (c == want) return true;
    return false;
}

float Det3(const float* m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
float RowNorm(const float* m, int r) { return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]); }

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

void ProbeDraw(std::uintptr_t pawn) {
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

}  // namespace

bool Install(const Config& cfg, bool pipelineHooked) {
    g_cfg = cfg;
    if (!cfg.manualReload && !cfg.debugReloadProbe) return false;
    ParseLines(cfg.iniPath);
    if (cfg.manualReload)
        MLOG("reload: Weapon.ManualReload=1 -- %zu guns with [ManualReload] lines; KeepChambered=%d, Hook=%d", g_lines.size(),
             cfg.keepChambered ? 1 : 0, cfg.reloadHook ? 1 : 0);
    const bool wantHook = cfg.debugReloadProbe || (cfg.manualReload && cfg.reloadHook && pipelineHooked);
    if (cfg.manualReload && cfg.reloadHook && !pipelineHooked)
        MLOG("reload: the Draw hook or the arm bake is missing -- the game keeps its own reload (no hook)");
    if (!wantHook) return true;
    // Standing rule 4: the 59 bytes through the result's store (RELOAD-DESIGN 2.5).
    if (!patch::BytesMatch(addr::kExecHasReserveAmmo, addr::kExecHasReserveAmmoBytes, sizeof(addr::kExecHasReserveAmmoBytes))) {
        MLOG("reload: execHasReserveAmmo bytes differ -- no hook: the game keeps its own reload");
        return true;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecHasReserveAmmo),
                                              reinterpret_cast<void*>(&Hook_ExecHasReserveAmmo));
    if (!res) {
        MLOG("reload: inline hook failed (error %d) -- the game keeps its own reload", static_cast<int>(res.error().type));
        return true;
    }
    g_hook = std::move(*res);
    g_hooked = true;
    g_blockAllowed = cfg.manualReload && cfg.reloadHook && pipelineHooked;
    MLOG("reload: execHasReserveAmmo hooked at 0x%08X (%s)", static_cast<unsigned>(addr::kExecHasReserveAmmo),
         g_blockAllowed ? "blocks the game's own reload for a converted gun while the manual reload is engaged" : "count-only");
    return true;
}

void OnGunBake(std::uintptr_t comp, const float* saved, int num, const float* l2w, const float* a) {
    g_lastGunBake = GetTickCount();
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

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (g_cfg.debugReloadProbe) ProbeDraw(pawn);
    if (!g_cfg.manualReload) return;
    g_lastDraw = GetTickCount();
    g_flags = hdr ? hdr->reloadFlags : 0;
    if (pawn != g_pawn) {  // a new local pawn (death, a level load): every state starts over
        g_pawn = pawn;
        g_ws.clear();
        g_owed.clear();
        ++g_pawnSeq;
        if (hdr) hdr->reloadPawnSeq = g_pawnSeq;
        MLOG("reload: a new local pawn -- the manual reload's state reset");
    }
    const std::uintptr_t w = PawnWeapon(pawn);
    const GunLine* line = w ? Converted(w) : nullptr;
    WState* s = w ? &StateFor(w) : nullptr;
    int* clipP = w ? Field(w, "AmmoCount") : nullptr;
    if (s && line && clipP) {
        const int c = *clipP;
        if (s->lastClip >= 0 && c != s->lastClip) {
            if (c > s->lastClip) {  // RELOAD-DESIGN 2.3: the game filled it (an upgrade, a cheat, a load, its own reload)
                s->magIn = true;
                s->pending = false;
                s->cocked = true;
                MLOG("reload: %s's clip rose %d -> %d without the mod -- magazine in, ready", line->key.c_str(), s->lastClip, c);
            } else if (c == 0 && line->open && s->cocked) {
                s->cocked = false;  // the last shot: an open bolt closes on the empty chamber
                MLOG("reload: %s fired empty -- the bolt is forward (rack after a new magazine)", line->key.c_str());
            }
        }
    }
    if (g_cfg.debugReloadTrace && w) {
        const std::string st = StateName(w);
        if (st != g_lastState) {
            MLOG("reload: trace -- %s is in state %s, clip %d", AttachKey(w).c_str(), st.c_str(), clipP ? *clipP : -1);
            g_lastState = st;
        }
    }
    // The host's events, in order (RELOAD-DESIGN 4).
    if (hdr) {
        const std::uint32_t seq = hdr->reloadEvtSeq;
        if (!g_seenInit) {
            g_seen = seq;
            g_seenInit = true;
        }
        if (seq - g_seen > 8) {
            MLOG("reload: %u events overran the ring -- skipped", seq - g_seen);
            g_seen = seq;
        }
        const std::uint32_t myHash = w ? (shared::KeyHash(AttachKey(w).c_str()) & 0xFFFFFFu) : 0;
        for (int n = 0; g_seen != seq && n < 8; ++n) {
            const std::uint32_t e = hdr->reloadEvt[g_seen % 8];
            ++g_seen;
            const std::uint32_t type = e & 0xFFu;
            if (!s || !line || (e >> 8) != myHash) {
                MLOG("reload: %s rejected -- %s", type < 6 ? kEventName[type] : "?",
                     !line ? "no converted gun in hand" : "it was meant for another gun");
                continue;
            }
            Apply(pawn, w, *s, *line, type);
        }
        hdr->reloadEvtAck = g_seen;
    }
    if (s && clipP) s->lastClip = *clipP;
    if (!hdr) return;
    // Publish (RELOAD-DESIGN 4; the geometry comes with M2/M3).
    ++hdr->reloadGeoSeq;
    _ReadWriteBarrier();
    const int* maxP = w ? Field(w, "MaxAmmoCount") : nullptr;
    const std::string key = w ? AttachKey(w) : std::string();
    std::memset(hdr->reloadKey, 0, sizeof(hdr->reloadKey));
    std::memcpy(hdr->reloadKey, key.c_str(), std::min(key.size(), sizeof(hdr->reloadKey) - 1));
    hdr->reloadCaps = (line ? 1u : 0u) | (g_blockAllowed ? 4u : 0u) | (w && Bit(w, "bAlternateFireMode") ? 8u : 0u) |
                      (w && Blocking(w) ? 16u : 0u);
    hdr->ammoClip = clipP ? *clipP : 0;
    hdr->ammoMax = maxP ? maxP[0] : 0;
    hdr->ammoReserve = w ? ReserveAvailable(pawn, w) : 0;
    std::uint32_t st = 0;
    if (s && line) {
        const int c = clipP ? *clipP : 0;
        const bool ready = Ready(*s, *line, c);
        const bool rackNeeded = line->open ? !s->cocked : (c == 0 && s->magIn && s->pending);
        st = (s->magIn ? 1u : 0u) | (s->pending ? 2u : 0u) | (ready ? 4u : 0u) | (rackNeeded ? 16u : 0u) |
             (line->open ? 32u : 0u) | (Bit(w, "bInfiniteAmmo") ? 64u : 0u);
    }
    hdr->reloadState = st;
    _ReadWriteBarrier();
    ++hdr->reloadGeoSeq;
    // Blocks, now and then.
    static DWORD nextLog = 0;
    const DWORD now = GetTickCount();
    if (g_blocks && static_cast<LONG>(now - nextLog) >= 0) {
        nextLog = now + 10000;
        MLOG("reload: the game's own reload blocked %ld time(s) so far", g_blocks);
    }
}

}  // namespace mohavr::reload
