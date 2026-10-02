// The off-hand grenade (OFFHAND-DESIGN.md; the player, 2026-10-02: "being able to grab a grenade with the off hand and
// throw it without unequipping your gun would be very immersive") -- host side.
//
// The off hand's grip at a grenade holster (today's LeftHip SwitchGrenade spot) takes a grenade while a gun is in the
// other hand (TAKE: the pin in, safe); its trigger pulls the pin (PIN); a second squeeze lets the spoon go (COOK: the fuse
// burns in the hand); letting go of the grip throws it with the hand's velocity (THROW), or puts it back while the pin is
// in (PUT BACK). An armed grenade let go with almost no speed is tossed the game's gentlest way along the view
// ([OffHand] SlowRelease). Everything freezes while a menu is open or the hand's grip action sleeps (a cooking grenade is
// tossed when the MOHAVR menu opens: it doesn't pause the game); let go while the hand isn't tracked, it flies with the
// hand's last tracked velocity.
// The game (src/mohavr/offhand.cpp) executes the events in order (shared block v20), keeps the fuse clock and publishes
// what a take can get; the host follows its state (the reconcile, the manual reload's rules).
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

class OffHandGrenade {
public:
    // The shipped ini's [OffHand] (Grenade is the default; the menu's toggle is the player's).
    void Init(const std::wstring& ini);
    // The gun hand's grenade instead ([Weapon] GrenadePin; the player, round 43: "Arming the grenade with trigger is good,
    // add that to main hand grenades" -- and cooking, and the grip throw): with a grenade the weapon in the gun hand, its
    // trigger pulls the pin and a second pull cooks; squeeze the grip, swing and let go to throw. The game's own throw never
    // starts (its trigger is kept from the game). In: the gun hand (offHand = the gun hand, gunOk = a grenade in it).
    void InitMain(const std::wstring& ini);
    bool Main() const { return main_; }
    void SetOn(bool on);
    bool On() const { return on_; }
    // [OffHand] GrenadeHold (the menu's "Grenade hold"; the player, round 43): grip = held while the grip is, let go throws
    // (or puts back with the pin in); click = a click takes it and it stays, a squeeze-and-release throws once the pin is
    // out, a click at a holster puts it back with the pin in.
    void SetClick(bool on);
    bool Click() const { return click_; }
    // Start of the XR frame, before Hands::Update: the game's status (caps, counts, its state, acknowledgements, the
    // countdown ticks, a grenade gone off in the hand, a new pawn) and the reconcile.
    void Poll(shared::Header* hdr, double now);

    struct In {
        int     offHand = 0;          // the physical off hand (0 left, 1 right)
        bool    offTracked = false;   // really tracked (not held where it was by Hands.HoldLost)
        bool    modMenu = false;      // the MOHAVR menu is open (it doesn't pause the game: a cooking grenade is tossed)
        bool    gameMenu = false;     // one of the game's menus is open (it pauses the game: the fuse stands still)
        bool    gripActive = true;    // its grip action is active (a sleeping controller's grip reads 0)
        bool    gestures = false;     // no menu open
        bool    hasView = false;      // the game draws a head-tracked view
        bool    gunOk = false;        // the gun hand holds a gun (not a grenade)
        bool    gripHeld = false;     // the off grip held (hands.cpp's hysteresis)
        float   trigger = 0.0f;       // the off trigger
        std::uint32_t type = 0;       // (the gun hand's) the grenade in hand: 0 frag, 1 Gammon, 2 stick
        XrPosef hand{};               // the off hand's hand point (position) and aim orientation, LOCAL
        bool    testThrow = false;    // a test velocity (pad_cmd.txt "throwvel=") for the next release
        float   testVel[3]{};
        double  now = 0.0;
    };
    struct Out {
        bool  maskTrigger = false;    // the off trigger kept from the pad (a pin pull isn't the game's aim)
        bool  maskSwitch = false;     // Xbox RB (the game's own grenade switch) kept from the game
        bool  usedTest = false;       // the test velocity was taken
        float pulseAmp[2]{}, pulseMs[2]{};
    };
    // A take can happen now (the switch on, the game's side alive and available, the hands ready).
    bool Active() const { return active_; }
    bool Holding() const { return state_ != kNone; }
    // Hands::Update at an off-hand grip press in a grenade holster: true = the press is the off-hand grenade's (a take, or
    // refused: nothing of that type left, or not now -- the landing, a weapon switch); false = it doesn't apply (switched
    // off, the game's side quiet, no gun in the other hand: the holster's own command, as before). `type`: 0 frag,
    // 1 Gammon, 2 stick, 0xFF any.
    bool TakePress(const In& in, std::uint32_t type);
    // Hands::Update at a grip press of this grenade's hand while one is held: true = the press is the grenade's. The off
    // hand's always is (click mode: at a holster with the pin in it goes back; with the pin out the squeeze starts the
    // throw); the gun hand's only once the pin is out (the squeeze starts the throw).
    bool HeldPress(const In& in, bool atHolster);
    // Hands::Update, every frame after the presses.
    void Frame(const In& in, Out& out);
    // After Hands::Update: the queued events to the game (never more than the ring holds unread).
    void Send(shared::Header* hdr, double now);
    // Written inside the view seqlock.
    std::uint32_t Flags() const;
    shared::Pose  HandPose() const;

private:
    enum State { kNone = 0, kHeld = 1, kArmed = 2, kCooking = 3 };
    void Queue(std::uint32_t event, std::uint32_t type, const float (&pos)[3], const float (&vel)[3], double now);
    void To(State s, const char* why);
    void Pulse(int hand, float amp, float ms);
    // The grip let go of an armed / cooking grenade (forceToss: tossed along the view whatever the hand's speed).
    void Release(const In& in, bool slowAsToss, const char* why, bool forceToss = false);
    const char* UpdateActive(const In& in, bool* applies = nullptr);  // Active() from this frame's input; why not ("" if so)

    bool          on_ = false;
    bool          click_ = false;        // GrenadeHold=click
    bool          main_ = false;         // the gun hand's grenade (InitMain)
    bool          throwGrip_ = false;    // (click) the grip squeezed to throw: letting go throws
    // [OffHand]
    bool          pinAuto_ = false;      // Pin=auto: the take arms it
    int           cook_ = 1;             // Cook: 0 off, 1 spoon (a 2nd trigger squeeze), 2 pin (the fuse starts at the pin)
    float         minThrow_ = 1.0f;      // MinThrowSpeed (m/s): slower = SlowRelease
    bool          slowToss_ = true;      // SlowRelease=toss (else drop: the hand's own velocity)
    float         maxHand_ = 12.0f;      // MaxHandSpeed (m/s)
    bool          haptics_ = true;
    // The game's side.
    shared::NadeStatus status_{};
    std::uint32_t lastSeq_ = 0, pawnSeq_ = 0, boom_ = 0, ticks_ = 0;
    bool          seenStatus_ = false;
    double        statusAt_ = -1.0;
    bool          acksDone_ = true;
    int           disagree_ = 0, unavailable_ = 0;
    bool          active_ = false;
    std::string   whyInactive_ = "?";
    // Ours.
    State         state_ = kNone;
    std::uint32_t type_ = shared::kNadeAny;
    bool          frozen_ = false;
    bool          trigHeld_ = false;
    bool          relatch_ = true;       // the trigger's state is taken as it is (a hold starting, a freeze ending)
    bool          needRest_ = false;     // ... and it was squeezed then: no squeeze counts until it is let go (< 0.15)
    bool          trigMaskHeld_ = false; // the hold ended with the trigger squeezed: still kept from the game
    int           offHand_ = 0;
    XrPosef       hand_{};
    bool          haveHand_ = false;
    struct Sample {
        double t;
        float  p[3];
    };
    Sample        hist_[24]{};
    int           histNext_ = 0;
    float         pulseAmp_[2]{}, pulseMs_[2]{};
    bool          testUsed_ = false;     // (Release) the test velocity was taken
    struct Pending {
        std::uint32_t event, type;
        float         pos[3], vel[3];
        double        at;
    };
    std::deque<Pending> pending_;
};

}  // namespace mohavr::host
