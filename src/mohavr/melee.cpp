#include "melee.hpp"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "bridge.hpp"
#include "carrier.hpp"
#include "config.hpp"
#include "knife.hpp"
#include "log.hpp"
#include "names.hpp"
#include "offhand.hpp"
#include "script_call.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::melee {
using namespace script;  // Call, Obj, Int, Float, Bit, SetBit (script_call.hpp)
namespace {

#include "melee_points.inc"

Config g_cfg;
bool   g_bake = false;

// The engine's ImpactList buffer, reused across traces (as offpistol's): {Data, Num = 0, Max} of the last one in, the
// new header back. A mod-made pointer is never passed in.
struct List {
    std::uintptr_t data = 0;
    int            num = 0, max = 0;
} g_list;

// ImpactInfo (0x40): HitActor, HitLocation +4, HitNormal +0x10, RayDir +0x1C, HitInfo +0x28 (0x18: Material,
// PhysMaterial, Item, BoneName +0x34, HitComponent +0x3C).
constexpr std::size_t kImpact = 0x40, kImpactLocation = 4, kImpactRayDir = 0x1C, kImpactHitInfo = 0x28, kHitInfo = 0x18,
                      kImpactBone = 0x34;

// --- small helpers ---------------------------------------------------------------------------------------------------

bool SafeCopy(void* dst, std::uintptr_t src, std::size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Element i of a dynamic array property (n bytes each); false if out of range.
bool ArrayAt(std::uintptr_t obj, const char* name, int i, void* out, std::size_t n) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o < 0 || i < 0 || i >= static_cast<int>(ReadU32(obj + o + 4))) return false;
    const std::uintptr_t data = names::ReadPointer(obj + o);
    return data && SafeCopy(out, data + n * static_cast<std::uintptr_t>(i), n);
}

void SetFloat(std::uintptr_t obj, const char* name, float v) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o >= 0) *reinterpret_cast<float*>(obj + o) = v;
}

int Byte(std::uintptr_t obj, const char* name) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    return o >= 0 ? static_cast<int>(ReadU32(obj + o) & 0xFF) : -1;
}

float GameTime(std::uintptr_t pawn) { return Float(Obj(pawn, "WorldInfo"), "TimeSeconds", 0.0f); }

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len(const float* v) { return std::sqrt(Dot(v, v)); }
float Dist(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Len(d);
}

// p x m (row vectors: rows X, Y, Z, origin).
void Xform(const float* p, const float* m, float* out) {
    float r[3];
    for (int j = 0; j < 3; ++j) r[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    std::memcpy(out, r, sizeof(r));
}

// The inverse of a rigid frame (orthonormal 3x3 rows, an origin).
void RigidInverse(const float (&m)[16], float (&out)[16]) {
    float r[16] = {m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1};
    for (int j = 0; j < 3; ++j) r[12 + j] = -(m[12] * r[j] + m[13] * r[4 + j] + m[14] * r[8 + j]);
    std::memcpy(out, r, sizeof(out));
}

// v rotated by the unit quaternion q (x, y, z, w).
void Rotate(const float* q, const float* v, float* out) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    float r[3];
    r[0] = v[0] + w * tx + (y * tz - z * ty);
    r[1] = v[1] + w * ty + (z * tx - x * tz);
    r[2] = v[2] + w * tz + (x * ty - y * tx);
    std::memcpy(out, r, sizeof(r));
}

bool Head(shared::Header* hdr, float (&pos)[3], float (&fwd)[3], float& upm) {
    shared::Pose head{}, eye[2]{};
    shared::Fov fov[2]{};
    return hdr && shared::ReadViews(hdr, head, eye, fov) && view::PoseToWorld(head, pos, fwd, upm);
}

int g_logLines = 0;
#define TRACE_LOG(...)                                                   \
    do {                                                                 \
        if (g_cfg.debugMeleeTrace && g_logLines < 400) {                 \
            ++g_logLines;                                                \
            MLOG(__VA_ARGS__);                                           \
        }                                                                \
    } while (0)

// --- the host's samples (LOCAL) --------------------------------------------------------------------------------------

// One host frame: the gun pose, the head and the off hand, in LOCAL (metres), at its display time; and when the game
// took it (QPC seconds).
struct Sample {
    double t = 0.0, at = 0.0;
    float  gp[3]{}, gq[4]{0, 0, 0, 1};
    float  hp[3]{}, hq[4]{0, 0, 0, 1};
    float  op[3]{};
    bool   two = false, tracked = false;
};
constexpr int kHist = 64;  // ~0.7 s at 90 Hz

// The head's heading frame (right, forward) at a sample.
void Heading(const Sample& s, float (&right)[3], float (&fwd)[3]) {
    const float* q = s.hq;
    const float fx = -(2.0f * (q[0] * q[2] + q[3] * q[1])), fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    const float heading = std::atan2(-fx, -fz);
    const float r[3] = {std::cos(heading), 0.0f, -std::sin(heading)}, f[3] = {-std::sin(heading), 0.0f, -std::cos(heading)};
    std::memcpy(right, r, sizeof(r));
    std::memcpy(fwd, f, sizeof(f));
}
// A LOCAL point in the head's heading frame (right, up, forward): the body's own turns and steps (in the room or by the
// stick: LOCAL is the room) cancel, a swing doesn't.
void Rel(const Sample& s, const float* p, float* out) {
    float right[3], fwd[3];
    Heading(s, right, fwd);
    const float d[3] = {p[0] - s.hp[0], p[1] - s.hp[1], p[2] - s.hp[2]};
    float r[3] = {Dot(d, right), d[1], Dot(d, fwd)};
    std::memcpy(out, r, sizeof(r));
}
void RelDir(const Sample& s, const float* v, float* out) {
    float right[3], fwd[3];
    Heading(s, right, fwd);
    float r[3] = {Dot(v, right), v[1], Dot(v, fwd)};
    std::memcpy(out, r, sizeof(r));
}
// A point fixed on the gun (o: LOCAL metres in the gun pose's frame) at a sample, in that sample's heading frame.
void PointRel(const Sample& s, const float* o, float* out) {
    float w[3];
    Rotate(s.gq, o, w);
    for (int j = 0; j < 3; ++j) w[j] += s.gp[j];
    Rel(s, w, out);
}

// --- the strikes -----------------------------------------------------------------------------------------------------

enum Kind { kButt, kGrip, kFront, kBayonet, kKnife, kKinds };
const char* const kKindName[kKinds] = {"butt", "grip", "front", "bayonet", "knife"};

struct Strike {
    Kind   kind = kButt;
    int    n = 0;
    float  sample[3][3]{};   // mesh space: [0] the centre (the bayonet's tip), then the plate's top and bottom / the blade
    // The centre's lever: its place in the gun pose's frame (LOCAL metres: right, up, back), from the drawn gun while the
    // weapon is quiet -- a constant of the held gun, so the game's own gun animation (a kick) is never speed.
    float  o[3]{};
    float  axis[3]{0, 0, -1};  // the blade's direction in the same frame (the gun's barrel: -Z)
    bool   haveLever = false;
    // Drawn (world) positions, last Draw.
    float  prev[3][3]{};
    bool   havePrev = false;
    // The gate.
    int    over = 0;            // consecutive estimates over the gate
    double armedUntil = -1.0;   // QPC seconds (the peak hold)
    bool   firstArmed = false;  // armed since the last Draw: also swept from the hand (a point already inside a body)
    bool   ready = true;        // false after a hit until the speed drops below half its gate (one swing, one hit)
    bool   thrust = false;      // this swing's: a blade's thrust (else a stroke or a slash)
    bool   worldFx = false;     // this swing's world contact already shown
    float  speed = 0.0f, hand = 0.0f, along = 0.0f, travel = 0.0f, turn = 0.0f;  // the newest estimate's
    float  peak = 0.0f, peakHand = 0.0f, gate = 0.0f;
    bool   loggedArm = false;
    bool   loggedOne = false;   // (the trace: a stroke in one frame only)
    bool   loggedStep = false;  // (the trace: a tracking step)
};

// One hand's strikes: the gun in the gun hand, the off-hand knife (OFFKNIFE-DESIGN A4) -- its pose's history (gp / gq: the gun
// pose, or the off hand's aim pose), its strike points and their swings, its gates and re-melee interval.
struct Gates {
    float butt, hand, travel, thrust, thrustCos, thrustTravel, maxTurn, slash, slashCos, bladeHand;
};
struct Channel {
    Sample        hist[kHist];
    int           hn = 0, hhead = 0;
    double        lastT = -1.0;          // the newest host display time taken (s)
    double        glitchUntil = -1.0;    // display time: no arming before (a tracking jump)
    float         quietUntil = -1.0f;    // game time: no strike before (the re-melee interval)
    Strike        strikes[4];
    int           strikeN = 0;
    Gates         gates{};
    std::uint32_t epochSeen = 0xFFFFFFFFu;
    const Sample& Hist(int back) const { return hist[(hhead + kHist - 1 - back) % kHist]; }  // 0 = the newest
};
Channel        g_gun, g_knife;
std::uintptr_t g_strikesFor = 0;  // the weapon they are for
int            g_strikesLevel = -9;
std::uintptr_t g_strikesAtt = 0;
bool           g_leverMirrored = false;

bool Blade(Kind k) { return k == kFront || k == kBayonet || k == kKnife; }

// The gun's strikes at its upgrade level: per kind, the row at the highest level not above it. The launchers strike with
// nothing (their ends are behind the head and a metre out: the right stick's melee only).
void BuildStrikes(std::uintptr_t gun, std::uintptr_t att, int level) {
    g_gun.strikeN = 0;
    g_strikesFor = gun;
    g_strikesLevel = level;
    g_strikesAtt = att;
    const std::string key = names::Name(Obj(gun, "AttachmentClass"));
    const bool pistol = names::IsA(gun, "MOHAPistol");
    const bool launcher = key == "Attachment_Panzerschreck" || key == "Attachment_M18RecoillessRifle";
    const MeleeStrike* best[4] = {};
    for (const MeleeStrike& s : kMeleeStrikes) {
        if (key != s.gun || s.level > level) continue;
        const int k = !std::strcmp(s.kind, "butt") ? kButt : !std::strcmp(s.kind, "grip") ? kGrip : !std::strcmp(s.kind, "front") ? kFront
                    : !std::strcmp(s.kind, "bayonet") ? kBayonet : -1;
        if (k < 0) continue;
        if (!best[k] || s.level > best[k]->level) best[k] = &s;
    }
    auto add = [&](Kind kind, const MeleeStrike* s) {
        if (!s || g_gun.strikeN >= 4) return;
        Strike& st = g_gun.strikes[g_gun.strikeN++];
        st = Strike{};
        st.kind = kind;
        std::memcpy(st.sample[0], s->mesh, sizeof(st.sample[0]));
        st.n = 1;
        if (kind == kButt) {  // the plate's top and bottom too (mesh Y is down)
            for (int k = 0; k < 2; ++k) {
                std::memcpy(st.sample[1 + k], s->mesh, sizeof(st.sample[0]));
                st.sample[1 + k][1] += (k ? 0.7f : -0.7f) * s->radius;
            }
            st.n = 3;
        } else if (kind == kBayonet) {  // the blade's middle
            for (int j = 0; j < 3; ++j) st.sample[1][j] = 0.5f * (s->mesh[j] + s->base[j]);
            st.n = 2;
        }
    };
    if (!launcher) {
        // A pistol strikes with its grip (the game's whip), or the C96's shoulder stock when fitted; a long gun with its butt.
        if (pistol) {
            add(kGrip, best[kGrip]);
            if (best[kButt] && std::strstr(best[kButt]->bone, "stock")) add(kButt, best[kButt]);
        } else {
            add(kButt, best[kButt]);
        }
        if (g_cfg.meleeMuzzle) add(kFront, best[kFront]);
        add(kBayonet, best[kBayonet]);
    }
    std::string what;
    for (int i = 0; i < g_gun.strikeN; ++i) {
        char b[96];
        sprintf_s(b, " %s (%.1f %.1f %.1f, %d point%s)", kKindName[g_gun.strikes[i].kind], g_gun.strikes[i].sample[0][0], g_gun.strikes[i].sample[0][1],
                  g_gun.strikes[i].sample[0][2], g_gun.strikes[i].n, g_gun.strikes[i].n > 1 ? "s" : "");
        what += b;
    }
    MLOG("melee: %s at level %d strikes with:%s", key.c_str(), level,
         g_gun.strikeN ? what.c_str() : launcher ? " nothing (a launcher: the right stick's melee only)" : " nothing (no strike points)");
}

// The swings start again (the levers stay: they belong to the gun).
void ResetSwings(Channel& c) {
    for (int i = 0; i < c.strikeN; ++i) {
        Strike& s = c.strikes[i];
        s.havePrev = false;
        s.over = 0;
        s.armedUntil = -1.0;
        s.firstArmed = false;
        s.ready = true;
        s.loggedArm = false;
        s.loggedOne = false;
        s.loggedStep = false;
    }
}
void ResetHistory(Channel& c) {
    c.hn = c.hhead = 0;
    ResetSwings(c);
}

// --- the state -------------------------------------------------------------------------------------------------------

std::uintptr_t g_pawn = 0;
bool           g_wasOn = false;
std::string    g_why;
bool           g_knifeOn = false;
std::string    g_knifeWhy;
bool           g_knifeMirrored = false;
int            g_skipDraws = 0;
int            g_knifeSkip = 0;         // (the knife's: its own count down)         // contact skipped this many Draws (a snap turn's carry, a teleport, a reset)
std::uint32_t  g_epochSeen = 0xFFFFFFFFu, g_flagsSeen = 0xFFFFFFFFu, g_recenterSeen = 0xFFFFFFFFu;
float          g_lastYaw = 0.0f, g_lastLoc[3]{};
bool           g_havePawnPose = false;
DWORD          g_flashAt = 0, g_ammoAt = 0, g_quietSince = 0;  // the gun's last shot / ammo change, its state since
int            g_flashSeen = -1, g_ammoSeen = -1;
std::string    g_stateSeen;
float          g_speedHist[16]{};       // the pawn's speed (units/s) over the last Draws, for the charge rule
float          g_speedAt[16]{};
int            g_speedHead = 0;
struct Recent {
    std::uintptr_t actor = 0;
    float          until = 0.0f;
} g_recent[8];
unsigned       g_hits = 0;
std::uintptr_t g_test = 0;              // the test soldier ("mohavr melee enemy")

bool Recently(std::uintptr_t actor, float now) {
    for (const Recent& r : g_recent)
        if (r.actor == actor && now < r.until) return true;
    return false;
}
void Remember(std::uintptr_t actor, float until) {
    Recent* slot = &g_recent[0];
    for (Recent& r : g_recent) {
        if (r.actor == actor) { slot = &r; break; }
        if (r.until < slot->until) slot = &r;
    }
    *slot = Recent{actor, until};
}

// The game's own ally rule (MOHADamageType.AdjustDamagePlayertoNPC): the pawn's Kynapse controller -- or, with none (a
// gunner on a mounted MG42 is unpossessed, his owner the gun), its owner's -- on team 0, or no Kynapse controller at all:
// the player's damage is zeroed there and a friendly-fire line plays.
bool Friendly(std::uintptr_t ai) {
    std::uintptr_t ctrl = Obj(ai, "Controller");
    if (!ctrl) ctrl = Obj(Obj(ai, "Owner"), "Controller");
    if (!ctrl || !names::IsA(ctrl, "KynapseAIController")) return true;
    return Int(ctrl, "TeamIndex", 0) == 0;
}

// A prop the game's melee is meant for: a Kismet damage event listing the melee damage type (Der Flakturm's vent covers),
// or a physics actor (a push). Anything else is struck with effects only unless [Melee] Props (a fuel barrel, a radio,
// an objective's controls: the button melee still damages them).
bool MeleeProp(std::uintptr_t actor, std::uintptr_t dtype) {
    if (g_cfg.meleeProps) return true;
    if (names::IsA(actor, "KActor") && !names::IsA(actor, "MOHADestructibleProp") && Bit(actor, "bDamageAppliesImpulse")) return true;
    const int eo = names::PropertyOffset(actor, "GeneratedEvents");
    const int n = eo >= 0 ? static_cast<int>(ReadU32(actor + eo + 4)) : 0;
    const std::uintptr_t evs = eo >= 0 ? names::ReadPointer(actor + eo) : 0;
    for (int i = 0; i < n && i < 32 && evs; ++i) {
        const std::uintptr_t e = names::ReadPointer(evs + 4u * static_cast<std::uintptr_t>(i));
        if (!e || !names::IsA(e, "SeqEvent_TakeDamage")) continue;
        const int to = names::PropertyOffset(e, "DamageTypes");
        const int m = to >= 0 ? static_cast<int>(ReadU32(e + to + 4)) : 0;
        const std::uintptr_t types = to >= 0 ? names::ReadPointer(e + to) : 0;
        for (int k = 0; k < m && k < 16 && types; ++k)
            if (names::ReadPointer(types + 4u * static_cast<std::uintptr_t>(k)) == dtype) return true;
    }
    return false;
}

// Weapon states a strike may come from (MELEE-DESIGN 2.8): the gun at rest or firing. Not the game's own melee, a reload or
// rechamber animation, an equip / put-down, the sights' transitions or the alternate mode's.
bool StrikeState(const std::string& s) {
    return s == "Active" || s == "WeaponFiring" || s == "WeaponBurstFire" || s == "WeaponSingleFire" || s == "WeaponIronsights";
}

// --- the hit ---------------------------------------------------------------------------------------------------------

// One trace start -> end with the bullets' own collision (per-bone bodies), aim.cpp's bullet hook standing down; the first
// blocking impact (HitActor 0 if none).
bool Trace(std::uintptr_t pawn, std::uintptr_t gun, const float (&start)[3], const float (&end)[3], std::uint8_t (&impact)[kImpact]) {
    Call trace(gun, "CalcWeaponFireNative");
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
    if (!ok || !lst || !rv) return false;
    std::memcpy(&g_list.data, lst, 4);
    std::memcpy(&g_list.num, lst + 4, 4);
    std::memcpy(&g_list.max, lst + 8, 4);
    std::memcpy(impact, rv, kImpact);
    return true;
}

// The attachment's melee impact effects (the material's sound, a particle on dirt / sandbags; a hit-spang on a living
// soldier), with the weapon's own melee impact type (24 a pistol, 25 a long gun).
void Effects(std::uintptr_t pawn, std::uint8_t (&impact)[kImpact]) {
    const std::uintptr_t gun = Obj(pawn, "Weapon"), att = Obj(pawn, "CurrentWeaponAttachment");
    if (!gun || !att || Bit(gun, "bDeleteMe") || Bit(att, "bDeleteMe")) return;
    int type = 25;
    Call gm(gun, "GetMeleeImpactType", true);
    if (gm.ok && gm.Run()) {
        const std::uint8_t* r = gm.At("ReturnValue", 1);
        if (r) type = *r;
    }
    Call fx(att, "PlayImpactEffectsWithImpactInfo");
    const std::uintptr_t none = 0;
    if (fx.ok && fx.Set("Impact", impact, kImpact) && fx.Set("ImpactType", &type, sizeof(type))) {
        if (fx.Off("DamageType") >= 0) fx.Set("DamageType", &none, sizeof(none));
        fx.Run();
    }
}

// The pawn's top speed over the last 0.3 s of game time (the game samples it as the melee starts, ~0.3 s before its
// trace: a sprinting stroke lands as the body slows).
float RecentSpeed(float now) {
    float best = 0.0f;
    for (int i = 0; i < 16; ++i)
        if (now - g_speedAt[i] <= 0.3f && g_speedHist[i] > best) best = g_speedHist[i];
    return best;
}

// The game's PerformMeleeTrace for one impact (EALAWeapon.uc:1576-1606): TakeDamage on the hit actor with the weapon's melee
// damage, the player's controller, the impulse along the swing, the melee damage type and the hit's bone; then the
// attachment's melee impact effects (re-read after the damage: a kill may change the weapon). `bayonet`: the blade's
// damage (InstantHitDamage[2]: 200 on the M12 at level 2), else the weapon's base melee damage (InstantHitDamageThird_TUNE,
// 50: the upgrades never write it). `own` > 0: the strike's own damage (the off-hand knife's).
bool ApplyHit(std::uintptr_t pawn, std::uintptr_t gun, std::uint8_t (&impact)[kImpact], bool bayonet, float own, float now, int& dmgOut,
              int& healthBefore, int& healthAfter) {
    std::uintptr_t actor = 0;
    std::memcpy(&actor, impact, 4);
    const std::uintptr_t ctrl = Obj(pawn, "Controller");
    if (!actor || !ctrl) return false;
    float base = Float(gun, "InstantHitDamageThird_TUNE", -1.0f);
    float dmg2 = -1.0f;
    ArrayAt(gun, "InstantHitDamage", 2, &dmg2, sizeof(dmg2));
    if (base <= 0.0f) base = dmg2 > 0.0f ? dmg2 : 50.0f;
    float dmg = own > 0.0f ? own : bayonet && dmg2 > 0.0f ? dmg2 : base;
    // Above GroundSpeed (a sprint) the target's Health, the game's rule.
    const float speed = RecentSpeed(now), ground = Float(pawn, "GroundSpeed", 490.0f);
    const bool isPawn = names::IsA(actor, "Pawn");
    healthBefore = isPawn ? Int(actor, "Health", -1) : -1;
    if (g_cfg.meleeChargeKill && isPawn && speed > ground && healthBefore > 0) dmg = static_cast<float>(healthBefore);
    const float impulse = speed >= ground ? Float(gun, "MeleeImpulse_Max", 75000.0f) : Float(gun, "MeleeImpulse_Min", 30000.0f);
    float ray[3];
    std::memcpy(ray, impact + kImpactRayDir, sizeof(ray));
    const float momentum[3] = {ray[0] * impulse, ray[1] * impulse, ray[2] * impulse};
    std::uintptr_t dtype = 0;
    ArrayAt(gun, "InstantHitDamageTypes", 2, &dtype, sizeof(dtype));
    SetFloat(gun, "fMeleeStartingSpeed", speed);  // (the sprint-melee achievement reads it)
    // TakeDamage: the parameters' names differ by class (Actor: DamageAmount, EventInstigator; Pawn: Damage, InstigatedBy).
    Call td(actor, "TakeDamage");
    const int damage = static_cast<int>(dmg);
    const char* dn = td.Off("Damage") >= 0 ? "Damage" : "DamageAmount";
    const char* in = td.Off("InstigatedBy") >= 0 ? "InstigatedBy" : "EventInstigator";
    float hitLoc[3];
    std::memcpy(hitLoc, impact + kImpactLocation, sizeof(hitLoc));
    const bool ran = td.ok && td.Set(dn, &damage, sizeof(damage)) && td.Set(in, &ctrl, sizeof(ctrl)) &&
                     td.Set("HitLocation", hitLoc, sizeof(hitLoc)) && td.Set("Momentum", momentum, sizeof(momentum)) &&
                     td.Set("DamageType", &dtype, sizeof(dtype)) && td.Set("HitInfo", impact + kImpactHitInfo, kHitInfo) &&
                     td.Set("DamageCauser", &gun, sizeof(gun)) && td.Run();
    dmgOut = damage;
    healthAfter = isPawn ? Int(actor, "Health", -1) : -1;
    Effects(pawn, impact);
    return ran;
}

// --- per Draw --------------------------------------------------------------------------------------------------------

// The levers (each strike's centre in the gun pose's frame), from the drawn gun brought back into the controller's frame
// (the mirror and the catch-up undone) -- only while the weapon is quiet (Active, no shot or ammo change for 0.5 s): the
// game's own animation of the gun (the kick) never enters them.
void TakeLevers(const float (&G)[16], const float (&ctrlInv)[16], const float* R, const float* Winv, bool mirrored, float upm) {
    for (int i = 0; i < g_gun.strikeN; ++i) {
        Strike& s = g_gun.strikes[i];
        float w[3];
        Xform(s.sample[0], G, w);
        if (R) Xform(w, R, w);        // back out of the mirror world
        if (Winv) Xform(w, Winv, w);  // and the body's move since the view (the bake's catch-up)
        float r[3];
        Xform(w, ctrlInv, r);         // in the controller frame: forward, right, up (units)
        s.o[0] = (mirrored ? -r[1] : r[1]) / upm;
        s.o[1] = r[2] / upm;
        s.o[2] = -r[0] / upm;
        s.haveLever = true;
    }
}

// A point fixed on the gun at a sample, in LOCAL (the room).
void PointLocal(const Sample& s, const float* o, float* out) {
    Rotate(s.gq, o, out);
    for (int j = 0; j < 3; ++j) out[j] += s.gp[j];
}

// The newest sample against the one ~35 ms before (a 3-4 frame window: one frame's difference spikes), in two frames: the
// room (LOCAL) and the head's heading frame. A swing moves the gun in both; a body turn or a step in the room moves it only
// in the room, a glance or a duck only against the head (the review of D49) -- so a stroke counts only when it is the same
// stroke in both frames, and its numbers are the smaller frame's. The newest sample alone must move at half the gate as
// well: a swing is moving now, a tracking step (one sample's jump, then still) stays in the window for a few samples but
// isn't (the second review of D49). Per strike: its speed, the hand's (the faster hand when two-handed), along the blade,
// the gun's turn.
constexpr float kSustain = 0.5f;
void Estimate(Channel& ch) {
    if (ch.hn < 3) return;
    const Sample& a = ch.Hist(0);
    const Sample& q = ch.Hist(1);  // the sample before the newest
    int k = 1;
    while (k < ch.hn - 1 && a.t - ch.Hist(k).t < 0.030) ++k;
    const Sample& b = ch.Hist(k);
    const float dt = static_cast<float>(a.t - b.t), dt1 = static_cast<float>(a.t - q.t);
    if (dt <= 0.0f || dt > 0.12f || dt1 <= 0.0f) return;
    const float zero[3] = {0, 0, 0};
    // The travel window: the sample ~0.25 s back.
    int k2 = k;
    while (k2 < ch.hn - 1 && a.t - ch.Hist(k2).t < 0.25) ++k2;
    const Sample& c = ch.Hist(k2);
    // The gun's turn over the window (in the room).
    const float qd = std::fabs(a.gq[0] * b.gq[0] + a.gq[1] * b.gq[1] + a.gq[2] * b.gq[2] + a.gq[3] * b.gq[3]);
    const float turn = 2.0f * std::acos(qd > 1.0f ? 1.0f : qd) / dt;
    // Per frame (0 the room, 1 against the head): the hand's velocity (the faster hand two-handed) and over the newest sample
    // alone, its move over the travel window, the barrel's direction (the gun pose's forward, -Z).
    struct Frame {
        float vh[3], vh1[3], hand, handDisp[3];
    } fr[2];
    for (int f = 0; f < 2; ++f) {
        Frame& F = fr[f];
        float ha[3], hb[3], hc[3], hq[3];
        if (f == 0) {
            PointLocal(a, zero, ha), PointLocal(b, zero, hb), PointLocal(c, zero, hc), PointLocal(q, zero, hq);
        } else {
            PointRel(a, zero, ha), PointRel(b, zero, hb), PointRel(c, zero, hc), PointRel(q, zero, hq);
        }
        for (int j = 0; j < 3; ++j) F.vh[j] = (ha[j] - hb[j]) / dt, F.vh1[j] = (ha[j] - hq[j]) / dt1, F.handDisp[j] = ha[j] - hc[j];
        F.hand = Len(F.vh);
        if (a.two && b.two) {  // two-handed: the front hand can drive the stroke
            float oa[3], ob[3];
            if (f == 0) {
                std::memcpy(oa, a.op, sizeof(oa)), std::memcpy(ob, b.op, sizeof(ob));
            } else {
                Rel(a, a.op, oa), Rel(b, b.op, ob);
            }
            const float vo[3] = {(oa[0] - ob[0]) / dt, (oa[1] - ob[1]) / dt, (oa[2] - ob[2]) / dt};
            if (Len(vo) > F.hand) F.hand = Len(vo);
        }
    }
    const float hand = std::fmin(fr[0].hand, fr[1].hand);
    const double now = Now();
    for (int i = 0; i < ch.strikeN; ++i) {
        Strike& s = ch.strikes[i];
        if (!s.haveLever) continue;
        // The blade's direction (the gun's barrel; the knife's blade) in each frame.
        float blade[2][3];
        Rotate(a.gq, s.axis, blade[0]);
        RelDir(a, blade[0], blade[1]);
        // The gates (MELEE-DESIGN 2.4) in each frame: a butt or grip stroke; a blade's thrust along the barrel (the hand: a
        // thrust drives the whole gun), or its slash across it.
        float speed = 1e9f, travel = 1e9f, along = 1e9f;
        bool pass[2] = {false, false}, thr[2] = {false, false}, sla[2] = {false, false};
        bool win[2] = {false, false}, thrWin[2] = {false, false}, slaWin[2] = {false, false};  // (the window alone: the trace)
        for (int f = 0; f < 2; ++f) {
            const Frame& F = fr[f];
            float pa[3], pb[3], pc[3], pq[3];
            if (f == 0) {
                PointLocal(a, s.o, pa), PointLocal(b, s.o, pb), PointLocal(c, s.o, pc), PointLocal(q, s.o, pq);
            } else {
                PointRel(a, s.o, pa), PointRel(b, s.o, pb), PointRel(c, s.o, pc), PointRel(q, s.o, pq);
            }
            const float v[3] = {(pa[0] - pb[0]) / dt, (pa[1] - pb[1]) / dt, (pa[2] - pb[2]) / dt};
            const float d[3] = {pa[0] - pc[0], pa[1] - pc[1], pa[2] - pc[2]};
            const float d1[3] = {pa[0] - pq[0], pa[1] - pq[1], pa[2] - pq[2]};
            const float sl = Len(v), tr = Len(d), al = Dot(F.vh, blade[f]);
            const float sl1 = Len(d1) / dt1, al1 = Dot(F.vh1, blade[f]);  // (the newest sample alone)
            speed = std::fmin(speed, sl);
            travel = std::fmin(travel, tr);
            along = std::fmin(along, al);
            if (!Blade(s.kind)) {
                win[f] = sl >= ch.gates.butt && F.hand >= ch.gates.hand && tr >= ch.gates.travel;
                pass[f] = win[f] && sl1 >= kSustain * ch.gates.butt;
            } else {
                thrWin[f] = al >= ch.gates.thrust && al >= ch.gates.thrustCos * Len(F.vh) &&
                            Dot(F.handDisp, blade[f]) >= ch.gates.thrustTravel && turn < ch.gates.maxTurn;
                slaWin[f] = sl >= ch.gates.slash && std::fabs(Dot(v, blade[f])) <= ch.gates.slashCos * sl &&
                            F.hand >= ch.gates.bladeHand && tr >= ch.gates.travel;
                thr[f] = thrWin[f] && al1 >= kSustain * ch.gates.thrust;
                sla[f] = slaWin[f] && sl1 >= kSustain * ch.gates.slash;
                win[f] = thrWin[f] || slaWin[f];
                pass[f] = thr[f] || sla[f];
            }
        }
        s.speed = speed;
        s.hand = hand;
        s.travel = travel;
        s.along = along;
        s.turn = turn;
        const bool thrust = thr[0] && thr[1];
        const bool fast = Blade(s.kind) ? thrust || (sla[0] && sla[1]) : pass[0] && pass[1];
        const float gate = !Blade(s.kind) ? ch.gates.butt : thrust ? ch.gates.thrust : ch.gates.slash;
        if (pass[0] != pass[1]) {
            if (!s.loggedOne) {
                s.loggedOne = true;
                TRACE_LOG("melee: %s -- a stroke %s only (%s): not armed", kKindName[s.kind], pass[0] ? "in the room" : "against the head",
                          pass[0] ? "the body turning or stepping" : "a glance or a duck");
            }
        } else if (!pass[0]) {
            s.loggedOne = false;
        }
        const bool winBoth = Blade(s.kind) ? (thrWin[0] && thrWin[1]) || (slaWin[0] && slaWin[1]) : win[0] && win[1];
        if (winBoth && !fast && now > s.armedUntil) {
            if (!s.loggedStep) {
                s.loggedStep = true;
                TRACE_LOG("melee: %s -- a step, not a swing (the newest sample still): not armed", kKindName[s.kind]);
            }
        } else if (!win[0] && !win[1]) {
            s.loggedStep = false;
        }
        // Re-armed once slow again; inferred (untracked) samples and a tracking jump never arm.
        if (!s.ready && speed < 0.5f * (Blade(s.kind) ? ch.gates.slash : ch.gates.butt) &&
            (!Blade(s.kind) || along < 0.5f * ch.gates.thrust))
            s.ready = true;
        s.over = fast && a.tracked && b.tracked && a.t >= ch.glitchUntil ? s.over + 1 : 0;
        if (s.over >= 2 && s.ready) {
            if (now > s.armedUntil) {  // a new swing
                s.peak = s.peakHand = 0.0f;
                s.worldFx = false;
                s.loggedArm = false;
                s.firstArmed = true;
                s.thrust = thrust;
                s.gate = gate;
            }
            s.armedUntil = now + g_cfg.meleeHold;
            if (speed > s.peak) s.peak = speed;
            if (hand > s.peakHand) s.peakHand = hand;
            if (!s.loggedArm) {
                s.loggedArm = true;
                TRACE_LOG("melee: %s armed -- %.1f m/s, hand %.1f m/s (along the barrel %.1f), %.2f m in 0.25 s, turning %.1f rad/s%s",
                          kKindName[s.kind], speed, hand, along, travel, turn,
                          Blade(s.kind) ? (thrust ? " (a thrust)" : " (a slash)") : "");
            }
        }
    }
}

// The gun hand in the world (the drawn controller frame's origin: through the mirror when mirrored).
void GripWorld(const float (&ctrl)[16], const float* R, float (&out)[3]) {
    const float o[3] = {0, 0, 0};
    Xform(o, ctrl, out);
    if (R) Xform(out, R, out);
}

// No strike through a wall: the game's own eye to the gun hand, then the hand to the contact (the shots' rule, aim.cpp).
bool Seen(std::uintptr_t pawn, std::uintptr_t target, const float (&grip)[3], const float (&contact)[3]) {
    float cam[3], pitch = 0.0f, yaw = 0.0f, hit[3];
    std::uintptr_t by = 0;
    auto blocked = [&](const float (&a)[3], const float (&b)[3]) {
        by = 0;
        float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float l = Len(d);
        if (l < 1.0f) return false;
        const float end[3] = {b[0] - d[0] / l * 2.0f, b[1] - d[1] / l * 2.0f, b[2] - d[2] / l * 2.0f};
        return aim::WorldTrace(pawn, a, end, hit, &by) && by != target && !(by && names::IsA(by, "Pawn"));
    };
    if (view::GameCamera(cam, pitch, yaw) && blocked(cam, grip)) {
        TRACE_LOG("melee: the gun hand is behind %s from the eye -- no strike", names::Name(by).c_str());
        return false;
    }
    if (blocked(grip, contact)) {
        TRACE_LOG("melee: %s is behind %s from the hand -- no strike", names::Name(target).c_str(), names::Name(by).c_str());
        return false;
    }
    return true;
}

// The host's pulse: kind 1 a soldier, 2 an actor, 3 the world; | 0x100 the off-hand knife's (its own counter).
void Feedback(shared::Header* hdr, std::uint32_t kind, float power) {
    if (!hdr) return;
    power = power < 0.0f ? 0.0f : power > 1.0f ? 1.0f : power;
    if (kind & 0x100u) {
        hdr->knifePower = power;
        hdr->knifeKind = kind & 0xFFu;
        _ReadWriteBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->knifeHits));
        return;
    }
    hdr->meleePower = power;
    hdr->meleeKind = kind;
    _ReadWriteBarrier();
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->meleeHits));
}

// A contact along start -> end (a strike point's sweep): the first thing met decides; past the world (effects only, the
// swing goes on) the sweep looks again, twice. True if the swing was spent (a soldier, or a prop meant for it).
bool Contact(shared::Header* hdr, std::uintptr_t pawn, std::uintptr_t gun, Channel& ch, Strike& st, const float (&start)[3],
             const float (&end)[3], const float (&grip)[3], float now) {
    const bool knife = st.kind == kKnife;
    const float own = knife ? g_cfg.knifeDamage : -1.0f;
    const std::uint32_t hand = knife ? 0x100u : 0u;  // (meleeKind bit8: the off hand's)
    float from[3] = {start[0], start[1], start[2]};
    float dir[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float l = Len(dir);
    if (l < 0.5f) return false;
    for (float& x : dir) x /= l;
    for (int pass = 0; pass < 3; ++pass) {
        std::uint8_t impact[kImpact] = {};
        if (!Trace(pawn, gun, from, end, impact)) return false;
        std::uintptr_t actor = 0;
        std::memcpy(&actor, impact, 4);
        if (!actor || actor == pawn) return false;
        float hitLoc[3];
        std::memcpy(hitLoc, impact + kImpactLocation, sizeof(hitLoc));
        const std::string bone = names::NameAt(reinterpret_cast<std::uintptr_t>(impact + kImpactBone));
        if (names::IsA(actor, "Pawn")) {
            if (Int(actor, "Health", 0) <= 0 || Bit(actor, "bDeleteMe") || names::StateName(actor) == "Dying") return false;
            if (names::IsA(actor, "MOHAAIPawn") && Friendly(actor)) {
                if (!Recently(actor, now)) {
                    TRACE_LOG("melee: %s met %s -- an ally, left alone", kKindName[st.kind], names::Name(actor).c_str());
                    Remember(actor, now + 1.0f);
                }
                return false;
            }
            if (Recently(actor, now)) return false;
            if (g_cfg.meleeLOS && !Seen(pawn, actor, grip, hitLoc)) return false;
            int dmg = 0, before = -1, after = -1;
            const bool ok = ApplyHit(pawn, gun, impact, st.kind == kBayonet, own, now, dmg, before, after);
            ++g_hits;
            float interval = Float(gun, "MeleeInterruptInterval", 0.6f);
            const float impactI = Float(gun, "MeleeImpactInterval", 0.3f);
            if (impactI > interval) interval = impactI;  // (GetMeleeInterruptInterval: the larger)
            if (knife) interval = 0.4f;  // (the knife's own: the MP40's knife melee)
            ch.quietUntil = now + interval;
            Remember(actor, now + g_cfg.meleeTargetCooldown);
            st.ready = false;
            st.armedUntil = -1.0;
            st.loggedArm = false;
            Feedback(hdr, 1u | hand, st.gate > 0.0f ? st.peak / st.gate - 1.0f : 0.0f);
            MLOG("melee: strike %u -- %s%s at %.1f m/s (hand %.1f) hit %s (%s) for %d: Health %d -> %d%s", g_hits, kKindName[st.kind],
                 st.kind == kBayonet || knife ? (st.thrust ? " thrust" : " slash") : "", st.peak, st.peakHand, names::Name(actor).c_str(), bone.c_str(),
                 dmg, before, after, ok ? "" : " -- TakeDamage did not run");
            return true;
        }
        if (!g_cfg.meleeWorld) return false;
        // A prop or the world: the game's damage only for the props meant for it; effects (once a swing) for the rest, and
        // the swing goes on past it (over a low wall, at a soldier behind).
        std::uintptr_t dtype = 0;
        ArrayAt(gun, "InstantHitDamageTypes", 2, &dtype, sizeof(dtype));
        if (!Bit(actor, "bStatic") && MeleeProp(actor, dtype) && !Recently(actor, now)) {
            if (g_cfg.meleeLOS && !Seen(pawn, actor, grip, hitLoc)) return false;
            int dmg = 0, before = -1, after = -1;
            ApplyHit(pawn, gun, impact, st.kind == kBayonet, own, now, dmg, before, after);
            ++g_hits;
            ch.quietUntil = now + 0.25f;
            Remember(actor, now + 0.5f);
            st.ready = false;
            st.armedUntil = -1.0;
            st.loggedArm = false;
            Feedback(hdr, 2u | hand, st.gate > 0.0f ? st.peak / st.gate - 1.0f : 0.0f);
            MLOG("melee: strike %u -- %s at %.1f m/s hit %s (a prop) for %d", g_hits, kKindName[st.kind], st.peak, names::Name(actor).c_str(), dmg);
            return true;
        }
        if (!st.worldFx) {
            st.worldFx = true;
            Effects(pawn, impact);
            Feedback(hdr, 3u | hand, 0.0f);
            TRACE_LOG("melee: %s struck %s (the world: effects only)", kKindName[st.kind], names::Name(actor).c_str());
        }
        for (int j = 0; j < 3; ++j) from[j] = hitLoc[j] + dir[j] * 1.0f;  // on past it
        if (Dot(dir, from) >= Dot(dir, end)) return false;
    }
    return false;
}

// The off-hand knife's strikes (OFFKNIFE-DESIGN A4): its tip and the blade's middle on the off hand's pose (the knife's place
// in the hand is a constant of the hold, so the levers come straight from it: no quiet gun to wait for), with the bayonet's
// thrust and slash gates at the knife's numbers. Independent of the gun's: whatever the gun hand is doing, the knife strikes.
void KnifeDraw(shared::Header* hdr, std::uintptr_t pawn, const shared::Pose (&hands)[2], const shared::Pose& head, std::int64_t xrTime,
               std::uint32_t bits, std::uint32_t flags, float now) {
    Channel& c = g_knife;
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    float kc[16];
    const char* hard = !(bits & 8u) ? "no knife held" : !bridge::HostRunning() ? "no host"
                     : !knife::CarrierComponent() || !knife::KnifeInController(kc) ? "the knife isn't drawn"
                     : !gun || !names::IsA(gun, "EALAWeapon") ? "no gun in hand" : nullptr;
    if (hard) {
        if (g_knifeOn || (c.strikeN && g_knifeWhy != hard)) MLOG("melee: the knife off -- %s", hard);
        g_knifeOn = false;
        g_knifeWhy = hard;
        if (c.strikeN) {
            ResetHistory(c);
            c.strikeN = 0;
        }
        return;
    }
    const float upm = hdr->unitsPerMeter > 1.0f && hdr->unitsPerMeter < 1000.0f ? hdr->unitsPerMeter : 100.0f;
    float R[16];
    const bool mirrored = viewmodel::DrawMirror(R);
    // Its strike: the tip and the blade's middle (DE_MP40_altFire_Knife: the blade along mesh +Z from the guard at 0 to the
    // tip at 32.5; tools/melee_points.py's numbers for a 1-bone mesh), its lever and blade in the off hand's aim pose frame
    // (x right, y up, z back, m) -- from the knife in the controller's frame (forward, right, up; units), the right
    // un-mirrored in left-hand mode.
    if (!c.strikeN || mirrored != g_knifeMirrored) {
        g_knifeMirrored = mirrored;
        c.strikeN = 1;
        Strike& s = c.strikes[0];
        s = Strike{};
        s.kind = kKnife;
        const float tip[3] = {0.0f, 0.0f, 32.5f}, mid[3] = {0.0f, 0.0f, 16.0f};
        std::memcpy(s.sample[0], tip, sizeof(tip));
        std::memcpy(s.sample[1], mid, sizeof(mid));
        s.n = 2;
        ResetHistory(c);
    }
    {
        Strike& s = c.strikes[0];
        const float base[3] = {0, 0, 0};
        float t[3], b[3];
        Xform(s.sample[0], kc, t);
        Xform(base, kc, b);
        auto toXr = [&](const float* r, float* out, float scale) {
            out[0] = (mirrored ? -r[1] : r[1]) * scale;
            out[1] = r[2] * scale;
            out[2] = -r[0] * scale;
        };
        toXr(t, s.o, 1.0f / upm);
        float d[3] = {t[0] - b[0], t[1] - b[1], t[2] - b[2]};
        const float l = Len(d);
        if (l > 1.0f) toXr(d, s.axis, 1.0f / l);
        s.haveLever = true;
    }
    // A jump the host knows of (the knife drawn or put back, the off hand held by HoldLost or back, the gun hand changed, a
    // recentre): no speed across it.
    const std::uint32_t epoch = (bits >> 16) & 0xFFu;
    if (epoch != c.epochSeen) {
        c.epochSeen = epoch;
        ResetHistory(c);
    }
    // A new host sample: the off hand's aim pose.
    const double ts = static_cast<double>(xrTime) * 1e-9;
    bool newSample = false;
    if (xrTime && ts != c.lastT) {
        newSample = true;
        const shared::Pose& off = hands[(flags & 4u) ? 1 : 0];
        Sample smp;
        smp.t = ts;
        smp.at = Now();
        const float gp[3] = {off.px, off.py, off.pz}, gq[4] = {off.qx, off.qy, off.qz, off.qw};
        std::memcpy(smp.gp, gp, sizeof(gp));
        std::memcpy(smp.gq, gq, sizeof(gq));
        const float hp[3] = {head.px, head.py, head.pz}, hq[4] = {head.qx, head.qy, head.qz, head.qw};
        std::memcpy(smp.hp, hp, sizeof(hp));
        std::memcpy(smp.hq, hq, sizeof(hq));
        smp.tracked = (bits & 16u) != 0;
        if (c.hn > 0) {
            const Sample& last = c.Hist(0);
            const float dts = static_cast<float>(ts - last.t);
            if (dts > 0.1f) {
                ResetHistory(c);
            } else {
                const float qd = std::fabs(smp.gq[0] * last.gq[0] + smp.gq[1] * last.gq[1] + smp.gq[2] * last.gq[2] + smp.gq[3] * last.gq[3]);
                const float turned = 2.0f * std::acos(qd > 1.0f ? 1.0f : qd);
                const float dg = Dist(smp.gp, last.gp), dh = Dist(smp.hp, last.hp);
                if (dts <= 0.0f || dg > 20.0f * dts || dh > 20.0f * dts || turned > 40.0f * dts) {
                    TRACE_LOG("melee: the knife -- a tracking jump in %.0f ms (the off hand %.2f m, the head %.2f m, turned %.0f deg) -- "
                              "no strike for 0.1 s", dts * 1000.0f, dg, dh, turned * 57.2958f);
                    ResetHistory(c);
                    c.glitchUntil = ts + 0.1;
                }
            }
        }
        c.hist[c.hhead] = smp;
        c.hhead = (c.hhead + 1) % kHist;
        if (c.hn < kHist) ++c.hn;
        c.lastT = ts;
    }
    // Soft gates: the off hand busy (a press or a release just now, a menu), the game's menus and gates, the gun's own melee
    // (the button melee: no double hit).
    const char* why = nullptr;
    const char* baseWhy = "";
    if (bits & 32u) why = "the off hand is busy (a press, the menu)";
    else if (hdr->gameUiMenu) why = "a game menu";
    else if (!offhand::BaseAvailable(pawn, inv, gun, baseWhy)) why = baseWhy;
    else if (Obj(Obj(pawn, "WorldInfo"), "Pauser")) why = "paused";
    else if (names::StateName(gun).find("Melee") != std::string::npos) why = "the gun's own melee";
    const bool on = !why;
    if (on != g_knifeOn || (!on && g_knifeWhy != why)) {
        MLOG("melee: the knife %s", on ? "on" : (std::string("off -- ") + why).c_str());
        g_knifeOn = on;
        g_knifeWhy = why ? why : "";
    }
    if (newSample) Estimate(c);
    // The drawn knife (as the bake draws it: its frame x the body's move since the view, through the mirror) and the off hand.
    float gw[16], ctrl[16], offF[16], W[16];
    bool offValid = false, two = false;
    if (!knife::CarrierFrame(gw) || !viewmodel::HandFrames(ctrl, offF, offValid, two) || !offValid) {
        ResetSwings(c);
        return;
    }
    if (g_cfg.catchUp && viewmodel::BodyMoveSinceView(W)) {
        float t[16];
        carrier::Mul16(gw, W, t);
        std::memcpy(gw, t, sizeof(gw));
    }
    if (mirrored) {
        float t[16];
        carrier::Mul16(gw, R, t);
        std::memcpy(gw, t, sizeof(gw));
    }
    const double qnow = Now();
    const bool fresh = c.hn > 0 && qnow - c.Hist(0).at < 0.05;
    float grip[3];
    GripWorld(offF, mirrored ? R : nullptr, grip);
    const float pad = 5.0f * upm / 100.0f, maxStep = 60.0f * upm / 100.0f;
    Strike& s = c.strikes[0];
    float w[3][3];
    for (int k = 0; k < s.n; ++k) Xform(s.sample[k], gw, w[k]);
    const bool armed = on && fresh && qnow <= s.armedUntil && g_knifeSkip == 0 && now >= c.quietUntil;
    bool struck = false;
    if (armed) {
        for (int k = 0; k < s.n && !struck; ++k) {
            if (s.firstArmed) {  // from the hand out: a blade already inside a body is entered from its side
                float d[3] = {w[k][0] - grip[0], w[k][1] - grip[1], w[k][2] - grip[2]};
                const float l = Len(d);
                if (l > 1.0f) {
                    const float end[3] = {w[k][0] + d[0] / l * pad, w[k][1] + d[1] / l * pad, w[k][2] + d[2] / l * pad};
                    struck = Contact(hdr, pawn, gun, c, s, grip, end, grip, now);
                }
            }
            if (struck || !s.havePrev) continue;
            float d[3] = {w[k][0] - s.prev[k][0], w[k][1] - s.prev[k][1], w[k][2] - s.prev[k][2]};
            const float l = Len(d);
            if (l < 0.25f || l > maxStep) continue;
            const float start[3] = {s.prev[k][0] - d[0] / l * pad, s.prev[k][1] - d[1] / l * pad, s.prev[k][2] - d[2] / l * pad};
            const float end[3] = {w[k][0] + d[0] / l * pad, w[k][1] + d[1] / l * pad, w[k][2] + d[2] / l * pad};
            struck = Contact(hdr, pawn, gun, c, s, start, end, grip, now);
        }
        s.firstArmed = false;
    }
    for (int k = 0; k < s.n; ++k) std::memcpy(s.prev[k], w[k], sizeof(w[k]));
    s.havePrev = true;
    if (qnow > s.armedUntil && s.loggedArm) {
        TRACE_LOG("melee: knife swing over -- peak %.1f m/s (hand %.1f), no contact", s.peak, s.peakHand);
        s.loggedArm = false;
    }
    if (g_knifeSkip > 0) --g_knifeSkip;
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
    g_gun.gates = {cfg.meleeButtSpeed, cfg.meleeHandSpeed, cfg.meleeTravel, cfg.meleeThrustSpeed, cfg.meleeThrustCos,
                   cfg.meleeThrustTravel, cfg.meleeMaxTurn, cfg.meleeSlashSpeed, cfg.meleeSlashCos, cfg.meleeBladeHandSpeed};
    // The knife: a stab along its blade, a slash across it -- a hand-held blade's speeds (the bayonet's 8 m/s slash is a
    // tip a metre out on a rifle).
    g_knife.gates = {cfg.meleeButtSpeed, cfg.meleeHandSpeed, cfg.meleeTravel, cfg.knifeThrustSpeed, cfg.knifeThrustCos,
                     cfg.knifeThrustTravel, cfg.meleeMaxTurn, cfg.knifeSlashSpeed, cfg.knifeSlashCos, cfg.knifeHandSpeed};
    MLOG("melee: physical melee %s (the shipped default; the menu's switch is the player's) -- hand %.1f, butt %.1f m/s; blade: "
         "hand %.1f, slash %.1f (cos <= %.2f), thrust %.1f m/s (cos %.2f, %.2f m, under %.1f rad/s); travel %.2f m, hold %.2f s; "
         "muzzle %d, world %d, props %d, line of sight %d, charge kill %d%s",
         cfg.meleePhysical ? "on" : "off", cfg.meleeHandSpeed, cfg.meleeButtSpeed, cfg.meleeBladeHandSpeed, cfg.meleeSlashSpeed, cfg.meleeSlashCos,
         cfg.meleeThrustSpeed, cfg.meleeThrustCos, cfg.meleeThrustTravel, cfg.meleeMaxTurn, cfg.meleeTravel, cfg.meleeHold, cfg.meleeMuzzle ? 1 : 0,
         cfg.meleeWorld ? 1 : 0, cfg.meleeProps ? 1 : 0, cfg.meleeLOS ? 1 : 0, cfg.meleeChargeKill ? 1 : 0,
         bake ? "" : "; no arm bake (Weapon.ArmIK, ViewModel=2): off");
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (pawn != g_pawn) {
        g_pawn = pawn;
        g_strikesFor = 0;
        g_gun.strikeN = 0;
        for (Recent& r : g_recent) r = Recent{};
        g_test = 0;
        g_gun.quietUntil = -1.0f;
        g_havePawnPose = false;
        ResetHistory(g_gun);
        ResetHistory(g_knife);
        g_knife.strikeN = 0;
        g_knife.quietUntil = -1.0f;
    }
    if (!hdr || !pawn || !g_bake) return;
    // The host's side, one seqlock read: the gun pose, the head, the hands, the display time and the melee bits. A torn read
    // changes nothing.
    shared::Pose gunPose{}, ray{}, head{}, hands[2]{};
    std::uint32_t flags = 0, bits = 0;
    std::int64_t xrTime = 0;
    const bool haveGun = shared::ReadGun(hdr, gunPose, ray, flags, &xrTime, &bits, &head, hands);
    if (flags == 0xFFFFFFFFu) return;
    const float now = GameTime(pawn);
    // The pawn's speed (the charge rule) and its turn / move since the last Draw (a snap turn's carry, a teleport).
    {
        float vel[3] = {0, 0, 0}, loc[3];
        const int vo = names::PropertyOffset(pawn, "Velocity");
        if (vo >= 0) names::ReadVector(pawn + vo, vel);
        g_speedHist[g_speedHead] = Len(vel);
        g_speedAt[g_speedHead] = now;
        g_speedHead = (g_speedHead + 1) % 16;
        names::ReadVector(pawn + addr::kActorLocation, loc);
        const int yawI = static_cast<int>(ReadU32(pawn + addr::kActorRotation + 4));
        const float yaw = static_cast<float>(yawI & 0xFFFF);
        if (g_havePawnPose) {
            float dy = std::fabs(yaw - g_lastYaw);
            if (dy > 32768.0f) dy = 65536.0f - dy;
            const float upm0 = hdr->unitsPerMeter > 1.0f && hdr->unitsPerMeter < 1000.0f ? hdr->unitsPerMeter : 100.0f;
            if (dy > 1820.0f || Dist(loc, g_lastLoc) > 0.6f * upm0) g_skipDraws = g_knifeSkip = 2;  // (10 deg, 0.6 m in one Draw)
        }
        g_lastYaw = yaw;
        std::memcpy(g_lastLoc, loc, sizeof(loc));
        g_havePawnPose = true;
    }
    // The off-hand knife's strikes (whatever the gun's switch and state).
    KnifeDraw(hdr, pawn, hands, head, xrTime, bits, flags, now);
    // Hard gates (the speed history starts again): the switch, the host, the gun.
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    const char* hard = nullptr;
    if (!(bits & 1u)) hard = "switched off";
    else if (!haveGun || !bridge::HostRunning()) hard = "no gun pose";
    else if (!gun || names::IsA(gun, "EALAGrenade") || !names::IsA(gun, "EALAWeapon")) hard = "no gun in hand";
    if (hard) {
        if (g_wasOn || g_why != hard) {
            if (g_wasOn) MLOG("melee: off -- %s", hard);
            g_wasOn = false;
            g_why = hard;
        }
        ResetHistory(g_gun);
        return;
    }
    // The gun in hand's strikes (again on a switch or an upgrade; the levers too).
    const std::uintptr_t att = Obj(pawn, "CurrentWeaponAttachment");
    const int level = Int(gun, "CurrentUpgradeLevel", -1);
    if (gun != g_strikesFor || level != g_strikesLevel || att != g_strikesAtt) {
        BuildStrikes(gun, att, level);
        ResetHistory(g_gun);
    }
    // A gun-pose jump the host knows of (the foregrip taken or let go, a hand held by HoldLost or back, the gun hand
    // changed, a recentre): no speed across it.
    const std::uint32_t epoch = (bits >> 8) & 0xFFu;
    if (epoch != g_epochSeen || (flags & 6u) != (g_flagsSeen & 6u) || hdr->recenterSeq != g_recenterSeen) {
        g_epochSeen = epoch;
        g_flagsSeen = flags;
        g_recenterSeen = hdr->recenterSeq;
        ResetHistory(g_gun);
        g_skipDraws = 2;
    }
    // A new host sample: into the history (a gap or a hand jump starts it again).
    const double ts = static_cast<double>(xrTime) * 1e-9;
    bool newSample = false;
    if (xrTime && ts != g_gun.lastT) {
        newSample = true;
        Sample smp;
        smp.t = ts;
        smp.at = Now();
        const float gp[3] = {gunPose.px, gunPose.py, gunPose.pz};
        std::memcpy(smp.gp, gp, sizeof(gp));
        const float gq[4] = {gunPose.qx, gunPose.qy, gunPose.qz, gunPose.qw};
        std::memcpy(smp.gq, gq, sizeof(gq));
        const float hp[3] = {head.px, head.py, head.pz};
        std::memcpy(smp.hp, hp, sizeof(hp));
        const float hq[4] = {head.qx, head.qy, head.qz, head.qw};
        std::memcpy(smp.hq, hq, sizeof(hq));
        const shared::Pose& off = hands[(flags & 4u) ? 1 : 0];
        const float op[3] = {off.px, off.py, off.pz};
        std::memcpy(smp.op, op, sizeof(op));
        smp.two = (flags & 2u) != 0;
        smp.tracked = (bits & 2u) != 0;
        if (g_gun.hn > 0) {
            const Sample& last = g_gun.Hist(0);
            const float dts = static_cast<float>(ts - last.t);
            if (dts > 0.1f) {
                ResetHistory(g_gun);
            } else {
                // Over 20 m/s for the gun hand, the head or (two-handed: it steers the gun) the off hand, or the gun turning
                // over 40 rad/s in one sample: tracking, not a swing.
                const float qd = std::fabs(smp.gq[0] * last.gq[0] + smp.gq[1] * last.gq[1] + smp.gq[2] * last.gq[2] + smp.gq[3] * last.gq[3]);
                const float turned = 2.0f * std::acos(qd > 1.0f ? 1.0f : qd);
                const float dg = Dist(smp.gp, last.gp), dh = Dist(smp.hp, last.hp), dof = smp.two && last.two ? Dist(smp.op, last.op) : 0.0f;
                if (dts <= 0.0f || dg > 20.0f * dts || dh > 20.0f * dts || dof > 20.0f * dts || turned > 40.0f * dts) {
                    TRACE_LOG("melee: a tracking jump in %.0f ms (the gun hand %.2f m, the head %.2f m, the off hand %.2f m, the gun "
                              "turned %.0f deg) -- no strike for 0.1 s", dts * 1000.0f, dg, dh, dof, turned * 57.2958f);
                    ResetHistory(g_gun);
                    g_gun.glitchUntil = ts + 0.1;
                }
            }
        }
        g_gun.hist[g_gun.hhead] = smp;
        g_gun.hhead = (g_gun.hhead + 1) % kHist;
        if (g_gun.hn < kHist) ++g_gun.hn;
        g_gun.lastT = ts;
    }
    // Soft gates (contact refused, the history kept): busy, menus, the pause, the weapon's state, the game's own gates.
    const char* why = nullptr;
    const char* baseWhy = "";
    if (bits & 4u) why = "the gun hand is busy (a holster, the pouch, a reload, the menu)";
    else if (hdr->gameUiMenu) why = "a game menu";
    else if (!offhand::BaseAvailable(pawn, inv, gun, baseWhy)) why = baseWhy;
    else if (Obj(inv, "PendingWeapon")) why = "a weapon switch";
    else if (Obj(Obj(pawn, "WorldInfo"), "Pauser")) why = "paused";
    else if (!StrikeState(names::StateName(gun))) why = "the weapon is busy";
    else if (!g_gun.strikeN) why = "no strike points";
    const bool on = !why;
    if (on != g_wasOn || (!on && g_why != why)) {
        MLOG("melee: %s", on ? "on" : (std::string("off -- ") + why).c_str());
        g_wasOn = on;
        g_why = why ? why : "";
    }
    // The drawn gun: the mesh x its LocalToWorld x the bake's move (catch-up included) x the left hand's mirror.
    const std::uintptr_t comp = Obj(att, "Mesh");
    const int lo = comp ? names::PropertyOffset(comp, "LocalToWorld") : -1;
    float D[16], ctrl[16], off[16], R[16], Wm[16];
    bool offValid = false, two = false;
    if (lo < 0 || !armsik::BakedMove(comp, D) || !viewmodel::HandFrames(ctrl, off, offValid, two)) {
        ResetSwings(g_gun);
        return;
    }
    float l2w[16], G[16];
    if (!SafeCopy(l2w, comp + lo, sizeof(l2w))) return;
    carrier::Mul16(l2w, D, G);
    const bool mirrored = viewmodel::DrawMirror(R);
    if (mirrored) {
        float t[16];
        carrier::Mul16(G, R, t);
        std::memcpy(G, t, sizeof(G));
    }
    float pos[3], axes[3][3], upm = 100.0f;
    if (!view::PoseFrameToWorld(gunPose, pos, axes, upm) || upm <= 1.0f) return;
    // The levers, only while the weapon is quiet (Active for 0.3 s, no shot or ammo change for 0.5 s: not mid-raise, not
    // kicking); a gun without them can't strike yet.
    {
        const DWORD tick = GetTickCount();
        const int flash = Byte(pawn, "FlashCount"), ammo = Int(gun, "AmmoCount", -1);
        const std::string state = names::StateName(gun);
        if (flash != g_flashSeen) g_flashSeen = flash, g_flashAt = tick;
        if (ammo != g_ammoSeen) g_ammoSeen = ammo, g_ammoAt = tick;
        if (state != g_stateSeen) g_stateSeen = state, g_quietSince = tick;
        if (mirrored != g_leverMirrored) {  // the other hand: its levers again from a quiet gun
            for (int i = 0; i < g_gun.strikeN; ++i) g_gun.strikes[i].haveLever = false;
            g_leverMirrored = mirrored;
        }
        const bool quiet = state == "Active" && tick - g_flashAt > 500 && tick - g_ammoAt > 500 && tick - g_quietSince > 300;
        if (quiet) {
            const bool carry = g_cfg.catchUp && viewmodel::BodyMoveSinceView(Wm);
            float Winv[16], ctrlInv[16];
            if (carry) RigidInverse(Wm, Winv);
            RigidInverse(ctrl, ctrlInv);
            TakeLevers(G, ctrlInv, mirrored ? R : nullptr, carry ? Winv : nullptr, mirrored, upm);
        }
    }
    if (newSample) Estimate(g_gun);
    // The contact: each armed strike's points swept from last Draw's drawn positions to this one's (from 10 units before
    // to 10 past: a braked stroke, a start just inside); a newly armed strike also from the hand out to each point.
    const double qnow = Now();
    const bool fresh = g_gun.hn > 0 && qnow - g_gun.Hist(0).at < 0.05;  // (the host's newest sample under 50 ms old)
    float grip[3];
    GripWorld(ctrl, mirrored ? R : nullptr, grip);
    const float pad = 10.0f * upm / 100.0f, maxStep = 60.0f * upm / 100.0f;
    bool struck = false;
    for (int i = 0; i < g_gun.strikeN; ++i) {
        Strike& s = g_gun.strikes[i];
        float w[3][3];
        for (int k = 0; k < s.n; ++k) Xform(s.sample[k], G, w[k]);
        const bool armed = on && fresh && qnow <= s.armedUntil && g_skipDraws == 0 && now >= g_gun.quietUntil;
        if (armed && !struck) {
            for (int k = 0; k < s.n && !struck; ++k) {
                if (s.firstArmed) {  // from the hand out: a point already inside a body is entered from its side
                    float d[3] = {w[k][0] - grip[0], w[k][1] - grip[1], w[k][2] - grip[2]};
                    const float l = Len(d);
                    if (l > 1.0f) {
                        const float end[3] = {w[k][0] + d[0] / l * pad, w[k][1] + d[1] / l * pad, w[k][2] + d[2] / l * pad};
                        struck = Contact(hdr, pawn, gun, g_gun, s, grip, end, grip, now);
                    }
                }
                if (struck || !s.havePrev) continue;
                float d[3] = {w[k][0] - s.prev[k][0], w[k][1] - s.prev[k][1], w[k][2] - s.prev[k][2]};
                const float l = Len(d);
                if (l < 0.25f || l > maxStep) continue;  // (still, or a jump)
                const float start[3] = {s.prev[k][0] - d[0] / l * pad, s.prev[k][1] - d[1] / l * pad, s.prev[k][2] - d[2] / l * pad};
                const float end[3] = {w[k][0] + d[0] / l * pad, w[k][1] + d[1] / l * pad, w[k][2] + d[2] / l * pad};
                struck = Contact(hdr, pawn, gun, g_gun, s, start, end, grip, now);
            }
            s.firstArmed = false;
        }
        for (int k = 0; k < s.n; ++k) std::memcpy(s.prev[k], w[k], sizeof(w[k]));
        s.havePrev = true;
        if (qnow > s.armedUntil && s.loggedArm) {
            TRACE_LOG("melee: %s swing over -- peak %.1f m/s (hand %.1f), no contact", kKindName[s.kind], s.peak, s.peakHand);
            s.loggedArm = false;
        }
    }
    if (g_skipDraws > 0) --g_skipDraws;
}

// --- tests -----------------------------------------------------------------------------------------------------------

namespace {

// A bone of an actor's skeletal mesh in the world (SpaceBases x LocalToWorld: the pose the engine last computed).
bool BoneWorld(std::uintptr_t actor, const char* bone, float (&out)[3]) {
    const std::uintptr_t comp = Obj(actor, "Mesh");
    const std::uintptr_t mesh = Obj(comp, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    const int so = comp ? names::PropertyOffset(comp, "SpaceBases") : -1, lo = comp ? names::PropertyOffset(comp, "LocalToWorld") : -1;
    if (!data || so < 0 || lo < 0) return false;
    const std::uintptr_t bases = names::ReadPointer(comp + so);
    const int nb = static_cast<int>(ReadU32(comp + so + 4));
    for (int i = 0; i < num && i < nb && i < 256; ++i) {
        if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) != bone) continue;
        float m[16], l2w[16];
        if (!SafeCopy(m, bases + 64u * static_cast<std::uintptr_t>(i), sizeof(m)) || !SafeCopy(l2w, comp + lo, sizeof(l2w))) return false;
        Xform(m + 12, l2w, out);
        return true;
    }
    return false;
}

// The nearest living axis soldier (any distance; under 200 Health first: not the 850 MG42 elite), or allied one.
std::uintptr_t NearestAxis(std::uintptr_t pawn, const float (&from)[3], bool ally = false) {
    std::uintptr_t best = 0;
    float bestD = 1e30f;
    std::uintptr_t q = Obj(Obj(pawn, "WorldInfo"), "PawnList");
    for (int n = 0; q && n < 512; q = Obj(q, "NextPawn"), ++n) {
        if (q == pawn || !names::IsA(q, ally ? "MOHAAlliedAIPawn" : "MOHAAxisAIPawn") || Int(q, "Health", 0) <= 0) continue;
        float loc[3];
        names::ReadVector(q + addr::kActorLocation, loc);
        float d = Dist(from, loc);
        if (Int(q, "Health", 0) > 200) d += 1e6f;
        if (d < bestD) best = q, bestD = d;
    }
    return best;
}

// The kept test soldier, if still a pawn in the world (a kill and a purge may reuse his address).
std::uintptr_t TestSoldier(std::uintptr_t pawn) {
    if (!g_test) return 0;
    std::uintptr_t q = Obj(Obj(pawn, "WorldInfo"), "PawnList");
    for (int n = 0; q && n < 512; q = Obj(q, "NextPawn"), ++n)
        if (q == g_test) return names::IsA(q, "MOHAAIPawn") && !Bit(q, "bDeleteMe") ? q : 0;
    return 0;
}

void Status() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn(), gun = Obj(pawn, "Weapon");
    const std::uintptr_t t = TestSoldier(pawn);
    const int bo = t ? names::PropertyOffset(t, "LastHitRigdBodyBoneName") : -1;
    const std::string bone = bo >= 0 ? names::NameAt(t + bo) : "-";
    // The player's melee kills (the stats screens' count, PRI SPStatsGlobal.iMeleeKills, the native OnMeleeKill's): the
    // credit's oracle.
    const std::uintptr_t pri = Obj(Obj(pawn, "Controller"), "PlayerReplicationInfo");
    const int meleeKills = pri ? static_cast<int>(ReadU32(pri + addr::kPriMeleeKills)) : -1;
    const std::string onOff = g_wasOn ? "on" : std::string("off -- ") + g_why;
    MLOG("melee: status -- target %s: Health %d, numHits %d, last hit %s on %s, state %s; in hand %s (state %s, fire mode %d, "
         "ammo %d); strikes %u, melee kills %d, %s",
         t ? names::Name(t).c_str() : "none", t ? Int(t, "Health", -1) : -1, t ? Int(t, "numHits", -1) : -1,
         t ? names::Name(Obj(t, "LastHitDamageType")).c_str() : "-", bone.c_str(), t ? names::StateName(t).c_str() : "-",
         names::Name(gun).c_str(), names::StateName(gun).c_str(), Byte(gun, "CurrentFireMode"), Int(gun, "AmmoCount", -1), g_hits, meleeKills,
         onOff.c_str());
}

}  // namespace

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr melee", 12) != 0) return false;
    shared::Header* hdr = bridge::SharedHeader();
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    float eye[3], fwd[3], upm = 100.0f;
    const bool haveEye = Head(hdr, eye, fwd, upm);
    float dist = 0.9f, right = 0.0f;
    int n = 0;
    if (!std::wcscmp(line, L"mohavr melee where")) {
        const std::uintptr_t t = TestSoldier(pawn);
        float sp[3] = {0, 0, 0}, loc[3] = {0, 0, 0};
        const bool haveSpine = t && BoneWorld(t, "Spine2", sp);
        if (t) names::ReadVector(t + addr::kActorLocation, loc);
        MLOG("melee: where -- target %s at %.0f %.0f %.0f, Spine2 %s %.0f %.0f %.0f; eye %.0f %.0f %.0f", t ? names::Name(t).c_str() : "none",
             loc[0], loc[1], loc[2], haveSpine ? "at" : "unknown", sp[0], sp[1], sp[2], eye[0], eye[1], eye[2]);
        for (const Channel* ch : {&g_gun, &g_knife})
            for (int i = 0; i < ch->strikeN; ++i)
                for (int k = 0; k < ch->strikes[i].n; ++k) {
                    const float* w = ch->strikes[i].prev[k];
                    MLOG("melee:   %s point %d at %.0f %.0f %.0f -- %.2f m from Spine2 (dx %.2f dy %.2f dz %.2f)", kKindName[ch->strikes[i].kind], k,
                         w[0], w[1], w[2], haveSpine ? Dist(w, sp) / upm : -1.0f, (w[0] - sp[0]) / upm, (w[1] - sp[1]) / upm, (w[2] - sp[2]) / upm);
                }
    } else if (!std::wcscmp(line, L"mohavr melee") || !std::wcscmp(line, L"mohavr melee status")) {
        Status();
        for (int i = 0; i < g_gun.strikeN + g_knife.strikeN; ++i) {
            const Strike& s = i < g_gun.strikeN ? g_gun.strikes[i] : g_knife.strikes[i - g_gun.strikeN];
            MLOG("melee:   %s -- %.1f m/s, hand %.1f (along %.1f), %.2f m in 0.25 s, turning %.1f rad/s, %s, %s; lever %.2f %.2f %.2f m%s; "
                 "drawn at %.0f %.0f %.0f",
                 kKindName[s.kind], s.speed, s.hand, s.along, s.travel, s.turn, s.ready ? "ready" : "spent",
                 Now() <= s.armedUntil ? "ARMED" : "not armed", s.o[0], s.o[1], s.o[2], s.haveLever ? "" : " (none yet)", s.prev[0][0],
                 s.prev[0][1], s.prev[0][2]);
        }
    } else if (swscanf_s(line, L"mohavr melee enemy health %d", &n) == 1) {
        const std::uintptr_t t = TestSoldier(pawn);
        const int o = t ? names::PropertyOffset(t, "Health") : -1;
        if (o >= 0) {
            *reinterpret_cast<int*>(t + o) = n;  // (Pawn.SetHealth clamps to the class default: a test write)
            MLOG("melee: test -- %s's Health set to %d", names::Name(t).c_str(), n);
        } else {
            MLOG("melee: test -- no test soldier (mohavr melee enemy first)");
        }
    } else if (!std::wcscmp(line, L"mohavr melee enemy off")) {
        const std::uintptr_t t = TestSoldier(pawn);
        if (t) SetBit(Obj(t, "Controller"), "bAIOff", false);
        MLOG("melee: test -- %s's AI on again, forgotten", t ? names::Name(t).c_str() : "none");
        g_test = 0;
    } else if (!std::wcsncmp(line, L"mohavr melee enemy", 18)) {
        // "mohavr melee enemy at <butt|grip|front|bayonet|knife> [depth]": his Spine2 depth m past that strike point;
        // "mohavr melee enemy [dist [right]] [ally]": dist m ahead (the nearest allied soldier with "ally").
        float depth = 0.05f;
        int atKind = -1;
        wchar_t kindName[16] = L"";
        if (swscanf_s(line, L"mohavr melee enemy at %15ls %f", kindName, static_cast<unsigned>(16), &depth) >= 1)
            for (int k = 0; k < kKinds; ++k) {
                wchar_t w[16];
                swprintf_s(w, L"%hs", kKindName[k]);
                if (!std::wcscmp(w, kindName)) atKind = k;
            }
        const bool atPoint = atKind >= 0;
        const bool ally = std::wcsstr(line, L" ally") != nullptr;
        if (!atPoint) swscanf_s(line, L"mohavr melee enemy %f %f", &dist, &right);
        if (!pawn || !haveEye) {
            MLOG("melee: test -- no pawn or head");
            return true;
        }
        float ploc[3];
        names::ReadVector(pawn + addr::kActorLocation, ploc);
        const std::uintptr_t kept = TestSoldier(pawn);
        const bool keep = kept && Int(kept, "Health", 0) > 0 && names::IsA(kept, ally ? "MOHAAlliedAIPawn" : "MOHAAxisAIPawn");
        const std::uintptr_t t = keep ? kept : NearestAxis(pawn, ploc, ally);
        if (!t) {
            MLOG("melee: test -- no living %s soldier", ally ? "allied" : "axis");
            return true;
        }
        // In front of the player (the head's heading), at the player's height: at least 0.85 m centre to centre (closer, the
        // engine refuses the move: an AI pawn encroaches on the player).
        if (dist < 0.85f) dist = 0.85f;
        float f[3] = {fwd[0], fwd[1], 0.0f};
        const float fl = Len(f);
        if (fl < 1e-3f) return true;
        f[0] /= fl, f[1] /= fl;
        const float r[3] = {-f[1], f[0], 0.0f};  // (UE: +Y right of +X)
        float spot[3] = {ploc[0] + (f[0] * dist + r[0] * right) * upm, ploc[1] + (f[1] * dist + r[1] * right) * upm, ploc[2]};
        const Strike* at = nullptr;
        for (const Channel* ch : {&g_gun, &g_knife})
            for (int i = 0; i < ch->strikeN; ++i)
                if (ch->strikes[i].kind == atKind && ch->strikes[i].havePrev) at = &ch->strikes[i];
        if (atPoint && !at) MLOG("melee: test -- the gun in hand has no %ls strike drawn", kindName);
        if (at) {
            // His Spine2 `depth` m past that point along the heading: his Location keeps its offset from the spine; at least
            // 0.85 m from the player's centre (closer, the move is refused).
            float sp[3], loc[3];
            if (BoneWorld(t, "Spine2", sp)) {
                names::ReadVector(t + addr::kActorLocation, loc);
                const float* b = at->prev[0];
                for (int j = 0; j < 2; ++j) spot[j] = b[j] + f[j] * depth * upm - (sp[j] - loc[j]);
                const float dx = spot[0] - ploc[0], dy = spot[1] - ploc[1];
                const float c = std::sqrt(dx * dx + dy * dy) / upm;
                if (c < 0.85f) {
                    MLOG("melee: test -- that spot is %.2f m from the player: moved out to 0.85 m", c);
                    spot[0] = ploc[0] + dx / c * 0.85f;
                    spot[1] = ploc[1] + dy / c * 0.85f;
                }
            } else {
                MLOG("melee: test -- no Spine2 on %s: placed by the distance", names::Name(t).c_str());
            }
        }
        float from[3];
        names::ReadVector(t + addr::kActorLocation, from);
        constexpr float kToUnr = 32768.0f / 3.14159265f;
        const int rot[3] = {0, static_cast<int>(std::atan2(-f[1], -f[0]) * kToUnr), 0};  // facing the player
        Call mv(t, "ClientSetLocation");
        const bool ran = mv.ok && mv.Set("NewLocation", spot, sizeof(spot)) && mv.Set("NewRotation", rot, sizeof(rot)) && mv.Run();
        float to[3];
        names::ReadVector(t + addr::kActorLocation, to);
        const std::uintptr_t c = Obj(t, "Controller");
        SetBit(c, "bAIOff", true);
        const bool stasis = Bit(t, "bStasis");
        SetBit(t, "bStasis", false);  // (a sleeping AI pawn does not tick: his mesh, and its collision, stay where he was)
        g_test = t;
        MLOG("melee: test enemy %s (Health %d, controller %s) %s from %.0f %.0f %.0f to %.0f %.0f %.0f (asked %.0f %.0f %.0f), %.2f m "
             "centre to centre -- ClientSetLocation %s; AI off%s",
             names::Name(t).c_str(), Int(t, "Health", -1), names::Name(c).c_str(), Dist(from, to) > 1.0f ? "moved" : "NOT moved", from[0],
             from[1], from[2], to[0], to[1], to[2], spot[0], spot[1], spot[2], Dist(ploc, to) / upm, ran ? "ran" : "FAILED",
             stasis ? ", woken from stasis" : "");
    } else if (!std::wcsncmp(line, L"mohavr melee hit", 16)) {
        // The strike executor alone: a 50-unit segment at the test soldier's chest (head: his head), toward him from the player.
        const std::uintptr_t gun = Obj(pawn, "Weapon");
        const std::uintptr_t t = TestSoldier(pawn);
        if (!t || !gun || !haveEye) {
            MLOG("melee: test hit -- no test soldier / gun");
            return true;
        }
        const bool head = std::wcsstr(line, L"head") != nullptr;
        float tl[3], sp[3];
        names::ReadVector(t + addr::kActorLocation, tl);
        const bool haveSpine = BoneWorld(t, head ? "Head" : "Spine2", sp);
        float d[3] = {tl[0] - eye[0], tl[1] - eye[1], 0.0f};
        const float l = Len(d);
        d[0] /= l, d[1] /= l;
        std::uint8_t impact[kImpact] = {};
        std::uintptr_t a = 0;
        // At the bone (Spine2 / Head) if known, else at heights about his Location; the first one that meets a pawn.
        const float heights[] = {0.0f, -40.0f, -20.0f, 20.0f, 40.0f, 60.0f};
        for (float h : heights) {
            const float cc[3] = {haveSpine ? sp[0] : tl[0], haveSpine ? sp[1] : tl[1], (haveSpine ? sp[2] : tl[2]) + h};
            const float start[3] = {cc[0] - d[0] * 40.0f, cc[1] - d[1] * 40.0f, cc[2]}, end[3] = {cc[0] + d[0] * 10.0f, cc[1] + d[1] * 10.0f, cc[2]};
            std::memset(impact, 0, sizeof(impact));
            if (!Trace(pawn, gun, start, end, impact)) continue;
            std::memcpy(&a, impact, 4);
            MLOG("melee: test hit -- at %+.0f from %s: %s (%s)", h, haveSpine ? (head ? "Head" : "Spine2") : "his Location",
                 a ? names::Name(a).c_str() : "nothing", names::NameAt(reinterpret_cast<std::uintptr_t>(impact + kImpactBone)).c_str());
            if (a && names::IsA(a, "Pawn")) break;
        }
        int dmg = 0, before = -1, after = -1;
        const bool ok = a && names::IsA(a, "Pawn") && ApplyHit(pawn, gun, impact, false, -1.0f, GameTime(pawn), dmg, before, after);
        MLOG("melee: test hit -- %s (%s): %d damage, Health %d -> %d%s", names::Name(a).c_str(),
             names::NameAt(reinterpret_cast<std::uintptr_t>(impact + kImpactBone)).c_str(), dmg, before, after, ok ? "" : " (not applied)");
        Status();
    } else {
        return false;
    }
    return true;
}

}  // namespace mohavr::melee
