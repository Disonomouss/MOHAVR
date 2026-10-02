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
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "vr_view.hpp"

namespace mohavr::offpistol {
using namespace script;  // Call, Obj, Bit, ... (script_call.hpp)
namespace {

Config g_cfg;

// The engine's ImpactList buffer, reused across shots: each trace is given {Data, Num = 0, Max} of the last one and hands
// back the new header. A mod-made pointer is never passed in (the engine may grow or free it).
struct List {
    std::uintptr_t data = 0;
    int            num = 0, max = 0;
} g_list;
std::uintptr_t g_cueFor = 0, g_cue = 0;  // the report cue found for this pistol (and its magnum state)
bool           g_cueMagnum = false;
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

// The pawn's pistol that isn't the weapon in hand: the inventory manager's PistolWeapon, else the inventory chain.
std::uintptr_t Pistol(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun) {
    const std::uintptr_t p = Obj(inv, "PistolWeapon");
    if (p && p != gun && names::IsA(p, "MOHAPistol") && Obj(p, "Instigator") == pawn) return p;
    std::uintptr_t item = Obj(inv, "InventoryChain");
    for (int n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (item != gun && names::IsA(item, "MOHAPistol") && Obj(item, "Instigator") == pawn) return item;
    return 0;
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

// Object.FindObject(path, the SoundCue class) -- the class taken from the gun's attachment's dry-fire cue.
std::uintptr_t FindCue(std::uintptr_t pawn, std::uintptr_t p, const wchar_t* path) {
    const std::uintptr_t any = Obj(Obj(pawn, "CurrentWeaponAttachment"), "WeaponDryFireSnd");
    const std::uintptr_t cls = any ? names::ReadPointer(any + addr::kObjectClass) : 0;
    if (!cls) return 0;
    struct FString {
        const wchar_t* data;
        int            num, max;
    } s{path, static_cast<int>(std::wcslen(path)) + 1, static_cast<int>(std::wcslen(path)) + 1};
    Call find(p, "FindObject");
    if (!find.Set("ObjectName", &s, sizeof(s)) || !find.Set("ObjectClass", &cls, sizeof(cls)) || !find.Run()) return 0;
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

// The game's invariant for the shared magnum mix (DESIGN finding 1): on only while the weapon in hand has magnum rounds.
void MagnumRule(std::uintptr_t pawn, std::uintptr_t p) {
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
    } else {
        MLOG("offpistol: the magnum mix %s (the weapon in hand %s magnum rounds)", on ? "on" : "off", want ? "has" : "hasn't");
    }
}

void LogPistol(std::uintptr_t pawn, std::uintptr_t p, const char* when) {
    float dmg = ArrayFloat0(p, "InstantHitDamage");
    MLOG("offpistol: %s -- %s (%s): CurrentUpgradeLevel %d, InstantHitDamage[0] %.0f, RefireCheckTime %.3f, FireInterval[0] %.3f, "
         "clip %d of %d, FiringStatesArray[0] %s, magnum %d, WeaponRange %.0f, falloff %.0f..%.0f, noise %.0f",
         when, names::Name(p).c_str(), names::ClassName(p).c_str(), Int(p, "CurrentUpgradeLevel", -9), dmg, Float(p, "RefireCheckTime", -1.0f),
         ArrayFloat0(p, "FireInterval"), Int(p, "AmmoCount", -1), Int(p, "MaxAmmoCount", -1), ArrayName0(p, "FiringStatesArray").c_str(),
         Bit(p, "bHasMagnumRounds") ? 1 : 0, Float(p, "WeaponRange", -1.0f), Float(p, "ForceFalloffDistanceStart", -1.0f),
         Float(p, "ForceFalloffDistanceMax", -1.0f), Float(p, "WeaponFireNoiseRadius", -1.0f));
    (void)pawn;
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
    LogPistol(pawn, p, "now");
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

void Upgrade() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const std::uintptr_t p = Pistol(pawn, inv, gun);
    const std::uintptr_t mgr = Obj(pawn, "WeaponUpgradeManager");
    if (!p || !mgr) {
        MLOG("offpistol: upgrade -- no pistol (%s) or upgrade manager (%s)", names::Name(p).c_str(), names::Name(mgr).c_str());
        return;
    }
    const std::uint8_t type = static_cast<std::uint8_t>(Byte(p, "WeaponType"));
    Call get(mgr, "GetAppliedUpgradeLevel");
    const int applied = get.Set("WeaponType", &type, 1) && get.Run() ? get.ReturnInt() : -9;
    const int cur = Int(p, "CurrentUpgradeLevel", -9);
    LogPistol(pawn, p, "before the upgrade");
    if (applied > cur) {
        Call up(mgr, "upgrade");
        int lvl = applied;
        const bool done = up.Set("WeaponType", &type, 1) && up.Set("UpgradeLevel", &lvl, sizeof(lvl)) && up.Set("W", &p, sizeof(p)) && up.Run();
        MLOG("offpistol: upgrade(%d, %d, %s) %s", type, lvl, names::Name(p).c_str(), done ? "called" : "FAILED");
    } else {
        MLOG("offpistol: the pistol is at level %d, the save's %d -- nothing to apply", cur, applied);
    }
    MagnumRule(pawn, p);
    LogPistol(pawn, p, "after the upgrade");
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
bool Shot(std::uintptr_t pawn, std::uintptr_t gun, std::uintptr_t p, const float (&start)[3], const float (&dir)[3],
          float upm, bool credit, bool quiet, std::uintptr_t* hitOut = nullptr) {
    // 0: the gate (the test's part: P live and the pawn's, the pawn alive, a round in the clip).
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
        std::uintptr_t a = 0;
        std::memcpy(&a, impact, 4);
        bones += " " + names::Name(a) + "/" + names::NameAt(reinterpret_cast<std::uintptr_t>(impact + kImpactBone));
    }
    bool restored = false;
    if (credit && wo >= 0 && names::ReadPointer(pawn + wo) == p && gun && !Bit(gun, "bDeleteMe") && Obj(gun, "Instigator") == pawn) {
        *reinterpret_cast<std::uintptr_t*>(pawn + wo) = gun;
        restored = true;
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
    // 12: the round, by a direct write (SetAmmoCount would drive the shared low-ammo mix).
    if (ao >= 0 && clip > 0) *reinterpret_cast<int*>(p + ao) = clip - 1;
    // 13: the report at the muzzle.
    bool played = false;
    if (const std::uintptr_t cue = ReportCue(pawn, p)) {
        Call snd(p, "PlaySoundAt");
        played = snd.Set("ASound", &cue, sizeof(cue)) && snd.Set("SourceLocation", start, sizeof(start)) && snd.Run();
    }
    ++g_shots;
    if (!quiet) {
        float wt[3];
        std::uintptr_t wtBy = 0;
        const bool wtHit = aim::WorldTrace(pawn, start, end, wt, &wtBy);
        MLOG("offpistol: shot %u -- %s: ImpactList %d (data 0x%08X, max %d); hit %s at %.1f m (WorldTrace: %s, %.1f units apart); "
             "%d damage call(s):%s; Pawn.Weapon %s; clip %d -> %d; report %s",
             g_shots, names::Name(p).c_str(), g_list.num, static_cast<unsigned>(g_list.data), g_list.max,
             hitActor ? names::Name(hitActor).c_str() : "nothing", (hitActor ? Dist(start, hitLoc) : range) / upm,
             wtHit ? names::Name(wtBy).c_str() : "nothing", wtHit && hitActor ? Dist(wt, hitLoc) : -1.0f, damaged, bones.c_str(),
             !credit ? "untouched (main)" : restored ? "the pistol for the damage, the gun again after" : "NOT restored",
             clip, ao >= 0 ? static_cast<int>(ReadU32(p + ao)) : -1, played ? "played" : "none");
    }
    return true;
}

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
        if (!Shot(pawn, gun, p, start, dir, upm, !main, repeat > 1)) break;
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

}  // namespace

void Configure(const Config& cfg) { g_cfg = cfg; }

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
            if (!ac || names::ClassName(ac) != "AudioComponent" || Obj(ac, "SoundCue") != g_cue || !g_cue) continue;
            const int wo = names::PropertyOffset(ac, "WaveInstances");
            ++found;
            MLOG("offpistol: sound -- %s plays %s: %d wave instance(s), %.2f s in", names::Name(ac).c_str(), names::Name(g_cue).c_str(),
                 wo >= 0 ? static_cast<int>(ReadU32(ac + wo + 4)) : -1, Float(ac, "PlaybackTime", -1.0f));
        }
        if (!found) MLOG("offpistol: sound -- no AudioComponent of WorldInfo plays %s", names::Name(g_cue).c_str());
    } else if (!std::wcscmp(line, L"mohavr pistol refill")) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const std::uintptr_t inv = Obj(pawn, "InvManager");
        const std::uintptr_t p = Pistol(pawn, inv, Obj(pawn, "Weapon"));
        const int ao = p ? names::PropertyOffset(p, "AmmoCount") : -1;
        if (ao >= 0) *reinterpret_cast<int*>(p + ao) = Int(p, "MaxAmmoCount", 7);
        MLOG("offpistol: refill -- %s's clip %d", names::Name(p).c_str(), Int(p, "AmmoCount", -1));
    } else if (!std::wcscmp(line, L"mohavr pistol upgrade")) {
        Upgrade();
    } else if (!std::wcscmp(line, L"mohavr pistol enemy")) {
        float eye[3], fwd[3], upm = 100.0f;
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        if (pawn && Head(bridge::SharedHeader(), eye, fwd, upm)) NearestEnemy(pawn, eye, upm, true);
    } else {
        Dump();
    }
    return true;
}

}  // namespace mohavr::offpistol
