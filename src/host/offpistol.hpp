// The off-hand pistol (OFFPISTOL-DESIGN.md; the player, 2026-10-02: "Is it possible to build similar system for using the
// pistol with the off hand?") -- host side.
//
// With a gun in the gun hand, the off hand's grip at a pistol holster (a holster whose command is SwitchPistol) draws the
// pistol into that hand (DRAW). [OffHand] PistolHold=toggle (the player's choice, "click"): it stays without the grip, and a
// squeeze at any holster puts it back (HOLSTER); grip: it is held while the grip is, letting go puts it back. Its trigger
// fires one round per pull (SHOT, with the off line of that moment; the game keeps the pistol's own rate and fires a C96
// at its 712 level while the trigger stays held). Everything freezes while a menu is open, the grip action sleeps or the
// off hand isn't tracked (a shot from a stale pose would aim wrong). The second red dot sits on its line.
// The game (src/mohavr/offpistol.cpp) executes the events in order (shared block v21), refills the pistol in the holster and
// publishes what a draw gets; the host follows its state (the reconcile, the grenade's rules).
#pragma once
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <deque>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class OffHandPistol {
public:
    // The shipped ini's [OffHand] (Pistol is the default; the menu's toggle is the player's).
    void Init(const std::wstring& ini);
    void SetOn(bool on);
    bool On() const { return on_; }
    // Start of the XR frame, before Hands::Update: the game's status (caps, the pistol, its state, acknowledgements, the
    // shots, dry clicks and refills for the pulses, a new pawn) and the reconcile.
    void Poll(shared::Header* hdr, double now);
    // The pistol a draw gets / the held one (its attachment class: the fit's key; "" none).
    const std::string& Key() const { return key_; }

    struct In {
        int            offHand = 0;          // the physical off hand (0 left, 1 right)
        bool           offValid = false;     // its aim pose is valid (tracked, or held where it was by Hands.HoldLost)
        bool           offTracked = false;   // really tracked
        bool           gripActive = true;    // its grip action is active (a sleeping controller's grip reads 0)
        bool           gestures = false;     // no menu open
        bool           hasView = false;      // the game draws a head-tracked view
        bool           gunOk = false;        // the gun hand holds a weapon
        bool           nadeHeld = false;     // the off hand holds a grenade
        bool           gripHeld = false;     // the off grip held (hands.cpp's hysteresis)
        float          trigger = 0.0f;       // the off trigger
        XrPosef        offAim{};             // the off hand's aim pose, LOCAL
        shared::GunFit fit{};                // the pistol's fit (the menu's, for Key())
        double         now = 0.0;
    };
    struct Out {
        bool          maskTrigger = false;   // the off trigger kept from the pad (the pistol's, not the game's aim)
        bool          maskSwitch = false;    // Xbox B (the game's switch weapon) kept back: it would take the held pistol
        float         pulseAmp[2]{}, pulseMs[2]{};
    };
    // A draw can happen now (the switch on, the game's side alive and available, the hands ready).
    bool Active() const { return active_; }
    bool Holding() const { return state_ != kNone; }
    bool Toggle() const { return toggle_; }
    // Hands::Update at an off-hand grip press in a pistol holster, nothing held: true = the press is the off-hand pistol's (a
    // draw, or refused: not now -- the landing, a weapon switch; the only pistol in the gun hand); false = it doesn't apply
    // (switched off, the game's side quiet, no gun in the other hand: the holster's own command, as before).
    bool DrawPress(const In& in);
    // Hands::Update at an off-hand grip press while one is held (toggle): at a holster it goes back; elsewhere the press is
    // just kept from everything else. Always the pistol's.
    void HeldPress(const In& in, bool atHolster);
    // Hands::Update, every frame after the presses.
    void Frame(const In& in, Out& out);
    // After Hands::Update: the queued events to the game (never more than the ring holds unread).
    void Send(shared::Header* hdr, double now);
    // Written inside the view seqlock.
    std::uint32_t Flags() const;
    float         Trigger() const { return trigger_; }
    shared::Pose  AimRay() const;
    void          Fit(float (&out)[4]) const;
    // The second dot: along this frame's off line, when held ([OffHand] PistolDot) and the game traced it.
    bool DotRay(XrPosef& ray) const;

private:
    enum State { kNone = 0, kHeld = 1 };
    void Queue(std::uint32_t event, const XrPosef& ray, double now);
    void To(State s, const char* why);
    void Pulse(int hand, float amp, float ms);
    void Holster(const In& in, const char* why);
    const char* UpdateActive(const In& in, bool* applies = nullptr);  // Active() from this frame's input; why not ("" if so)

    bool          on_ = false;
    // [OffHand]
    bool          toggle_ = true;        // PistolHold=toggle (else grip)
    bool          dot_ = true;           // PistolDot
    bool          keep_ = true;          // PistolKeep: Xbox B kept back while the game's switch would take the held pistol
    bool          pair_ = false;         // PistolPair (Phase 3: the gun hand's own pistol twinned)
    bool          haptics_ = true;
    // The game's side.
    shared::PistolStatus status_{};
    std::string   key_;
    std::uint32_t lastSeq_ = 0, pawnSeq_ = 0, shots_ = 0, dry_ = 0, refills_ = 0;
    bool          seenStatus_ = false;
    double        statusAt_ = -1.0;
    bool          acksDone_ = true;
    int           disagree_ = 0, unavailable_ = 0;
    bool          active_ = false;
    std::string   whyInactive_ = "?";
    // Ours.
    State         state_ = kNone;
    bool          frozen_ = false;
    bool          trigHeld_ = false;
    bool          relatch_ = true;       // the trigger's state is taken as it is (a hold starting, a freeze ending)
    bool          needRest_ = false;     // ... and it was squeezed then: no pull counts until it is let go (< 0.15)
    bool          trigMaskHeld_ = false; // the hold ended with the trigger squeezed: still kept from the game
    bool          letGoFrozen_ = false;  // (grip mode) let go during a freeze: put back at the unfreeze
    int           offHand_ = 0;
    float         trigger_ = 0.0f;
    XrPosef       ray_{};
    bool          rayOk_ = false;
    float         fit_[4]{};
    float         pulseAmp_[2]{}, pulseMs_[2]{};
    struct Pending {
        std::uint32_t event;
        XrPosef       ray;
        double        at;
    };
    std::deque<Pending> pending_;
};

}  // namespace mohavr::host
