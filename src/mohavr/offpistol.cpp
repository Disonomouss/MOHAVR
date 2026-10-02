#include "offpistol.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "aim.hpp"
#include "bridge.hpp"
#include "carrier.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "offhand.hpp"
#include "reload.hpp"
#include "script_call.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::offpistol {
using namespace script;  // Call, Obj, Bit, ... (script_call.hpp)
namespace {

Config g_cfg;
bool   g_bake = false;  // the arm bake is installed (it draws the pistol in the off hand)

// The engine's ImpactList buffer, reused across shots: each trace is given {Data, Num = 0, Max} of the last one and hands
// back the new header. A mod-made pointer is never passed in (the engine may grow or free it).
struct List {
    std::uintptr_t data = 0;
    int            num = 0, max = 0;
} g_list;
std::uintptr_t g_cueFor = 0, g_cue = 0;  // the report cue found for this pistol (and its magnum state)
bool           g_cueMagnum = false;
std::uintptr_t g_dryFor = 0, g_dry = 0;  // likewise its dry click
std::uintptr_t g_cueClass = 0;           // the SoundCue class (from any weapon attachment's dry-fire cue)
unsigned       g_shots = 0;

// ImpactInfo (0x40): HitActor, HitLocation +4, HitNormal +0x10, RayDir +0x1C, HitInfo +0x28 (Material, PhysMaterial, Item,
// BoneName +0x34, HitComponent +0x3C).
constexpr std::size_t kImpact = 0x40, kImpactLocation = 4, kImpactBone = 0x34;

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// No C++ objects here: SEH only.
bool SafeCopy(void* dst, std::uintptr_t src, std::size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int Byte(std::uintptr_t obj, const char* name) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    return o >= 0 ? static_cast<int>(ReadU32(obj + o) & 0xFF) : -1;
}

// A dynamic array's element 0 (Weapon's InstantHitDamage, FireInterval, FiringStatesArray ...).
bool Array0(std::uintptr_t obj, const char* name, void* out, std::size_t n) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o < 0 || static_cast<int>(ReadU32(obj + o + 4)) <= 0) return false;
    const std::uintptr_t data = names::ReadPointer(obj + o);
    return data && SafeCopy(out, data, n);
}
float ArrayFloat0(std::uintptr_t obj, const char* name) {
    float v = -1.0f;
    Array0(obj, name, &v, sizeof(v));
    return v;
}
std::string ArrayName0(std::uintptr_t obj, const char* name) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o < 0 || static_cast<int>(ReadU32(obj + o + 4)) <= 0) return "-";
    const std::uintptr_t data = names::ReadPointer(obj + o);
    return data ? names::NameAt(data) : "-";
}

float GameTime(std::uintptr_t pawn) { return Float(Obj(pawn, "WorldInfo"), "TimeSeconds", 0.0f); }

// The pawn's pistol that isn't the weapon in hand: the inventory manager's PistolWeapon, else the inventory chain (the
// other pistol of a Colt + C96 pair, with one of them in the gun hand).
std::uintptr_t Pistol(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun) {
    const std::uintptr_t p = Obj(inv, "PistolWeapon");
    if (p && p != gun && names::IsA(p, "MOHAPistol") && Obj(p, "Instigator") == pawn && !Bit(p, "bDeleteMe")) return p;
    std::uintptr_t item = Obj(inv, "InventoryChain");
    for (int n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (item != gun && names::IsA(item, "MOHAPistol") && Obj(item, "Instigator") == pawn && !Bit(item, "bDeleteMe")) return item;
    return 0;
}

// The held pistol is still the pawn's: alive, its instigator the pawn (ApplyDamage reads it unchecked), in the inventory.
bool Owned(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t p) {
    if (!p || !pawn || Bit(p, "bDeleteMe") || Obj(p, "Instigator") != pawn) return false;
    if (Obj(inv, "PistolWeapon") == p) return true;
    std::uintptr_t item = Obj(inv, "InventoryChain");
    for (int n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (item == p) return true;
    return false;
}

bool Head(shared::Header* hdr, float (&pos)[3], float (&fwd)[3], float& upm) {
    shared::Pose head{}, eye[2]{};
    shared::Fov fov[2]{};
    return hdr && shared::ReadViews(hdr, head, eye, fov) && view::PoseToWorld(head, pos, fwd, upm);
}

// The off controller's aim pose in the world (un-mirrored: the raw controller): the hand that isn't the gun hand.
bool OffHand(shared::Header* hdr, float (&pos)[3], float (&fwd)[3], float& upm) {
    shared::Pose hand[2];
    std::uint32_t valid = 0;
    if (!hdr || !shared::ReadHands(hdr, hand, valid)) return false;
    const int o = (hdr->gunFlags & 4u) ? 1 : 0;  // the gun in the left hand -> the off hand is the right
    return (valid & (1u << o)) && view::PoseToWorld(hand[o], pos, fwd, upm);
}

// What the shot must not touch: the gun in hand's state and counters (DESIGN 7.1 pass 2).
struct Snap {
    std::uintptr_t weapon = 0;
    std::string    state;
    int            ammo = -1, flash = -1, pf0 = -1, pf1 = -1, impactSnd = -1;
};
Snap Take(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun) {
    Snap s;
    s.weapon = Obj(pawn, "Weapon");
    s.state = names::StateName(gun);
    s.ammo = Int(gun, "AmmoCount", -1);
    s.flash = Byte(pawn, "FlashCount");
    const int po = inv ? names::PropertyOffset(inv, "PendingFire") : -1;
    if (po >= 0) {
        const std::uintptr_t d = names::ReadPointer(inv + po);
        const int n = static_cast<int>(ReadU32(inv + po + 4));
        s.pf0 = n > 0 && d ? static_cast<int>(ReadU32(d)) : -1;
        s.pf1 = n > 1 && d ? static_cast<int>(ReadU32(d + 4)) : -1;
    }
    s.impactSnd = Int(Obj(pawn, "CurrentWeaponAttachment"), "NextImpactSoundIndex", -1);
    return s;
}

// The player's per-weapon experience (MOHAPlayerStatsComponent.ExperienceComponents): which weapon a hit or kill counts for.
struct Exp {
    std::string cls;
    float       points;
    int         level;
};
std::vector<Exp> Experience(std::uintptr_t pawn) {
    std::vector<Exp> out;
    const std::uintptr_t stats = Obj(Obj(pawn, "Controller"), "playerStats");
    const int o = stats ? names::PropertyOffset(stats, "ExperienceComponents") : -1;
    if (o < 0) return out;
    const std::uintptr_t data = names::ReadPointer(stats + o);
    const int n = static_cast<int>(ReadU32(stats + o + 4));
    for (int i = 0; i < n && i < 32 && data; ++i) {
        const std::uintptr_t c = names::ReadPointer(data + 4 * i);
        if (c) out.push_back({names::Name(Obj(c, "WeaponClass")), Float(c, "ExperiencePoints", -1.0f), Int(c, "ExperienceLevel", -1)});
    }
    return out;
}

// The stats' kill record (MOHAPlayerStatsComponent.KillInfo: {TimeSeconds, WeaponType, DamageType}, 12 bytes each): how
// many, and the newest one's weapon type -- which weapon a kill was credited to.
int Kills(std::uintptr_t pawn, int& lastType, std::string& lastDamage) {
    lastType = -1;
    lastDamage = "-";
    const std::uintptr_t stats = Obj(Obj(pawn, "Controller"), "playerStats");
    const int o = stats ? names::PropertyOffset(stats, "KillInfo") : -1;
    if (o < 0) return -1;
    const std::uintptr_t data = names::ReadPointer(stats + o);
    const int n = static_cast<int>(ReadU32(stats + o + 4));
    if (n > 0 && data) {
        lastType = static_cast<int>(ReadU32(data + 12 * (n - 1) + 4) & 0xFF);
        lastDamage = names::Name(names::ReadPointer(data + 12 * (n - 1) + 8));
    }
    return n;
}

// Object.FindObject(path, the SoundCue class) -- the class taken once from a SoundCue the pawn or the gun's attachment
// refers to (a grenade's attachment has no dry-fire cue: the first off shot with a grenade in hand was silent).
std::uintptr_t FindCue(std::uintptr_t pawn, std::uintptr_t p, const wchar_t* path) {
    for (const char* prop : {"GotHitSnd", "DeathSnd", "MeleeImpactPC", "LeanInSnd"}) {
        if (g_cueClass) break;
        const std::uintptr_t any = Obj(pawn, prop);
        const std::uintptr_t cls = any ? names::ReadPointer(any + addr::kObjectClass) : 0;
        if (cls && names::Name(cls) == "SoundCue") g_cueClass = cls;
    }
    if (!g_cueClass) {
        const std::uintptr_t any = Obj(Obj(pawn, "CurrentWeaponAttachment"), "WeaponDryFireSnd");
        const std::uintptr_t cls = any ? names::ReadPointer(any + addr::kObjectClass) : 0;
        if (cls && names::Name(cls) == "SoundCue") g_cueClass = cls;
    }
    if (!g_cueClass) return 0;
    struct FString {
        const wchar_t* data;
        int            num, max;
    } s{path, static_cast<int>(std::wcslen(path)) + 1, static_cast<int>(std::wcslen(path)) + 1};
    Call find(p, "FindObject");
    if (!find.Set("ObjectName", &s, sizeof(s)) || !find.Set("ObjectClass", &g_cueClass, sizeof(g_cueClass)) || !find.Run()) return 0;
    return find.ReturnObject();
}

std::uintptr_t ReportCue(std::uintptr_t pawn, std::uintptr_t p) {
    const bool magnum = Bit(p, "bHasMagnumRounds");
    if (g_cueFor == p && g_cueMagnum == magnum && g_cue) return g_cue;
    const bool colt = names::IsA(p, "MOHAColt45");
    const wchar_t* path = colt ? (magnum ? L"Aud_GCm_FakeWAV.COLT.WpnFire_PC_Colt_Magnum" : L"Aud_GCm_FakeWAV.COLT.WpnFire_PC_Colt")
                               : L"Aud_GCm_FakeWAV.Mauser.WpnFire_PC_Mauser";
    g_cue = FindCue(pawn, p, path);
    g_cueFor = p;
    g_cueMagnum = magnum;
    MLOG("offpistol: the report %ls -> %s", path, g_cue ? names::Name(g_cue).c_str() : "not found (silent)");
    return g_cue;
}

// The attachments' WeaponDryFireSnd (Attachment_Colt45 / Attachment_Mauser defaults).
std::uintptr_t DryCue(std::uintptr_t pawn, std::uintptr_t p) {
    if (g_dryFor == p && g_dry) return g_dry;
    const wchar_t* path = names::IsA(p, "MOHAColt45") ? L"Aud_GCm_FakeWAV.COLT.WpnDryFire_PC_Colt" : L"Aud_GCm_FakeWAV.Mauser.WpnDryFire_PC_Mauser";
    g_dry = FindCue(pawn, p, path);
    g_dryFor = p;
    MLOG("offpistol: the dry click %ls -> %s", path, g_dry ? names::Name(g_dry).c_str() : "not found (silent)");
    return g_dry;
}

bool PlayAt(std::uintptr_t p, std::uintptr_t cue, const float (&at)[3]) {
    if (!cue) return false;
    Call snd(p, "PlaySoundAt");
    return snd.Set("ASound", &cue, sizeof(cue)) && snd.Set("SourceLocation", at, sizeof(at)) && snd.Run();
}

// The game's invariant for the shared magnum mix (DESIGN finding 1): on only while the weapon in hand has magnum rounds.
void MagnumRule(std::uintptr_t pawn, std::uintptr_t p, bool log) {
    const std::uintptr_t mix = Obj(p, "MagnumRoundsMix");
    if (!mix) return;
    Call active(mix, "IsActive");
    const bool on = active.Run() && active.ReturnBool();
    const bool want = Bit(Obj(pawn, "Weapon"), "bHasMagnumRounds");
    if (on && !want) {
        Call off(mix, "DeActivate");
        const bool done = off.Run();
        Call again(mix, "IsActive");
        MLOG("offpistol: the magnum mix was on with %s in hand -- DeActivate %s, IsActive now %d", names::Name(Obj(pawn, "Weapon")).c_str(),
             done ? "called" : "failed", again.Run() && again.ReturnBool() ? 1 : 0);
    } else if (log) {
        MLOG("offpistol: the magnum mix %s (the weapon in hand %s magnum rounds)", on ? "on" : "off", want ? "has" : "hasn't");
    }
}

void LogPistol(std::uintptr_t p, const char* when) {
    float dmg = ArrayFloat0(p, "InstantHitDamage");
    MLOG("offpistol: %s -- %s (%s): CurrentUpgradeLevel %d, InstantHitDamage[0] %.0f, RefireCheckTime %.3f, FireInterval[0] %.3f, "
         "clip %d of %d, FiringStatesArray[0] %s, magnum %d, WeaponRange %.0f, falloff %.0f..%.0f, noise %.0f, ReloadInterval[0] %.2f",
         when, names::Name(p).c_str(), names::ClassName(p).c_str(), Int(p, "CurrentUpgradeLevel", -9), dmg, Float(p, "RefireCheckTime", -1.0f),
         ArrayFloat0(p, "FireInterval"), Int(p, "AmmoCount", -1), Int(p, "MaxAmmoCount", -1), ArrayName0(p, "FiringStatesArray").c_str(),
         Bit(p, "bHasMagnumRounds") ? 1 : 0, Float(p, "WeaponRange", -1.0f), Float(p, "ForceFalloffDistanceStart", -1.0f),
         Float(p, "ForceFalloffDistanceMax", -1.0f), Float(p, "WeaponFireNoiseRadius", -1.0f), ArrayFloat0(p, "ReloadInterval"));
}

void LogFunction(std::uintptr_t obj, const char* fn, std::initializer_list<const char*> parms) {
    Call c(obj, fn, true);
    std::string offs;
    for (const char* q : parms) {
        char b[48];
        sprintf_s(b, " %s+0x%X", q, static_cast<unsigned>(c.Off(q)));
        offs += b;
    }
    MLOG("offpistol: %s.%s -- %s, flags 0x%08X, native index %u, parms 0x%X:%s", names::ClassName(obj).c_str(), fn,
         c.ok ? "callable" : "NOT callable", c.Flags(), c.Native(), c.ParmsSize(), offs.c_str());
}

void Dump() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const std::uintptr_t p = Pistol(pawn, inv, gun);
    MLOG("offpistol: pawn %s, in hand %s (%s), the pistol %s (%s), PistolWeapon %s", names::Name(pawn).c_str(), names::Name(gun).c_str(),
         names::ClassName(gun).c_str(), names::Name(p).c_str(), names::ClassName(p).c_str(), names::Name(Obj(inv, "PistolWeapon")).c_str());
    if (!p) return;
    const std::uintptr_t vt = names::ReadPointer(p);
    MLOG("offpistol: %s vtable 0x%08X: +0xF0 0x%08X, +0x344 0x%08X; Instigator %s; CurrentFireMode %d; bInfiniteAmmo %d", names::Name(p).c_str(),
         static_cast<unsigned>(vt), static_cast<unsigned>(names::ReadPointer(vt + 0xF0)), static_cast<unsigned>(names::ReadPointer(vt + 0x344)),
         Obj(p, "Instigator") == pawn ? "the pawn" : names::Name(Obj(p, "Instigator")).c_str(), Byte(p, "CurrentFireMode"),
         Bit(p, "bInfiniteAmmo") ? 1 : 0);
    LogPistol(p, "now");
    const std::uintptr_t mgr = Obj(pawn, "WeaponUpgradeManager");
    {
        Call get(mgr, "GetAppliedUpgradeLevel");
        const std::uint8_t type = static_cast<std::uint8_t>(Byte(p, "WeaponType"));
        const int applied = get.Set("WeaponType", &type, 1) && get.Run() ? get.ReturnInt() : -9;
        MLOG("offpistol: WeaponType %d, WeaponClass %d; the save's applied upgrade level %d", type, Byte(p, "WeaponClass"), applied);
    }
    LogFunction(p, "CalcWeaponFireNative", {"TraceOwner", "StartTrace", "EndTrace", "vTraceExtents", "ImpactList", "ReturnValue"});
    LogFunction(p, "ProcessInstantHit", {"FiringMode", "Impact"});
    LogFunction(p, "SpawnGunshotStimulus", {"Loc"});
    LogFunction(p, "SpawnImpactStimulus", {"StartTrace", "Loc"});
    LogFunction(p, "PlaySoundAt", {"ASound", "SourceLocation"});
    LogFunction(p, "FindObject", {"ObjectName", "ObjectClass", "ReturnValue"});
    LogFunction(pawn, "NoiseRadius", {"fNewRadius"});
    LogFunction(Obj(Obj(pawn, "Controller"), "playerStats"), "OnWeaponFire", {"WeaponType", "WeaponClass", "NumShots"});
    LogFunction(mgr, "upgrade", {"WeaponType", "UpgradeLevel", "W"});
    LogFunction(mgr, "GetAppliedUpgradeLevel", {"WeaponType", "ReturnValue"});
    ReportCue(pawn, p);
    for (const Exp& e : Experience(pawn)) MLOG("offpistol: experience %s: %.1f points, level %d", e.cls.c_str(), e.points, e.level);
}

// The save's upgrade level for the pistol applied if it is behind (a pistol never equipped this level fires at its base
// numbers otherwise; DESIGN 2.5), then the magnum mix rule. Returns the level now.
int UpgradeTo(std::uintptr_t pawn, std::uintptr_t p, bool log) {
    const std::uintptr_t mgr = Obj(pawn, "WeaponUpgradeManager");
    const int cur = Int(p, "CurrentUpgradeLevel", -9);
    if (!p || !mgr) return cur;
    const std::uint8_t type = static_cast<std::uint8_t>(Byte(p, "WeaponType"));
    Call get(mgr, "GetAppliedUpgradeLevel");
    const int applied = get.Set("WeaponType", &type, 1) && get.Run() ? get.ReturnInt() : -9;
    if (applied > cur) {
        if (log) LogPistol(p, "before the upgrade");
        Call up(mgr, "upgrade");
        int lvl = applied;
        const bool done = up.Set("WeaponType", &type, 1) && up.Set("UpgradeLevel", &lvl, sizeof(lvl)) && up.Set("W", &p, sizeof(p)) && up.Run();
        MLOG("offpistol: %s at upgrade level %d, the save's %d -- upgrade(%d, %d) %s", names::Name(p).c_str(), cur, applied, type, lvl,
             done ? "called" : "FAILED");
        MagnumRule(pawn, p, log);
        if (log) LogPistol(p, "after the upgrade");
    } else {
        if (log) MLOG("offpistol: the pistol is at level %d, the save's %d -- nothing to apply", cur, applied);
        MagnumRule(pawn, p, log);  // (the save's own restore leaves it on under a long gun: S1)
    }
    return Int(p, "CurrentUpgradeLevel", -9);
}

// The nearest live axis soldier within 50 m (in line of sight from the eye when `sight`; logged: every AI pawn when `list`).
std::uintptr_t NearestEnemy(std::uintptr_t pawn, const float (&eye)[3], float upm, bool list, bool sight = true) {
    std::uintptr_t best = 0;
    float bestD = 1e30f;
    std::uintptr_t q = Obj(Obj(pawn, "WorldInfo"), "PawnList");
    int seen = 0;
    for (int n = 0; q && n < 512; q = Obj(q, "NextPawn"), ++n) {
        if (q == pawn || !names::IsA(q, "MOHAAIPawn")) continue;
        ++seen;
        float loc[3] = {0, 0, 0};
        names::ReadVector(q + addr::kActorLocation, loc);
        const float d = Dist(eye, loc) / (upm > 1.0f ? upm : 100.0f);
        const int health = Int(q, "Health", -1);
        float hit[3];
        std::uintptr_t by = 0;
        const bool blocked = aim::WorldTrace(pawn, eye, loc, hit, &by) && by != q;
        const bool axis = names::IsA(q, "MOHAAxisAIPawn");
        if (list)
            MLOG("offpistol: %s (%s.%s) %.1f m, Health %d, controller %s, %s%s", names::Name(q).c_str(),
                 names::Name(names::Outer(names::ReadPointer(q + addr::kObjectClass))).c_str(), names::ClassName(q).c_str(), d, health,
                 names::Name(Obj(q, "Controller")).c_str(), blocked ? "blocked by " : "in sight", blocked ? names::Name(by).c_str() : "");
        if (axis && health > 0 && d < 50.0f && (!blocked || !sight) && d < bestD) {
            best = q;
            bestD = d;
        }
    }
    if (list) MLOG("offpistol: %d AI pawn(s); the nearest axis one in sight within 50 m: %s", seen, best ? names::Name(best).c_str() : "none");
    return best;
}

// One shot of the holstered pistol P along start -> dir (world), DESIGN 2.2. `credit`: Pawn.Weapon = P for the damage calls.
// `soundAt`: where the report plays (the muzzle). `log`: the shot's line; `dot` (with log): the off dot's point, to say how
// far from it the shot landed.
bool Shot(std::uintptr_t pawn, std::uintptr_t gun, std::uintptr_t p, const float (&start)[3], const float (&dir)[3],
          const float (&soundAt)[3], float upm, bool credit, bool log, const float* dot = nullptr, std::uintptr_t* hitOut = nullptr) {
    // 0: the gate (the caller's part too: P live and the pawn's, the pawn alive, a round in the clip).
    if (!p || Bit(p, "bDeleteMe") || Obj(p, "Instigator") != pawn || Int(pawn, "Health", 0) <= 0) {
        MLOG("offpistol: no shot -- the pistol %s, its instigator %s, the pawn's health %d", names::Name(p).c_str(),
             names::Name(Obj(p, "Instigator")).c_str(), Int(pawn, "Health", 0));
        return false;
    }
    const int ao = names::PropertyOffset(p, "AmmoCount");
    const int clip = ao >= 0 ? static_cast<int>(ReadU32(p + ao)) : -1;
    if (clip == 0) {
        MLOG("offpistol: the clip is empty -- a dry click");
        return false;
    }
    // 1: the line, the pistol's own range.
    float range = Float(p, "WeaponRange", 0.0f);
    if (range < 100.0f) range = 16384.0f;
    const float end[3] = {start[0] + dir[0] * range, start[1] + dir[1] * range, start[2] + dir[2] * range};
    // 2: ApplyDamage indexes the damage by CurrentFireMode.
    const int fmo = names::PropertyOffset(p, "CurrentFireMode");
    if (fmo >= 0) *reinterpret_cast<std::uint8_t*>(p + fmo) = 0;
    // 3-5: the bullets' own trace, aim.cpp's bullet hook standing down.
    Call trace(p, "CalcWeaponFireNative");
    const float zero[3] = {0, 0, 0};
    const std::uint32_t listIn[3] = {static_cast<std::uint32_t>(g_list.data), 0, static_cast<std::uint32_t>(g_list.max)};
    bool ok = trace.ok && trace.Set("TraceOwner", &pawn, sizeof(pawn)) && trace.Set("StartTrace", start, sizeof(start)) &&
              trace.Set("EndTrace", end, sizeof(end)) && trace.Set("vTraceExtents", zero, sizeof(zero)) &&
              trace.Set("ImpactList", listIn, sizeof(listIn));
    aim::BeginOffShot();
    ok = ok && trace.Run();
    aim::EndOffShot();
    const std::uint8_t* lst = trace.At("ImpactList", 12);
    const std::uint8_t* rv = trace.At("ReturnValue", kImpact);
    if (!ok || !lst || !rv) {
        MLOG("offpistol: CalcWeaponFireNative failed (%s)", !trace.ok ? "not callable" : !ok ? "the call" : "its parameters");
        return false;
    }
    std::memcpy(&g_list.data, lst, 4);
    std::memcpy(&g_list.num, lst + 4, 4);
    std::memcpy(&g_list.max, lst + 8, 4);
    std::uintptr_t hitActor = 0;
    float hitLoc[3];
    std::memcpy(&hitActor, rv, 4);
    std::memcpy(hitLoc, rv + kImpactLocation, sizeof(hitLoc));
    if (hitOut) *hitOut = hitActor;
    // 6: the AI's hearing and the impact stimulus, PerformWeaponTrace's order.
    Call gs(p, "SpawnGunshotStimulus");
    if (gs.Set("Loc", start, sizeof(start))) gs.Run();
    Call is(p, "SpawnImpactStimulus");
    if (is.Set("StartTrace", start, sizeof(start)) && is.Set("Loc", hitLoc, sizeof(hitLoc))) is.Run();
    // 12 (before the damage, as Weapon.FireAmmunition consumes before its trace: a level-up inside the damage refills the
    // clip, and that stands): the round, by a direct write (SetAmmoCount would drive the shared low-ammo mix). None while
    // the game's upgrade sequence runs ("magic bullets").
    const bool magic = names::StateName(Obj(pawn, "WeaponUpgradeManager")) == "UpgradeSequence";
    if (ao >= 0 && clip > 0 && !magic) *reinterpret_cast<int*>(p + ao) = clip - 1;
    // 7-9: the damage, with Pawn.Weapon = P for the calls (its hits, kills and experience), restored unless something
    // changed it meanwhile.
    const int wo = names::PropertyOffset(pawn, "Weapon");
    if (credit && wo >= 0) *reinterpret_cast<std::uintptr_t*>(pawn + wo) = p;
    int damaged = 0;
    std::string bones;
    for (int i = 0; i < g_list.num && i < 16 && g_list.data; ++i) {
        std::uint8_t impact[kImpact];
        if (!SafeCopy(impact, g_list.data + kImpact * i, kImpact)) break;
        Call hit(p, "ProcessInstantHit");
        const std::uint8_t mode = 0;
        if (hit.Set("FiringMode", &mode, 1) && hit.Set("Impact", impact, kImpact) && hit.Run()) ++damaged;
        if (log) {
            std::uintptr_t a = 0;
            std::memcpy(&a, impact, 4);
            bones += " " + names::Name(a) + "/" + names::NameAt(reinterpret_cast<std::uintptr_t>(impact + kImpactBone));
        }
    }
    bool restored = false;
    if (credit && wo >= 0 && names::ReadPointer(pawn + wo) == p) {
        if (gun && !Bit(gun, "bDeleteMe") && Obj(gun, "Instigator") == pawn) {
            *reinterpret_cast<std::uintptr_t*>(pawn + wo) = gun;
            restored = true;
        }
    } else if (credit && wo >= 0) {
        MLOG("offpistol: the shot changed the weapon in hand (%s) -- left as it is", names::Name(names::ReadPointer(pawn + wo)).c_str());
    }
    // 11: the noise and the stats.
    Call nr(pawn, "NoiseRadius");
    const float radius = Float(p, "WeaponFireNoiseRadius", 5500.0f);
    if (nr.Set("fNewRadius", &radius, sizeof(radius))) nr.Run();
    const std::uintptr_t stats = Obj(Obj(pawn, "Controller"), "playerStats");
    Call wf(stats, "OnWeaponFire");
    const std::uint8_t type = static_cast<std::uint8_t>(Byte(p, "WeaponType")), cls = static_cast<std::uint8_t>(Byte(p, "WeaponClass"));
    const int one = 1;
    if (wf.Set("WeaponType", &type, 1) && wf.Set("WeaponClass", &cls, 1) && wf.Set("NumShots", &one, sizeof(one))) wf.Run();
    // 13: the report at the muzzle.
    const bool played = PlayAt(p, ReportCue(pawn, p), soundAt);
    ++g_shots;
    if (log) {
        char fromDot[64] = "";
        if (dot && hitActor) sprintf_s(fromDot, "; %.0f cm from the off dot", Dist(hitLoc, *reinterpret_cast<const float(*)[3]>(dot)) * 100.0f / upm);
        MLOG("offpistol: shot %u -- %s: hit %s at %.1f m%s; %d damage call(s):%s; Pawn.Weapon %s; clip %d -> %d; report %s", g_shots,
             names::Name(p).c_str(), hitActor ? names::Name(hitActor).c_str() : "nothing", (hitActor ? Dist(start, hitLoc) : range) / upm,
             fromDot, damaged, bones.c_str(), !credit ? "untouched (PistolCredit=main)" : restored ? "the pistol for the damage, the gun again after" : "NOT restored",
             clip, ao >= 0 ? static_cast<int>(ReadU32(p + ao)) : -1, played ? "played" : "none");
    }
    return true;
}

// The test channel's shot ("mohavr pistol fire ...", spike S1): from the eye, the off hand or at the nearest enemy.
void Fire(const wchar_t* from, bool head, bool main, int repeat) {
    shared::Header* hdr = bridge::SharedHeader();
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const std::uintptr_t p = Pistol(pawn, inv, gun);
    float eye[3], fwd[3], upm = 100.0f;
    if (!p || !Head(hdr, eye, fwd, upm)) {
        MLOG("offpistol: fire -- no pistol (%s) or no head pose", names::Name(p).c_str());
        return;
    }
    float start[3] = {eye[0], eye[1], eye[2]}, dir[3] = {fwd[0], fwd[1], fwd[2]};
    std::uintptr_t target = 0;
    if (!std::wcscmp(from, L"hand")) {
        float hp[3], hf[3], u = upm;
        if (!OffHand(hdr, hp, hf, u)) {
            MLOG("offpistol: fire hand -- the off hand isn't tracked");
            return;
        }
        // From the hand unless something stands between the eye and it (a hand through a wall: then from the eye).
        float hit[3];
        if (!aim::WorldTrace(pawn, eye, hp, hit)) std::memcpy(start, hp, sizeof(start));
        std::memcpy(dir, hf, sizeof(dir));
    } else if (!std::wcscmp(from, L"enemy")) {
        target = NearestEnemy(pawn, eye, upm, false);
        bool fromNear = false;
        if (!target) {
            // None in sight: the nearest within 50 m all the same, shot from a point near it that sees it -- the damage,
            // the death and the credit are what this tests (ApplyDamage's falloff is measured from the pawn).
            target = NearestEnemy(pawn, eye, upm, false, false);
            fromNear = target != 0;
        }
        if (!target) {
            MLOG("offpistol: fire enemy -- no axis soldier within 50 m");
            return;
        }
        float loc[3];
        names::ReadVector(target + addr::kActorLocation, loc);
        if (head) loc[2] += Float(target, "BaseEyeHeight", 60.0f);
        if (fromNear) {
            float d[3] = {eye[0] - loc[0], eye[1] - loc[1], 0.0f};
            const float h = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            const float tx = h > 1.0f ? d[0] / h : 1.0f, ty = h > 1.0f ? d[1] / h : 0.0f;  // toward the player, level
            const float r = 1.2f * (upm > 1.0f ? upm : 100.0f);
            const float cand[6][3] = {{tx * r, ty * r, 0}, {tx * r, ty * r, r * 0.5f}, {-ty * r, tx * r, 0}, {ty * r, -tx * r, 0},
                                      {0, 0, r}, {-tx * r, -ty * r, 0}};
            bool found = false;
            for (const auto& c : cand) {
                const float s0[3] = {loc[0] + c[0], loc[1] + c[1], loc[2] + c[2]};
                float hit[3];
                std::uintptr_t by = 0;
                if (aim::WorldTrace(pawn, s0, loc, hit, &by) && by == target) {
                    std::memcpy(start, s0, sizeof(start));
                    found = true;
                    break;
                }
            }
            if (!found) {
                MLOG("offpistol: fire enemy -- %s is out of sight and no point near it sees it", names::Name(target).c_str());
                return;
            }
            MLOG("offpistol: fire enemy -- %s is out of sight from the eye: shot from a point 1.2 m from it", names::Name(target).c_str());
        }
        float d[3] = {loc[0] - start[0], loc[1] - start[1], loc[2] - start[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int i = 0; i < 3; ++i) dir[i] = len > 1.0f ? d[i] / len : fwd[i];
    }
    const Snap before = Take(pawn, inv, gun);
    const std::vector<Exp> expBefore = Experience(pawn);
    const int healthBefore = target ? Int(target, "Health", -1) : 0;
    int killType = -1;
    std::string killDamage;
    const int killsBefore = Kills(pawn, killType, killDamage);
    for (int i = 0; i < repeat; ++i)
        if (!Shot(pawn, gun, p, start, dir, start, upm, !main, repeat == 1)) break;
    const Snap after = Take(pawn, inv, gun);
    if (repeat > 1)
        MLOG("offpistol: loop -- %u shots so far; the ImpactList buffer 0x%08X (num %d, max %d) reused", g_shots,
             static_cast<unsigned>(g_list.data), g_list.num, g_list.max);
    MLOG("offpistol: the gun %s -- Pawn.Weapon %s, state %s -> %s, AmmoCount %d -> %d, FlashCount %d -> %d, PendingFire [%d %d] -> [%d %d]; "
         "its attachment's NextImpactSoundIndex %d -> %d",
         names::Name(gun).c_str(), before.weapon == after.weapon && after.weapon == gun ? "unchanged" : "CHANGED", before.state.c_str(),
         after.state.c_str(), before.ammo, after.ammo, before.flash, after.flash, before.pf0, before.pf1, after.pf0, after.pf1,
         before.impactSnd, after.impactSnd);
    if (target)
        MLOG("offpistol: %s -- Health %d -> %d, Physics %d, controller %s%s", names::Name(target).c_str(), healthBefore,
             Int(target, "Health", -1), Byte(target, "Physics"), names::Name(Obj(target, "Controller")).c_str(),
             Bit(target, "bDeleteMe") ? " (deleted)" : "");
    {
        const int killsAfter = Kills(pawn, killType, killDamage);
        if (killsAfter != killsBefore)
            MLOG("offpistol: kill record %d -> %d -- the newest credited to weapon type %d (%s; the pistol is %d, the gun %d), %s",
                 killsBefore, killsAfter, killType, killType == Byte(p, "WeaponType") ? "the pistol" : killType == Byte(gun, "WeaponType") ?
                 "the gun in hand" : "another", Byte(p, "WeaponType"), Byte(gun, "WeaponType"), killDamage.c_str());
    }
    const std::vector<Exp> expAfter = Experience(pawn);
    for (std::size_t i = 0; i < expAfter.size() && i < expBefore.size(); ++i)
        if (expAfter[i].points != expBefore[i].points || expAfter[i].level != expBefore[i].level)
            MLOG("offpistol: experience %s %.1f -> %.1f points, level %d -> %d", expAfter[i].cls.c_str(), expBefore[i].points,
                 expAfter[i].points, expBefore[i].level, expAfter[i].level);
}

// --- the pistol drawn in the off hand (OFFPISTOL-DESIGN 3.1-3.3; spike S2) -------------------------------------------
// A clone of the pistol's pickup mesh (carrier.cpp) placed as the main pistol is, mirrored onto the off controller:
// Gw_off = M_left x Pitch(the fit's angle) x F_off (x W in the bake), M_left = S x M_right x M_y with M_right = the pistol
// mesh in the game camera's frame at its idle, its origin moved back by the fit's grip -- the same axes, the origin's
// sideways part negated. The off hand snaps onto it with the game's own pistol grip, mirrored (reload_grips.inc "offgun").
carrier::Slot      g_carrier;
std::uintptr_t     g_carried = 0;  // the pistol whose clone it is
std::string        g_carriedKey;   // its attachment class
float              g_fitNow[4] = {0, 0, 0, 0};  // the fit the placement was made with (grip forward, right, up; angle)
float              g_mEff[16];     // the pistol mesh in the off controller's frame (rows X, Y, Z, origin)
float              g_hand[16];     // the off hand in the pistol mesh's frame
const float*       g_fingers = nullptr;
const float*       g_fingersPull = nullptr;  // the trigger finger pulled (the Colt's colt45_fire grip), past half a pull
const char* const* g_fingerNames = nullptr;
bool               g_haveGrip = false;
// The C96's parts on the clone (no AnimTree: the reference pose): its buttstock below upgrade level 0 and its box magazine
// below 1 collapsed; its clip (the rounds), parked 55 units behind the gun in the reference pose, put where the game's idle
// holds it (mauser_gun_idle: 0 -3.58 9.92 in the mesh).
struct BoneFix {
    int   index;
    bool  collapse;
    float pos[3];
};
BoneFix g_fix[4];
int     g_fixN = 0;

// out = a x b (row-major 4x4, Unreal's row vectors).
void Mul16(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[4 * i + k] * b[4 * k + j];
            r[4 * i + j] = s;
        }
    std::memcpy(out, r, sizeof(r));
}

// The pistol mesh's origin in the game camera's frame at its idle (forward, right, up; units), its axes the camera's:
// offline from the arms' idle (draw-hold 4.1 -- the Colt's matches viewmodel's in-game log to 0.05 units).
bool IdleOrigin(const std::string& key, float (&o)[3]) {
    if (key == "Attachment_Colt45") {
        o[0] = 37.59f, o[1] = 11.41f, o[2] = -12.64f;
        return true;
    }
    if (key == "Attachment_Mauser") {
        o[0] = 38.05f, o[1] = 11.34f, o[2] = -12.94f;
        return true;
    }
    return false;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

// The pistol's fit ([GunFit] <key> = grip forward right up angle ...: the player's, else the shipped one) -- only when the
// host hasn't published one (the test command without a host).
bool FitFromIni(const std::string& key, float (&fit)[4], const char*& from) {
    const std::wstring wkey(key.begin(), key.end());
    wchar_t v[128] = L"", local[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n && n < MAX_PATH) {
        GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", v, 128, (std::wstring(local) + L"\\MOHAVR\\MOHAVR.user.ini").c_str());
        from = "the player's ini";
    }
    if (!v[0]) {
        GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", v, 128, (ModuleDir() + L"\\MOHAVR.ini").c_str());
        from = "the shipped ini";
    }
    fit[3] = 0.0f;
    return v[0] && swscanf_s(v, L"%f %f %f %f", &fit[0], &fit[1], &fit[2], &fit[3]) >= 3;
}

// The pistol mesh in the off controller's frame for `key` and the fit (grip forward, right, up in units; angle in degrees).
bool Place(const std::string& key, const float (&fit)[4]) {
    float o[3];
    if (!IdleOrigin(key, o)) return false;
    const float ox = o[0] - fit[0], oy = o[1] - fit[1], oz = o[2] - fit[2];
    const float m[16] = {0, -1, 0, 0, 0, 0, -1, 0, 1, 0, 0, 0, ox, -oy, oz, 1};
    // The fit's angle (+ = muzzle up) pitches the gun about the controller's right axis, as the host's gunPose does: in the
    // controller's frame the gun's forward is (c, 0, s), its right (0, 1, 0), its up (-s, 0, c).
    const float a = fit[3] * 0.0174533f, c = std::cos(a), s = std::sin(a);
    const float pitch[16] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    Mul16(m, pitch, g_mEff);
    std::memcpy(g_fitNow, fit, sizeof(g_fitNow));
    return true;
}

// The C96's parts (BoneFix) by the pistol's upgrade level.
void FixBones(std::uintptr_t comp, const std::string& key, std::uintptr_t p) {
    g_fixN = 0;
    if (key != "Attachment_Mauser") return;
    const std::uintptr_t mesh = Obj(comp, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    const int level = Int(p, "CurrentUpgradeLevel", -1);
    std::string what;
    for (int i = 0; i < num && i < 64 && data && g_fixN < 4; ++i) {
        const std::string n = names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride);
        if ((n == "upgrade_01_buttstock" && level < 0) || (n == "upgrade_02_magazine" && level < 1)) {
            g_fix[g_fixN++] = {i, true, {0, 0, 0}};
            what += " " + n + " hidden";
        } else if (n == "clip") {
            g_fix[g_fixN++] = {i, false, {0.0f, -3.58f, 9.92f}};
            what += " clip placed";
        }
    }
    MLOG("offpistol: the C96's parts at upgrade level %d:%s", level, what.empty() ? " (no such bones)" : what.c_str());
}

void CarrierOff() {
    carrier::Detach(g_carrier, "offpistol", g_cfg.debugOffHandTrace);
    g_carried = 0;
    g_fixN = 0;
}

// The pistol drawn in the off hand. `fit`: the host's for this pistol (zero grip: none -- the inis', the test command).
bool CarrierOn(std::uintptr_t pawn, std::uintptr_t p, const float (&hostFit)[4], bool log) {
    const std::string key = names::Name(Obj(p, "AttachmentClass"));
    float fit[4] = {hostFit[0], hostFit[1], hostFit[2], hostFit[3]};
    const char* from = "the host's";
    if (fit[0] == 0.0f && fit[1] == 0.0f && fit[2] == 0.0f && !FitFromIni(key, fit, from)) {
        fit[0] = 34.0f, fit[1] = 11.0f, fit[2] = -17.0f, fit[3] = 0.0f;
        from = "the global default";
    }
    if (!Place(key, fit)) {
        MLOG("offpistol: carrier -- no idle pose for %s (not drawn in the hand)", key.c_str());
        return false;
    }
    const float* hand = nullptr;
    g_haveGrip = reload::GripRows(key, "offgun", hand, g_fingers, g_fingerNames);
    const float* pullHand = nullptr;
    const char* const* pullNames = nullptr;
    if (!reload::GripRows(key, "offgun_pull", pullHand, g_fingersPull, pullNames)) g_fingersPull = nullptr;
    if (g_haveGrip) {
        const float h[16] = {hand[0], hand[1], hand[2], 0, hand[3], hand[4], hand[5], 0, hand[6], hand[7], hand[8], 0,
                             hand[9], hand[10], hand[11], 1};
        std::memcpy(g_hand, h, sizeof(h));
    }
    if (!carrier::Attach(g_carrier, pawn, p, "offpistol", log)) return false;
    g_carried = p;
    g_carriedKey = key;
    FixBones(g_carrier.comp, key, p);
    if (log) {
        // Where it is drawn, in the off controller's frame (forward, right, up): the mesh origin, the muzzle (tag_barrell,
        // the Colt's (0, -7.40, 19.60) / the C96's (0, -7.70, 27.26) in the mesh), the hand bone.
        const bool colt = key == "Attachment_Colt45";
        const float tb[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, colt ? -7.40f : -7.70f, colt ? 19.60f : 27.26f, 1};
        float muzzle[16], rel[16];
        Mul16(tb, g_mEff, muzzle);
        Mul16(g_hand, g_mEff, rel);
        const std::uintptr_t c = g_carrier.comp;
        MLOG("offpistol: carrier %s -- mesh %s, Outer %s, PhysicsAsset %s, bAttached %d, FOV %.0f; %s fit %.1f %.1f %.1f, %.0f deg; in the "
             "off controller's frame: mesh origin %.2f %.2f %.2f, muzzle %.2f %.2f %.2f, the hand %.2f %.2f %.2f (grip %s)",
             names::Name(c).c_str(), names::Name(Obj(c, "SkeletalMesh")).c_str(), names::Name(names::Outer(c)).c_str(),
             names::Name(Obj(c, "PhysicsAsset")).c_str(), Bit(c, "bAttached") ? 1 : 0, Float(c, "FOV", -1.0f), from, fit[0], fit[1],
             fit[2], fit[3], g_mEff[12], g_mEff[13], g_mEff[14], muzzle[12], muzzle[13], muzzle[14], rel[12], rel[13], rel[14],
             g_haveGrip ? "offgun" : "none: the free hand");
    }
    return true;
}

// --- Phase 1: the held pistol (OFFPISTOL-DESIGN 5.3) -------------------------------------------------------------------
enum State { kNone = 0, kHeld = 1 };
enum Refusal : std::uint32_t { kOk = 0, kUnavailable = 1, kNoneCarried = 2, kInGunHand = 3, kTooSoon = 4, kShotFailed = 5,
                               kTaken = 6, kGone = 7 };
const char* const kEventName[4] = {"?", "DRAW", "SHOT", "HOLSTER"};

struct Hold {
    int            state = kNone;
    std::uintptr_t p = 0;
    float          lastShot = -1e9f;  // game time
    int            notOk = 0;         // Draws in a row the game wouldn't let it stay
} g_hold;
struct Refill {
    std::uintptr_t p = 0;
    float          due = 0.0f;  // game time
} g_refill[2];  // one per pistol (a Colt + C96 pair)

Refill* RefillOf(std::uintptr_t p) {
    for (Refill& r : g_refill)
        if (r.p == p) return &r;
    return nullptr;
}
bool RefillPending() { return g_refill[0].p || g_refill[1].p; }
std::uintptr_t     g_pawn = 0;
std::uint32_t      g_seen = 0, g_pawnSeq = 0, g_shotsN = 0, g_dryN = 0, g_refillsN = 0, g_refusal = kOk;
bool               g_seenInit = false;
shared::PistolView g_view{};   // the host's, last read whole
bool               g_haveView = false;
std::uintptr_t     g_resolvedFor = 0;
bool               g_resolvedOk = false;
std::string        g_lastWhy;
unsigned           g_logged = 0;   // shot lines logged (the first 200; all with Debug.OffHandTrace)
float              g_aimDist = 0.0f;
std::uintptr_t     g_levelFor = 0;  // the pistol whose upgrade level is watched (an off-hand level-up)
int                g_level = -9;
bool               g_inSequence = false;
DWORD              g_viewSeqAt = 0;   // when the host last wrote its views (a host that stopped: no held trigger)
std::uint32_t      g_viewSeqSeen = 0;

void Refuse(std::uint32_t type, std::uint32_t code, const char* why) {
    g_refusal = code;
    MLOG("offpistol: %s refused -- %s", type < 4 ? kEventName[type] : "?", why);
}

// Whether a DRAW can happen now; when not, `holdOk` says whether a pistol already held may stay (a switch under way that
// doesn't bring it to the gun hand).
bool Available(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, const char*& why, bool& holdOk) {
    holdOk = false;
    if (offhand::Holding()) {
        why = "a grenade in the off hand";
        return false;
    }
    if (!offhand::BaseAvailable(pawn, inv, gun, why)) return false;
    if (const std::uintptr_t pending = Obj(inv, "PendingWeapon")) {
        why = "a weapon switch";
        holdOk = pending != g_hold.p && (Int(pending, "WeaponType", 0) & 0xFF) != 23;
        return false;
    }
    holdOk = true;
    return true;
}

// The game's own switch weapon (MOHAInventoryManager.SwitchWeapon: primary -> secondary -> SwitchPistol -> primary; a
// grenade -> LastSmallArmsWeapon) would take the held pistol to the gun hand.
bool SwitchTakes(std::uintptr_t inv, std::uintptr_t gunIn, std::uintptr_t p) {
    // IsHoldingPrimary(true) / IsHoldingSecondary(true): a switch under way counts as its target (a second B mid-switch).
    const std::uintptr_t pending = Obj(inv, "PendingWeapon");
    const std::uintptr_t gun = pending ? pending : gunIn;
    if (!p || !gun) return false;
    const std::uintptr_t prim = Obj(inv, "PrimaryWeapon"), sec = Obj(inv, "SecondaryWeapon"), pist = Obj(inv, "PistolWeapon");
    if (gun == prim) return !sec && pist == p;
    if (gun == sec) return pist == p;
    if (names::IsA(gun, "MOHAPistol")) return false;
    return Obj(inv, "LastSmallArmsWeapon") == p;
}

void RefillNow(std::uintptr_t p, const char* when) {
    const int ao = names::PropertyOffset(p, "AmmoCount");
    const int max = Int(p, "MaxAmmoCount", -1);
    if (ao < 0 || max <= 0) return;
    const int was = *reinterpret_cast<int*>(p + ao);
    if (was >= max) return;
    *reinterpret_cast<int*>(p + ao) = max;  // a direct write (SetAmmoCount would drive the shared low-ammo mix)
    ++g_refillsN;
    MLOG("offpistol: %s refilled %d -> %d (%s)", names::Name(p).c_str(), was, max, when);
}

// Back in its holster: the clone gone; refilled after the pistol's own reload time ([OffHand] PistolRefill=game), at once
// (instant) or never (off).
void Holster(std::uintptr_t pawn, const char* what) {
    const std::uintptr_t p = g_hold.p;
    CarrierOff();
    const int clip = Int(p, "AmmoCount", -1), max = Int(p, "MaxAmmoCount", -1);
    char refill[64] = "full";
    if (clip >= 0 && max > 0 && clip < max) {
        if (g_cfg.offPistolRefill == 2) {
            RefillNow(p, "in the holster at once");
            sprintf_s(refill, "refilled at once");
        } else if (g_cfg.offPistolRefill == 1) {
            float t = ArrayFloat0(p, "ReloadInterval");
            if (!(t > 0.0f && t < 10.0f)) t = 1.5f;
            Refill* r = RefillOf(p);
            if (!r) r = RefillOf(0);
            if (!r) r = &g_refill[0];
            *r = Refill{p, GameTime(pawn) + t};
            sprintf_s(refill, "refilled in %.2f s", t);
        } else {
            sprintf_s(refill, "not refilled (PistolRefill=off)");
        }
    }
    MLOG("offpistol: %s -- %s, clip %d/%d, %s", what, names::Name(p).c_str(), clip, max, refill);
    g_hold = Hold{};
}

// The hold ended by the game (the pistol gone, taken to the gun hand, the game made it unavailable).
void End(std::uintptr_t pawn, std::uint32_t code, const char* why) {
    g_refusal = code;
    if (code == kUnavailable) {
        char what[96];
        sprintf_s(what, "put back: %s", why);
        Holster(pawn, what);
        return;
    }
    MLOG("offpistol: the hold ended -- %s", why);
    CarrierOff();
    g_hold = Hold{};
}

void Draw(std::uintptr_t pawn, std::uintptr_t p) {
    // A refill due is done now; one not yet due is called off -- drawn sooner, it keeps its count.
    if (Refill* r = RefillOf(p)) {
        if (GameTime(pawn) >= r->due) RefillNow(p, "due at the draw");
        else MLOG("offpistol: drawn %.2f s before its refill -- it keeps its count", r->due - GameTime(pawn));
        *r = Refill{};
    }
    const int level = g_cfg.offPistolUpgrades ? UpgradeTo(pawn, p, g_cfg.debugOffHandTrace) : Int(p, "CurrentUpgradeLevel", -9);
    g_hold = Hold{kHeld, p, -1e9f, 0};
    g_refusal = kOk;
    const bool drawn = g_cfg.offHandCarrier && g_bake && !g_cfg.hideViewModel && CarrierOn(pawn, p, g_view.fit, g_cfg.debugOffHandTrace || g_logged < 3);
    MLOG("offpistol: DRAW %s -- clip %d/%d, upgrade level %d, %.0f damage, refire %.2f s%s", names::Name(p).c_str(), Int(p, "AmmoCount", -1),
         Int(p, "MaxAmmoCount", -1), level, ArrayFloat0(p, "InstantHitDamage"), Float(p, "RefireCheckTime", -1.0f),
         drawn ? ", drawn in the hand" : "");
}

// The off line from the host's ray: where a shot goes and how far the dot is. The trace starts past anything the ray starts
// inside (20 cm steps up to 1 m, as the main aim line). The shot goes from there along the ray when nothing stands between
// the eye and that point (Aim.ShotFromGun), else from the eye at the ray's aim point (D12's rule). `muzzle`: the ray's own
// start; `dist`: metres along the ray to what it hits (or its end).
bool Line(shared::Header* hdr, std::uintptr_t pawn, const shared::Pose& ray, bool forShot, float (&start)[3], float (&dir)[3],
          float (&muzzle)[3], float (&point)[3], float& upm, float& dist, bool& fromEye) {
    float pos[3], fwd[3];
    if (!view::PoseToWorld(ray, pos, fwd, upm) || upm < 1.0f) return false;
    std::memcpy(muzzle, pos, sizeof(muzzle));
    const float reach = 300.0f * upm;
    const float end[3] = {pos[0] + fwd[0] * reach, pos[1] + fwd[1] * reach, pos[2] + fwd[2] * reach};
    float from[3] = {pos[0], pos[1], pos[2]};
    std::uintptr_t actor = 0;
    bool hit = aim::WorldTrace(pawn, from, end, point, &actor);
    const float step = 20.0f * upm / 100.0f;
    for (int k = 1; hit && k <= 5 && Dist(point, from) < step; ++k) {
        for (int i = 0; i < 3; ++i) from[i] = pos[i] + fwd[i] * step * static_cast<float>(k);
        hit = aim::WorldTrace(pawn, from, end, point, &actor);
    }
    if (!hit) std::memcpy(point, end, sizeof(point));
    dist = Dist(point, pos) / upm;
    std::memcpy(start, from, sizeof(start));
    std::memcpy(dir, fwd, sizeof(dir));
    fromEye = false;
    if (!forShot) return true;
    float eye[3], eyeF[3], u = upm, at[3];
    if (Head(hdr, eye, eyeF, u) && (!g_cfg.aimShotFromGun || aim::WorldTrace(pawn, eye, from, at))) {
        float d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1.0f) {
            std::memcpy(start, eye, sizeof(start));
            for (int i = 0; i < 3; ++i) dir[i] = d[i] / len;
            fromEye = true;
        }
    }
    return true;
}

// One pull (or one round of a held trigger: `autoFire`) of the held pistol along `ray`.
void Pull(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, const shared::Pose& ray, bool autoFire,
          bool holdOk, const char* notOk) {
    const std::uintptr_t p = g_hold.p;
    const char* why = "";
    if (!Owned(pawn, inv, p)) why = "the pistol is gone";
    else if (p == gun || p == Obj(inv, "PendingWeapon")) why = "it is going to the gun hand";
    else if (!holdOk) why = notOk;
    else if (Int(pawn, "Health", 0) <= 0) why = "the player is dead";
    else if (Bit(pawn, "bNoWeaponFiring")) why = "weapons held by the game";
    else if (Obj(Obj(pawn, "WorldInfo"), "Pauser")) why = "the game is paused";
    else if (hdr && hdr->gameUiMenu) why = "a game menu is open";
    if (*why) {
        if (!autoFire) Refuse(shared::kPistolShot, kUnavailable, why);
        return;
    }
    // The pistol's own rate in game time (upgrades change it); an early pull is dropped, as the game's single fire does.
    const float now = GameTime(pawn);
    float rate = Float(p, "RefireCheckTime", 0.0f);
    if (!(rate > 0.0f)) rate = ArrayFloat0(p, "FireInterval");
    if (!(rate > 0.0f && rate < 5.0f)) rate = 0.25f;
    if (now - g_hold.lastShot < rate - 0.002f) {
        if (!autoFire) {
            g_refusal = kTooSoon;
            if (g_cfg.debugOffHandTrace) MLOG("offpistol: SHOT too soon (%.3f s after the last, the rate %.3f s)", now - g_hold.lastShot, rate);
        }
        return;
    }
    const int clip = Int(p, "AmmoCount", 0);
    if (clip <= 0 && autoFire) return;  // one click per pull
    float start[3], dir[3], muzzle[3], point[3], upm = 100.0f, dist = 0.0f;
    bool fromEye = false;
    if (!Line(hdr, pawn, ray, true, start, dir, muzzle, point, upm, dist, fromEye)) {
        if (!autoFire) Refuse(shared::kPistolShot, kShotFailed, "no view mapping for the ray");
        return;
    }
    g_hold.lastShot = now;
    if (clip <= 0) {
        const bool played = PlayAt(p, DryCue(pawn, p), muzzle);
        ++g_dryN;
        MLOG("offpistol: SHOT -- %s is empty: a dry click (%s)", names::Name(p).c_str(), played ? "played" : "silent");
        return;
    }
    const bool log = g_cfg.debugOffHandTrace || g_logged < 200;
    if (log) ++g_logged;
    if (log && fromEye) MLOG("offpistol: something stands between the eye and the off gun -- the shot goes from the eye");
    if (!Shot(pawn, gun, p, start, dir, muzzle, upm, g_cfg.offPistolCredit, log, point)) {
        Refuse(shared::kPistolShot, kShotFailed, "the shot failed");
        return;
    }
    ++g_shotsN;
}

void Apply(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, std::uint32_t e, const shared::Pose& ray,
           bool avail, bool holdOk, const char* why) {
    const std::uint32_t type = e & 0xFFu;
    switch (type) {
    case shared::kPistolDraw: {
        if (g_hold.state != kNone) return Refuse(type, kUnavailable, "a pistol is already held");
        if (!avail) return Refuse(type, kUnavailable, why);
        const std::uintptr_t p = Pistol(pawn, inv, gun);
        if (!p) {
            const bool inHand = gun && names::IsA(gun, "MOHAPistol");
            return Refuse(type, inHand ? kInGunHand : kNoneCarried, inHand ? "the only pistol is in the gun hand" : "no pistol carried");
        }
        if (Obj(inv, "PendingWeapon") == p) return Refuse(type, kInGunHand, "a switch is bringing it to the gun hand");
        return Draw(pawn, p);
    }
    case shared::kPistolShot:
        if (g_hold.state != kHeld) return Refuse(type, kUnavailable, "no pistol held");
        return Pull(hdr, pawn, inv, gun, ray, false, holdOk, why);
    case shared::kPistolHolster:
        if (g_hold.state != kHeld) return Refuse(type, kUnavailable, "no pistol held");
        return Holster(pawn, "HOLSTER");
    default:
        return Refuse(type, kUnavailable, "unknown event");
    }
}

void Publish(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, bool avail, bool holdOk, bool off) {
    // The pistol a DRAW gets (or the held one). Switched off with nothing held: nothing is walked or resolved (rule 7) -- the
    // status still goes out (the host sees us alive).
    const std::uintptr_t p = g_hold.state == kHeld ? g_hold.p : (pawn && !off ? Pistol(pawn, inv, gun) : 0);
    if (p && p != g_resolvedFor) {
        g_resolvedFor = p;
        g_resolvedOk = Call(p, "CalcWeaponFireNative", true).ok && Call(p, "ProcessInstantHit", true).ok;
        if (!g_resolvedOk) MLOG("offpistol: %s's shot functions don't resolve -- no off-hand pistol with it", names::Name(p).c_str());
    }
    const bool installed = p && g_resolvedOk;
    const bool onlyInHand = !off && !p && gun && names::IsA(gun, "MOHAPistol");
    const bool held = g_hold.state == kHeld;
    const std::string key = p ? names::Name(Obj(p, "AttachmentClass")) : std::string();
    const int clip = p ? Int(p, "AmmoCount", 0) : 0, max = p ? Int(p, "MaxAmmoCount", 0) : 0;
    const bool autoFire = held && ArrayName0(p, "FiringStatesArray") == "WeaponBurstFire";
    ++hdr->pistolSeq;  // odd: writing
    _ReadWriteBarrier();
    hdr->pistolCaps = (installed ? 1u : 0u) | (installed && avail ? 2u : 0u) | (p && Bit(p, "bInfiniteAmmo") ? 4u : 0u) |
                      (g_cfg.offHandCarrier && g_bake && ObjectProcessEventOk() ? 8u : 0u) | (installed && holdOk ? 16u : 0u) |
                      (onlyInHand ? 32u : 0u) | (held && SwitchTakes(inv, gun, p) ? 64u : 0u) | (autoFire ? 128u : 0u);
    const size_t n = key.size() < 47 ? key.size() : 47;
    std::memcpy(hdr->pistolKey, key.c_str(), n);
    hdr->pistolKey[n] = 0;
    hdr->pistolClip = clip;
    hdr->pistolMax = max;
    hdr->pistolState = static_cast<std::uint32_t>(g_hold.state) | (p && clip <= 0 ? 4u : 0u) | (RefillPending() ? 8u : 0u) | (g_refusal << 8);
    hdr->pistolShots = g_shotsN;
    hdr->pistolDry = g_dryN;
    hdr->pistolEvtAck = g_seen;
    hdr->pistolPawnSeq = g_pawnSeq;
    hdr->pistolAimDistance = held ? g_aimDist : 0.0f;
    hdr->pistolRefills = g_refillsN;
    _ReadWriteBarrier();
    ++hdr->pistolSeq;  // even: done
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
    MLOG("offpistol: credit %s, refill %s, upgrades %d%s", cfg.offPistolCredit ? "the pistol" : "the gun in hand",
         cfg.offPistolRefill == 2 ? "instant" : cfg.offPistolRefill == 1 ? "after the reload time" : "off", cfg.offPistolUpgrades ? 1 : 0,
         bake ? "" : "; no arm bake (Weapon.ArmIK, ViewModel=2): the pistol isn't drawn in the hand");
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!hdr) return;
    // A new local pawn (death, a level load): what was held is gone, as the game's own would be.
    if (pawn != g_pawn) {
        if (g_hold.state != kNone) MLOG("offpistol: a new pawn -- the held %s is gone", names::Name(g_hold.p).c_str());
        g_carrier = carrier::Slot{};  // (attached to the old pawn's arms)
        g_carried = 0;
        g_fixN = 0;
        g_hold = Hold{};
        g_refill[0] = g_refill[1] = Refill{};
        g_resolvedFor = 0;
        g_cueFor = g_cue = g_dryFor = g_dry = 0;  // (a new pistol may get an old one's address)
        g_levelFor = 0;
        g_pawn = pawn;
        ++g_pawnSeq;
    }
    // The host's side: its switch, the trigger, the off line and the pistol's fit.
    shared::Pose hand[2];
    std::uint32_t valid = 0;
    shared::PistolView pv{};
    if (shared::ReadHands(hdr, hand, valid, nullptr, nullptr, &pv)) {
        g_view = pv;
        g_haveView = true;
    }
    const DWORD tick = GetTickCount();
    if (hdr->viewSeq != g_viewSeqSeen) {
        g_viewSeqSeen = hdr->viewSeq;
        g_viewSeqAt = tick;
    }
    const bool hostLive = bridge::HostRunning() && (hdr->viewValid & 1u) && tick - g_viewSeqAt < 150;
    if (!hostLive) g_view.flags = (g_view.flags & ~8u) | 4u;  // frozen: no trigger held, no auto fire
    // Switched off with nothing held or due: no script calls (rule 7).
    const bool off = !(g_view.flags & 1u) && g_hold.state == kNone && !RefillPending();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const char* why = !pawn ? "no pawn" : off ? "switched off" : "";
    bool holdOk = false;
    const bool avail = pawn && !off && Available(pawn, inv, gun, why, holdOk);
    if (g_cfg.debugOffHandTrace && g_lastWhy != why) {
        MLOG("offpistol: %s%s", *why ? "not available -- " : "available", why);
        g_lastWhy = why;
    }
    // The hold: ended by the game when the pistol is gone, taken to the gun hand, or the game won't let it stay for 2 Draws
    // (or the host is gone for good) -- before the events, so no SHOT goes out for a pistol already taken back.
    if (g_hold.state == kHeld && pawn) {
        const std::uintptr_t p = g_hold.p;
        if (!Owned(pawn, inv, p)) End(pawn, kGone, "the pistol is gone");
        else if (p == gun || p == Obj(inv, "PendingWeapon")) End(pawn, kTaken, "a weapon switch took it to the gun hand");
        else if (!bridge::HostRunning()) End(pawn, kUnavailable, "the host is gone");
        else if (!holdOk && ++g_hold.notOk >= 2) End(pawn, kUnavailable, why);
        else if (holdOk) g_hold.notOk = 0;
    }
    // The host's events, in order (the grenade's and the reload's ring rules).
    const std::uint32_t seq = hdr->pistolEvtSeq;
    if (!g_seenInit) {
        g_seen = seq;
        g_seenInit = true;
    }
    if (seq - g_seen > 8) {
        MLOG("offpistol: %u events overran the ring -- skipped", seq - g_seen);
        g_seen = seq;
    }
    for (int k = 0; g_seen != seq && k < 8; ++k) {
        const std::uint32_t slot = g_seen % 8u;
        const std::uint32_t e = hdr->pistolEvt[slot];
        const shared::Pose ray = hdr->pistolEvtRay[slot];
        ++g_seen;
        if (!pawn || !inv) {
            Refuse(e & 0xFFu, kUnavailable, "no pawn");
            continue;
        }
        Apply(hdr, pawn, inv, gun, e, ray, avail, holdOk, why);
    }
    // A held trigger: the next round at the rate (the C96 at its 712 level fires while held).
    if (g_hold.state == kHeld && pawn && (g_view.flags & 8u) && !(g_view.flags & 4u) &&
        ArrayName0(g_hold.p, "FiringStatesArray") == "WeaponBurstFire")
        Pull(hdr, pawn, inv, gun, g_view.ray, true, holdOk, why);
    // The refills in the holster.
    for (Refill& r : g_refill) {
        if (!r.p || !pawn || g_hold.p == r.p) continue;
        const std::uintptr_t p = r.p;
        if (!Owned(pawn, inv, p)) {
            r = Refill{};
        } else if (p == gun || p == Obj(inv, "PendingWeapon")) {
            MLOG("offpistol: %s's refill called off -- it went to the gun hand (the game's own equip)", names::Name(p).c_str());
            r = Refill{};
        } else if (GameTime(pawn) >= r.due) {
            RefillNow(p, "in the holster");
            r = Refill{};
        }
    }
    // An off-hand level-up (its kills leveled the pistol): the game's sequence turns on the shared magnum mix and drives the
    // low-ammo mix by the pistol's count -- both put right for the weapon in hand once it is done.
    if (pawn && !off) {
        const std::uintptr_t p = g_hold.state == kHeld ? g_hold.p : Pistol(pawn, inv, gun);
        const int level = p ? Int(p, "CurrentUpgradeLevel", -9) : -9;
        const bool inSeq = names::StateName(Obj(pawn, "WeaponUpgradeManager")) == "UpgradeSequence";
        if (p != g_levelFor) {
            g_levelFor = p;
            g_level = level;
        } else if (!inSeq && (level != g_level || g_inSequence)) {
            g_level = level;
            MLOG("offpistol: %s's upgrade (level %d) is done -- the weapon in hand's mixes put right", names::Name(p).c_str(), level);
            MagnumRule(pawn, p, false);
            Call low(gun, "UpdateLowAmmoMix", true);
            const std::uint8_t mode = static_cast<std::uint8_t>(Byte(gun, "CurrentFireMode") & 0xFF);
            if (low.ok && low.Set("FireModeNum", &mode, 1)) low.Run();
        }
        g_inSequence = inSeq;
    }
    // The second dot: how far along the off line its point is.
    g_aimDist = 0.0f;
    if (g_hold.state == kHeld && pawn && g_haveView && !(g_view.flags & 4u)) {
        float start[3], dir[3], muzzle[3], point[3], upm = 100.0f, dist = 0.0f;
        bool fromEye = false;
        if (Line(hdr, pawn, g_view.ray, false, start, dir, muzzle, point, upm, dist, fromEye)) g_aimDist = dist;
    }
    Publish(hdr, pawn, inv, gun, avail, holdOk, off);
}

std::uintptr_t CarrierComponent() { return carrier::Component(g_carrier); }

bool CarrierFrame(float (&gw)[16]) {
    if (!CarrierComponent()) return false;
    float gun[16], off[16];
    bool offValid = false, two = false;
    if (!viewmodel::HandFrames(gun, off, offValid, two) || !offValid) return false;
    // The host's fit may change while it is held (the Gun fit page with the same pistol in the gun hand).
    if (g_haveView && (g_view.fit[0] != 0.0f || g_view.fit[1] != 0.0f || g_view.fit[2] != 0.0f) &&
        std::memcmp(g_view.fit, g_fitNow, sizeof(g_fitNow)) != 0)
        Place(g_carriedKey, g_view.fit);
    Mul16(g_mEff, off, gw);
    return true;
}

bool CarrierBone(int index, bool& collapse, float (&pos)[3]) {
    for (int i = 0; i < g_fixN; ++i)
        if (g_fix[i].index == index) {
            collapse = g_fix[i].collapse;
            std::memcpy(pos, g_fix[i].pos, sizeof(pos));
            return true;
        }
    return false;
}

bool HandOnGun(float (&rel)[16], const float*& fingers, const char* const*& names) {
    if (!CarrierComponent() || !g_haveGrip) return false;
    Mul16(g_hand, g_mEff, rel);
    fingers = g_fingersPull && (g_view.flags & 2u) && g_view.trigger >= 0.5f ? g_fingersPull : g_fingers;
    names = g_fingerNames;
    return true;
}

bool Holding() { return g_hold.state == kHeld; }

bool SkipHeldPistol() {
    if (g_hold.state != kHeld || !g_cfg.offPistolKeep) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    return pawn && Obj(Obj(pawn, "InvManager"), "PendingWeapon") == g_hold.p;
}

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr pistol", 13) != 0) return false;
    wchar_t a[16] = L"", b[16] = L"", c[16] = L"";
    int n = 0;
    if (swscanf_s(line, L"mohavr pistol loop %d", &n) == 1) {
        Fire(L"eye", false, false, n > 0 && n <= 1000 ? n : 100);
    } else if (swscanf_s(line, L"mohavr pistol fire %15ls %15ls %15ls", a, static_cast<unsigned>(16), b, static_cast<unsigned>(16), c,
                         static_cast<unsigned>(16)) >= 1) {
        const bool head = !std::wcscmp(b, L"head") || !std::wcscmp(c, L"head");
        const bool main = !std::wcscmp(b, L"main") || !std::wcscmp(c, L"main");
        Fire(a, head, main, 1);
    } else if (!std::wcsncmp(line, L"mohavr pistol kill", 18)) {
        // Head shots at the nearest axis soldier until he dies (refilled between), with or without the credit swap.
        const bool main = std::wcsstr(line, L"main") != nullptr;
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        float eye[3], fwd[3], upm = 100.0f;
        std::uintptr_t first = 0;
        if (pawn && Head(bridge::SharedHeader(), eye, fwd, upm)) {
            first = NearestEnemy(pawn, eye, upm, false);
            if (!first) first = NearestEnemy(pawn, eye, upm, false, false);
        }
        for (int i = 0; i < 6 && first && Int(first, "Health", 0) > 0; ++i) {
            const std::uintptr_t p = Pistol(pawn, Obj(pawn, "InvManager"), Obj(pawn, "Weapon"));
            const int ao = p ? names::PropertyOffset(p, "AmmoCount") : -1;
            if (ao >= 0) *reinterpret_cast<int*>(p + ao) = Int(p, "MaxAmmoCount", 7);
            Fire(L"enemy", true, main, 1);
        }
        MLOG("offpistol: kill -- %s %s", names::Name(first).c_str(), first && Int(first, "Health", 0) <= 0 ? "dead" : "still alive");
    } else if (!std::wcscmp(line, L"mohavr pistol sound")) {
        // The report's AudioComponents among WorldInfo's (PlaySoundAt makes one there): wave instances = the sound bank is in.
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const std::uintptr_t wi = Obj(pawn, "WorldInfo");
        const int co = wi ? names::PropertyOffset(wi, "Components") : -1;
        const std::uintptr_t arr = co >= 0 ? names::ReadPointer(wi + co) : 0;
        const int count = co >= 0 ? static_cast<int>(ReadU32(wi + co + 4)) : 0;
        int found = 0;
        for (int i = 0; i < count && i < 4096 && arr; ++i) {
            const std::uintptr_t ac = names::ReadPointer(arr + 4 * i);
            if (!ac || names::ClassName(ac) != "AudioComponent") continue;
            const std::uintptr_t cue = Obj(ac, "SoundCue");
            if (!cue || (cue != g_cue && cue != g_dry)) continue;
            const int wo = names::PropertyOffset(ac, "WaveInstances");
            ++found;
            MLOG("offpistol: sound -- %s plays %s: %d wave instance(s), %.2f s in", names::Name(ac).c_str(), names::Name(cue).c_str(),
                 wo >= 0 ? static_cast<int>(ReadU32(ac + wo + 4)) : -1, Float(ac, "PlaybackTime", -1.0f));
        }
        if (!found) MLOG("offpistol: sound -- no AudioComponent of WorldInfo plays %s or %s", names::Name(g_cue).c_str(), names::Name(g_dry).c_str());
    } else if (!std::wcsncmp(line, L"mohavr pistol carrier", 21)) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        if (std::wcsstr(line, L"off")) {
            if (g_hold.state == kNone) CarrierOff();
        } else if (g_hold.state == kNone) {
            const std::uintptr_t p = Pistol(pawn, Obj(pawn, "InvManager"), Obj(pawn, "Weapon"));
            const float none[4] = {0, 0, 0, 0};
            if (!p || !CarrierOn(pawn, p, none, true)) MLOG("offpistol: carrier -- not drawn (the pistol %s)", names::Name(p).c_str());
        }
    } else if (!std::wcscmp(line, L"mohavr pistol refill")) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const std::uintptr_t inv = Obj(pawn, "InvManager");
        const std::uintptr_t p = g_hold.state == kHeld ? g_hold.p : Pistol(pawn, inv, Obj(pawn, "Weapon"));
        const int ao = p ? names::PropertyOffset(p, "AmmoCount") : -1;
        if (ao >= 0) *reinterpret_cast<int*>(p + ao) = Int(p, "MaxAmmoCount", 7);
        MLOG("offpistol: refill -- %s's clip %d", names::Name(p).c_str(), Int(p, "AmmoCount", -1));
    } else if (!std::wcscmp(line, L"mohavr pistol upgrade")) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        UpgradeTo(pawn, Pistol(pawn, Obj(pawn, "InvManager"), Obj(pawn, "Weapon")), true);
    } else if (!std::wcscmp(line, L"mohavr pistol enemy")) {
        float eye[3], fwd[3], upm = 100.0f;
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        if (pawn && Head(bridge::SharedHeader(), eye, fwd, upm)) NearestEnemy(pawn, eye, upm, true);
    } else if (!std::wcsncmp(line, L"mohavr pistol empty", 19)) {
        // Tests: the held (or holstered) pistol's clip set to N (default 1).
        int left = 1;
        swscanf_s(line, L"mohavr pistol empty %d", &left);
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const std::uintptr_t p = g_hold.state == kHeld ? g_hold.p : Pistol(pawn, Obj(pawn, "InvManager"), Obj(pawn, "Weapon"));
        const int ao = p ? names::PropertyOffset(p, "AmmoCount") : -1;
        if (ao >= 0) *reinterpret_cast<int*>(p + ao) = left;
        MLOG("offpistol: test -- %s's clip set to %d", names::Name(p).c_str(), Int(p, "AmmoCount", -1));
    } else {
        Dump();
    }
    return true;
}

}  // namespace mohavr::offpistol
