#include "offpistol.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
const char* const kEventName[4] = {"?", "DRAW", "SHOT", "HOLSTER"};

XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const XrVector3f cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
    return {v.x + 2.0f * (c.x * q.w + cc.x), v.y + 2.0f * (c.y * q.w + cc.y), v.z + 2.0f * (c.z * q.w + cc.z)};
}

// The off pistol's aim line: the off aim pose pitched by the pistol fit's angle (+ = muzzle up, as the gun hand's gunPose),
// moved along the gun's up and right by the fit's aim line -- right mirrored for a left off hand (the pistol is drawn as the
// mirror image of the gun hand's hold; hands.cpp's rule for a left gun hand, OFFPISTOL-DESIGN 4.8).
XrPosef RayFrom(const XrPosef& aim, const shared::GunFit& fit, int offHand) {
    const float a = fit.angle * 0.0174533f;
    XrPosef r;
    r.orientation = QMul(aim.orientation, XrQuaternionf{std::sin(a * 0.5f), 0.0f, 0.0f, std::cos(a * 0.5f)});
    const float right = offHand == 0 ? -fit.rayRight : fit.rayRight;
    const XrVector3f d = QRot(r.orientation, XrVector3f{right / 100.0f, fit.rayUp / 100.0f, 0.0f});
    r.position = {aim.position.x + d.x, aim.position.y + d.y, aim.position.z + d.z};
    return r;
}
}  // namespace

void OffHandPistol::Init(const std::wstring& ini) {
    on_ = GetPrivateProfileIntW(L"OffHand", L"Pistol", 0, ini.c_str()) != 0;
    wchar_t b[32] = L"";
    GetPrivateProfileStringW(L"OffHand", L"PistolHold", L"toggle", b, 32, ini.c_str());
    toggle_ = _wcsicmp(b, L"grip") != 0;
    dot_ = GetPrivateProfileIntW(L"OffHand", L"PistolDot", 1, ini.c_str()) != 0;
    keep_ = GetPrivateProfileIntW(L"OffHand", L"PistolKeep", 1, ini.c_str()) != 0;
    pair_ = GetPrivateProfileIntW(L"OffHand", L"PistolPair", 0, ini.c_str()) != 0;
    haptics_ = GetPrivateProfileIntW(L"OffHand", L"Haptics", 1, ini.c_str()) != 0;
    MLOG("offpistol: OffHand.Pistol=%d (the default; the menu's toggle is the player's); %s, the second dot %d, switch weapon kept "
         "from the held pistol %d, haptics %d",
         on_ ? 1 : 0, toggle_ ? "a click draws and a click at a holster puts it back (PistolHold=toggle)" : "held while the grip is (PistolHold=grip)",
         dot_ ? 1 : 0, keep_ ? 1 : 0, haptics_ ? 1 : 0);
}

void OffHandPistol::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    MLOG("offpistol: the off-hand pistol %s (the menu)", on ? "on" : "off");
}

void OffHandPistol::Pulse(int hand, float amp, float ms) {
    if (!haptics_ || hand < 0 || hand > 1) return;
    pulseAmp_[hand] = std::max(pulseAmp_[hand], amp);
    pulseMs_[hand] = std::max(pulseMs_[hand], ms);
}

void OffHandPistol::To(State s, const char* why) {
    if (s == state_) return;
    MLOG("offpistol: %s -> %s (%s)", state_ == kHeld ? "held" : "none", s == kHeld ? "held" : "none", why);
    state_ = s;
    if (s == kNone) {
        frozen_ = false;
        relatch_ = true;
        letGoFrozen_ = false;
    }
}

void OffHandPistol::Queue(std::uint32_t event, const XrPosef& ray, double now) { pending_.push_back({event, ray, now}); }

void OffHandPistol::Poll(shared::Header* hdr, double now) {
    if (!hdr) return;
    // Every event sent has been taken (read before the status: a state older than the acknowledgement can't pass).
    acksDone_ = pending_.empty() && hdr->pistolEvtSeq == hdr->pistolEvtAck;
    shared::PistolStatus s{};
    std::uint32_t seq = 0;
    if (!shared::ReadPistolStatus(hdr, s, seq) || seq == lastSeq_) return;
    lastSeq_ = seq;
    statusAt_ = now;
    if (!seenStatus_) {
        seenStatus_ = true;
        pawnSeq_ = s.pawnSeq;
        shots_ = s.shots;
        dry_ = s.dry;
        refills_ = s.refills;
    }
    status_ = s;
    if (key_ != s.key) {
        key_ = s.key;
        MLOG("offpistol: the pistol a draw gets: %s", key_.empty() ? "none" : key_.c_str());
    }
    if (s.pawnSeq != pawnSeq_) {
        pawnSeq_ = s.pawnSeq;
        if (state_ != kNone) To(kNone, "a new pawn: the held pistol is gone");
    }
    if (s.shots != shots_) {  // a round fired: the recoil pulse
        shots_ = s.shots;
        Pulse(offHand_, 0.8f, 40.0f);
    }
    if (s.dry != dry_) {  // the dry click
        dry_ = s.dry;
        Pulse(offHand_, 0.3f, 15.0f);
    }
    if (s.refills != refills_) {  // refilled in the holster
        refills_ = s.refills;
        Pulse(offHand_, 0.3f, 20.0f);
    }
    unavailable_ = (s.caps & 16u) ? 0 : unavailable_ + 1;  // bit4: one held may stay
    // The reconcile: the game's state differs from ours for two of its frames with nothing in flight (a refused draw, the
    // hold ended by the game, the host restarted) -- the game's wins.
    const State game = (s.state & 3u) == 1u ? kHeld : kNone;
    if (acksDone_ && game != state_) {
        if (++disagree_ >= 2) {
            disagree_ = 0;
            static const char* kWhy[] = {"", "unavailable", "none carried", "the only pistol is in the gun hand", "too soon",
                                         "the shot failed", "a weapon switch took it", "it is gone"};
            const std::uint32_t refusal = (s.state >> 8) & 0xFFu;
            if (game == kNone && state_ != kNone) {
                MLOG("offpistol: the game ended the hold (%s)", refusal < 8 ? kWhy[refusal] : "?");
                Pulse(offHand_, 0.2f, 60.0f);
            }
            To(game, "the game's (reconciled)");
        }
    } else {
        disagree_ = 0;
    }
}

// Whether a draw would work now (from this frame's input: a press comes before Frame), and why not ("" when it would).
// `applies`: an off-hand press at a pistol holster is the off-hand pistol's even when a draw can't happen now -- switched on,
// the game's side alive, a gun in the other hand (the grenade's nade3 rule: the holster's own SwitchPistol would put the
// pistol in the off hand as the main weapon and leave the gun hand empty).
const char* OffHandPistol::UpdateActive(const In& in, bool* applies) {
    const bool alive = statusAt_ >= 0.0 && in.now - statusAt_ < 0.25;
    // A press at a pistol holster stays ours through a short stall of the game (a hitch, a checkpoint save): the holster's
    // own SwitchPistol would put the pistol in the off hand as the main weapon (review: a 300 ms hitch did).
    const bool seen = statusAt_ >= 0.0 && in.now - statusAt_ < 3.0;
    if (applies) *applies = on_ && seen && in.gunOk;
    const char* why = !on_ ? "switched off" : !alive ? "the game's side is quiet" :
                      ((status_.caps & 32u) && !pair_) ? "the only pistol is in the gun hand" :
                      !(status_.caps & 1u) ? "no pistol the game can fire" : !(status_.caps & 2u) ? "not now in the game" :
                      !in.gunOk ? "no gun in the other hand" : in.nadeHeld ? "a grenade in the off hand" :
                      !in.offTracked ? "the off hand isn't tracked" : !in.gripActive ? "the off hand's grip is asleep" :
                      !in.gestures ? "a menu is open" : !in.hasView ? "no head-tracked view" : "";
    const bool active = !*why;
    if (active != active_ || (!active && whyInactive_ != why)) {
        if (active != active_ || on_) MLOG("offpistol: %s%s%s", active ? "a draw works now" : "no draw (", active ? "" : why, active ? "" : ")");
        whyInactive_ = why;
    }
    active_ = active;
    return why;
}

bool OffHandPistol::DrawPress(const In& in) {
    bool applies = false;
    const char* why = UpdateActive(in, &applies);
    if (!applies || state_ != kNone) return false;
    offHand_ = in.offHand;
    if (!active_) {
        MLOG("offpistol: a draw at the pistol holster -- not now (%s)", why);
        Pulse(offHand_, 0.2f, 60.0f);
        return true;  // the press is ours: the gun stays in its hand
    }
    Queue(shared::kPistolDraw, RayFrom(in.offAim, in.fit, in.offHand), in.now);
    To(kHeld, "drawn at the holster");
    relatch_ = true;
    Pulse(offHand_, 0.5f, 30.0f);
    return true;
}

void OffHandPistol::Holster(const In& in, const char* why) {
    Queue(shared::kPistolHolster, ray_, in.now);
    Pulse(offHand_, 0.2f, 20.0f);
    To(kNone, why);
}

void OffHandPistol::HeldPress(const In& in, bool atHolster) {
    if (state_ == kNone) return;
    offHand_ = in.offHand;
    if (toggle_ && atHolster && !frozen_) Holster(in, "put back at a holster");
}

void OffHandPistol::Frame(const In& in, Out& out) {
    offHand_ = in.offHand;
    trigger_ = in.trigger;
    rayOk_ = in.offValid;
    if (rayOk_) ray_ = RayFrom(in.offAim, in.fit, in.offHand);
    fit_[0] = in.fit.grip[0];
    fit_[1] = in.fit.grip[1];
    fit_[2] = in.fit.grip[2];
    fit_[3] = in.fit.angle;
    const char* why = UpdateActive(in);
    if (state_ != kNone) {
        out.maskTrigger = true;  // the pistol's trigger, not the game's
        trigMaskHeld_ = true;
        out.maskSwitch = keep_ && (status_.caps & 64u) != 0;
        // Frozen while a menu is open, the grip action sleeps or the hand isn't tracked (a shot from a stale pose would aim
        // wrong).
        const bool frozen = !in.gestures || !in.gripActive || !in.offTracked;
        if (frozen != frozen_) {
            frozen_ = frozen;
            MLOG("offpistol: %s (%s)", frozen ? "frozen" : "carries on", frozen ? why : "the hand and grip are back");
        }
        // The trigger's pulls (>= 0.6 after < 0.4): none while frozen, and one already under way when the hold starts or a
        // freeze ends must be let go first (a fist closing on the draw; a trigger held through a menu or the dashboard).
        if (frozen) {
            relatch_ = true;
        } else if (relatch_) {
            needRest_ = in.trigger >= 0.15f;
            trigHeld_ = false;
            relatch_ = false;
        }
        if (needRest_ && in.trigger < 0.15f) needRest_ = false;
        const bool press = !frozen && !needRest_ && !trigHeld_ && in.trigger >= 0.6f;
        if (press) trigHeld_ = true;
        else if (!frozen && trigHeld_ && in.trigger < 0.4f) trigHeld_ = false;
        if (frozen) {
            if (!toggle_ && !in.gripHeld) letGoFrozen_ = true;
        } else if (unavailable_ >= 2) {
            // The game made it unavailable while held (a ladder, a cutscene, the HellBox); it puts it back itself.
            Holster(in, "put back: the game made it unavailable");
            Pulse(offHand_, 0.2f, 60.0f);
        } else if (!toggle_ && !in.gripHeld) {
            Holster(in, letGoFrozen_ ? "put back: let go during the freeze" : "put back: let go");
        } else if (press && rayOk_) {
            Queue(shared::kPistolShot, ray_, in.now);
        }
        if (!frozen) letGoFrozen_ = false;
    } else {
        relatch_ = true;  // the next hold starts with the trigger as it is then
        frozen_ = false;
        trigHeld_ = false;
        if (trigMaskHeld_ && in.trigger >= 0.4f) out.maskTrigger = true;  // still squeezed from the hold: kept until let go
        else trigMaskHeld_ = false;
    }
    for (int h = 0; h < 2; ++h) {
        out.pulseAmp[h] = pulseAmp_[h];
        out.pulseMs[h] = pulseMs_[h];
        pulseAmp_[h] = pulseMs_[h] = 0.0f;
    }
}

void OffHandPistol::Send(shared::Header* hdr, double now) {
    if (!hdr) return;
    // Events, in order; never more than the ring holds unread (RELOAD-DESIGN 4).
    while (!pending_.empty()) {
        const Pending p = pending_.front();
        if (hdr->pistolEvtSeq - hdr->pistolEvtAck >= 8u) {
            if (now - p.at > 0.25) {
                MLOG("offpistol: %s dropped -- the game has not taken the last 8 events", kEventName[p.event & 3u]);
                pending_.pop_front();
            }
            break;
        }
        const std::uint32_t s = hdr->pistolEvtSeq;
        hdr->pistolEvt[s % 8u] = p.event;
        hdr->pistolEvtRay[s % 8u] = {p.ray.position.x, p.ray.position.y, p.ray.position.z, p.ray.orientation.x, p.ray.orientation.y,
                                     p.ray.orientation.z, p.ray.orientation.w};
        _ReadWriteBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->pistolEvtSeq));
        if (p.event != shared::kPistolShot) MLOG("offpistol: sent %s", kEventName[p.event & 3u]);
        pending_.pop_front();
    }
}

std::uint32_t OffHandPistol::Flags() const {
    return (on_ ? 1u : 0u) | (state_ == kHeld ? 2u : 0u) | (frozen_ ? 4u : 0u) | (state_ == kHeld && trigHeld_ && !frozen_ ? 8u : 0u);
}

shared::Pose OffHandPistol::AimRay() const {
    const XrPosef& p = ray_;
    return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

void OffHandPistol::Fit(float (&out)[4]) const { std::memcpy(out, fit_, sizeof(out)); }

bool OffHandPistol::DotRay(XrPosef& ray) const {
    if (state_ != kHeld || !dot_ || !rayOk_ || frozen_) return false;
    ray = ray_;
    return true;
}

}  // namespace mohavr::host
