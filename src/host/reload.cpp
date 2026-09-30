#include "reload.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP", "INSERT (the other half)"};
const char* kMagName[] = {"in the gun", "grabbed", "in the off hand", "out"};

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
    GetPrivateProfileStringW(L"ManualReload", L"Hold", L"0 0 0", b, 64, ini.c_str());
    float h[3] = {0, 0, 0};
    swscanf_s(b, L"%f %f %f", &h[0], &h[1], &h[2]);
    for (int i = 0; i < 3; ++i) hold_[i] = h[i] / 100.0f;
    static const char* kButtons[] = {"none", "upper (B / Y)", "lower (A / X)"};
    MLOG("reload: Weapon.ManualReload=%d (the default; the menu's toggle is the player's); release button %s, pull out %.0f cm, "
         "insert within %.0f cm and %.0f deg, hold %.0f %.0f %.0f cm; the action: grab within %.0f cm, armed at %.0f%% of its "
         "travel (at least %.0f cm), a tug of %.0f cm when held back",
         on_ ? 1 : 0, kButtons[releaseButton_], pullOut_ * 100.0f, insertR_ * 100.0f, insertAngle_, h[0], h[1], h[2],
         boltGrabR_ * 100.0f, rackArm_ * 100.0f, rackMin_ * 100.0f, rackTug_ * 100.0f);
}

void ManualReload::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    MLOG("reload: manual reload %s (the menu)", on ? "on" : "off");
}

void ManualReload::Queue(std::uint32_t type, double now) {
    if (type < shared::kReloadEject || type > shared::kReloadInsertOther) return;
    pending_.push_back({type, keyHash_, now});
}

void ManualReload::SetMag(Mag m, const char* why) {
    if (m == mag_) return;
    MLOG("reload: magazine %s -> %s (%s)", kMagName[mag_], kMagName[m], why);
    mag_ = m;
    if (m == kInHand) flipped_ = false;  // held as it came
    if (m != kGrabbed) pull_ = 0.0f;
}

void ManualReload::Poll(shared::Header* hdr, double now, bool handsOk) {
    if (!hdr) return;
    // Every event sent has been taken (read before the geometry: a state older than the acknowledgement can't pass).
    const bool acksDone = pending_.empty() && hdr->reloadEvtSeq == hdr->reloadEvtAck;
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
            SetMag(gameIn ? kInGun : kOut, "another gun in hand");
        } else if (acksDone && (geo_.caps & 1u)) {
            // Reconcile (3.2): the game's magazine differs from ours for two of its frames with nothing in flight.
            const bool hostIn = mag_ == kInGun || mag_ == kGrabbed;
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
        MLOG("reload: %s -- clip %d/%d, reserve %d; magazine %s%s%s%s%s%s", geo_.key[0] ? geo_.key : "(no gun)", geo_.clip,
             geo_.max, geo_.reserve, (geo_.state & 1u) ? "in" : "out", (geo_.state & 2u) ? ", pending" : "",
             (geo_.state & 4u) ? ", ready" : "", (geo_.state & 8u) ? ", action held back" : "",
             (geo_.state & 16u) ? ", rack needed" : "", (geo_.caps & 1u) ? "" : " (not converted)");
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
    if (!active_) {
        // 3.1: leaving mid-gesture -- a grabbed magazine slides back, one in the hand is dropped, the action is let go
        // without a rack.
        if (boltHeld_) MLOG("reload: the action let go (not driving): no rack");
        boltHeld_ = false;
        if (mag_ == kGrabbed) SetMag(kInGun, "let go: not driving");
        else if (mag_ == kInHand) {
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
    if (mag_ == kOut && pouchR > 0.0f) {
        const float s = Len(Sub(P(hand), P(pouch))) / pouchR;
        if (s < best) {
            best = s;
            which = kPressPouch;
        }
    }
    if (mag_ == kInGun && lastGunOk_ && geo_.magGrabR > 0.0f) {
        const float s = Len(Sub(P(hand), P(lastGrabW_))) / (geo_.magGrabR * ringScale_);
        if (s < best) {
            best = s;
            which = kPressMag;
        }
    }
    if ((geo_.caps & 2u) && lastGunOk_ && boltGrabR_ > 0.0f) {
        const float s = Len(Sub(P(hand), P(lastBoltW_))) / (boltGrabR_ * ringScale_);
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
    if (!active_) {
        press_ = kPressNone;
        return;
    }
    const V3 gunP = P(in.gun.position), offP = P(in.off.position);
    const V3 grabW = Add(gunP, Rotate(in.gun.orientation, A3(geo_.magGrab)));
    const V3 outW = Rotate(in.gun.orientation, A3(geo_.magOut));
    const V3 boltW = Add(gunP, Rotate(in.gun.orientation, A3(geo_.boltGrab)));
    const V3 backW = Rotate(in.gun.orientation, A3(geo_.boltBack));
    lastGrabW_ = X(grabW);
    lastBoltW_ = X(boltW);
    out.targetOk[0] = true;
    out.target[0] = X(grabW);
    out.targetOk[2] = true;
    out.target[2] = X(grabW);
    if (mag_ == kInHand)  // the aim point that puts the held magazine's grab point at the well
        out.target[2] = X(Sub(Sub(grabW, Sub(P(in.off.position), P(in.offAim.position))), Rotate(in.off.orientation, P(heldRel_.position))));
    if (geo_.caps & 2u) {
        out.targetOk[1] = true;
        out.target[1] = X(boltW);
    }
    auto heldAt = [&](float pull) {  // the magazine as it sits drawn out by `pull`, in the off hand's frame
        const XrPosef m{in.gun.orientation, X(Add(grabW, Scale(outW, pull)))};
        return Relative(in.off, m);
    };

    if (edge[g]) {
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
    if (press_ == kPressMag && mag_ == kInGun) {
        start_ = X(offP);
        SetMag(kGrabbed, "the off hand's grip at it");
        MLOG("reload: grabbed %.1f cm from its grab point", 100.0f * Len(Sub(offP, grabW)));
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
    }
    if (press_ == kPressBolt) {
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

    // The action (3.3).
    if (boltHeld_) {
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

    if (mag_ == kGrabbed) {
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
        if (twin) {
            trigLatch_ = true;
            flipped_ = !flipped_;
            MLOG("reload: the taped pair flipped (%s half toward the well)", flipped_ ? "the other" : "the same");
            Pulse(out, o, 0.4f, 25.0f);
        }
    } else if (trigHeld_ && in.offTrigger < 0.4f) {
        trigHeld_ = false;
        trigLatch_ = false;
    }
    out.maskTrigger[o] = twin || trigLatch_;
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
            dist = Len(Sub(P(magPose_.position), grabW));
            const V3 heldOut = Rotate(magPose_.orientation, A3(geo_.magOut));
            angle = std::acos(std::clamp(Dot(heldOut, outW), -1.0f, 1.0f)) * 57.2958f;
            if (!armed_ && dist > insertR_ + 0.02f) armed_ = true;  // away from the well first (a pull ends inside it)
            if (armed_ && dist < insertR_ && angle >= insertAngle_ && in.now - nearMissAt_ > 1.0) {
                nearMissAt_ = in.now;
                MLOG("reload: at the well (%.1f cm) but turned %.0f deg from its way (InsertAngle %.0f)", 100.0f * dist, angle,
                     insertAngle_);
            }
            if (armed_ && dist < insertR_ && angle < insertAngle_) {
                Queue(twin && flipped_ ? shared::kReloadInsertOther : shared::kReloadInsert, in.now);
                SetMag(kInGun, "inserted");
                MLOG("reload: inserted %.1f cm from the well, %.0f deg off its way", 100.0f * dist, angle);
                Pulse(out, g, 0.9f, 50.0f);
                Pulse(out, o, 0.9f, 50.0f);
            }
        }
    }
    // Rings: the magazine's grab spot while in the gun; the well while one is in the hand (lit where it would go in).
    if (mag_ == kInGun || mag_ == kGrabbed) {
        const float d = Len(Sub(offP, grabW)), r = geo_.magGrabR * ringScale_;
        out.rings[out.ringCount++] = {X(grabW), r, d < r, d < 2.0f * r};
    } else if (mag_ == kInHand && armed_) {
        out.rings[out.ringCount++] = {X(grabW), insertR_, dist < insertR_ && angle < insertAngle_, dist < 3.0f * insertR_};
    }
    // The action's ring while a rack is needed (a fed magazine waiting, or an open bolt forward).
    if ((geo_.caps & 2u) && (geo_.state & 16u) && !boltHeld_) {
        const float d = Len(Sub(offP, boltW)), r = boltGrabR_ * ringScale_;
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
        MLOG("reload: sent %s for %s", kEventName[p.type], geo_.key[0] ? geo_.key : "(no gun)");
        pending_.pop_front();
    }
}

std::uint32_t ManualReload::Flags() const {
    return (on_ ? 1u : 0u) | (static_cast<std::uint32_t>(mag_) << 1) | (boltHeld_ ? 8u : 0u) | (engaged_ ? 16u : 0u) |
           (mag_ == kInHand && flipped_ ? 32u : 0u);
}

shared::Pose ManualReload::MagPose() const {
    const XrPosef& p = magPose_;
    return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

}  // namespace mohavr::host
