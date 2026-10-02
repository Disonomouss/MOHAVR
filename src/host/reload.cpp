#include "reload.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP", "INSERT (the other half)", "BOLT UP", "BOLT BACK",
                            "BOLT FORWARD", "BOLT DOWN"};
const char* kMagName[] = {"in the gun", "grabbed", "in the off hand", "out"};
// (GOAL A3: a pump gun's BOLT BACK / FORWARD are its pump's strokes.)
const char* EventName(std::uint32_t type, bool pump) {
    if (pump && type == shared::kReloadBoltBack) return "PUMP BACK";
    if (pump && type == shared::kReloadBoltForward) return "PUMP FORWARD";
    return type <= shared::kReloadBoltDown ? kEventName[type] : "?";
}

struct V3 { float x, y, z; };
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 P(const XrVector3f& v) { return {v.x, v.y, v.z}; }
XrVector3f X(V3 v) { return {v.x, v.y, v.z}; }
V3 A3(const float (&a)[3]) { return {a[0], a[1], a[2]}; }

XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf QConj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
V3 Rotate(const XrQuaternionf& q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 c = Cross(u, v), cc = Cross(u, c);
    return Add(v, Scale(Add(Scale(c, q.w), cc), 2.0f));
}
// a x b: b given in a's frame.
XrPosef Compose(const XrPosef& a, const XrPosef& b) {
    return {QMul(a.orientation, b.orientation), X(Add(P(a.position), Rotate(a.orientation, P(b.position))))};
}
// b in a's frame.
XrPosef Relative(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf ai = QConj(a.orientation);
    return {QMul(ai, b.orientation), X(Rotate(ai, Sub(P(b.position), P(a.position))))};
}
}  // namespace

void ManualReload::Init(const std::wstring& ini) {
    on_ = GetPrivateProfileIntW(L"Weapon", L"ManualReload", 0, ini.c_str()) != 0;
    auto iniFloat = [&](const wchar_t* key, float def) {
        wchar_t b[32] = L"";
        GetPrivateProfileStringW(L"ManualReload", key, L"", b, 32, ini.c_str());
        return b[0] ? static_cast<float>(_wtof(b)) : def;
    };
    wchar_t b[64] = L"";
    GetPrivateProfileStringW(L"ManualReload", L"ReleaseButton", L"upper", b, 64, ini.c_str());
    releaseButton_ = !_wcsicmp(b, L"none") ? 0 : !_wcsicmp(b, L"lower") ? 2 : 1;
    pullOut_ = iniFloat(L"PullOut", 4.0f) / 100.0f;
    insertR_ = iniFloat(L"InsertRadius", 5.0f) / 100.0f;
    insertAngle_ = iniFloat(L"InsertAngle", 40.0f);
    boltGrabR_ = iniFloat(L"BoltGrabR", 5.0f) / 100.0f;
    rackArm_ = iniFloat(L"RackArm", 0.85f);
    rackMin_ = iniFloat(L"RackMin", 4.0f) / 100.0f;
    rackTug_ = iniFloat(L"RackTug", 1.0f) / 100.0f;
    pumpArm_ = std::clamp(iniFloat(L"PumpArm", 0.85f), 0.3f, 1.0f);
    pumpTrigger_ = GetPrivateProfileIntW(L"ManualReload", L"PumpTrigger", 1, ini.c_str()) != 0;
    foreGrabTrigger_ = GetPrivateProfileIntW(L"ManualReload", L"ForeGrabTrigger", 1, ini.c_str()) != 0;
    MLOG("reload: the pump %s; the foregrip hand's trigger %s", pumpTrigger_ ? "only with the foregrip hand's trigger held" :
         "whenever the foregrip is held", foreGrabTrigger_ ? "takes a GrabTrigger gun's magazine" : "does nothing on the foregrip");
    GetPrivateProfileStringW(L"ManualReload", L"Hold", L"0 0 0", b, 64, ini.c_str());
    float h[3] = {0, 0, 0};
    swscanf_s(b, L"%f %f %f", &h[0], &h[1], &h[2]);
    for (int i = 0; i < 3; ++i) hold_[i] = h[i] / 100.0f;
    static const char* kButtons[] = {"none", "upper (B / Y)", "lower (A / X)"};
    MLOG("reload: Weapon.ManualReload=%d (the default; the menu's toggle is the player's); release button %s, pull out %.0f cm, "
         "insert within %.0f cm and %.0f deg, hold %.0f %.0f %.0f cm; the action: grab within %.0f cm, armed at %.0f%% of its "
         "travel (at least %.0f cm), a tug of %.0f cm when held back; a pump back at %.0f%% of its travel",
         on_ ? 1 : 0, kButtons[releaseButton_], pullOut_ * 100.0f, insertR_ * 100.0f, insertAngle_, h[0], h[1], h[2],
         boltGrabR_ * 100.0f, rackArm_ * 100.0f, rackMin_ * 100.0f, rackTug_ * 100.0f, pumpArm_ * 100.0f);
}

void ManualReload::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    MLOG("reload: manual reload %s (the menu)", on ? "on" : "off");
}

void ManualReload::Queue(std::uint32_t type, double now) {
    if (type < shared::kReloadEject || type > shared::kReloadBoltDown) return;
    pending_.push_back({type, keyHash_, now});
}

void ManualReload::SetMag(Mag m, const char* why) {
    if (m == mag_) return;
    MLOG("reload: magazine %s -> %s (%s)", kMagName[mag_], kMagName[m], why);
    mag_ = m;
    if (m == kInHand) flipped_ = false;  // held as it came
    if (m != kGrabbed) {
        pull_ = 0.0f;
        entering_ = false;
    }
}

void ManualReload::Poll(shared::Header* hdr, double now, bool handsOk) {
    if (!hdr) return;
    // Every event sent has been taken (read before the geometry: a state older than the acknowledgement can't pass).
    const bool acksDone = pending_.empty() && hdr->reloadEvtSeq == hdr->reloadEvtAck;
    acksDone_ = acksDone;
    const std::uint32_t pawnSeq = hdr->reloadPawnSeq;
    if (pawnSeq != pawnSeq_) {
        pawnSeq_ = pawnSeq;
        SetMag(kInGun, "a new pawn: the game starts every gun loaded");
        disagree_ = 0;
    }
    std::uint32_t seq = 0;
    shared::ReloadGeo g{};
    if (shared::ReadReloadGeo(hdr, g, seq) && seq != lastGeoSeq_) {
        lastGeoSeq_ = seq;
        geoAt_ = now;
        geo_ = g;
        keyHash_ = shared::KeyHash(geo_.key);
        const bool gameIn = (geo_.state & 1u) != 0;
        if (stateKey_ != geo_.key) {
            // Another gun in hand: what the off hand held of the old one is gone; the new one's magazine as the game has it.
            stateKey_ = geo_.key;
            disagree_ = 0;
            boltHeld_ = false;
            actHeld_ = false;  // (the review of D49: the old gun's bolt and pump holds posed the new gun's action)
            pumpHeld_ = pumpByFore_ = rackArmed_ = false;
            rack_ = 0.0f;
            SetMag(gameIn ? kInGun : kOut, "another gun in hand");
        } else if (acksDone && (geo_.caps & 1u)) {
            // Reconcile (3.2): the game's magazine differs from ours for two of its frames with nothing in flight.
            const bool hostIn = mag_ == kInGun || (mag_ == kGrabbed && !entering_);  // (sliding in: still out)
            if (hostIn != gameIn) {
                if (++disagree_ >= 2) {
                    disagree_ = 0;
                    SetMag(gameIn ? kInGun : kOut, "the game's (reconciled)");
                }
            } else {
                disagree_ = 0;
            }
        }
    }
    const bool alive = geoAt_ >= 0.0 && now - geoAt_ < 0.25;
    engaged_ = on_ && alive && handsOk && (geo_.caps & 4u) && (geo_.caps & 1u);
    if (engaged_ != loggedEngaged_) {
        loggedEngaged_ = engaged_;
        MLOG("reload: %s (%s)", engaged_ ? "engaged" : "not engaged",
             !on_ ? "switched off" : !alive ? "the game's side is quiet" : !handsOk ? "no hands" :
             !(geo_.caps & 4u) ? "no hook in the game" : !(geo_.caps & 1u) ? "no converted gun in hand" :
             "the game's side alive, hands tracked, a converted gun in hand");
    }
    if (geo_.key != loggedKey_ || geo_.state != loggedState_) {  // not every shot: the game logs the events' ammo
        loggedKey_ = geo_.key;
        loggedState_ = geo_.state;
        char act[96] = "";  // (GOAL A2 / A3: the chamber and the action)
        if (geo_.caps & (2048u | 4096u))
            snprintf(act, sizeof(act), "; %s%s%s%s%s", (geo_.caps & 2048u) ? "pump" : "bolt", (geo_.state & 256u) ? " back" : " closed",
                     (geo_.state & 128u) ? ", a spent case in" : "", (geo_.state & 8192u) ? ", the chamber empty" : "",
                     (geo_.state & 2048u) ? ", the trigger held" : "");
        MLOG("reload: %s -- clip %d/%d, reserve %d; magazine %s%s%s%s%s%s%s", geo_.key[0] ? geo_.key : "(no gun)", geo_.clip,
             geo_.max, geo_.reserve, (geo_.state & 1u) ? "in" : "out", (geo_.state & 2u) ? ", pending" : "",
             (geo_.state & 4u) ? ", ready" : "", (geo_.state & 8u) ? ", action held back" : "",
             (geo_.state & 16u) ? ", rack needed" : "", (geo_.caps & 1u) ? "" : " (not converted)", act);
        if (geo_.caps & 1u)
            MLOG("reload: %s geometry (cm, the gun frame: right up back) -- magazine grab %.1f %.1f %.1f r %.0f, out %.2f %.2f %.2f; "
                 "action grab %.1f %.1f %.1f, back %.2f %.2f %.2f, travel %.1f", geo_.key, geo_.magGrab[0] * 100.0f,
                 geo_.magGrab[1] * 100.0f, geo_.magGrab[2] * 100.0f, geo_.magGrabR * 100.0f, geo_.magOut[0], geo_.magOut[1],
                 geo_.magOut[2], geo_.boltGrab[0] * 100.0f, geo_.boltGrab[1] * 100.0f, geo_.boltGrab[2] * 100.0f,
                 geo_.boltBack[0], geo_.boltBack[1], geo_.boltBack[2], geo_.boltTravel * 100.0f);
        if (geo_.caps & 64u)
            MLOG("reload: %s -- a held magazine sits at %.1f %.1f %.1f cm from the aim point (the reload grip)", geo_.key,
                 geo_.magHeld.px * 100.0f, geo_.magHeld.py * 100.0f, geo_.magHeld.pz * 100.0f);
    }
}

bool ManualReload::Begin(const In& in) {
    const bool alive = geoAt_ >= 0.0 && in.now - geoAt_ < 0.25;
    const char* why = !on_ ? "switched off" : !engaged_ ? "not engaged" : !(geo_.caps & 1u) ? "not a converted gun" :
                      (geo_.caps & 8u) ? "alternate fire" : !alive ? "the game's side is quiet" : !in.gunOk ? "no gun hand" :
                      !in.offOk ? "no off hand" : !in.gestures ? "a menu is open" : !in.hasView ? "no head-tracked view" : "";
    const bool active = !*why;
    if (active != active_) MLOG("reload: %s%s%s", active ? "drives the gun (" : "stops driving the gun (", active ? geo_.key : why, ")");
    active_ = active;
    press_ = kPressNone;
    offTrigger_ = in.offTrigger;
    foregrip_ = in.foregrip;
    // Round 33 (the player: "as soon as I enter the menu, the off hand drops the magazine"): with only a menu in the way,
    // what the off hand holds stays in it -- so the Reload grip page shows the grip -- and nothing new starts.
    menuHold_ = !active_ && !std::strcmp(why, "a menu is open") && (mag_ == kGrabbed || mag_ == kInHand || boltHeld_);
    if (!active_ && !menuHold_) {
        // 3.1: leaving mid-gesture -- a grabbed magazine slides back, one in the hand is dropped, the action is let go
        // without a rack.
        if (pumpHeld_ && rackArmed_) {  // (GOAL A3: a pump let go after its back stroke closes)
            Queue(shared::kReloadBoltForward, in.now);
            MLOG("reload: the pump let go (not driving): it closes");
        } else if (boltHeld_) {
            MLOG("reload: the action let go (not driving): no rack");
        }
        if (pumpHeld_) rackArmed_ = false;
        pumpHeld_ = false;
        boltHeld_ = false;
        actHeld_ = false;
        if (mag_ == kGrabbed && !entering_) SetMag(kInGun, "let go: not driving");
        else if (mag_ == kInHand || mag_ == kGrabbed) {
            Queue(shared::kReloadDrop, in.now);
            SetMag(kOut, "dropped: not driving");
        }
    }
    return active_;
}

bool ManualReload::TakePress(const XrVector3f& hand, const XrVector3f& pouch, float pouchR) {
    if (!active_) return false;
    float best = 1.0f;
    Press which = kPressNone;
    // (GOAL A2: a bolt action loads a clip or a round only through its open action, with room. GOAL A3: a pump gun takes a
    // shell while its tube has room; with it full, the press is taken and nothing given.)
    const bool twoStage = (geo_.caps & 4096u) != 0, pump = (geo_.caps & 2048u) != 0;
    if (mag_ == kOut && pouchR > 0.0f && (!twoStage || ((geo_.state & 256u) && (geo_.state & 512u)))) {
        const float s = Len(Sub(P(hand), P(pouch))) / pouchR;
        if (s < best) {
            best = s;
            which = pump && !(geo_.state & 512u) ? kPressFull : kPressPouch;
        }
    }
    // Round 32 (GrabTrigger, the MP40): the magazine only with the off hand's trigger held, so the grip takes the foregrip.
    const bool magArmed = !(geo_.caps & 128u) || offTrigger_ >= 0.5f;
    // GOAL A1 (NoGrab, the Garand's clip): a seated clip isn't pulled out by hand.
    if (mag_ == kInGun && lastGunOk_ && geo_.magGrabR > 0.0f && magArmed && !(geo_.caps & 512u)) {
        const float s = Len(Sub(P(hand), P(lastGrabW_))) / (geo_.magGrabR * ringScale_ * spotAdj_[0][3]);
        if (s < best) {
            best = s;
            which = kPressMag;
        }
    }
    // (GOAL A3: a pump is the foregrip; its own grab only on a gun without one -- else it would take the foregrip's press.)
    if ((((geo_.caps & 2u) && !(pump && foregrip_)) || twoStage) && lastGunOk_ && boltGrabR_ > 0.0f) {
        const float s = Len(Sub(P(hand), P(lastBoltW_))) / (boltGrabR_ * ringScale_ * spotAdj_[1][3]);
        if (s < best) {
            best = s;
            which = kPressBolt;
        }
    }
    press_ = which;
    return which != kPressNone;
}

void ManualReload::Pulse(Out& out, int hand, float amp, float ms) const {
    out.pulseAmp[hand] = amp;
    out.pulseMs[hand] = ms;
}

void ManualReload::Frame(const In& in, Out& out) {
    const int g = in.gunHand, o = 1 - g;
    // The release button (hysteresis 0.6 / 0.4), per physical hand; a press begun while driving stays from the pad till
    // it ends (3.6).
    bool edge[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        const float v = releaseButton_ ? in.release[h] : 0.0f;
        if (!relHeld_[h] && v >= 0.6f) {
            relHeld_[h] = true;
            edge[h] = true;
            if (active_ && h == g) maskLatch_[h] = true;
        } else if (relHeld_[h] && v < 0.4f) {
            relHeld_[h] = false;
            maskLatch_[h] = false;
        }
        out.mask[h] = releaseButton_ && ((active_ && h == g) || maskLatch_[h]);
    }
    lastGunOk_ = active_ && in.gunOk;
    const V3 gunP = P(in.gun.position), offP = P(in.off.position);
    const V3 grabW = Add(gunP, Rotate(in.gun.orientation, A3(geo_.magGrab)));  // the well (the insert)
    const V3 outW = Rotate(in.gun.orientation, A3(geo_.magOut));
    // Where the hand grabs (round 33: the player's rings moved on the gun), and how big.
    const V3 magRingW = Add(grabW, Rotate(in.gun.orientation, V3{spotAdj_[0][0], spotAdj_[0][1], spotAdj_[0][2]}));
    const V3 boltW = Add(Add(gunP, Rotate(in.gun.orientation, A3(geo_.boltGrab))),
                         Rotate(in.gun.orientation, V3{spotAdj_[1][0], spotAdj_[1][1], spotAdj_[1][2]}));
    const float magRingR = geo_.magGrabR * ringScale_ * spotAdj_[0][3], boltRingR = boltGrabR_ * ringScale_ * spotAdj_[1][3];
    const V3 backW = Rotate(in.gun.orientation, A3(geo_.boltBack));
    if (!active_ && !menuHold_) {
        press_ = kPressNone;
        // The menu's Reload spots page: both grab rings, to move them.
        if (in.showSpots && in.gunOk && (geo_.caps & 1u)) {
            out.rings[out.ringCount++] = {X(magRingW), magRingR, false, true};
            if (geo_.caps & 2u) out.rings[out.ringCount++] = {X(boltW), boltRingR, false, true};
        }
        return;
    }
    lastGrabW_ = X(magRingW);
    lastBoltW_ = X(boltW);
    out.targetOk[0] = true;
    out.target[0] = X(magRingW);
    out.targetOk[2] = true;
    out.target[2] = X(grabW);
    // Insert=slide (v17): the held magazine's front goes to the well's mouth first -- its grab point MagLen behind it.
    const V3 mouthW = Add(grabW, Scale(outW, geo_.magSeat));
    const V3 insertAt = geo_.magLen > 0.0f ? Add(mouthW, Scale(outW, geo_.magLen)) : grabW;
    if (mag_ == kGrabbed && entering_) {  // sliding in: the aim point that pushes it home
        out.target[2] = X(Sub(Add(P(start_), Scale(outW, geo_.magSeat)), Sub(P(in.off.position), P(in.offAim.position))));
        out.alignOk = true;
        out.align = {in.offAim.orientation, out.target[2]};
    }
    if (mag_ == kInHand) {  // the aim point that puts the held magazine's grab point at the well (for the slide insert:
                            // where the grab point is when its front is at the mouth)
        out.target[2] = X(Sub(Sub(insertAt, Sub(P(in.off.position), P(in.offAim.position))), Rotate(in.off.orientation, P(heldRel_.position))));
        // ... and, for "align", the aim pose that also turns it as seated (the held frame = the gun's axes): the hand point
        // offset (off - offAim, fixed in the hand) and the grip turned with it.
        const XrQuaternionf qa = QMul(in.gun.orientation, QConj(heldRel_.orientation));
        const V3 hp = Rotate(QConj(in.off.orientation), Sub(P(in.off.position), P(in.offAim.position)));
        out.alignOk = true;
        out.align = {qa, X(Sub(Sub(insertAt, Rotate(qa, hp)), Rotate(qa, P(heldRel_.position))))};
    }
    if (geo_.caps & 2u) {
        out.targetOk[1] = true;
        out.target[1] = X(boltW);
    }
    // GOAL A2 (a two-stage action): the knob's path from the game (host gun frame), as world points; the knob where it
    // is now is the press candidate, and the test targets are it, lifted, and drawn back.
    const bool twoStage = (geo_.caps & 4096u) && geo_.actPathN >= 2;
    const int pathN = static_cast<int>(geo_.actPathN);
    auto actAt = [&](int i) { return Add(gunP, Rotate(in.gun.orientation, A3(geo_.actPath[i]))); };
    auto actPoint = [&](float sv) {
        int i = 0;
        while (i + 2 < pathN && geo_.actPathS[i + 1] < sv) ++i;
        const float s0 = geo_.actPathS[i], s1 = geo_.actPathS[i + 1];
        const float t = s1 > s0 ? std::clamp((sv - s0) / (s1 - s0), 0.0f, 1.0f) : 0.0f;
        const V3 a0 = actAt(i), a1 = actAt(i + 1);
        return Add(a0, Scale(Sub(a1, a0), t));
    };
    auto actProject = [&](V3 p) {  // the path's s nearest to p
        float best = 1e9f, bs = 0.0f;
        for (int i = 0; i + 1 < pathN; ++i) {
            const V3 a0 = actAt(i), a1 = actAt(i + 1), d = Sub(a1, a0);
            const float dd = Dot(d, d);
            const float t = dd > 1e-9f ? std::clamp(Dot(Sub(p, a0), d) / dd, 0.0f, 1.0f) : 0.0f;
            const float dist = Len(Sub(p, Add(a0, Scale(d, t))));
            if (dist < best) {
                best = dist;
                bs = geo_.actPathS[i] + t * (geo_.actPathS[i + 1] - geo_.actPathS[i]);
            }
        }
        return bs;
    };
    if (twoStage) {
        lastBoltW_ = X(actPoint(actS_));  // the press candidate: the knob where it is now
        out.targetOk[1] = true;
        out.target[1] = X(actPoint(0.0f));  // the tests' @bolt: the knob closed (a fixed spot, so a held knob can be driven)
        out.targetOk[3] = true;
        out.target[3] = X(actPoint(1.0f));
        out.targetOk[4] = true;
        out.target[4] = X(actPoint(2.0f));
    }
    auto heldAt = [&](float pull) {  // the magazine as it sits drawn out by `pull`, in the off hand's frame
        const XrPosef m{in.gun.orientation, X(Add(grabW, Scale(outW, pull)))};
        return Relative(in.off, m);
    };

    // (Latch=0: the release button does nothing on this gun; it stays masked from the pad while driving.)
    if (edge[g] && active_ && !(geo_.caps & 1024u)) {
        if (mag_ == kInGun) {
            Queue(shared::kReloadEject, in.now);
            SetMag(kOut, "the release button");
            Pulse(out, g, 0.6f, 40.0f);
        } else if (mag_ == kGrabbed) {
            Queue(shared::kReloadEject, in.now);
            heldRel_ = heldAt(pull_);
            armed_ = false;
            SetMag(kInHand, "the release button, the off hand holding it");
            Pulse(out, g, 0.6f, 40.0f);
            Pulse(out, o, 0.6f, 40.0f);
        }
    }
    // The player (2026-10-01): a GrabTrigger gun (the MP40) held by its foregrip -- a squeeze of that hand's trigger takes the
    // magazine out of the well (the foregrip lets go; the grip that held it now holds the magazine). ForeGrabTrigger.
    {
        const bool squeeze = !foreTrigHeld_ && in.offTrigger >= 0.6f;
        if (in.offTrigger >= 0.6f) foreTrigHeld_ = true;
        else if (in.offTrigger < 0.4f) foreTrigHeld_ = false;
        if (squeeze && foreGrabTrigger_ && active_ && (geo_.caps & 128u) && in.foreHeld && in.offHeld && mag_ == kInGun &&
            !(geo_.caps & 512u) && Len(Sub(offP, magRingW)) < 3.0f * magRingR) {  // (the MP40's is ~15 cm below its foregrip)
            start_ = X(offP);
            SetMag(kGrabbed, "the foregrip hand's trigger");
            out.releaseForegrip = true;
            MLOG("reload: the foregrip hand took the magazine, %.1f cm from its grab ring's centre", 100.0f * Len(Sub(offP, magRingW)));
            Pulse(out, o, 0.5f, 30.0f);
        }
    }
    if (press_ == kPressMag && mag_ == kInGun) {
        start_ = X(offP);
        SetMag(kGrabbed, "the off hand's grip at it");
        MLOG("reload: grabbed %.1f cm from its grab ring's centre", 100.0f * Len(Sub(offP, magRingW)));
        Pulse(out, o, 0.5f, 30.0f);
    } else if (press_ == kPressPouch && mag_ == kOut) {
        if (geo_.reserve > 0 || (geo_.state & 64u)) {
            Queue(shared::kReloadTake, in.now);
            const float a = in.fitAngle * 0.0174533f;
            heldRel_.orientation = {std::sin(a * 0.5f), 0.0f, 0.0f, std::cos(a * 0.5f)};
            // Hold is authored for the left off hand; the right one holds it mirrored (3.2).
            heldRel_.position = {o == 1 ? -hold_[0] : hold_[0], hold_[1], hold_[2]};
            armed_ = false;
            snapHeld_ = true;
            SetMag(kInHand, "taken from the pouch");
            Pulse(out, o, 0.5f, 30.0f);
        } else {
            MLOG("reload: the pouch is empty (reserve 0)");
            Pulse(out, o, 0.2f, 60.0f);
        }
    } else if (press_ == kPressFull) {
        MLOG("reload: the tube is full (clip %d/%d): no shell from the pouch", geo_.clip, geo_.max);
        Pulse(out, o, 0.2f, 60.0f);
    }
    if (press_ == kPressBolt && twoStage) {
        actHeld_ = true;
        actKnobAtGrab_ = X(actPoint(actS_));
        actHandAtGrab_ = X(offP);
        MLOG("reload: the bolt taken %.1f cm from its knob (s %.2f)", 100.0f * Len(Sub(offP, P(actKnobAtGrab_))), actS_);
        Pulse(out, o, 0.5f, 30.0f);
    } else if (press_ == kPressBolt) {
        boltHeld_ = true;
        boltStart_ = X(offP);
        rack_ = 0.0f;
        rackArmed_ = false;
        tug_ = (geo_.state & 8u) != 0;  // held back: a tug and let go (3.3)
        if (tug_ && rackTug_ <= 0.0f) rackArmed_ = true;
        MLOG("reload: the action taken %.1f cm from its grip point (%s; travel %.1f cm)", 100.0f * Len(Sub(offP, boltW)),
             tug_ ? "held back: tug and let go" : "pull it back", geo_.boltTravel * 100.0f);
        Pulse(out, o, 0.5f, 30.0f);
    }
    press_ = kPressNone;

    // GOAL A2: the two-stage action -- the knob follows the off hand along its path (up, then back); each step is sent as
    // it is passed, in order (up at s 0.9, back at 1.85, forward at 1.1, down at 0.1). An emptied bolt is held open by
    // the follower (the game's state): it can't be pushed forward. Let go, it rests where the game has it.
    if (twoStage) {
        const bool heldOpen = (geo_.state & 1024u) != 0;
        if (actHeld_) {
            if (!in.offHeld) {
                actHeld_ = false;
                MLOG("reload: the bolt let go (s %.2f)", actS_);
            } else {
                float sv = actProject(Add(P(actKnobAtGrab_), Sub(offP, P(actHandAtGrab_))));
                if (heldOpen && actStage_ == 2) sv = std::max(sv, 1.85f);
                actS_ = sv;
                if (actStage_ == 0 && actS_ >= 0.9f) {
                    Queue(shared::kReloadBoltUp, in.now);
                    actStage_ = 1;
                    Pulse(out, o, 0.3f, 20.0f);
                }
                if (actStage_ == 1 && actS_ >= 1.85f) {
                    Queue(shared::kReloadBoltBack, in.now);
                    actStage_ = 2;
                    Pulse(out, o, 0.6f, 30.0f);
                }
                if (actStage_ == 2 && actS_ <= 1.1f) {
                    Queue(shared::kReloadBoltForward, in.now);
                    actStage_ = 3;
                    Pulse(out, o, 0.4f, 20.0f);
                }
                if ((actStage_ == 3 || actStage_ == 1) && actS_ <= 0.1f) {
                    Queue(shared::kReloadBoltDown, in.now);
                    actStage_ = 0;
                    Pulse(out, o, 0.8f, 40.0f);
                    Pulse(out, g, 0.4f, 30.0f);
                }
            }
        }
        if (!actHeld_ && acksDone_) {  // at rest: where the game has it (after it has taken what was sent)
            const int gs = (geo_.state & 256u) ? 2 : (geo_.state & 16384u) ? 1 : (geo_.state & 32768u) ? 3 : 0;
            actStage_ = gs;
            actS_ = gs == 2 ? 2.0f : gs == 0 ? 0.0f : 1.0f;
        }
    }

    // GOAL A3: a pump gun -- the pump is the foregrip. Two-handed, the off hand drawn back along the gun draws the pump back
    // (at PumpArm of its travel: PUMP BACK, the case out); forward again closes it (PUMP FORWARD: a shell chambered), and
    // so does letting go after the back stroke. The stroke is measured along the gun from the most forward the hand has
    // been since it took hold, so the gun hand's moves don't count. Without a foregrip the pump's own grab (boltHeld_).
    const bool pump = (geo_.caps & 2048u) != 0;
    if (pump) {
        const float along = -Dot(Sub(offP, gunP), backW);  // how far ahead of the gun hand, along the bore
        // (The player, 2026-10-01: the foregrip held normally doesn't pump; the foregrip hand's trigger engages the pump --
        // PumpTrigger; hysteresis 0.6 / 0.4.)
        const bool trigOk = !pumpTrigger_ || in.offTrigger >= (pumpHeld_ && pumpByFore_ ? 0.4f : 0.6f);
        const bool byFore = in.foreHeld && foregrip_ && trigOk;
        const bool held = byFore || boltHeld_;
        if (held && !pumpHeld_) {
            pumpHeld_ = true;
            pumpByFore_ = byFore;
            pumpAnchor_ = along;
            rack_ = 0.0f;
            rackArmed_ = (geo_.state & 256u) != 0;  // the game has it back: forward closes it
            MLOG("reload: the pump taken (%s)%s", byFore ? "the foregrip" : "its grip", rackArmed_ ? " -- it is back" : "");
        }
        if (pumpHeld_ && !held) {
            if (rackArmed_) {
                Queue(shared::kReloadBoltForward, in.now);
                MLOG("reload: PUMP FORWARD (let go)");
                Pulse(out, o, 0.4f, 30.0f);
            }
            pumpHeld_ = false;
            boltHeld_ = false;
            rackArmed_ = false;
            rack_ = 0.0f;
        } else if (pumpHeld_) {
            pumpAnchor_ = std::max(pumpAnchor_, along);
            const float pulled = pumpAnchor_ - along;
            rack_ = std::clamp(pulled / std::max(geo_.boltTravel, rackMin_), 0.0f, 1.0f);
            if (!rackArmed_ && rack_ >= pumpArm_) {
                Queue(shared::kReloadBoltBack, in.now);
                rackArmed_ = true;
                MLOG("reload: PUMP BACK (%.1f cm back%s)", 100.0f * pulled, (geo_.state & 128u) ? "; a spent case in" : "");
                Pulse(out, o, 0.6f, 30.0f);
            } else if (rackArmed_ && rack_ < 0.3f) {
                Queue(shared::kReloadBoltForward, in.now);
                rackArmed_ = false;
                const bool feeds = (geo_.state & (128u | 8192u)) && geo_.clip >= 1;
                MLOG("reload: PUMP FORWARD (%s)", feeds ? "a shell to the chamber" : "closed");
                Pulse(out, o, feeds ? 0.8f : 0.3f, feeds ? 40.0f : 30.0f);
                if (feeds) Pulse(out, g, 0.8f, 40.0f);
            }
        }
    }

    // The action (3.3).
    if (boltHeld_ && !pump) {
        const float pulled = Dot(Sub(offP, P(boltStart_)), backW);
        rack_ = std::clamp(pulled / std::max(geo_.boltTravel, rackMin_), 0.0f, 1.0f);
        const bool needed = (geo_.state & 16u) != 0;
        auto sendRack = [&](const char* how) {
            Queue(shared::kReloadRack, in.now);
            MLOG("reload: RACK (%s; %s)", how, needed ? "a rack was needed" : "a press check");
            if (needed) {
                Pulse(out, g, 0.8f, 40.0f);
                Pulse(out, o, 0.8f, 40.0f);
            } else {
                Pulse(out, o, 0.3f, 30.0f);
            }
        };
        if (!in.offHeld) {
            if (rackArmed_) sendRack(tug_ ? "tugged and let go" : "let go");
            else MLOG("reload: the action let go before it was armed: no rack");
            boltHeld_ = false;
            rack_ = 0.0f;
        } else if (!rackArmed_ && (tug_ ? pulled >= rackTug_ : rack_ >= rackArm_)) {
            rackArmed_ = true;
            MLOG("reload: the action armed (%.1f cm back)", 100.0f * pulled);
            Pulse(out, o, 0.4f, 20.0f);
        } else if (rackArmed_ && !tug_ && rack_ < 0.3f) {
            sendRack("brought forward");
            rackArmed_ = false;
        }
    }

    if (mag_ == kGrabbed && entering_) {
        // Insert=slide: the front is in the mouth; the off hand pushes it along the way in until it is home.
        const float full = geo_.magSeat + geo_.magLen;
        if (!in.offHeld) {
            Queue(shared::kReloadInsert, in.now);
            SetMag(twoStage ? kOut : kInGun, "let go in the mouth: it slides home");  // (GOAL A5: the M18's breech)
            Pulse(out, g, 0.9f, 50.0f);
        } else {
            pull_ = std::clamp(Dot(Sub(offP, P(start_)), outW), 0.0f, full + 0.05f);
            if (pull_ <= geo_.magSeat + 0.01f) {
                Queue(shared::kReloadInsert, in.now);
                SetMag(twoStage ? kOut : kInGun, "pushed home");
                MLOG("reload: slid home (%.1f cm in)", 100.0f * full);
                Pulse(out, g, 0.9f, 50.0f);
                Pulse(out, o, 0.9f, 50.0f);
            } else if (pull_ > full + 0.02f) {
                armed_ = false;
                SetMag(kInHand, "drawn back out of the mouth");
            }
        }
    } else if (mag_ == kGrabbed) {
        if (!in.offHeld) {
            SetMag(kInGun, "let go: it slides back");
        } else {
            pull_ = std::clamp(Dot(Sub(offP, P(start_)), outW), 0.0f, pullOut_);
            if (pull_ >= pullOut_) {
                Queue(shared::kReloadEject, in.now);
                heldRel_ = heldAt(pullOut_);
                armed_ = false;
                SetMag(kInHand, "pulled out");
                Pulse(out, o, 0.6f, 40.0f);
            }
        }
    }
    // Twin magazines: the off hand's trigger flips a held taped pair (edge, hysteresis 0.6 / 0.4); the trigger is kept from
    // the pad while a pair is held and until a press begun then ends.
    const bool twin = (geo_.caps & 32u) != 0 && mag_ == kInHand;
    if (!trigHeld_ && in.offTrigger >= 0.6f) {
        trigHeld_ = true;
        if (twin && active_) {
            trigLatch_ = true;
            flipped_ = !flipped_;
            MLOG("reload: the taped pair flipped (%s half toward the well)", flipped_ ? "the other" : "the same");
            Pulse(out, o, 0.4f, 25.0f);
        }
    } else if (trigHeld_ && in.offTrigger < 0.4f) {
        trigHeld_ = false;
        trigLatch_ = false;
    }
    // GrabTrigger (the MP40): the off hand's trigger is the grab's, not the game's aim, near the magazine and while it is
    // in the hand.
    const bool grabTrig = (geo_.caps & 128u) &&
                          ((mag_ == kInGun && Len(Sub(offP, magRingW)) < 2.0f * magRingR) || mag_ == kGrabbed ||
                           mag_ == kInHand);
    out.maskTrigger[o] = twin || trigLatch_ || grabTrig;
    // TriggerRack (the Colt): with a magazine in a locked-back action, the gun hand's trigger releases it (a RACK); that
    // press is kept from the game.
    if (!gunTrigHeld_ && in.gunTrigger >= 0.6f) {
        gunTrigHeld_ = true;
        if (active_ && (geo_.caps & 256u) && (geo_.state & 8u) && (geo_.state & 16u) && !boltHeld_) {
            gunTrigLatch_ = true;
            Queue(shared::kReloadRack, in.now);
            MLOG("reload: RACK (the trigger released the action)");
            Pulse(out, g, 0.8f, 40.0f);
        }
    } else if (gunTrigHeld_ && in.gunTrigger < 0.4f) {
        gunTrigHeld_ = false;
        gunTrigLatch_ = false;
    }
    if (gunTrigLatch_) out.maskTrigger[g] = true;
    if (twoStage && (geo_.state & 2048u)) out.maskTrigger[g] = true;  // GOAL A2: work the bolt before the next shot
    if (pump && ((geo_.state & 2048u) || rackArmed_)) out.maskTrigger[g] = true;  // GOAL A3: pump it first
    if (pump && pumpHeld_ && pumpByFore_ && pumpTrigger_) out.maskTrigger[o] = true;  // (the pump's trigger isn't the game's)
    float dist = 1e9f, angle = 180.0f;
    const float dt = lastNow_ > 0.0 ? static_cast<float>(std::min(0.1, std::max(0.0, in.now - lastNow_))) : 0.0f;
    lastNow_ = in.now;
    if (mag_ == kInHand) {
        if (!in.offHeld) {
            Queue(shared::kReloadDrop, in.now);
            SetMag(kOut, "let go: dropped");
        } else {
            // Round 31: the game's reload grip says where the magazine sits in the drawn hand (magHeld, in the aim
            // frame); a pouch magazine goes straight there, a pulled one eases over ~0.1 s.
            if (geo_.caps & 64u) {
                const shared::Pose& h = geo_.magHeld;
                const XrPosef heldAim{{h.qx, h.qy, h.qz, h.qw}, {h.px, h.py, h.pz}};
                const XrPosef want = Relative(in.off, Compose(in.offAim, heldAim));
                if (snapHeld_) {
                    heldRel_ = want;
                } else {
                    const float k = 1.0f - std::exp(-dt / 0.05f);
                    const V3 p = Add(P(heldRel_.position), Scale(Sub(P(want.position), P(heldRel_.position)), k));
                    XrQuaternionf a = heldRel_.orientation, b = want.orientation;
                    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = {-b.x, -b.y, -b.z, -b.w};
                    XrQuaternionf q{a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k};
                    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                    if (n > 1e-6f) q = {q.x / n, q.y / n, q.z / n, q.w / n};
                    heldRel_ = {q, X(p)};
                }
            }
            snapHeld_ = false;
            magPose_ = Compose(in.off, heldRel_);
            const V3 heldOut = Rotate(magPose_.orientation, A3(geo_.magOut));
            // Insert=slide: the held magazine's front (MagLen ahead of its grab point) against the mouth.
            dist = geo_.magLen > 0.0f ? Len(Sub(Sub(P(magPose_.position), Scale(heldOut, geo_.magLen)), mouthW))
                                      : Len(Sub(P(magPose_.position), grabW));
            angle = std::acos(std::clamp(Dot(heldOut, outW), -1.0f, 1.0f)) * 57.2958f;
            if (!armed_ && dist > insertR_ + 0.02f) armed_ = true;  // away from the well first (a pull ends inside it)
            if (armed_ && dist < insertR_ && angle >= insertAngle_ && in.now - nearMissAt_ > 1.0) {
                nearMissAt_ = in.now;
                MLOG("reload: at the well (%.1f cm) but turned %.0f deg from its way (InsertAngle %.0f)", 100.0f * dist, angle,
                     insertAngle_);
            }
            if (active_ && armed_ && dist < insertR_ && angle < insertAngle_ && geo_.magLen > 0.0f) {
                // Insert=slide: into the mouth -- now it slides along the way in as the hand pushes (grabbed, entering).
                SetMag(kGrabbed, "its front in the mouth: sliding in");
                entering_ = true;
                pull_ = geo_.magSeat + geo_.magLen;
                start_ = X(Sub(offP, Scale(outW, pull_)));
                MLOG("reload: into the mouth %.1f cm from it, %.0f deg off its way", 100.0f * dist, angle);
                Pulse(out, o, 0.5f, 30.0f);
            } else if (active_ && armed_ && dist < insertR_ && angle < insertAngle_) {
                Queue(twin && flipped_ ? shared::kReloadInsertOther : shared::kReloadInsert, in.now);
                // (GOAL A2: through a bolt's open action the clip strips / the round goes in: the hand is empty again. GOAL
                // A3: likewise a shell into a pump gun's tube.)
                SetMag(twoStage || pump ? kOut : kInGun, twoStage ? "loaded through the open action" : pump ? "a shell into the tube" : "inserted");
                MLOG("reload: inserted %.1f cm from the well, %.0f deg off its way", 100.0f * dist, angle);
                Pulse(out, g, 0.9f, 50.0f);
                Pulse(out, o, 0.9f, 50.0f);
            }
        }
    }
    // Rings: the magazine's grab spot while in the gun; the well while one is in the hand (lit where it would go in).
    if ((mag_ == kInGun && !(geo_.caps & 512u)) || (mag_ == kGrabbed && !entering_) || in.showSpots) {
        const float d = Len(Sub(offP, magRingW));
        out.rings[out.ringCount++] = {X(magRingW), magRingR, d < magRingR, d < 2.0f * magRingR || in.showSpots};
    } else if (mag_ == kInHand && armed_) {  // the well -- or the mouth, for the slide insert
        out.rings[out.ringCount++] = {X(geo_.magLen > 0.0f ? mouthW : grabW), insertR_, dist < insertR_ && angle < insertAngle_,
                                      dist < 3.0f * insertR_};
    }
    // GOAL A2: the knob's ring while the bolt must be worked (a spent case, or not closed).
    if (twoStage && !actHeld_ && ((geo_.state & 2048u) || actStage_ != 0)) {
        const V3 kw = actPoint(actS_);
        const float d = Len(Sub(offP, kw));
        out.rings[out.ringCount++] = {X(kw), boltRingR, d < boltRingR, d < 2.0f * boltRingR};
    }
    // GOAL A3: a pump gun without a foregrip -- the pump's ring while it must be worked.
    if (pump && !foregrip_ && !pumpHeld_ && (geo_.state & 2048u) && out.ringCount < 2) {
        const float d = Len(Sub(offP, boltW));
        out.rings[out.ringCount++] = {X(boltW), boltRingR, d < boltRingR, d < 2.0f * boltRingR};
    }
    // The action's ring while a rack is needed (a fed magazine waiting, or an open bolt forward).
    if ((geo_.caps & 2u) && (((geo_.state & 16u) && !boltHeld_) || in.showSpots) && out.ringCount < 2) {
        const float d = Len(Sub(offP, boltW)), r = boltRingR;
        out.rings[out.ringCount++] = {X(boltW), r, d < r, d < 2.0f * r};
    }
}

void ManualReload::Send(shared::Header* hdr, double now) {
    if (!hdr) return;
    // Events, in order; never more than the ring holds unread (RELOAD-DESIGN 4).
    while (!pending_.empty()) {
        const Pending p = pending_.front();
        if (hdr->reloadEvtSeq - hdr->reloadEvtAck >= 8u) {
            if (now - p.at > 0.25) {
                MLOG("reload: %s dropped -- the game has not taken the last 8 events", kEventName[p.type]);
                pending_.pop_front();
            }
            break;
        }
        const std::uint32_t s = hdr->reloadEvtSeq;
        hdr->reloadEvt[s % 8u] = p.type | ((p.hash & 0xFFFFFFu) << 8);
        _ReadWriteBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->reloadEvtSeq));
        MLOG("reload: sent %s for %s", EventName(p.type, (geo_.caps & 2048u) != 0), geo_.key[0] ? geo_.key : "(no gun)");
        pending_.pop_front();
    }
}

std::uint32_t ManualReload::Flags() const {
    // (A2: the host poses the bolt; A3: the pump while held)
    const bool posed = boltHeld_ || pumpHeld_ || ((geo_.caps & 4096u) && (actHeld_ || actS_ > 0.001f));
    // bit6: the off hand holds the action now (the grips: an open bolt let go is posed, but not held).
    const bool held = boltHeld_ || pumpHeld_ || actHeld_;
    return (on_ ? 1u : 0u) | (static_cast<std::uint32_t>(mag_) << 1) | (posed ? 8u : 0u) | (engaged_ ? 16u : 0u) |
           (mag_ == kInHand && flipped_ ? 32u : 0u) | (held ? 64u : 0u);
}

shared::Pose ManualReload::MagPose() const {
    const XrPosef& p = magPose_;
    return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

}  // namespace mohavr::host
