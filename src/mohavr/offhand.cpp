#include "offhand.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "script_call.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::offhand {
using namespace script;  // Call, Obj, Bit, ... (script_call.hpp)
namespace {

Config g_cfg;
bool   g_bake = false;  // the arm bake is installed (it places the carrier in the off hand)

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// The reserve of a grenade weapon's ammo class in the pawn's inventory manager (MOHAInventoryManager.AmmoStorage[10]:
// AmmoClass, ReserveAmmoAmount, MaxAmmoCount), as reload.cpp's ReserveOf.
int* ReserveOf(std::uintptr_t inv, std::uintptr_t g, int& cap) {
    const std::uintptr_t cls = Obj(g, "AmmoClass");
    const int so = names::PropertyOffset(inv, "AmmoStorage"), no = names::PropertyOffset(inv, "NumAmmoClasses");
    if (!cls || so < 0 || no < 0) return nullptr;
    const int n = *reinterpret_cast<const int*>(inv + no);
    for (int i = 0; i < n && i < 10; ++i) {
        const std::uintptr_t e = inv + so + 12 * i;
        if (names::ReadPointer(e) != cls) continue;
        cap = *reinterpret_cast<const int*>(e + 8);
        return reinterpret_cast<int*>(e + 4);
    }
    return nullptr;
}

const char* const kTypeProp[3] = {"FragGrenadeWeapon", "GammonGrenadeWeapon", "StickGrenadeWeapon"};
const char* const kTypeName[3] = {"frag", "gammon", "stick"};
const char* const kTypeClass[3] = {"MOHA_MKIIFragGrenade", "MOHA_GammonGrenade", "MOHA_StickGrenade"};

// The grenade weapon of a type: the inventory manager's pointer, else the inventory chain (a type given later:
// SetupWeaponPointers only runs with the loadout).
std::uintptr_t GrenadeOf(std::uintptr_t pawn, std::uintptr_t inv, int type) {
    std::uintptr_t g = Obj(inv, kTypeProp[type]);
    if (g && Obj(g, "Instigator") == pawn) return g;
    for (std::uintptr_t item = Obj(inv, "InventoryChain"), n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (names::ClassName(item) == kTypeClass[type] && Obj(item, "Instigator") == pawn) return item;
    return 0;
}

// How many a TAKE can get: the reserve plus a grenade already in the weapon's clip (normally 0 while it isn't in the
// hand); 99 with infinite ammo; -1 not carried.
int CountOf(std::uintptr_t inv, std::uintptr_t g) {
    if (!g) return -1;
    if (Bit(g, "bInfiniteAmmo")) return 99;
    int cap = 0;
    const int* r = ReserveOf(inv, g, cap);
    const int clip = Int(g, "AmmoCount", 0);
    return (r ? *r : 0) + (clip > 0 ? clip : 0);
}

// One grenade out (none with infinite ammo): the reserve first, else the weapon's clip.
void CountOut(std::uintptr_t inv, std::uintptr_t g) {
    if (!g || Bit(g, "bInfiniteAmmo")) return;
    int cap = 0;
    if (int* r = ReserveOf(inv, g, cap); r && *r > 0) {
        --*r;
        MLOG("offhand: %s's reserve -> %d of %d", names::Name(g).c_str(), *r, cap);
        return;
    }
    const int ao = names::PropertyOffset(g, "AmmoCount");
    if (ao >= 0 && *reinterpret_cast<int*>(g + ao) > 0) {
        --*reinterpret_cast<int*>(g + ao);
        MLOG("offhand: %s's clip -> %d", names::Name(g).c_str(), *reinterpret_cast<int*>(g + ao));
    }
}

// The type "any" gives: the inventory manager's last grenade if it has some, else frag, Gammon, stick (SwitchGrenade's
// order); -1 none.
int NextType(std::uintptr_t pawn, std::uintptr_t inv) {
    const std::uintptr_t last = Obj(inv, "LastGrenadeWeapon");
    for (int t = 0; t < 3; ++t)
        if (last && GrenadeOf(pawn, inv, t) == last && CountOf(inv, last) > 0) return t;
    for (int t = 0; t < 3; ++t)
        if (CountOf(inv, GrenadeOf(pawn, inv, t)) > 0) return t;
    return -1;
}

// --- the thrown grenades, followed until they go off (or 10 s) ----------------------------------------------------
struct Thrown {
    std::uintptr_t proj = 0;
    DWORD          at = 0;
    float          start[3]{};
    bool           logged1s = false;
};
Thrown g_thrown[4];

void Follow(std::uintptr_t proj, const float (&start)[3]) {
    for (Thrown& t : g_thrown)
        if (!t.proj) {
            t = Thrown{proj, GetTickCount(), {start[0], start[1], start[2]}, false};
            return;
        }
}

// --- the launch: the inventory grenade weapon `g` puts its projectile at `start` with world velocity `vel` ----------
// EALAWeapon.SpawnProjectile (reads CurrentFireMode, WeaponProjectiles, the pool, Instigator and its Controller only),
// then what EALAGrenade.ProjectileFire does after its spawn. A pooled projectile whose move fails (it stays where it last
// went off) goes back to the pool and the launch is tried once more from `retryFrom` (the game's own start, the eye).
// The grenade is counted out only once it flies.
std::uintptr_t Launch(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t g, const float (&startIn)[3], const float (&vel)[3],
                      float fuse, const float* retryFrom) {
    float start[3] = {startIn[0], startIn[1], startIn[2]};
    const float speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    const float dir[3] = {speed > 1.0f ? vel[0] / speed : 1.0f, speed > 1.0f ? vel[1] / speed : 0.0f, speed > 1.0f ? vel[2] / speed : 0.0f};
    std::uintptr_t proj = 0;
    float loc[3] = {0, 0, 0};
    for (int attempt = 0; attempt < 2 && !proj; ++attempt) {
        // SpawnProjectile takes the projectile class of CurrentFireMode (it stays 2 after a grenade melee).
        const int fm = names::PropertyOffset(g, "CurrentFireMode");
        if (fm >= 0) *reinterpret_cast<std::uint8_t*>(g + fm) = 0;
        Call spawn(g, "SpawnProjectile");
        if (!spawn.Set("vStartPos", start, sizeof(start)) || !spawn.Set("vForward", dir, sizeof(dir)) || !spawn.Run()) return 0;
        const std::uintptr_t p = spawn.ReturnObject();
        if (!p || !names::IsA(p, "MOHAProj_Explosive")) {
            MLOG("offhand: SpawnProjectile gave %s (%s) -- no grenade", names::Name(p).c_str(), names::ClassName(p).c_str());
            return 0;
        }
        names::ReadVector(p + addr::kActorLocation, loc);
        if (Dist(loc, start) <= 2.0f) {
            proj = p;
            break;
        }
        MLOG("offhand: %s is %.0f units from where it should start (a pooled one that didn't move) -- back to the pool%s",
             names::Name(p).c_str(), Dist(loc, start), attempt == 0 && retryFrom ? ", again from the eye" : "");
        Call recycle(p, "RecycleProjectile");
        recycle.Run();
        if (!retryFrom) return 0;
        for (int i = 0; i < 3; ++i) start[i] = retryFrom[i];
    }
    if (!proj) return 0;
    const float scaleBefore = Float(proj, "DrawScale", -1.0f);
    // What EALAGrenade.ProjectileFire does after its spawn: the draw scale, the explosion light, the cooked damage type
    // (every player throw is cooked: the pin pull starts the fuse), the fuse, the velocity. And: a pooled projectile's last
    // life's impact is cleared; it ignores the thrower ([OffHand] PassThrower).
    if (names::PropertyOffset(proj, "ImpactedActor") >= 0) SetObj(proj, "ImpactedActor", 0);
    if (g_cfg.offHandPassThrower) SetBit(proj, "bBlockedByInstigator", false);
    Call scale(proj, "SetDrawScale");
    const float s = Float(g, "ExplosiveDrawScale", 1.5f);
    if (scale.Set("NewScale", &s, sizeof(s))) scale.Run();
    if (Bit(g, "bUseExplosionLight")) {
        Call light(proj, "CreateLight");
        const int co = names::PropertyOffset(g, "LightColor");
        std::uint32_t color = co >= 0 ? ReadU32(g + co) : 0xFFFFFFFFu;
        const float size = Float(g, "LightRadius", 0), life = Float(g, "LightLifespan", 0), bright = Float(g, "LightBrightness", 0),
                    fall = Float(g, "LightFalloffSpeed", 0), expo = Float(g, "LightFalloffExponent", 0), delay = Float(g, "LightCreationDelay", 0);
        if (light.Set("LightColor", &color, 4) && light.Set("LightSize", &size, 4) && light.Set("LightLife", &life, 4) &&
            light.Set("LightBrightness", &bright, 4) && light.Set("FalloffSpeed", &fall, 4) &&
            light.Set("LightFalloffExponent", &expo, 4) && light.Set("LightDelay", &delay, 4))
            light.Run();
    }
    const int cdo = names::PropertyOffset(proj, "MyCookedDamageType"), mdo = names::PropertyOffset(proj, "MyDamageType");
    if (cdo >= 0 && mdo >= 0 && names::ReadPointer(proj + cdo))
        *reinterpret_cast<std::uintptr_t*>(proj + mdo) = names::ReadPointer(proj + cdo);
    Call timer(proj, "SetFuseTime");
    if (timer.Set("newFuseTime", &fuse, sizeof(fuse))) timer.Run();
    const int vo = names::PropertyOffset(proj, "Velocity");
    if (vo >= 0) std::memcpy(reinterpret_cast<void*>(proj + vo), vel, sizeof(vel));
    MLOG("offhand: %s launched %s (%s): owner %s, instigator %s, controller %s, %.1f units from the start; DrawScale %.2f -> %.2f, "
         "damage %s, fuse %.2f s, velocity %.0f %.0f %.0f", names::Name(g).c_str(), names::Name(proj).c_str(), names::ClassName(proj).c_str(),
         names::Name(Obj(proj, "Owner")).c_str(), Obj(proj, "Instigator") == pawn ? "the pawn" : names::Name(Obj(proj, "Instigator")).c_str(),
         names::Name(Obj(proj, "InstigatorController")).c_str(), Dist(loc, start), scaleBefore, Float(proj, "DrawScale", -1.0f),
         names::Name(Obj(proj, "MyDamageType")).c_str(), fuse, vel[0], vel[1], vel[2]);
    CountOut(inv, g);
    Follow(proj, start);
    return proj;
}

// --- the carrier: the grenade drawn in the off hand while it is held ------------------------------------------------
// A clone of the grenade weapon's first-person pickup mesh (DroppedPickupMesh, the same skeletal mesh as its attachment;
// the game never attaches it while the grenade is in the inventory), attached to the arms at their Camera bone and baked
// at the off hand by the arms' bake (arms_ik.cpp; collapsed -- nothing drawn -- when there is no off-hand frame).
struct Carrier {
    std::uintptr_t comp = 0, arms = 0, pawn = 0;
} g_carrier;

template <class T>
void CopyField(std::uintptr_t to, std::uintptr_t from, const char* name) {
    const int o = names::PropertyOffset(from, name), p = names::PropertyOffset(to, name);
    if (o >= 0 && p >= 0) std::memcpy(reinterpret_cast<void*>(to + p), reinterpret_cast<const void*>(from + o), sizeof(T));
}

void CarrierDetach() {
    if (!g_carrier.comp) return;
    if (g_carrier.arms && g_carrier.pawn == aim::LocalPlayerPawn()) {
        Call detach(g_carrier.arms, "DetachComponent");
        if (detach.Set("Component", &g_carrier.comp, sizeof(g_carrier.comp)) && detach.Run() && g_cfg.debugOffHandTrace)
            MLOG("offhand: carrier %s detached", names::Name(g_carrier.comp).c_str());
    }
    g_carrier = Carrier{};
}

bool CarrierAttach(std::uintptr_t pawn, std::uintptr_t g) {
    CarrierDetach();
    const std::uintptr_t arms = Obj(pawn, "FPArms"), tmpl = Obj(g, "DroppedPickupMesh");
    if (!arms || !tmpl) {
        MLOG("offhand: carrier -- arms %s, pickup mesh %s: none (nothing drawn in the hand)", names::Name(arms).c_str(),
             names::Name(tmpl).c_str());
        return false;
    }
    Call clone(tmpl, "Clone");
    if (!clone.Set("InOuter", &g, sizeof(g)) || !clone.Run()) return false;
    const std::uintptr_t c = clone.ReturnObject();
    if (!c || !names::IsA(c, "MOHASkeletalMeshComponent")) {
        MLOG("offhand: carrier -- Clone gave %s (%s)", names::Name(c).c_str(), names::ClassName(c).c_str());
        return false;
    }
    // Drawn as the arms are: their depth group, light environment, LOD and first-person FOV (the proxy hook then draws it
    // in true 3D like them, through the mirror in left-hand mode); no collision, their shadow settings.
    CopyField<std::uint8_t>(c, arms, "DepthPriorityGroup");
    CopyField<std::uintptr_t>(c, arms, "LightEnvironment");
    CopyField<int>(c, arms, "ForcedLodModel");
    CopyField<int>(c, arms, "iMinLODLevel");
    std::memcpy(reinterpret_cast<void*>(c + addr::kMohaSkelMeshFov), reinterpret_cast<const void*>(arms + addr::kMohaSkelMeshFov), 4);
    const int bo = names::PropertyOffset(c, "fCustomBoundsSize");
    if (bo >= 0) *reinterpret_cast<float*>(c + bo) = 200.0f;
    for (const char* b : {"CollideActors", "BlockActors", "BlockZeroExtent", "BlockNonZeroExtent", "BlockRigidBody"}) SetBit(c, b, false);
    SetBit(c, "CastShadow", Bit(arms, "CastShadow"));
    SetBit(c, "bCastDynamicShadow", Bit(arms, "bCastDynamicShadow"));
    // The arms' 'Camera' bone's FName (8 bytes) from their skeleton.
    const std::uintptr_t mesh = Obj(arms, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    std::uint8_t bone[8] = {};
    bool found = false;
    for (int i = 0; i < num && i < 512 && data && !found; ++i)
        if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) == "Camera") {
            std::memcpy(bone, reinterpret_cast<const void*>(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride), 8);
            found = true;
        }
    Call attach(arms, "AttachComponent");
    const float one[3] = {1.0f, 1.0f, 1.0f};  // RelativeScale: no default through ProcessEvent
    if (!found || !attach.Set("Component", &c, sizeof(c)) || !attach.Set("BoneName", bone, 8) ||
        !attach.Set("RelativeScale", one, sizeof(one)) || !attach.Run()) {
        MLOG("offhand: carrier -- not attached (the Camera bone %s)", found ? "found" : "missing");
        return false;
    }
    g_carrier = Carrier{c, arms, pawn};
    if (!Bit(c, "bAttached")) MLOG("offhand: carrier %s -- AttachComponent left it unattached", names::Name(c).c_str());
    else if (g_cfg.debugOffHandTrace)
        MLOG("offhand: carrier %s (mesh %s) attached to the arms' Camera bone", names::Name(c).c_str(),
             names::Name(Obj(c, "SkeletalMesh")).c_str());
    return true;
}

// --- the held grenade (OFFHAND-DESIGN 6.2) ----------------------------------------------------------------------------
enum State { kNone = 0, kHeld = 1, kArmed = 2, kCooking = 3 };
const char* const kStateName[4] = {"none", "held", "armed", "cooking"};
const char* const kEventName[6] = {"?", "TAKE", "PIN", "COOK", "THROW", "PUT BACK"};
enum Refusal : std::uint32_t { kOk = 0, kUnavailable = 1, kEmpty = 2, kNoWeapon = 3, kSpawnFailed = 4 };

struct Hold {
    int            state = kNone, type = -1;
    std::uintptr_t g = 0;
    float          fuseLen = 4.0f, cookStart = 0.0f, nextTick = 0.0f;
} g_hold;
std::uintptr_t g_pawn = 0;
std::uint32_t  g_seen = 0, g_pawnSeq = 0, g_boom = 0, g_ticks = 0, g_refusal = kOk;
bool           g_seenInit = false;
float          g_hand[3]{};       // the off hand's hand point in the world, last known (a grenade going off in the hand)
bool           g_haveHand = false;
// Availability: the cheap tests every Draw, the script ones (IsWeaponDisabled, the controller's state) at 4 Hz.
DWORD          g_slowAt = 0;
bool           g_slowDone = false;  // the first pass runs at once (GetTickCount can be past 2^31: 24.9-49.7 days up)
bool           g_slowOk = false;
std::uint32_t  g_hostFlags = 0;     // the host's nadeFlags, last read whole (bit0 the switch, bit1 one held)
const char*    g_slowWhy = "";
std::string    g_lastWhy;

float GameTime(std::uintptr_t pawn) { return Float(Obj(pawn, "WorldInfo"), "TimeSeconds", 0.0f); }

// Whether a TAKE can happen now; when not, `holdOk` says whether a grenade already held may stay (only a switch to another
// gun is under way: the gun hand may change guns while the off hand holds one).
bool Available(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, const char*& why, bool& holdOk) {
    why = "";
    holdOk = false;
    if (!pawn || !inv || !names::IsA(pawn, "MOHAPlayerPawn") || Bit(pawn, "bDeleteMe") || Int(pawn, "Health", 0) <= 0)
        why = "no live player pawn";
    else if (!gun || !names::IsA(gun, "EALAWeapon"))
        why = "no weapon in hand";
    else if (names::IsA(gun, "EALAGrenade"))
        why = "a grenade is the weapon in hand";
    else if ((Int(gun, "WeaponType", 0) & 0xFF) == 23)
        why = "the HellBox in hand";
    else if (viewmodel::NoGunDrawn() || view::LandingHeld())
        why = "the gun isn't drawn (the parachute, the landing)";
    else if (Bit(Obj(pawn, "Controller"), "bCinematicMode"))
        why = "a cinematic";
    else if (Bit(pawn, "bNoWeaponFiring"))
        why = "weapons held by the game";
    if (*why) return false;
    const DWORD now = GetTickCount();
    if (!g_slowDone || static_cast<LONG>(now - g_slowAt) >= 0) {
        g_slowDone = true;
        g_slowAt = now + 250;
        g_slowOk = true;
        g_slowWhy = "";
        // EALAWeapon.IsWeaponDisabled -> MOHAPlayerPawn.IsWeaponDisabled (a native without an index): the ladders, the
        // airdrop and ITP states, the briefing, dying...
        Call disabled(gun, "IsWeaponDisabled", true);
        if (disabled.ok && disabled.Run() && disabled.ReturnBool()) {
            g_slowOk = false;
            g_slowWhy = "the game has the weapons disabled";
        }
        // A mounted gun (the controller's PlayerMountedMG / PlayerUsingMG states: no VR handling yet, D33).
        const std::string st = names::StateName(Obj(pawn, "Controller"));
        if (st.find("MG") != std::string::npos) {
            g_slowOk = false;
            g_slowWhy = "a mounted gun";
        }
    }
    if (!g_slowOk) {
        why = g_slowWhy;
        return false;
    }
    // A weapon switch: no take now; one held stays unless the switch brings a grenade (or the HellBox) to the hand.
    if (const std::uintptr_t pending = Obj(inv, "PendingWeapon")) {
        why = "a weapon switch";
        holdOk = !names::IsA(pending, "EALAGrenade") && (Int(pending, "WeaponType", 0) & 0xFF) != 23;
        return false;
    }
    holdOk = true;
    return true;
}

void Refuse(std::uint32_t type, std::uint32_t code, const char* why) {
    g_refusal = code;
    MLOG("offhand: %s refused -- %s", type < 6 ? kEventName[type] : "?", why);
}

void SetState(int state, const char* why) {
    if (state == g_hold.state) return;
    if (g_cfg.debugOffHandTrace || state == kNone)
        MLOG("offhand: %s -> %s (%s)", kStateName[g_hold.state], kStateName[state], why);
    g_hold.state = state;
    if (state == kNone) {
        CarrierDetach();
        g_hold = Hold{};
    }
}

// The held grenade's weapon is still the pawn's -- not destroyed with its inventory (a death, a Kismet loadout: its
// Instigator survives that) -- and one is left to count out (a rifle grenade takes from the same reserve); else the hold
// ends without touching it.
bool HeldWeaponOk(std::uintptr_t pawn, std::uintptr_t inv, std::uint32_t event) {
    const std::uintptr_t g = g_hold.g;
    std::uint32_t code = kOk;
    const char* why = "";
    if (!g || g_hold.type < 0 || GrenadeOf(pawn, inv, g_hold.type) != g || Bit(g, "bDeleteMe")) {
        code = kNoWeapon;
        why = "the grenade weapon is gone";
    } else if (CountOf(inv, g) <= 0) {
        code = kEmpty;
        why = "none left to count out (a rifle grenade took it)";
    }
    if (code == kOk) return true;
    Refuse(event, code, why);
    SetState(kNone, why);
    return false;
}

// The head in the world (its forward; units per metre), from the host's views.
bool HeadWorld(shared::Header* hdr, float (&headW)[3], float (&headF)[3], float& upm) {
    shared::Pose head{}, eye[2]{};
    shared::Fov fov[2]{};
    return hdr && shared::ReadViews(hdr, head, eye, fov) && view::PoseToWorld(head, headW, headF, upm);
}

// A launch point pulled back toward the head when something stands between them (a hand through a wall).
void PullBack(std::uintptr_t pawn, const float (&headW)[3], float (&start)[3], const char* what) {
    float hit[3];
    std::uintptr_t by = 0;
    if (!aim::WorldTrace(pawn, headW, start, hit, &by)) return;
    float d[3] = {start[0] - headW[0], start[1] - headW[1], start[2] - headW[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) start[i] = hit[i] - (len > 1.0f ? d[i] / len * 5.0f : 0.0f);
    MLOG("offhand: the %s point is behind %s (blocked) -- started on the player's side", what, names::Name(by).c_str());
}

// The throw (OFFHAND-DESIGN 3.5): from the release point (LOCAL), with the release velocity (LOCAL) or tossed.
void ThrowHeld(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, const float (&posL)[3], const float (&velL)[3], bool toss,
               const char* what) {
    const float now = GameTime(pawn);
    const float fuse = g_hold.state == kCooking ? std::fmax(g_hold.fuseLen - (now - g_hold.cookStart), 0.05f) : g_hold.fuseLen;
    if (!HeldWeaponOk(pawn, inv, shared::kNadeThrow)) return;
    const std::uintptr_t g = g_hold.g;
    // The start: the release point, pulled back toward the head if something stands between them (a hand through a wall).
    float headW[3] = {0, 0, 0}, headF[3] = {1, 0, 0}, upm = 100.0f;
    const bool haveHead = HeadWorld(hdr, headW, headF, upm);
    float start[3], fwd[3];
    const shared::Pose p{posL[0], posL[1], posL[2], 0.0f, 0.0f, 0.0f, 1.0f};
    if (!view::PoseToWorld(p, start, fwd, upm)) {
        Refuse(shared::kNadeThrow, kUnavailable, "no view mapping");
        return;
    }
    if (haveHead) PullBack(pawn, headW, start, "release");
    // The velocity: the hand's (x [OffHand] ThrowScale), or tossed along the view pitched up by the weapon's
    // DirectionOffset at its ExplosiveSpeed (the game's gentlest throw); plus the pawn's own (x ExplosivePawnVelocityScale).
    float v[3];
    bool handVel = !toss;
    if (toss) {
        const int doff = names::PropertyOffset(g, "DirectionOffset");
        const float up = (doff >= 0 ? static_cast<float>(static_cast<std::int32_t>(ReadU32(g + doff))) : 2100.0f) * (3.14159265f / 32768.0f);
        const float h = std::sqrt(headF[0] * headF[0] + headF[1] * headF[1]);
        // The view's pitch, never below the horizon (looking down at the hand would put it at the feet) nor above 45 deg.
        const float el = std::fmin(std::fmax(std::atan2(headF[2], h), 0.0f), 0.785f) + up;
        const float hx = h > 1e-4f ? headF[0] / h : 1.0f, hy = h > 1e-4f ? headF[1] / h : 0.0f;
        const float sp = Float(g, "ExplosiveSpeed", 750.0f);
        // Cover close ahead would bounce it back (OFFHAND-DESIGN 0.2): its first 2 m are traced; blocked, it goes 30
        // degrees higher; blocked again, it leaves with the hand's own velocity (a slow release's; a forced toss -- the
        // menu, a freeze, unavailable -- has none: it drops where the hand is).
        handVel = true;
        float blockedAt = -1.0f;
        std::uintptr_t blockedBy = 0;
        for (int k = 0; k < 2 && handVel; ++k) {
            const float e = std::fmin(el + 0.52f * k, 1.55f);
            const float d[3] = {hx * std::cos(e), hy * std::cos(e), std::sin(e)};
            const float end[3] = {start[0] + d[0] * 2.0f * upm, start[1] + d[1] * 2.0f * upm, start[2] + d[2] * 2.0f * upm};
            float hit[3];
            std::uintptr_t by = 0;
            if (aim::WorldTrace(pawn, start, end, hit, &by)) {
                if (blockedAt < 0.0f) {
                    blockedAt = Dist(hit, start) * 100.0f / upm;
                    blockedBy = by;
                }
                continue;
            }
            for (int i = 0; i < 3; ++i) v[i] = d[i] * sp;
            handVel = false;
            if (k) MLOG("offhand: the toss is blocked %.0f cm ahead (%s) -- lobbed 30 degrees higher", blockedAt, names::Name(blockedBy).c_str());
        }
        if (handVel)
            MLOG("offhand: the toss is blocked %.0f cm ahead (%s) and higher -- it leaves with the hand's own velocity", blockedAt,
                 names::Name(blockedBy).c_str());
    }
    if (handVel) {
        float ue[3];
        if (!view::VectorToWorld(velL, ue)) {
            Refuse(shared::kNadeThrow, kUnavailable, "no view mapping for the velocity");
            return;
        }
        for (int i = 0; i < 3; ++i) v[i] = ue[i] * g_cfg.offHandThrowScale;
    }
    float pawnVel[3] = {0, 0, 0};
    const int pvo = names::PropertyOffset(pawn, "Velocity");
    if (pvo >= 0) names::ReadVector(pawn + pvo, pawnVel);
    const float carry = Float(g, "ExplosivePawnVelocityScale", 0.25f);
    for (int i = 0; i < 3; ++i) v[i] += pawnVel[i] * carry;
    float eyeW[3], pitch = 0.0f, yaw = 0.0f;
    const bool haveEye = view::GameCamera(eyeW, pitch, yaw);
    const int fo = names::PropertyOffset(pawn, "FlashCount");
    const std::uint32_t flashBefore = fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0;
    const std::uintptr_t gun = Obj(pawn, "Weapon");
    MLOG("offhand: %s %s %s at %.1f m/s (%s), fuse %.2f s", what, kTypeName[g_hold.type], toss ? "tossed" : "thrown",
         std::sqrt(velL[0] * velL[0] + velL[1] * velL[1] + velL[2] * velL[2]), toss ? "along the view" : "the hand's", fuse);
    const std::uintptr_t proj = Launch(pawn, inv, g, start, v, fuse, haveEye ? eyeW : nullptr);
    const std::uintptr_t gunAfter = Obj(pawn, "Weapon");
    const std::uint32_t flashAfter = fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0;
    if (gunAfter != gun || flashAfter != flashBefore)
        MLOG("offhand: after the throw -- the weapon in hand %s -> %s, FlashCount %u -> %u (expected unchanged)", names::Name(gun).c_str(),
             names::Name(gunAfter).c_str(), flashBefore, flashAfter);
    if (!proj) Refuse(shared::kNadeThrow, kSpawnFailed, "no projectile (the grenade stays counted)");
    SetState(kNone, proj ? "thrown" : "the launch failed");
}

// The fuse ran out in the hand: the game's own over-cook, at the hand (a launch with no velocity of its own and a fuse
// that ends at once).
void Boom(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv) {
    if (!HeldWeaponOk(pawn, inv, shared::kNadeCook)) return;
    float eyeW[3], pitch = 0.0f, yaw = 0.0f;
    const bool haveEye = view::GameCamera(eyeW, pitch, yaw);
    float start[3];
    if (g_haveHand) std::memcpy(start, g_hand, sizeof(start));
    else if (haveEye) std::memcpy(start, eyeW, sizeof(start));
    else {
        SetState(kNone, "went off: no hand, no view");
        return;
    }
    float headW[3], headF[3], upm = 100.0f;
    if (g_haveHand && HeadWorld(hdr, headW, headF, upm)) PullBack(pawn, headW, start, "hand");
    float pawnVel[3] = {0, 0, 0};
    const int pvo = names::PropertyOffset(pawn, "Velocity");
    if (pvo >= 0) names::ReadVector(pawn + pvo, pawnVel);
    MLOG("offhand: the %s went off in the hand (cooked %.1f s)", kTypeName[g_hold.type], g_hold.fuseLen);
    const std::uintptr_t proj = Launch(pawn, inv, g_hold.g, start, pawnVel, 0.05f, haveEye ? eyeW : nullptr);
    if (proj) ++g_boom;
    else Refuse(shared::kNadeCook, kSpawnFailed, "the over-cooked grenade could not be launched (it stays counted)");
    SetState(kNone, proj ? "went off in the hand" : "the over-cook's launch failed");
}

void Apply(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, std::uint32_t e, const float (&pos)[3], const float (&vel)[3],
           bool avail, const char* why) {
    const std::uint32_t type = e & 0xFFu, want = (e >> 8) & 0xFFu;
    const bool toss = (e & shared::kNadeToss) != 0;
    switch (type) {
    case shared::kNadeTake: {
        if (g_hold.state != kNone) return Refuse(type, kUnavailable, "a grenade is already held");
        if (!avail) return Refuse(type, kUnavailable, why);
        const int t = want == shared::kNadeAny ? NextType(pawn, inv) : (want < 3 ? static_cast<int>(want) : -1);
        const std::uintptr_t g = t >= 0 ? GrenadeOf(pawn, inv, t) : 0;
        if (!g) return Refuse(type, kNoWeapon, t < 0 ? "no grenades" : "that grenade isn't carried");
        const int count = CountOf(inv, g);
        if (count <= 0) return Refuse(type, kEmpty, "none of that type left");
        g_hold = Hold{kHeld, t, g, Float(g, "FuseTime", 4.0f), 0.0f, 0.0f};
        g_refusal = kOk;
        // [OffHand] HudType: the HUD's grenade count shows the held type (GetHUDGrenade reads LastGrenadeWeapon), and the
        // game's own grenade switch later takes the type last used -- what its equip does.
        if (g_cfg.offHandHudType) SetObj(inv, "LastGrenadeWeapon", g);
        const bool drawn = g_cfg.offHandCarrier && g_bake && CarrierAttach(pawn, g);
        MLOG("offhand: TAKE %s -- %d left (counted out at the throw), fuse %.1f s%s", kTypeName[t], count, g_hold.fuseLen,
             drawn ? ", drawn in the hand" : "");
        return;
    }
    case shared::kNadePin:
        if (g_hold.state != kHeld) return Refuse(type, kUnavailable, "nothing held with the pin in");
        SetState(kArmed, "the pin pulled");
        MLOG("offhand: PIN (%s)", kTypeName[g_hold.type]);
        return;
    case shared::kNadeCook:
        if (g_hold.state != kArmed) return Refuse(type, kUnavailable, "no armed grenade");
        g_hold.cookStart = GameTime(pawn);
        g_hold.nextTick = g_hold.cookStart + 0.5f;
        SetState(kCooking, "the spoon let go");
        MLOG("offhand: COOK (%s) -- the fuse burns: %.1f s", kTypeName[g_hold.type], g_hold.fuseLen);
        return;
    case shared::kNadeThrow:
        if (g_hold.state != kArmed && g_hold.state != kCooking) return Refuse(type, kUnavailable, "no armed grenade");
        return ThrowHeld(hdr, pawn, inv, pos, vel, toss, "THROW");
    case shared::kNadePutBack:
        if (g_hold.state == kCooking) {  // a live grenade is never silently removed: tossed from where the hand let go
            const float zero[3] = {0, 0, 0};
            return ThrowHeld(hdr, pawn, inv, pos, zero, true, "PUT BACK while cooking:");
        }
        if (g_hold.state == kNone) return Refuse(type, kUnavailable, "nothing held");
        MLOG("offhand: PUT BACK (%s) -- nothing used", kTypeName[g_hold.type]);
        SetState(kNone, "put back");
        return;
    default:
        return Refuse(type, kUnavailable, "unknown event");
    }
}

void Publish(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t inv, bool avail, bool holdOk, bool off) {
    // bit0: the throw can be done here (SpawnProjectile callable on a carried grenade: resolved once per weapon). Switched
    // off with nothing held: nothing is walked or resolved (rule 7) -- the status still goes out (the host sees us alive).
    bool installed = false, infinite = false;
    std::int32_t count[3] = {-1, -1, -1};
    static std::uintptr_t resolvedFor[3] = {};
    static bool resolvedOk[3] = {};
    for (int t = 0; t < 3 && !off; ++t) {
        const std::uintptr_t g = pawn ? GrenadeOf(pawn, inv, t) : 0;
        count[t] = g ? CountOf(inv, g) : -1;
        if (g && g != resolvedFor[t]) {
            resolvedFor[t] = g;
            resolvedOk[t] = Call(g, "SpawnProjectile", true).ok;
        }
        if (g && resolvedOk[t]) installed = true;
        if (g && Bit(g, "bInfiniteAmmo")) infinite = true;
    }
    const int next = pawn && !off ? NextType(pawn, inv) : -1;
    const float now = GameTime(pawn);
    const float fuse = g_hold.state == kCooking ? std::fmax(g_hold.fuseLen - (now - g_hold.cookStart), 0.0f) : 0.0f;
    ++hdr->nadeSeq;  // odd: writing
    _ReadWriteBarrier();
    hdr->nadeCaps = (installed ? 1u : 0u) | (installed && avail ? 2u : 0u) | (infinite ? 4u : 0u) |
                    (g_cfg.offHandCarrier && g_bake && ObjectProcessEventOk() ? 8u : 0u) | (installed && holdOk ? 16u : 0u);
    for (int t = 0; t < 3; ++t) hdr->nadeCount[t] = count[t];
    hdr->nadeNext = next >= 0 ? static_cast<std::uint32_t>(next) : shared::kNadeAny;
    hdr->nadeState = static_cast<std::uint32_t>(g_hold.state) | (g_hold.type >= 0 ? static_cast<std::uint32_t>(g_hold.type) << 2 : 0u) |
                     (g_refusal << 8);
    hdr->nadeFuse = fuse;
    hdr->nadeFuseLen = g_hold.state != kNone ? g_hold.fuseLen : 0.0f;
    hdr->nadeTicks = g_ticks;
    hdr->nadeEvtAck = g_seen;
    hdr->nadePawnSeq = g_pawnSeq;
    hdr->nadeBoom = g_boom;
    _ReadWriteBarrier();
    ++hdr->nadeSeq;  // even: done
}

// --- the spike's test commands (Debug.GameCommands) -------------------------------------------------------------------
void Dump() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const int fo = pawn ? names::PropertyOffset(pawn, "FlashCount") : -1;
    const int po = inv ? names::PropertyOffset(inv, "PendingFire") : -1;
    const std::uintptr_t pf = po >= 0 ? names::ReadPointer(inv + po) : 0;
    const int pfn = po >= 0 ? static_cast<int>(ReadU32(inv + po + 4)) : 0;
    MLOG("offhand: pawn %s, in hand %s (%s, AmmoCount %d), FlashCount %u, PendingFire %d entries [%d %d]; held %s",
         names::Name(pawn).c_str(), names::Name(gun).c_str(), names::ClassName(gun).c_str(), Int(gun, "AmmoCount", -1),
         fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0u, pfn,
         pfn > 0 ? static_cast<int>(ReadU32(pf)) : -1, pfn > 1 ? static_cast<int>(ReadU32(pf + 4)) : -1, kStateName[g_hold.state]);
    for (int t = 0; t < 3; ++t) {
        const std::uintptr_t g = GrenadeOf(pawn, inv, t);
        if (!g) {
            MLOG("offhand: %s -- none in the inventory", kTypeName[t]);
            continue;
        }
        int cap = 0;
        const int* r = ReserveOf(inv, g, cap);
        MLOG("offhand: %s -- %s (%s), the inventory manager's %s; CurrentFireMode %u, AmmoCount[0] %d, reserve %d of %d%s, "
             "bStasis %d, FuseTime %.2f, ExplosiveSpeed %.0f, DrawScale %.2f, pawn velocity x%.2f",
             kTypeName[t], names::Name(g).c_str(), names::ClassName(g).c_str(), Obj(inv, kTypeProp[t]) == g ? "pointer" : "chain",
             static_cast<unsigned>(Int(g, "CurrentFireMode", 255) & 0xFF), Int(g, "AmmoCount", -1), r ? *r : -1, cap,
             Bit(g, "bInfiniteAmmo") ? " (infinite)" : "", Bit(g, "bStasis") ? 1 : 0, Float(g, "FuseTime", -1.0f),
             Float(g, "ExplosiveSpeed", -1.0f), Float(g, "ExplosiveDrawScale", -1.0f), Float(g, "ExplosivePawnVelocityScale", -1.0f));
    }
}

// "mohavr nade throw <frag|gammon|stick|any> <hand|eye> <vx> <vy> <vz> [fuse]": a launch without the host (the spike).
void TestThrow(const wchar_t* typeW, const wchar_t* fromW, const float (&xr)[3], float fuse) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager");
    if (!pawn || !inv) return;
    const int want = !wcscmp(typeW, L"frag") ? 0 : !wcscmp(typeW, L"gammon") ? 1 : !wcscmp(typeW, L"stick") ? 2 : -1;
    const int t = want >= 0 ? want : NextType(pawn, inv);
    const std::uintptr_t g = t >= 0 ? GrenadeOf(pawn, inv, t) : 0;
    if (!g || CountOf(inv, g) <= 0) {
        MLOG("offhand: test throw -- no %ls grenade", typeW);
        return;
    }
    float start[3] = {0, 0, 0}, pitch = 0.0f, yaw = 0.0f;
    bool fromHand = false;
    if (!wcscmp(fromW, L"hand")) {
        float gunF[16], off[16];
        bool offValid = false, two = false;
        if (viewmodel::HandFrames(gunF, off, offValid, two) && offValid) {
            float r[16];
            std::memcpy(start, off + 12, sizeof(start));
            if (viewmodel::DrawMirror(r))
                for (int j = 0; j < 3; ++j) start[j] = off[12] * r[0 + j] + off[13] * r[4 + j] + off[14] * r[8 + j] + r[12 + j];
            fromHand = true;
        }
    }
    if (!fromHand && !view::GameCamera(start, pitch, yaw)) return;
    float ue[3];
    if (!view::VectorToWorld(xr, ue)) return;
    const float vel[3] = {ue[0] * g_cfg.offHandThrowScale, ue[1] * g_cfg.offHandThrowScale, ue[2] * g_cfg.offHandThrowScale};
    MLOG("offhand: test throw %s from the %s", kTypeName[t], fromHand ? "off hand" : "eye");
    Launch(pawn, inv, g, start, vel, fuse > 0.0f ? fuse : Float(g, "FuseTime", 4.0f), nullptr);
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
    if (cfg.offHandCarrier && !bake) MLOG("offhand: no arm bake (Weapon.ArmIK, ViewModel=2) -- the grenade isn't drawn in the hand");
}

bool TestCommand(const wchar_t* line) {
    if (wcsncmp(line, L"mohavr nade", 11) != 0) return false;
    wchar_t type[16] = L"", from[16] = L"", what[16] = L"";
    float xr[3] = {0, 0, 0}, fuse = 0.0f;
    const int n = swscanf_s(line, L"mohavr nade throw %15ls %15ls %f %f %f %f", type, static_cast<unsigned>(16), from,
                            static_cast<unsigned>(16), &xr[0], &xr[1], &xr[2], &fuse);
    if (n >= 5) {
        TestThrow(type, from, xr, n >= 6 ? fuse : 0.0f);
    } else if (swscanf_s(line, L"mohavr nade carrier %15ls", what, static_cast<unsigned>(16)) == 1) {
        if (!wcscmp(what, L"off")) {
            CarrierDetach();
        } else {
            const std::uintptr_t pawn = aim::LocalPlayerPawn();
            const std::uintptr_t inv = Obj(pawn, "InvManager");
            const int t = !wcscmp(what, L"gammon") ? 1 : !wcscmp(what, L"stick") ? 2 : 0;
            if (const std::uintptr_t g = GrenadeOf(pawn, inv, t)) CarrierAttach(pawn, g);
        }
    } else {
        Dump();
    }
    return true;
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const DWORD tick = GetTickCount();
    // The thrown grenades: gone off (the projectile disabled -- Explode, RecycleProjectile -- or being destroyed when its
    // class's pool is full), else logged once a second later.
    for (Thrown& t : g_thrown) {
        if (!t.proj) continue;
        const float secs = (tick - t.at) / 1000.0f;
        if (!Bit(t.proj, "bEnabled") || Bit(t.proj, "bDeleteMe")) {
            MLOG("offhand: %s went off %.2f s after the throw", names::Name(t.proj).c_str(), secs);
            t.proj = 0;
            continue;
        }
        if (!t.logged1s && secs >= 1.0f) {
            t.logged1s = true;
            float loc[3], vel[3];
            const int vo = names::PropertyOffset(t.proj, "Velocity");
            if (names::ReadVector(t.proj + addr::kActorLocation, loc) && vo >= 0 && names::ReadVector(t.proj + vo, vel))
                MLOG("offhand: 1 s later %s is %.0f units from the start (%.0f %.0f %.0f), velocity %.0f %.0f %.0f",
                     names::Name(t.proj).c_str(), Dist(loc, t.start), loc[0], loc[1], loc[2], vel[0], vel[1], vel[2]);
        }
        if (secs > 10.0f) t.proj = 0;
    }
    if (!hdr) return;
    // A new local pawn (death, a level load): what was held is gone, as the game's own would be.
    if (pawn != g_pawn) {
        if (g_hold.state != kNone) MLOG("offhand: a new pawn -- the held %s is gone", kTypeName[g_hold.type]);
        g_carrier = Carrier{};  // (attached to the old pawn's arms)
        g_hold = Hold{};
        g_pawn = pawn;
        ++g_pawnSeq;
    }
    // The host's side (its switch; the off hand's hand point now, where a grenade going off in the hand starts -- kept
    // while one is held even if the switch was turned off meanwhile).
    shared::Pose hand[2];
    std::uint32_t valid = 0;
    shared::NadeView nv{};
    if (shared::ReadHands(hdr, hand, valid, nullptr, &nv)) {
        g_hostFlags = nv.flags;
        if (nv.flags & 3u) {
            float fwd[3], upm = 100.0f;
            g_haveHand = view::PoseToWorld(nv.pose, g_hand, fwd, upm);
        }
    }
    // Switched off with nothing held: no script calls (rule 7).
    const bool off = !(g_hostFlags & 1u) && g_hold.state == kNone;
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const char* why = !pawn ? "no pawn" : off ? "switched off" : "";
    bool holdOk = false;
    const bool avail = pawn && !off && Available(pawn, inv, gun, why, holdOk);
    if (g_cfg.debugOffHandTrace && g_lastWhy != why) {
        MLOG("offhand: %s%s", *why ? "not available -- " : "available", why);
        g_lastWhy = why;
    }
    // The host's events, in order (OFFHAND-DESIGN 6.3; the reload's ring rules).
    const std::uint32_t seq = hdr->nadeEvtSeq;
    if (!g_seenInit) {
        g_seen = seq;
        g_seenInit = true;
    }
    if (seq - g_seen > 8) {
        MLOG("offhand: %u events overran the ring -- skipped", seq - g_seen);
        g_seen = seq;
    }
    for (int n = 0; g_seen != seq && n < 8; ++n) {
        const std::uint32_t slot = g_seen % 8u;
        const std::uint32_t e = hdr->nadeEvt[slot];
        const float pos[3] = {hdr->nadeEvtPos[slot][0], hdr->nadeEvtPos[slot][1], hdr->nadeEvtPos[slot][2]};
        const float vel[3] = {hdr->nadeEvtVel[slot][0], hdr->nadeEvtVel[slot][1], hdr->nadeEvtVel[slot][2]};
        ++g_seen;
        if (!pawn || !inv) {
            Refuse(e & 0xFFu, kUnavailable, "no pawn");
            continue;
        }
        Apply(hdr, pawn, inv, e, pos, vel, avail, why);
    }
    // The fuse while cooking: the countdown ticks (the game's UpdateCookingSound cadence: 0.5 s after the spoon, then every
    // 0.25 s, every 0.1 s in the last 40 %), and going off in the hand.
    if (g_hold.state == kCooking && pawn) {
        const float now = GameTime(pawn);
        const float left = g_hold.fuseLen - (now - g_hold.cookStart);
        if (left <= 0.0f) {
            Boom(hdr, pawn, inv);
        } else if (now >= g_hold.nextTick) {
            ++g_ticks;
            g_hold.nextTick = now + (left < 0.4f * g_hold.fuseLen ? 0.1f : 0.25f);
            if (g_cfg.debugOffHandTrace) MLOG("offhand: tick %u -- %.2f s left", g_ticks, left);
        }
    }
    Publish(hdr, pawn, inv, avail, holdOk, off);
}

std::uintptr_t CarrierComponent() { return g_carrier.pawn && g_carrier.pawn == aim::LocalPlayerPawn() ? g_carrier.comp : 0; }

bool CarrierFrame(float (&gw)[16]) {
    if (!CarrierComponent()) return false;
    float gun[16], off[16];
    bool offValid = false, two = false;
    if (!viewmodel::HandFrames(gun, off, offValid, two) || !offValid) return false;
    // The off controller's frame (rows forward, right, up, origin; the mirror world in left-hand mode, drawn back through
    // the mirror like the arms), the grenade a little ahead of and below the controller: in the palm (the next phase puts
    // the game's own grenade grip here).
    std::memcpy(gw, off, sizeof(gw));
    for (int j = 0; j < 3; ++j) gw[12 + j] += off[j] * 6.0f - off[8 + j] * 2.0f;
    return true;
}

}  // namespace mohavr::offhand
