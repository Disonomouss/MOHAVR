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
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::offhand {
namespace {

Config g_cfg;

// Components (the carrier) dispatch straight to UObject::ProcessEvent: pinned, its bytes checked once (standing rule 4).
bool ObjectProcessEventOk() {
    static int ok = -1;
    if (ok < 0) {
        ok = patch::BytesMatch(addr::kObjectProcessEvent, addr::kObjectProcessEventBytes, sizeof(addr::kObjectProcessEventBytes)) ? 1 : 0;
        if (!ok) MLOG("offhand: UObject::ProcessEvent bytes differ -- no component calls");
    }
    return ok == 1;
}

// No C++ objects here: SEH only (as reload.cpp's).
bool CallProcessEvent(std::uintptr_t pe, std::uintptr_t obj, std::uintptr_t fn, void* parms) {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*)>(pe)(obj, nullptr, fn, parms, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t ReadU32(std::uintptr_t at) {
    __try {
        return *reinterpret_cast<const std::uint32_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Bit(std::uintptr_t obj, const char* name) {
    int o = -1;
    std::uint32_t m = 0;
    return obj && names::BoolProperty(obj, name, o, m) && (ReadU32(obj + o) & m) != 0;
}

void SetBit(std::uintptr_t obj, const char* name, bool on) {
    int o = -1;
    std::uint32_t m = 0;
    if (!obj || !names::BoolProperty(obj, name, o, m)) return;
    auto* w = reinterpret_cast<std::uint32_t*>(obj + o);
    *w = on ? (*w | m) : (*w & ~m);
}

std::uintptr_t Obj(std::uintptr_t obj, const char* name) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    return o >= 0 ? names::ReadPointer(obj + o) : 0;
}

float Float(std::uintptr_t obj, const char* name, float fallback) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o < 0) return fallback;
    float v = fallback;
    std::memcpy(&v, reinterpret_cast<const void*>(obj + o), sizeof(v));
    return v;
}

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// A script function of `obj`'s class, callable through its ProcessEvent (an actor's AActor::ProcessEvent, a component's
// UObject::ProcessEvent), with its parameters' offsets; empty if anything is off.
struct Call {
    std::uintptr_t obj = 0, fn = 0, pe = 0;
    std::uint8_t parms[256] = {};
    bool ok = false;

    Call(std::uintptr_t o, const char* name) : obj(o) {
        const std::uintptr_t cls = o ? names::ReadPointer(o + addr::kObjectClass) : 0;
        fn = cls ? names::FindFieldProbe(cls, name) : 0;
        const std::uintptr_t vt = o ? names::ReadPointer(o) : 0;
        pe = vt ? names::ReadPointer(vt + addr::kVtProcessEvent) : 0;
        const std::uint16_t native = fn ? static_cast<std::uint16_t>(ReadU32(fn + addr::kFunctionNative) & 0xFFFF) : 1;
        const std::uint16_t size = fn ? static_cast<std::uint16_t>(ReadU32(fn + addr::kFunctionParmsSize) & 0xFFFF) : 0;
        ok = fn && names::ClassName(fn) == "Function" && native == 0 && size <= sizeof(parms) &&
             (pe == addr::kActorProcessEvent || (pe == addr::kObjectProcessEvent && ObjectProcessEventOk()));
        if (!ok)
            MLOG("offhand: %s.%s can't be called (function %s, native index %u, parms %u, ProcessEvent 0x%08X)",
                 names::Name(o).c_str(), name, fn ? names::ClassName(fn).c_str() : "none", native, size, static_cast<unsigned>(pe));
    }
    int Off(const char* parm) const {
        const std::uintptr_t p = fn ? names::FindFieldProbe(fn, parm) : 0;
        const int o = p ? static_cast<int>(names::ReadPointer(p + addr::kPropertyOffset)) : -1;
        return o >= 0 && o < static_cast<int>(sizeof(parms)) - 16 ? o : -1;
    }
    bool Set(const char* parm, const void* v, size_t n) {
        const int o = Off(parm);
        if (o < 0) {
            MLOG("offhand: %s has no parameter %s", names::Name(fn).c_str(), parm);
            return false;
        }
        std::memcpy(parms + o, v, n);
        return true;
    }
    bool Run() {
        if (!ok) return false;
        const bool done = CallProcessEvent(pe, obj, fn, parms);
        if (!done) MLOG("offhand: %s.%s FAULTED", names::Name(obj).c_str(), names::Name(fn).c_str());
        return done;
    }
    std::uintptr_t ReturnObject() const {
        const int o = Off("ReturnValue");
        return o >= 0 ? *reinterpret_cast<const std::uintptr_t*>(parms + o) : 0;
    }
};

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
    if (g) return g;
    for (std::uintptr_t item = Obj(inv, "InventoryChain"), n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (names::ClassName(item) == kTypeClass[type] && Obj(item, "Instigator") == pawn) return item;
    return 0;
}

int ReserveCount(std::uintptr_t inv, std::uintptr_t g) {
    int cap = 0;
    const int* r = g ? ReserveOf(inv, g, cap) : nullptr;
    return r ? *r : -1;
}

// The thrown grenades, followed until they go off (or 10 s).
struct Thrown {
    std::uintptr_t proj = 0;
    DWORD          at = 0;
    float          start[3]{};
    bool           logged1s = false;
};
Thrown g_thrown[4];

void Dump() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const int fo = pawn ? names::PropertyOffset(pawn, "FlashCount") : -1;
    const int po = inv ? names::PropertyOffset(inv, "PendingFire") : -1;
    const std::uintptr_t pf = po >= 0 ? names::ReadPointer(inv + po) : 0;
    const int pfn = po >= 0 ? static_cast<int>(ReadU32(inv + po + 4)) : 0;
    MLOG("offhand: pawn %s, in hand %s (%s), FlashCount %u, PendingFire %d entries [%d %d]", names::Name(pawn).c_str(),
         names::Name(gun).c_str(), names::ClassName(gun).c_str(), fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0u, pfn,
         pfn > 0 ? static_cast<int>(ReadU32(pf)) : -1, pfn > 1 ? static_cast<int>(ReadU32(pf + 4)) : -1);
    for (int t = 0; t < 3; ++t) {
        const std::uintptr_t g = GrenadeOf(pawn, inv, t);
        if (!g) {
            MLOG("offhand: %s -- none in the inventory", kTypeName[t]);
            continue;
        }
        int cap = 0;
        const int* r = ReserveOf(inv, g, cap);
        const int fm = names::PropertyOffset(g, "CurrentFireMode"), ac = names::PropertyOffset(g, "AmmoCount");
        MLOG("offhand: %s -- %s (%s), the inventory manager's %s; CurrentFireMode %u, AmmoCount[0] %d, reserve %d of %d%s, "
             "instigator %s, bStasis %d, FuseTime %.2f, ExplosiveSpeed %.0f, DrawScale %.2f, pawn velocity x%.2f",
             kTypeName[t], names::Name(g).c_str(), names::ClassName(g).c_str(), Obj(inv, kTypeProp[t]) == g ? "pointer" : "chain",
             fm >= 0 ? ReadU32(g + fm) & 0xFF : 255u, ac >= 0 ? static_cast<int>(ReadU32(g + ac)) : -1, r ? *r : -1, cap,
             Bit(g, "bInfiniteAmmo") ? " (infinite)" : "", Obj(g, "Instigator") == pawn ? "the pawn" : names::Name(Obj(g, "Instigator")).c_str(),
             Bit(g, "bStasis") ? 1 : 0, Float(g, "FuseTime", -1.0f), Float(g, "ExplosiveSpeed", -1.0f),
             Float(g, "ExplosiveDrawScale", -1.0f), Float(g, "ExplosivePawnVelocityScale", -1.0f));
        if (t == 0) {
            const std::uintptr_t cls = names::ReadPointer(g + addr::kObjectClass);
            const std::uintptr_t fn = cls ? names::FindFieldProbe(cls, "SpawnProjectile") : 0;
            Call c(g, "SpawnProjectile");
            MLOG("offhand: SpawnProjectile %s -- flags 0x%X, native index %u, parms %u bytes; vStartPos +%d, vForward +%d, "
                 "ReturnValue +%d", fn ? names::Name(names::Outer(fn)).c_str() : "none", fn ? ReadU32(fn + addr::kFunctionFlags) : 0u,
                 fn ? ReadU32(fn + addr::kFunctionNative) & 0xFFFF : 0u, fn ? ReadU32(fn + addr::kFunctionParmsSize) & 0xFFFF : 0u,
                 c.Off("vStartPos"), c.Off("vForward"), c.Off("ReturnValue"));
        }
    }
}

// The spike's throw: the inventory grenade weapon `g` launches its projectile at `start` with world velocity `vel`.
std::uintptr_t Launch(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t g, const float (&start)[3], const float (&vel)[3],
                      float fuse) {
    const float speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    const float dir[3] = {speed > 1.0f ? vel[0] / speed : 1.0f, speed > 1.0f ? vel[1] / speed : 0.0f, speed > 1.0f ? vel[2] / speed : 0.0f};
    // SpawnProjectile reads CurrentFireMode for the projectile class (it stays 2 after a grenade melee).
    const int fm = names::PropertyOffset(g, "CurrentFireMode");
    if (fm >= 0) *reinterpret_cast<std::uint8_t*>(g + fm) = 0;
    Call spawn(g, "SpawnProjectile");
    if (!spawn.Set("vStartPos", start, sizeof(start)) || !spawn.Set("vForward", dir, sizeof(dir)) || !spawn.Run()) return 0;
    const std::uintptr_t proj = spawn.ReturnObject();
    if (!proj || !names::IsA(proj, "MOHAProj_Explosive")) {
        MLOG("offhand: SpawnProjectile gave %s (%s) -- no grenade", names::Name(proj).c_str(), names::ClassName(proj).c_str());
        return 0;
    }
    float loc[3] = {0, 0, 0};
    names::ReadVector(proj + addr::kActorLocation, loc);
    const float scaleBefore = Float(proj, "DrawScale", -1.0f);
    MLOG("offhand: %s launched %s (%s): owner %s, instigator %s, InstigatorController %s, enabled %d, %.1f units from the start",
         names::Name(g).c_str(), names::Name(proj).c_str(), names::ClassName(proj).c_str(), names::Name(Obj(proj, "Owner")).c_str(),
         Obj(proj, "Instigator") == pawn ? "the pawn" : names::Name(Obj(proj, "Instigator")).c_str(),
         names::Name(Obj(proj, "InstigatorController")).c_str(), Bit(proj, "bEnabled") ? 1 : 0, Dist(loc, start));
    // What EALAGrenade.ProjectileFire does after its spawn: the draw scale, the explosion light, the cooked damage type
    // (every player throw is cooked: the pin pull starts the fuse), the fuse, the velocity.
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
    MLOG("offhand: %s -- DrawScale %.2f -> %.2f, damage %s, fuse %.2f s (fFuseTime %.2f), velocity %.0f %.0f %.0f",
         names::Name(proj).c_str(), scaleBefore, Float(proj, "DrawScale", -1.0f), names::Name(Obj(proj, "MyDamageType")).c_str(), fuse,
         Float(proj, "fFuseTime", -1.0f), vel[0], vel[1], vel[2]);
    // One grenade out of the reserve (none with infinite ammo).
    if (!Bit(g, "bInfiniteAmmo")) {
        int cap = 0;
        if (int* r = ReserveOf(inv, g, cap)) {
            if (*r > 0) --*r;
            MLOG("offhand: %s's reserve -> %d of %d", names::Name(g).c_str(), *r, cap);
        }
    }
    return proj;
}

void Throw(const wchar_t* typeW, const wchar_t* fromW, const float (&xr)[3], float fuse) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    if (!pawn || !inv) {
        MLOG("offhand: no pawn / inventory manager");
        return;
    }
    if (names::IsA(gun, "EALAGrenade")) {
        MLOG("offhand: the weapon in hand is a grenade (%s) -- the game's own throw", names::ClassName(gun).c_str());
        return;
    }
    // The type: the one asked for, or 'any' -- the inventory manager's last grenade if it has some, else frag, Gammon, stick.
    std::uintptr_t g = 0;
    const int want = !wcscmp(typeW, L"frag") ? 0 : !wcscmp(typeW, L"gammon") ? 1 : !wcscmp(typeW, L"stick") ? 2 : -1;
    if (want >= 0) g = GrenadeOf(pawn, inv, want);
    if (!g && !wcscmp(typeW, L"any")) {
        const std::uintptr_t last = Obj(inv, "LastGrenadeWeapon");
        if (last && ReserveCount(inv, last) > 0) g = last;
        for (int t = 0; t < 3 && !g; ++t) {
            const std::uintptr_t c = GrenadeOf(pawn, inv, t);
            if (c && ReserveCount(inv, c) > 0) g = c;
        }
    }
    if (!g) {
        MLOG("offhand: no %ls grenade", typeW);
        return;
    }
    if (!Bit(g, "bInfiniteAmmo") && ReserveCount(inv, g) <= 0) {
        MLOG("offhand: no %s left", names::ClassName(g).c_str());
        return;
    }
    // From the off hand (the controller not holding the gun; un-mirrored in left-hand mode) or the eye.
    float start[3] = {0, 0, 0};
    bool fromHand = false;
    if (!wcscmp(fromW, L"hand")) {
        float gunF[16], off[16];
        bool offValid = false, two = false;
        if (viewmodel::HandFrames(gunF, off, offValid, two) && offValid) {
            float p[3] = {off[12], off[13], off[14]};
            float r[16];
            if (viewmodel::DrawMirror(r))
                for (int j = 0; j < 3; ++j) p[j] = off[12] * r[0 + j] + off[13] * r[4 + j] + off[14] * r[8 + j] + r[12 + j];
            std::memcpy(start, p, sizeof(start));
            fromHand = true;
        } else {
            MLOG("offhand: no off-hand frame (the gun isn't drawn in the hand) -- from the eye");
        }
    }
    if (!fromHand) {
        float pitch = 0.0f, yaw = 0.0f;
        if (!view::GameCamera(start, pitch, yaw)) {
            MLOG("offhand: no view");
            return;
        }
    }
    float ue[3];
    if (!view::VectorToWorld(xr, ue)) {
        MLOG("offhand: no view mapping for the velocity");
        return;
    }
    float pawnVel[3] = {0, 0, 0};
    const int pvo = names::PropertyOffset(pawn, "Velocity");
    if (pvo >= 0) names::ReadVector(pawn + pvo, pawnVel);
    const float carry = Float(g, "ExplosivePawnVelocityScale", 0.25f);
    const float vel[3] = {ue[0] * g_cfg.throwScale + pawnVel[0] * carry, ue[1] * g_cfg.throwScale + pawnVel[1] * carry,
                          ue[2] * g_cfg.throwScale + pawnVel[2] * carry};
    const int fo = names::PropertyOffset(pawn, "FlashCount");
    const std::uint32_t flashBefore = fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0;
    if (fuse <= 0.0f) fuse = Float(g, "FuseTime", 4.0f);
    MLOG("offhand: throw %s from the %s (%.0f %.0f %.0f), %.1f %.1f %.1f m/s -> %.0f %.0f %.0f units/s (x%.2f, pawn x%.2f); "
         "the gun in hand %s", names::ClassName(g).c_str(), fromHand ? "off hand" : "eye", start[0], start[1], start[2], xr[0], xr[1],
         xr[2], vel[0], vel[1], vel[2], g_cfg.throwScale, carry, names::Name(gun).c_str());
    const std::uintptr_t proj = Launch(pawn, inv, g, start, vel, fuse);
    const std::uintptr_t gunAfter = Obj(pawn, "Weapon");
    const std::uint32_t flashAfter = fo >= 0 ? ReadU32(pawn + fo) & 0xFF : 0;
    MLOG("offhand: after the throw -- the weapon in hand %s (%s), FlashCount %u -> %u", names::Name(gunAfter).c_str(),
         gunAfter == gun ? "unchanged" : "CHANGED", flashBefore, flashAfter);
    if (!proj) return;
    for (Thrown& t : g_thrown)
        if (!t.proj) {
            t = Thrown{proj, GetTickCount(), {start[0], start[1], start[2]}, false};
            break;
        }
}

// Spike S2, the grenade in the off hand: a clone of the grenade weapon's first-person pickup mesh (DroppedPickupMesh, the
// same skeletal mesh as its attachment; the game never attaches it while the grenade is in the inventory), attached to
// the arms at their Camera bone and baked at the off hand by the arms' bake (arms_ik.cpp; collapsed -- nothing drawn --
// when there is no off-hand frame).
struct Carrier {
    std::uintptr_t comp = 0, arms = 0, pawn = 0;
} g_carrier;

template <class T>
void CopyField(std::uintptr_t to, std::uintptr_t from, const char* name) {
    const int o = names::PropertyOffset(from, name), p = names::PropertyOffset(to, name);
    if (o >= 0 && p >= 0) std::memcpy(reinterpret_cast<void*>(to + p), reinterpret_cast<const void*>(from + o), sizeof(T));
}

void CarrierCommand(const wchar_t* typeW) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t arms = Obj(pawn, "FPArms");
    if (g_carrier.comp) {
        if (g_carrier.arms && g_carrier.pawn == pawn) {
            Call detach(g_carrier.arms, "DetachComponent");
            if (detach.Set("Component", &g_carrier.comp, sizeof(g_carrier.comp)) && detach.Run())
                MLOG("offhand: carrier %s detached", names::Name(g_carrier.comp).c_str());
        }
        g_carrier = Carrier{};
    }
    if (!wcscmp(typeW, L"off")) return;
    const std::uintptr_t inv = Obj(pawn, "InvManager");
    const int want = !wcscmp(typeW, L"gammon") ? 1 : !wcscmp(typeW, L"stick") ? 2 : 0;
    const std::uintptr_t g = GrenadeOf(pawn, inv, want), tmpl = Obj(g, "DroppedPickupMesh");
    if (!arms || !g || !tmpl) {
        MLOG("offhand: carrier -- arms %s, grenade %s, pickup mesh %s: none", names::Name(arms).c_str(), names::Name(g).c_str(),
             names::Name(tmpl).c_str());
        return;
    }
    Call clone(tmpl, "Clone");
    if (!clone.Set("InOuter", &g, sizeof(g)) || !clone.Run()) return;
    const std::uintptr_t c = clone.ReturnObject();
    if (!c || !names::IsA(c, "MOHASkeletalMeshComponent")) {
        MLOG("offhand: carrier -- Clone gave %s (%s)", names::Name(c).c_str(), names::ClassName(c).c_str());
        return;
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
        return;
    }
    g_carrier = Carrier{c, arms, pawn};
    const int dpo = names::PropertyOffset(c, "DepthPriorityGroup");
    MLOG("offhand: carrier %s (%s, mesh %s, outer %s) attached to %s's Camera bone: attached %d, FOV %.0f, depth group %u",
         names::Name(c).c_str(), names::ClassName(c).c_str(), names::Name(Obj(c, "SkeletalMesh")).c_str(),
         names::Name(names::Outer(c)).c_str(), names::Name(arms).c_str(), Bit(c, "bAttached") ? 1 : 0,
         *reinterpret_cast<const float*>(c + addr::kMohaSkelMeshFov), dpo >= 0 ? *reinterpret_cast<const std::uint8_t*>(c + dpo) : 255u);
}

}  // namespace

void Configure(const Config& cfg) { g_cfg = cfg; }

std::uintptr_t CarrierComponent() { return g_carrier.pawn && g_carrier.pawn == aim::LocalPlayerPawn() ? g_carrier.comp : 0; }

bool CarrierFrame(float (&gw)[16]) {
    if (!CarrierComponent()) return false;
    float gun[16], off[16];
    bool offValid = false, two = false;
    if (!viewmodel::HandFrames(gun, off, offValid, two) || !offValid) return false;
    // The off controller's frame (rows forward, right, up, origin; the mirror world in left-hand mode, drawn back through
    // the mirror like the arms), the grenade a little ahead of and below the controller: in the palm.
    std::memcpy(gw, off, sizeof(gw));
    for (int j = 0; j < 3; ++j) gw[12 + j] += off[j] * 6.0f - off[8 + j] * 2.0f;
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        MLOG("offhand: carrier drawn at %.0f %.0f %.0f (the off controller %.0f %.0f %.0f)", gw[12], gw[13], gw[14], off[12], off[13],
             off[14]);
    }
    return true;
}

bool TestCommand(const wchar_t* line) {
    if (wcsncmp(line, L"mohavr nade", 11) != 0) return false;
    wchar_t type[16] = L"", from[16] = L"";
    float xr[3] = {0, 0, 0}, fuse = 0.0f;
    const int n = swscanf_s(line, L"mohavr nade throw %15ls %15ls %f %f %f %f", type, static_cast<unsigned>(16), from,
                            static_cast<unsigned>(16), &xr[0], &xr[1], &xr[2], &fuse);
    wchar_t what[16] = L"";
    if (n >= 5) Throw(type, from, xr, n >= 6 ? fuse : 0.0f);
    else if (swscanf_s(line, L"mohavr nade carrier %15ls", what, static_cast<unsigned>(16)) == 1) CarrierCommand(what);
    else Dump();
    return true;
}

void OnDraw() {
    const DWORD now = GetTickCount();
    for (Thrown& t : g_thrown) {
        if (!t.proj) continue;
        const float secs = (now - t.at) / 1000.0f;
        // Gone off: the projectile is disabled (Explode -> RecycleProjectile, back to the pool) or being destroyed.
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
        if (secs > 10.0f) {
            MLOG("offhand: %s still live 10 s after the throw -- stopped following", names::Name(t.proj).c_str());
            t.proj = 0;
        }
    }
}

}  // namespace mohavr::offhand
