#include "knife.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "carrier.hpp"
#include "log.hpp"
#include "names.hpp"
#include "offhand.hpp"
#include "reload.hpp"
#include "script_call.hpp"
#include "viewmodel.hpp"

namespace mohavr::knife {
using namespace script;  // Call, Obj, Int, Float (script_call.hpp)
namespace {

Config        g_cfg;
bool          g_bake = false;
carrier::Slot g_carrier;
std::uintptr_t g_pawn = 0;
bool          g_forward = true;          // the grip: blade forward (out of the thumb side), else the game's icepick
float         g_knifeInCtrl[16];         // the knife mesh in the off controller's frame (adjusted)
float         g_knifeBase[16];           // ... as placed, before the player's adjustment (the menu's Knife grip page)
float         g_adj[6] = {};             // the adjustment applied (hdr->knifeAdj)
float         g_adjUpm = 0.0f;
float         g_handInCtrl[16];          // the off hand (its bone frame) in the off controller's frame
bool          g_placed = false;
const float*  g_fingers = nullptr;
const char* const* g_fingerNames = nullptr;
bool          g_grip = false;
// The host's hold (knifeFlags, v25) and what the game publishes about it.
std::uint32_t g_pawnSeq = 0;
std::uintptr_t g_template = 0;      // found once per pawn (the MP40's classes are cooked into every level package)
bool          g_templateTried = false;
bool          g_earned = false;     // the Dagger: the save's MP40 upgrade level >= 2 ([Knife] Require=carried: and an MP40 carried)
DWORD         g_slowAt = 0;
bool          g_slowDone = false;
std::uint32_t g_refusal = 0;        // knifeState bits 8-15
DWORD         g_retryAt = 0;        // a failed draw is tried again from then
bool          g_wantWas = false;
bool          g_pending = false;      // wanted, not drawn yet
bool          g_testHold = false;     // "mohavr knife carrier on": held whatever the host says, until "carrier off"

enum Refusal : std::uint32_t { kUnavailable = 1, kNotEarned = 2, kNoTemplate = 3, kDrawFailed = 4 };

// Object.FindObject(path, cls) through `ctx` (an unnumbered static native), as offpistol::FindByPath.
std::uintptr_t FindByPath(std::uintptr_t ctx, const wchar_t* path, std::uintptr_t cls) {
    if (!ctx || !cls) return 0;
    struct FString {
        const wchar_t* data;
        int            num, max;
    } s{path, static_cast<int>(std::wcslen(path)) + 1, static_cast<int>(std::wcslen(path)) + 1};
    Call find(ctx, "FindObject");
    if (!find.Set("ObjectName", &s, sizeof(s)) || !find.Set("ObjectClass", &cls, sizeof(cls)) || !find.Run()) return 0;
    return find.ReturnObject();
}

// The MP40's knife: the class default Attachment_MP40's KnifeMesh subobject (a MOHASkeletalMeshComponent holding
// GCm_Wpn_MP_altFire.SkeletalMesh.DE_MP40_altFire_Knife), cooked into the level packages with the MP40's classes.
std::uintptr_t Template(std::uintptr_t pawn, bool log) {
    const std::uintptr_t arms = Obj(pawn, "FPArms");
    const std::uintptr_t cls = arms ? names::ReadPointer(arms + addr::kObjectClass) : 0;
    const std::uintptr_t t = FindByPath(pawn, L"MOHAGameNonNative.Default__Attachment_MP40.KnifeMeshComponent", cls);
    if (log)
        MLOG("knife: the template %s (%s), mesh %s, depth group %d", t ? names::Name(t).c_str() : "not found",
             t ? names::ClassName(t).c_str() : "-", t ? names::Name(Obj(t, "SkeletalMesh")).c_str() : "-",
             t ? (names::PropertyOffset(t, "DepthPriorityGroup") >= 0 ? static_cast<int>(ReadU32(t + names::PropertyOffset(t, "DepthPriorityGroup")) & 0xFF) : -1) : -1);
    return t;
}

void Mul(const float* a, const float* b, float* out) { carrier::Mul16(a, b, out); }

// UE3 FRotationMatrix rows for a rotator (65536ths), with an origin.
void Rotator(int pitch, int yaw, int roll, const float (&t)[3], float (&m)[16]) {
    const float k = 3.14159265f / 32768.0f;
    const float p = pitch * k, y = yaw * k, r = roll * k;
    const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    const float o[16] = {cp * cy, cp * sy, sp, 0,
                         sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp, 0,
                         -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp, 0,
                         t[0], t[1], t[2], 1};
    std::memcpy(m, o, sizeof(o));
}

// The knife's mesh in the off hand's bone frame: the mesh's RotOrigin (0, -16384, 16384: mesh Z forward, Y down, X left --
// the same as the guns', offpistol::Place) x the arms' KnifeSocket on LeftHand (8, 2.5, 0; pitch -20024, roll 3640: the
// game's icepick grip, the blade out of the pinky side). Forward: turned 180 deg about the blade's flat (mesh X) through the
// handle's middle (mesh z -2) -- the blade out of the thumb side.
void KnifeInHand(float (&k)[16]) {
    const float rotOrigin[16] = {0, -1, 0, 0, 0, 0, -1, 0, 1, 0, 0, 0, 0, 0, 0, 1};
    float socket[16];
    Rotator(-20024, 0, 3640, {8.0f, 2.5f, 0.0f}, socket);
    float kh[16];
    Mul(rotOrigin, socket, kh);
    if (!g_forward) {
        std::memcpy(k, kh, sizeof(k));
        return;
    }
    const float c = -2.0f;  // (mesh z of the handle's middle)
    // p' = (p - c) x Rx(180) + c: x -> x, y -> -y, z -> 2c - z
    const float flip[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 2.0f * c, 1};
    Mul(flip, kh, k);
}

// Where it sits: the knife on the off hand as the hand is now against its controller (the free hand's relation, from the
// last bake -- so the hand doesn't move when it takes the knife); the fingers closed as on the stick grenade's handle.
bool Place() {
    g_placed = false;
    float rel[16];
    if (!armsik::FreeHandRel(rel)) {
        MLOG("knife: no free hand frame yet -- not placed");
        return false;
    }
    float k[16];
    KnifeInHand(k);
    std::memcpy(g_handInCtrl, rel, sizeof(rel));
    Mul(k, rel, g_knifeInCtrl);
    if (g_forward) {
        // The fist holds a controller's handle at a slant: the blade out of it points ~45 deg up from the controller's
        // forward. Turned about the blade's flat (mesh X) through the handle so it points along the controller (then
        // [OffHand] KnifeTilt up): a stab is a push along the controller.
        auto turned = [&](float a, float (&out)[16]) {
            const float c = -2.0f, s = std::sin(a), co = std::cos(a);
            const float r[16] = {1, 0, 0, 0, 0, co, s, 0, 0, -s, co, 0, 0, c * s, c - c * co, 1};
            float k2[16];
            Mul(r, k, k2);
            Mul(k2, rel, out);
        };
        auto score = [&](const float (&m)[16]) {  // the blade (mesh +Z row) against the wanted direction
            const float t = g_cfg.offKnifeTilt * 0.0174533f;
            return m[8] * std::cos(t) + m[10] * std::sin(t);
        };
        float best[16], cand[16];
        std::memcpy(best, g_knifeInCtrl, sizeof(best));
        for (int i = -180; i < 180; i += 2) {  // (a coarse search: a constant per draw)
            turned(i * 0.0174533f, cand);
            if (score(cand) > score(best)) std::memcpy(best, cand, sizeof(best));
        }
        std::memcpy(g_knifeInCtrl, best, sizeof(best));
    }
    std::memcpy(g_knifeBase, g_knifeInCtrl, sizeof(g_knifeBase));
    g_adjUpm = 0.0f;  // (applied again on the next Draw)
    const float* hand = nullptr;
    g_grip = reload::GripRows("Attachment_StickGrenade", "offnade", hand, g_fingers, g_fingerNames);
    g_placed = true;
    MLOG("knife: placed (%s grip) -- the hand at %.1f %.1f %.1f in its controller's frame, the knife's origin %.1f %.1f %.1f, "
         "its blade along %.2f %.2f %.2f (fingers %s)", g_forward ? "forward" : "icepick", rel[12], rel[13], rel[14],
         g_knifeInCtrl[12], g_knifeInCtrl[13], g_knifeInCtrl[14], g_knifeInCtrl[8], g_knifeInCtrl[9], g_knifeInCtrl[10],
         g_grip ? "the stick grenade's" : "open");
    return true;
}

// The player's adjustment of the hold (the menu's Knife grip page): the knife moved in the off controller's frame (x
// forward, y right, z up: cm), turned about its handle's middle (tilt about y, turn about z, roll about x). The same numbers
// in left-hand mode give the mirror image (the frame is the mirror world's).
void ApplyAdj(const float (&adj)[6], float upm) {
    std::memcpy(g_adj, adj, sizeof(g_adj));
    g_adjUpm = upm;
    const float k = 65536.0f / 360.0f, s = upm / 100.0f;
    float pivot[3];
    const float mid[3] = {0.0f, 0.0f, -2.0f};
    for (int j = 0; j < 3; ++j) pivot[j] = mid[0] * g_knifeBase[j] + mid[1] * g_knifeBase[4 + j] + mid[2] * g_knifeBase[8 + j] + g_knifeBase[12 + j];
    float r[16];
    const float zero[3] = {0, 0, 0};
    Rotator(static_cast<int>(adj[3] * k), static_cast<int>(adj[4] * k), static_cast<int>(adj[5] * k), zero, r);
    // M = T(-pivot) x R x T(pivot + t)
    float m[16];
    std::memcpy(m, r, sizeof(m));
    for (int j = 0; j < 3; ++j) {
        const float rp = -(pivot[0] * r[j] + pivot[1] * r[4 + j] + pivot[2] * r[8 + j]);
        m[12 + j] = rp + pivot[j] + adj[j] * s;
    }
    Mul(g_knifeBase, m, g_knifeInCtrl);
}

// 1 drawn, 0 no free hand frame yet (try again soon), -1 failed.
int Draw(std::uintptr_t pawn) {
    const std::uintptr_t t = g_template ? g_template : Template(pawn, true);
    if (!t || !g_bake) return -1;
    if (!Place()) return 0;
    return carrier::AttachTemplate(g_carrier, pawn, t, pawn, "knife", true) ? 1 : -1;
}

void Sheathe() {
    carrier::Detach(g_carrier, "knife", true);
    g_placed = false;
}

// The knife allowed: always ([Knife] Require=any, the player after round 50: "Make the knife always available, not gated
// behind mp40 upgrade"), or the Dagger earned -- the save's applied upgrade level for the MP40 (WeaponType 3) is 2 or more,
// what the game gives the MP40 it hands out; Require=carried also wants an MP40 in the inventory.
bool Earned(std::uintptr_t pawn, int& level) {
    level = -9;
    if (g_cfg.knifeRequire == 0) return true;
    const std::uintptr_t mgr = Obj(pawn, "WeaponUpgradeManager");
    if (!mgr) return false;
    Call get(mgr, "GetAppliedUpgradeLevel");
    const std::uint8_t type = 3;
    level = get.Set("WeaponType", &type, 1) && get.Run() ? get.ReturnInt() : -9;
    if (level < 2) return false;
    if (g_cfg.knifeRequire == 1) return true;
    const std::uintptr_t inv = Obj(pawn, "InvManager");
    std::uintptr_t item = Obj(inv, "InventoryChain");
    for (int n = 0; item && n < 64; item = Obj(item, "Inventory"), ++n)
        if (names::IsA(item, "MOHA_MP40") && !Bit(item, "bDeleteMe")) return true;
    return false;
}

void Publish(shared::Header* hdr, bool installed, bool canDraw, bool buttonKnife) {
    if (!hdr) return;
    ++hdr->knifeSeq;  // odd: writing
    _ReadWriteBarrier();
    hdr->knifeCaps = (installed ? 1u : 0u) | (canDraw ? 2u : 0u) | (g_earned ? 4u : 0u) | (Holding() ? 8u : 0u) | (buttonKnife ? 64u : 0u);
    hdr->knifeState = (Holding() ? 1u : 0u) | (g_refusal << 8);
    hdr->knifeDamage = g_cfg.knifeDamage;
    hdr->knifePawnSeq = g_pawnSeq;
    _ReadWriteBarrier();
    ++hdr->knifeSeq;  // even: done
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
    g_forward = cfg.offKnifeForward;  // (the test commands' grip; the host's flags choose it for a hold)
}

bool Holding() { return CarrierComponent() != 0; }

bool Pending() { return g_pending; }

std::uintptr_t CarrierComponent() { return carrier::Component(g_carrier); }

bool CarrierFrame(float (&gw)[16]) {
    if (!CarrierComponent() || !g_placed) return false;
    float gun[16], off[16];
    bool offValid = false, two = false;
    if (!viewmodel::HandFrames(gun, off, offValid, two) || !offValid) return false;
    Mul(g_knifeInCtrl, off, gw);
    return true;
}

bool HandOnKnife(float (&rel)[16], const float*& fingers, const char* const*& names) {
    if (!CarrierComponent() || !g_placed || !g_grip) return false;
    std::memcpy(rel, g_handInCtrl, sizeof(rel));
    fingers = g_fingers;
    names = g_fingerNames;
    return true;
}

bool KnifeInController(float (&m)[16]) {
    if (!g_placed) return false;
    std::memcpy(m, g_knifeInCtrl, sizeof(m));
    return true;
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (pawn != g_pawn) {
        g_pawn = pawn;
        g_carrier = carrier::Slot{};  // (the old pawn's clone goes with it)
        g_placed = false;
        g_template = 0;
        g_templateTried = false;
        g_slowDone = false;
        g_earned = false;
        g_testHold = false;
        ++g_pawnSeq;
    }
    if (!hdr) return;
    std::uint32_t flags = 0;
    if (!shared::ReadKnifeFlags(hdr, flags)) return;  // (the host mid-write: next Draw)
    const bool on = (flags & 1u) != 0;
    if (!on && !Holding() && !g_testHold) {  // switched off with nothing held: nothing resolved or called (rule 7); the status
        g_pending = false;                     // still goes out
        Publish(hdr, false, false, false);
        return;
    }
    const std::uintptr_t inv = Obj(pawn, "InvManager"), gun = Obj(pawn, "Weapon");
    // The slow tests at 4 Hz: the template (once per pawn), the Dagger earned.
    const DWORD now = GetTickCount();
    if (pawn && (!g_slowDone || static_cast<LONG>(now - g_slowAt) >= 0)) {
        g_slowDone = true;
        g_slowAt = now + 250;
        if (!g_templateTried) {
            g_templateTried = true;
            g_template = Template(pawn, true);
        }
        int level = -9;
        const bool earned = Earned(pawn, level);
        if (earned != g_earned)
            MLOG("knife: the Dagger %s (%s)", earned ? "earned" : "not earned",
                 g_cfg.knifeRequire == 0 ? "always available" : g_cfg.knifeRequire == 1 ? "the MP40's upgrade level" : "the MP40's upgrade level, an MP40 carried");
        g_earned = earned;
    }
    const bool installed = g_template && g_bake && ObjectProcessEventOk();
    const char* why = "";
    const bool base = pawn && offhand::BaseAvailable(pawn, inv, gun, why);
    const bool canDraw = installed && g_earned && base;
    const bool buttonKnife = gun && names::IsA(gun, "MOHA_MP40") && Int(gun, "CurrentUpgradeLevel", 0) >= 2;
    const bool want = (on && (flags & 2u) && g_earned && installed) || (g_testHold && installed);
    if (want && !g_wantWas) g_retryAt = 0;  // a new hold: try at once
    g_wantWas = want;
    if (want && !Holding()) {
        // Drawn when it can be (a weapon switch, the parachute: it waits, still held by the host).
        g_forward = (flags & 4u) == 0;
        if (!base) {
            g_refusal = kUnavailable;
        } else if (static_cast<LONG>(now - g_retryAt) >= 0) {
            const int r = Draw(pawn);
            if (r > 0) {
                g_refusal = 0;
                MLOG("knife: drawn in the off hand (%s grip)", g_forward ? "forward" : "icepick");
            } else {
                // No free hand frame yet: the bake frees the hand for a pending draw (Pending), so the next try comes soon.
                g_refusal = kDrawFailed;
                g_retryAt = now + (r == 0 ? 100 : 1000);
                if (r < 0) MLOG("knife: the draw failed -- tried again in 1 s");
            }
        }
    } else if (!want && Holding()) {
        Sheathe();
        MLOG("knife: put back (%s)", !on ? "switched off" : !(flags & 2u) ? "the host let go" : !g_earned ? "the Dagger not earned" : "not installed");
    } else if (Holding() && !g_testHold && ((flags & 4u) == 0) != g_forward) {  // the grip changed while held: placed again
        g_forward = (flags & 4u) == 0;
        Place();
    }
    g_pending = want && !Holding();
    // The player's adjustment of the hold, as it changes (and after each placing).
    if (g_placed) {
        const float upm = hdr->unitsPerMeter > 1.0f && hdr->unitsPerMeter < 1000.0f ? hdr->unitsPerMeter : 100.0f;
        float adj[6];
        for (int i = 0; i < 6; ++i) {
            const float v = hdr->knifeAdj[i];
            adj[i] = std::isfinite(v) ? (i < 3 ? (v < -20.0f ? -20.0f : v > 20.0f ? 20.0f : v) : (v < -180.0f ? -180.0f : v > 180.0f ? 180.0f : v)) : 0.0f;
        }
        if (upm != g_adjUpm || std::memcmp(adj, g_adj, sizeof(adj)) != 0) {
            ApplyAdj(adj, upm);
            MLOG("knife: the hold adjusted -- %.0f %.0f %.0f cm, tilt %.0f turn %.0f roll %.0f", adj[0], adj[1], adj[2], adj[3], adj[4], adj[5]);
        }
    }
    // Why a draw can't happen now (the host's log on a refused press); a failed draw stands until one succeeds.
    if (!g_template && g_templateTried) g_refusal = kNoTemplate;
    else if (!g_earned) g_refusal = kNotEarned;
    else if (!base) g_refusal = kUnavailable;
    else if (g_refusal != kDrawFailed) g_refusal = 0;
    if (g_cfg.debugKnifeTrace) {
        static std::uint32_t was = 0xFFFFFFFFu;
        const std::uint32_t st = flags | (installed ? 0x100u : 0u) | (canDraw ? 0x200u : 0u) | (Holding() ? 0x400u : 0u) | (g_refusal << 16);
        if (st != was) {
            was = st;
            MLOG("knife: host flags 0x%X, installed %d, can draw %d%s%s, held %d, refusal %u", flags, installed, canDraw,
                 base ? "" : " -- ", base ? "" : why, Holding(), g_refusal);
        }
    }
    Publish(hdr, installed, canDraw, buttonKnife);
}

std::string TemplateStatus(std::uintptr_t pawn) {
    const std::uintptr_t t = Template(pawn, false);
    return t && Obj(t, "SkeletalMesh") ? "found (" + names::Name(Obj(t, "SkeletalMesh")) + ")" : "NOT FOUND";
}

bool TestCommand(const wchar_t* line) {
    if (wcsncmp(line, L"mohavr knife", 12) != 0) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (wcsstr(line, L"template")) {
        Template(pawn, true);
    } else if (wcsstr(line, L"carrier on")) {
        if (wcsstr(line, L"icepick")) g_forward = false;
        else if (wcsstr(line, L"forward")) g_forward = true;
        if (pawn != g_pawn) g_template = 0;  // (a new pawn: OnDraw resets the rest)
        g_testHold = true;
        Sheathe();
        MLOG("knife: test draw -> %s (held until 'carrier off')", Draw(pawn) > 0 ? "drawn" : "not yet");
    } else if (wcsstr(line, L"carrier off")) {
        g_testHold = false;
        Sheathe();
    } else {
        float gw[16];
        const bool f = CarrierFrame(gw);
        MLOG("knife: status -- %s, %s grip, frame %s (origin %.0f %.0f %.0f)", CarrierComponent() ? "held" : "not held",
             g_forward ? "forward" : "icepick", f ? "yes" : "no", f ? gw[12] : 0.0f, f ? gw[13] : 0.0f, f ? gw[14] : 0.0f);
    }
    return true;
}

}  // namespace mohavr::knife
