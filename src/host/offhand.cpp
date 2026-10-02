#include "offhand.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
const char* const kStateName[4] = {"none", "held", "armed", "cooking"};
const char* const kEventName[6] = {"?", "TAKE", "PIN", "COOK", "THROW", "PUT BACK"};
const char* const kTypeName[3] = {"frag", "gammon", "stick"};
const char* TypeName(std::uint32_t t) { return t < 3 ? kTypeName[t] : "any"; }
}  // namespace

void OffHandGrenade::Init(const std::wstring& ini) {
    on_ = GetPrivateProfileIntW(L"OffHand", L"Grenade", 0, ini.c_str()) != 0;
    wchar_t b[32] = L"";
    GetPrivateProfileStringW(L"OffHand", L"GrenadeHold", L"grip", b, 32, ini.c_str());
    click_ = !_wcsicmp(b, L"click");
    GetPrivateProfileStringW(L"OffHand", L"Pin", L"trigger", b, 32, ini.c_str());
    pinAuto_ = !_wcsicmp(b, L"auto");
    GetPrivateProfileStringW(L"OffHand", L"Cook", L"spoon", b, 32, ini.c_str());
    cook_ = !_wcsicmp(b, L"off") ? 0 : !_wcsicmp(b, L"pin") ? 2 : 1;
    GetPrivateProfileStringW(L"OffHand", L"SlowRelease", L"toss", b, 32, ini.c_str());
    slowToss_ = _wcsicmp(b, L"drop") != 0;
    auto iniFloat = [&](const wchar_t* key, float def) {
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"OffHand", key, L"", v, 32, ini.c_str());
        return v[0] ? static_cast<float>(_wtof(v)) : def;
    };
    minThrow_ = std::clamp(iniFloat(L"MinThrowSpeed", 1.0f), 0.1f, 5.0f);
    maxHand_ = std::clamp(iniFloat(L"MaxHandSpeed", 12.0f), 2.0f, 30.0f);
    haptics_ = GetPrivateProfileIntW(L"OffHand", L"Haptics", 1, ini.c_str()) != 0;
    static const char* kCook[] = {"off", "a 2nd trigger squeeze lets the spoon go", "from the pin pull"};
    MLOG("offhand: OffHand.Grenade=%d (the default; the menu's toggle is the player's); pin by %s, cooking %s, a release under "
         "%.1f m/s is %s, hand speed at most %.0f m/s, haptics %d", on_ ? 1 : 0, pinAuto_ ? "the take (auto)" : "the trigger",
         kCook[cook_], minThrow_, slowToss_ ? "tossed along the view" : "dropped with the hand's own velocity", maxHand_, haptics_ ? 1 : 0);
}

void OffHandGrenade::SetClick(bool on) {
    if (on == click_) return;
    click_ = on;
    throwGrip_ = false;
    MLOG("offhand: grenade hold -> %s", on ? "click (a click takes it; squeeze and let go to throw)" : "grip (held while gripped)");
}

void OffHandGrenade::InitMain(const std::wstring& ini) {
    Init(ini);
    main_ = true;
    on_ = GetPrivateProfileIntW(L"Weapon", L"GrenadePin", 0, ini.c_str()) != 0;
    MLOG("offhand: Weapon.GrenadePin=%d (the default; the menu's toggle is the player's): the gun hand's grenade by its trigger "
         "(pin, cook) and its grip (squeeze, swing, let go)", on_ ? 1 : 0);
}

bool OffHandGrenade::HeldPress(const In& in, bool atHolster) {
    if (main_) {
        if (state_ != kArmed && state_ != kCooking) return false;  // the pin in: the grip is the gun hand's as ever
        if (!frozen_) {
            throwGrip_ = true;
            Pulse(offHand_, 0.3f, 15.0f);
        }
        return true;
    }
    if (state_ == kNone || !click_ || frozen_) return true;
    offHand_ = in.offHand;
    if (state_ == kHeld) {
        if (!atHolster) return true;  // the pin in: only a holster takes it back
        const float pos[3] = {in.hand.position.x, in.hand.position.y, in.hand.position.z}, zero[3] = {0, 0, 0};
        Queue(shared::kNadePutBack, type_, pos, zero, in.now);
        Pulse(offHand_, 0.2f, 20.0f);
        To(kNone, "put back at a holster (click)");
        return true;
    }
    throwGrip_ = true;  // armed or cooking: the throw starts -- let go to throw
    Pulse(offHand_, 0.3f, 15.0f);
    return true;
}

void OffHandGrenade::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    MLOG("offhand: the off-hand grenade %s (the menu)", on ? "on" : "off");
}

void OffHandGrenade::Pulse(int hand, float amp, float ms) {
    if (!haptics_ || hand < 0 || hand > 1) return;
    pulseAmp_[hand] = std::max(pulseAmp_[hand], amp);
    pulseMs_[hand] = std::max(pulseMs_[hand], ms);
}

void OffHandGrenade::To(State s, const char* why) {
    if (s == state_) return;
    MLOG("offhand: %s -> %s (%s)", kStateName[state_], kStateName[s], why);
    state_ = s;
    if (s == kNone) {
        frozen_ = false;
        relatch_ = true;
        throwGrip_ = false;
    }
}

void OffHandGrenade::Queue(std::uint32_t event, std::uint32_t type, const float (&pos)[3], const float (&vel)[3], double now) {
    Pending p{event | (main_ ? shared::kNadeMain : 0u), type, {pos[0], pos[1], pos[2]}, {vel[0], vel[1], vel[2]}, now};
    pending_.push_back(p);
}

void OffHandGrenade::Poll(shared::Header* hdr, double now) {
    if (!hdr) return;
    // Every event sent has been taken (read before the status: a state older than the acknowledgement can't pass).
    acksDone_ = pending_.empty() && hdr->nadeEvtSeq == hdr->nadeEvtAck;
    shared::NadeStatus s{};
    std::uint32_t seq = 0;
    if (!shared::ReadNadeStatus(hdr, s, seq) || seq == lastSeq_) return;
    lastSeq_ = seq;
    statusAt_ = now;
    if (!seenStatus_) {
        seenStatus_ = true;
        pawnSeq_ = s.pawnSeq;
        boom_ = s.boom;
        ticks_ = s.ticks;
    }
    status_ = s;
    if (s.pawnSeq != pawnSeq_) {
        pawnSeq_ = s.pawnSeq;
        if (state_ != kNone) To(kNone, "a new pawn: the held grenade is gone");
    }
    if (s.boom != boom_) {
        boom_ = s.boom;
        To(kNone, "it went off in the hand");
        Pulse(0, 1.0f, 200.0f);
        Pulse(1, 1.0f, 200.0f);
    }
    if (s.ticks != ticks_) {  // the fuse's countdown (the last 0.75 s stronger)
        ticks_ = s.ticks;
        if (state_ == kCooking) Pulse(offHand_, s.fuse < 0.75f ? 0.7f : 0.2f, s.fuse < 0.75f ? 50.0f : 10.0f);
    }
    unavailable_ = (s.caps & 16u) || main_ ? 0 : unavailable_ + 1;  // bit4: one held may stay (the off hand's; a switch to
                                                                      // another gun doesn't end it)
    // The reconcile: the game's state differs from ours for two of its frames with nothing in flight (a refused take,
    // a grenade gone with a new pawn, the host restarted) -- the game's wins.
    const bool gameMain = (s.state & 16u) != 0;
    const State game = gameMain == main_ ? static_cast<State>(s.state & 3u) : kNone;
    const State mine = main_ && state_ == kHeld ? kNone : state_;  // (the gun hand's pin in: no event, the game holds nothing)
    if (acksDone_ && game != mine) {
        if (++disagree_ >= 2) {
            disagree_ = 0;
            static const char* kWhy[] = {"", "unavailable", "none left", "not carried", "the launch failed"};
            const std::uint32_t refusal = (s.state >> 8) & 0xFFu;
            if (game == kNone && state_ != kNone && refusal) {
                MLOG("offhand: the game refused (%s)", refusal < 5 ? kWhy[refusal] : "?");
                Pulse(offHand_, 0.2f, 60.0f);
            }
            type_ = (s.state >> 2) & 3u;
            To(game, "the game's (reconciled)");
        }
    } else {
        disagree_ = 0;
    }
}

// Whether a take would work now (from this frame's input: a press comes before Frame), and why not ("" when it would).
// `applies`: an off-hand press at a grenade holster is the off-hand grenade's even when a take can't happen now -- switched
// on, the game's side alive, a gun in the other hand (nade3: a press during the parachute's landing fell through to the
// holster's SwitchGrenade, which the game ignored, and the gun went to the off hand).
const char* OffHandGrenade::UpdateActive(const In& in, bool* applies) {
    if (main_) {  // (the gun hand's: no take -- the grenade in hand is held)
        active_ = on_ && in.gunOk;
        if (applies) *applies = false;
        return "";
    }
    const bool alive = statusAt_ >= 0.0 && in.now - statusAt_ < 0.25;
    // (Ours through a short stall of the game too: the holster's own draw would put the gun in the off hand.)
    const bool seen = statusAt_ >= 0.0 && in.now - statusAt_ < 3.0;
    if (applies) *applies = on_ && seen && in.gunOk;
    const char* why = !on_ ? "switched off" : !alive ? "the game's side is quiet" : !(status_.caps & 1u) ? "no grenades the game can throw" :
                      !(status_.caps & 2u) ? "not now in the game" : !in.gunOk ? "no gun in the other hand" :
                      !in.offTracked ? "the off hand isn't tracked" : !in.gripActive ? "the off hand's grip is asleep" :
                      !in.gestures ? "a menu is open" : !in.hasView ? "no head-tracked view" : "";
    const bool active = !*why;
    if (active != active_ || (!active && whyInactive_ != why)) {
        if (active != active_ || on_) MLOG("offhand: %s%s%s", active ? "a take works now" : "no take (", active ? "" : why, active ? "" : ")");
        whyInactive_ = why;
    }
    active_ = active;
    return why;
}

bool OffHandGrenade::TakePress(const In& in, std::uint32_t type) {
    bool applies = false;
    const char* why = UpdateActive(in, &applies);
    if (!applies || state_ != kNone) return false;
    offHand_ = in.offHand;
    if (!active_) {
        MLOG("offhand: a take at the grenade holster -- not now (%s)", why);
        Pulse(offHand_, 0.2f, 60.0f);
        return true;  // the press is ours: the gun stays in its hand
    }
    const std::uint32_t t = type == shared::kNadeAny ? status_.next : type;
    const int count = t < 3 ? status_.count[t] : 0;
    if (count <= 0) {
        MLOG("offhand: a take at the grenade holster -- %s", t < 3 && status_.count[t] < 0 ? "that grenade isn't carried" : "no grenades left");
        Pulse(offHand_, 0.2f, 60.0f);
        return true;  // the press is ours: no grenade becomes the main weapon either
    }
    const float zero[3] = {0, 0, 0};
    type_ = t;
    Queue(shared::kNadeTake, t, zero, zero, in.now);
    To(kHeld, "taken at the holster");
    Pulse(offHand_, 0.5f, 30.0f);
    if (pinAuto_) {
        Queue(shared::kNadePin, t, zero, zero, in.now);
        To(kArmed, "Pin=auto");
        if (cook_ == 2) {
            Queue(shared::kNadeCook, t, zero, zero, in.now);
            To(kCooking, "Pin=auto, Cook=pin: the fuse burns");
        }
    }
    return true;
}

// The grip let go of an armed or cooking grenade: thrown with the hand's velocity, or -- slower than MinThrowSpeed --
// tossed along the view (SlowRelease=toss) or let fall with the hand's own velocity (drop).
void OffHandGrenade::Release(const In& in, bool slowAsToss, const char* why, bool forceToss) {
    float v[3] = {0, 0, 0};
    bool test = false;
    // The newest tracked sample (this frame's while tracked): with the hand untracked, the release point is where it was
    // last seen, and its velocity then counts only if that was under 0.25 s ago.
    const Sample& newest = hist_[(histNext_ + 23) % 24];
    const bool seen = newest.t > 0.0, stale = !seen || in.now - newest.t > 0.25;
    float pos[3] = {in.hand.position.x, in.hand.position.y, in.hand.position.z};
    if (!in.offTracked && seen)
        for (int i = 0; i < 3; ++i) pos[i] = newest.p[i];
    const float zero[3] = {0, 0, 0};
    if (forceToss) {
        Queue(shared::kNadeThrow | shared::kNadeToss, type_, pos, zero, in.now);
        Pulse(offHand_, 0.3f, 30.0f);
        MLOG("offhand: tossed along the view (%s)", why);
        To(kNone, "released");
        return;
    }
    if (in.testThrow) {
        for (int i = 0; i < 3; ++i) v[i] = in.testVel[i];
        test = true;
        testUsed_ = true;
    } else if (!stale) {
        // The newest sample of the hand point and the one closest to 0.1 s before it (the gun hand's throw rule).
        const Sample* then = nullptr;
        for (int k = 2; k < 24; ++k) {
            const Sample& c = hist_[(histNext_ + 24 - k) % 24];
            if (c.t <= 0.0 || newest.t - c.t > 0.2) break;
            then = &c;
            if (newest.t - c.t >= 0.1) break;
        }
        if (then && newest.t > then->t)
            for (int i = 0; i < 3; ++i) v[i] = static_cast<float>((newest.p[i] - then->p[i]) / (newest.t - then->t));
    }
    float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (speed > maxHand_) {
        for (float& x : v) x *= maxHand_ / speed;
        speed = maxHand_;
    }
    const char* untracked = in.offTracked ? "" : stale ? " (the hand untracked: no velocity)" : " (the hand untracked: its last velocity)";
    if (speed >= minThrow_) {
        Queue(shared::kNadeThrow, type_, pos, v, in.now);
        Pulse(offHand_, 0.5f, 30.0f);
        MLOG("offhand: thrown at %.1f m/s (%.1f %.1f %.1f)%s%s -- %s", speed, v[0], v[1], v[2], test ? " (the test velocity)" : "",
             untracked, why);
    } else if (slowAsToss) {
        Queue(shared::kNadeThrow | shared::kNadeToss, type_, pos, v, in.now);  // (v: if the toss is blocked, the game's fallback)
        Pulse(offHand_, 0.3f, 30.0f);
        MLOG("offhand: let go at %.1f m/s%s -- tossed along the view (%s)", speed, untracked, why);
    } else {
        Queue(shared::kNadeThrow, type_, pos, v, in.now);
        MLOG("offhand: let go at %.1f m/s%s -- dropped (%s)", speed, untracked, why);
    }
    To(kNone, "released");
}

void OffHandGrenade::Frame(const In& in, Out& out) {
    offHand_ = in.offHand;
    hand_ = in.hand;
    haveHand_ = true;
    // The hand point's history, tracked samples only (Release: the last good velocity).
    if (in.offTracked) {
        Sample& s = hist_[histNext_];
        histNext_ = (histNext_ + 1) % 24;
        s.t = in.now;
        s.p[0] = in.hand.position.x;
        s.p[1] = in.hand.position.y;
        s.p[2] = in.hand.position.z;
    }
    const char* why = UpdateActive(in);
    if (main_) {
        const bool inHand = on_ && in.gunOk;
        if (state_ == kNone && inHand) {
            type_ = in.type;
            To(kHeld, "a grenade in the gun hand");
        } else if (state_ != kNone && !inHand) {
            if (state_ == kCooking) {
                Release(in, true, "it left the gun hand while cooking", true);
            } else {
                if (state_ == kArmed) {
                    const float p[3] = {in.hand.position.x, in.hand.position.y, in.hand.position.z}, z[3] = {0, 0, 0};
                    Queue(shared::kNadePutBack, type_, p, z, in.now);
                }
                To(kNone, "it left the gun hand");
            }
        }
        if (inHand) out.maskTrigger = true;  // the game's own throw never starts
    }
    if (state_ == kCooking && in.modMenu && !in.gameMenu)
        Release(in, true, "the MOHAVR menu opened while cooking: the game runs on", true);
    if (state_ != kNone) {
        out.maskTrigger = true;  // a pin pull isn't the game's aim
        out.maskSwitch = !main_; // nor its own grenade switch (the gun hand's grenade may switch to another type)
        trigMaskHeld_ = true;
        // Frozen while a menu is open or the grip action sleeps (not for lost tracking: a release then throws with the
        // last good velocity, OFFHAND-DESIGN 0.2).
        const bool frozen = !in.gestures || !in.gripActive;
        const bool wasFrozen = frozen_;
        if (frozen != frozen_) {
            frozen_ = frozen;
            MLOG("offhand: %s (%s)", frozen ? "frozen" : "carries on", frozen ? why : "the hand and grip are back");
            // (Click or the gun hand: a throw squeeze under way ends with the freeze -- the armed grenade stays in the hand,
            // a new squeeze-and-release throws it; review: let go during the freeze, it was tossed. Cooking keeps its toss.)
            if (frozen && state_ != kCooking) throwGrip_ = false;
        }
        // The trigger's squeezes (>= 0.6 after < 0.4): none while frozen, and one already under way when the hold starts or
        // a freeze ends must be let go first (a fist closing on the grab; a trigger held through a menu or the dashboard).
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
        const float pos[3] = {in.hand.position.x, in.hand.position.y, in.hand.position.z};
        const float zero[3] = {0, 0, 0};
        if (!frozen) {
            if (unavailable_ >= 2) {
                // The game made it unavailable while held (a ladder, a cutscene, a grenade became the main weapon).
                if (state_ == kCooking) {
                    Release(in, true, "the game made it unavailable", true);
                } else {
                    Queue(shared::kNadePutBack, type_, pos, zero, in.now);
                    To(kNone, "put back: the game made it unavailable");
                }
            } else if (click_ || main_ ? (throwGrip_ && !in.gripHeld) : !in.gripHeld) {
                if (state_ == kHeld || (state_ == kArmed && wasFrozen && !click_ && !main_)) {
                    // Let go with the pin in -- or found let go after a freeze, the pin out but the spoon still on.
                    const Sample& last = hist_[(histNext_ + 23) % 24];
                    float at[3] = {pos[0], pos[1], pos[2]};
                    if (!in.offTracked && last.t > 0.0)
                        for (int i = 0; i < 3; ++i) at[i] = last.p[i];
                    Queue(shared::kNadePutBack, type_, at, zero, in.now);
                    Pulse(offHand_, 0.2f, 20.0f);
                    To(kNone, wasFrozen ? "put back: let go during the freeze" : "put back");
                } else if (wasFrozen) {
                    Release(in, true, "let go during the freeze", true);  // cooking: tossed, with no velocity of its own
                } else {
                    Release(in, slowToss_, "let go");
                }
            } else if (press && state_ == kHeld) {
                Queue(shared::kNadePin, type_, zero, zero, in.now);
                To(kArmed, "the pin pulled");
                Pulse(offHand_, 0.6f, 20.0f);
                if (cook_ == 2) {
                    Queue(shared::kNadeCook, type_, zero, zero, in.now);
                    To(kCooking, "Cook=pin: the fuse burns");
                }
            } else if (press && state_ == kArmed && cook_ == 1) {
                Queue(shared::kNadeCook, type_, zero, zero, in.now);
                To(kCooking, "the spoon let go: the fuse burns");
                Pulse(offHand_, 0.8f, 40.0f);
            }
        }
    } else {
        relatch_ = true;  // the next hold starts with the trigger as it is then
        frozen_ = false;
        if (trigMaskHeld_ && in.trigger >= 0.4f) out.maskTrigger = true;  // still squeezed from the hold: kept until let go
        else trigMaskHeld_ = false;
    }
    out.usedTest = testUsed_;
    testUsed_ = false;
    for (int h = 0; h < 2; ++h) {
        out.pulseAmp[h] = pulseAmp_[h];
        out.pulseMs[h] = pulseMs_[h];
        pulseAmp_[h] = pulseMs_[h] = 0.0f;
    }
}

void OffHandGrenade::Send(shared::Header* hdr, double now) {
    if (!hdr) return;
    // Events, in order; never more than the ring holds unread (RELOAD-DESIGN 4).
    while (!pending_.empty()) {
        const Pending p = pending_.front();
        if (hdr->nadeEvtSeq - hdr->nadeEvtAck >= 8u) {
            if (now - p.at > 0.25) {
                MLOG("offhand: %s dropped -- the game has not taken the last 8 events", kEventName[p.event & 0xFFu]);
                pending_.pop_front();
            }
            break;
        }
        const std::uint32_t s = hdr->nadeEvtSeq;
        hdr->nadeEvt[s % 8u] = p.event | ((p.type & 0xFFu) << 8);
        for (int i = 0; i < 3; ++i) {
            hdr->nadeEvtPos[s % 8u][i] = p.pos[i];
            hdr->nadeEvtVel[s % 8u][i] = p.vel[i];
        }
        _ReadWriteBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->nadeEvtSeq));
        MLOG("offhand: sent %s%s (%s)", kEventName[p.event & 0xFFu], (p.event & shared::kNadeToss) ? " (a toss)" : "", TypeName(p.type));
        pending_.pop_front();
    }
}

std::uint32_t OffHandGrenade::Flags() const {
    return (on_ ? 1u : 0u) | (state_ >= kHeld ? 2u : 0u) | (state_ >= kArmed ? 4u : 0u) | (state_ == kCooking ? 8u : 0u) |
           ((type_ & 3u) << 4) | (frozen_ ? 64u : 0u) | (main_ && state_ != kNone ? 128u : 0u);
}

shared::Pose OffHandGrenade::HandPose() const {
    const XrPosef& p = hand_;
    return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

}  // namespace mohavr::host
