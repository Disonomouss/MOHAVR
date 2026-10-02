// Hands (M8): what the player's two controllers do besides buttons -- worked out in the host, where the poses are
// freshest, and handed to the game as a gun pose, an aim line and commands (shared block v9).
//
//   * The gun hand (right, or left with LeftHanded) holds the gun: gunPose = its aim pose pitched by the fit's angle.
//   * Foregrip: the other hand's grip squeezed at the gun's foregrip (the fit's foreFwd/foreUp from the gun hand)
//     makes it two-handed while held -- the gun then points from the gun hand through the other hand.
//   * The aim line: gunPose offset by the fit's aim line (up/right).
//   * Holsters: a grip squeezed at a body spot (relative to the head's heading) draws that weapon -- right shoulder
//     long gun 1, left shoulder long gun 2, right hip pistol, left hip grenade, chest pistol ([Holsters] in MOHAVR.ini;
//     what each holds is the player's, the menu's Holsters page).
//   * Reload gesture: the other hand's grip squeezed at the gun's magazine (not while the manual reload drives the gun).
//   * The manual reload (D21, reload.hpp): its spots (the magazine, the belt pouch) come before the holsters and the
//     foregrip at an off-hand press; the belt pouch is the 5th body spot ([Holsters] MagPouchSpot).
//   * The off-hand grenade (offhand.hpp): the off hand at a grenade holster takes a grenade while a gun is in the other
//     hand (after the reload's spots); the gun hand there is refused while one is held.
//   * The off-hand pistol (offpistol.hpp): the off hand at a pistol holster draws the pistol while a gun is in the other
//     hand; while it holds it, its presses are the pistol's (a click at a holster puts it back), the foregrip and the
//     reload's spots are out of reach, and the gun hand at a pistol holster is refused.
//   A grip used for any of these is kept from the pad mapping until released (Consumed). A short pulse on the
//   controller marks a hand entering a holster spot or the foregrip.
#pragma once
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>

#include "../common/shared_frame.hpp"
#include "offhand.hpp"
#include "offpistol.hpp"
#include "reload.hpp"

namespace mohavr::host {

// A holster spot: metres from the head in its heading frame (right, up, forward), and its radius.
struct HolsterSpot {
    float x, y, z, r;
};
constexpr int kHolsters = 5;  // right shoulder, left shoulder, right hip, left hip, chest
constexpr int kSpots = kHolsters + 1;  // ... and the magazine pouch (the manual reload; it runs no command)

class Hands {
public:
    // Reads [Holsters] / [Hands] from the shipped ini.
    void Init(const std::wstring& ini);

    struct Input {
        XrPosef        aim[2];      // left, right aim poses (LOCAL)
        std::uint32_t  valid;       // bit per hand
        XrPosef        head;        // VIEW in LOCAL
        float          grip[2];     // squeeze 0..1
        shared::GunFit fit;         // the weapon in hand's
        bool           startLeft;   // the gun hand until a hand draws (the menu's Gun hand); a change applies at once
        bool           gestures;    // false while a menu is open (the MOHAVR menu or the game's)
        // Throwing: the triggers, whether the weapon in hand is a grenade, the time (s), and a test velocity.
        float          trigger[2];
        bool           grenade;
        std::uint32_t  weaponKind;  // 0 long gun, 1 pistol, 2 grenade (hdr->weaponKind)
        double         now;
        bool           testThrow;
        float          testThrowVel[3];
        // The manual reload: its release button per physical hand, whether the game draws a head-tracked view, and
        // whether the pouch's ring shows anyway (the menu's Holsters page).
        float          release[2];
        bool           hasView;
        bool           pouchShown;
        bool           reloadSpotsShown;  // the menu's Reload spots page: the grab rings show
        // The off-hand grenade: which hands are really tracked (before Hands.HoldLost keeps a lost one), whose grip action
        // is active.
        std::uint32_t  tracked = 3u;
        bool           gripActive[2] = {true, true};
        bool           modMenu = false;  // the MOHAVR menu is open (it doesn't pause the game)
        bool           gameMenu = false; // one of the game's menus is open (it does)
        shared::GunFit pistolFit{};      // the off-hand pistol's fit (the menu's, for its key)
        int            grenadeType = -1; // the grenade in the gun hand: 0 frag, 1 Gammon, 2 stick (-1 none)
    };
    // Where the gesture spots are this frame (LOCAL), for the rings (markers.cpp).
    enum SpotKind { kHolster, kForegrip, kMagazine, kPouch, kMagWell };
    struct Spot {
        SpotKind   kind;
        XrVector3f pos;
        float      radius;
        bool       inside;   // a hand is in it (a squeeze would act)
        bool       close;    // a hand is within twice its radius
    };
    struct Output {
        Spot        spots[kHolsters + 5]{};
        int         spotCount = 0;
        bool        offValid = false;
        XrVector3f  offHand{};      // the off hand's aim pose position (its marker)
        int         gunHand = 1;    // 0 left, 1 right: the hand that drew last (the pad's fire side follows it)
        bool        gunValid = false, twoHanded = false;
        XrPosef     gun{}, aimRay{};
        std::string command;        // to run in the game this frame ("" none)
        bool        consumed[2]{};  // grips kept from the pad
        bool        pulse[2]{};     // haptic pulse this frame
        float       pulseAmp[2]{}, pulseMs[2]{};  // ... of this strength and length (0: the default short one)
        bool        maskFace[2]{};  // the manual reload's release button kept from the pad (per physical hand)
        bool        maskTrigger[2]{};  // ... and a trigger (the flip of a held taped pair)
        bool        targetOk[10]{}; // tests (pad_cmd.txt hand=l,@mag|@pouch|@bolt|@magin|@boltup|@boltback|@fore|@grenade|
        XrVector3f  target[10]{};   // @pistol|@chest): the magazine, the pouch, the action, the aim point that seats a held
                                    // magazine, a bolt lifted / back, the foregrip (GOAL A3: a pump gun's pump), the grenade
                                    // holster, the pistol holster, the chest holster
        bool        alignOk = false;  // tests: @magin with "align": the aim pose that seats the held magazine, turned too
        XrPosef     align{};
        bool        maskSwitch = false;  // the off hand holds a grenade: the game's own grenade switch (Xbox RB) kept back
        bool        maskSwitchB = false; // the off hand holds the pistol the game's switch weapon (Xbox B) would take
        bool        thrown = false; // the gun hand's trigger let go of a grenade this frame, fast enough to count
        float       throwVel[3]{};  // its velocity then (LOCAL, m/s)
    };
    Output Update(const Input& in);
    // The fit without a menu: the shipped defaults ([Aim] RayUp, [Hands] ForeFwd/ForeUp).
    const shared::GunFit& DefaultFit() const { return defaultFit_; }
    // The holster spots: the shipped ones ([Holsters] RightShoulderSpot=x y z r, cm), and the player's (the menu).
    const HolsterSpot& DefaultSpot(int i) const { return defaultSpots_[i]; }
    void SetSpot(int i, const HolsterSpot& s) { spots_[i] = s; }
    static const wchar_t* SpotName(int i);
    // What each holster holds: the game command it runs ("" none) -- the shipped ones ([Holsters] RightHip=SwitchPistol),
    // and the player's (the menu's Holsters page: Holds).
    const std::string& DefaultCommand(int i) const { return defaultCommands_[i]; }
    void SetCommand(int i, const std::string& c);
    // The manual reload (owned by the host's main loop; null = none).
    void SetReload(ManualReload* r) { reload_ = r; }
    // The off-hand grenade (likewise).
    void SetOffHand(OffHandGrenade* n) { nade_ = n; }
    // The gun hand's grenade (an OffHandGrenade in its InitMain mode; likewise).
    void SetGunNade(OffHandGrenade* n) { gunNade_ = n; }
    // The off-hand pistol (likewise).
    void SetOffPistol(OffHandPistol* p) { pistol_ = p; }
    // Round 31: where on each controller the hand interacts (the white dot; metres from the aim point: forward, up, in
    // toward the palm -- mirrored for the left hand), the foregrip ring's radius (m) and the reload rings' scale.
    void SetHandPoint(const float (&fwdUpIn)[3]) { for (int i = 0; i < 3; ++i) handPoint_[i] = fwdUpIn[i]; }
    void SetForegripRadius(float r) { foregripR_ = r; }
    void SetRingScale(float s) { ringScale_ = s; }

private:
    struct Zone {
        const wchar_t* key;
        std::string    command;
    };
    Zone        zones_[kHolsters];
    std::string defaultCommands_[kHolsters];
    HolsterSpot spots_[kSpots]{}, defaultSpots_[kSpots]{};
    ManualReload* reload_ = nullptr;
    OffHandGrenade* nade_ = nullptr;
    OffHandGrenade* gunNade_ = nullptr;
    OffHandPistol* pistol_ = nullptr;
    float handPoint_[3]{};       // forward, up, in (m)
    float foregripR_ = 0.12f;    // the foregrip ring's radius (m)
    float ringScale_ = 1.0f;     // the reload gesture's and the manual reload's grab rings
    bool  holsters_ = true, foregrip_ = true, reloadGesture_ = true;
    bool  mirrorLeft_ = true;  // the game draws the left hand's gun mirrored ([Weapon] LeftHandMirror): so is the aim line
    float gripWas_[2]{};
    bool  held_[2]{};          // grip pressed (hysteresis)
    bool  consumed_[2]{};      // this press was used by a gesture
    bool  twoHanded_ = false;
    int   inZone_[2] = {-1, -1};
    bool  nearFore_ = false;
    // Throwing: recent positions of each hand (for its velocity at release), the gun hand's trigger state.
    struct Sample { double t; float p[3]; };
    Sample hist_[2][16]{};
    int    histNext_[2]{};
    bool   triggerHeld_ = false;
    bool   testThrow_ = false;     // a test velocity waiting for the next release
    float  testThrowVel_[3]{};
    float  minThrowSpeed_ = 1.0f;  // m/s: slower releases keep the game's own trigger-strength throw
    int   gunHand_ = 1;
    int   lastStart_ = -1;       // the start setting last applied
    shared::GunFit defaultFit_{};
};

}  // namespace mohavr::host
