// Hands (M8): what the player's two controllers do besides buttons -- worked out in the host, where the poses are
// freshest, and handed to the game as a gun pose, an aim line and commands (shared block v9).
//
//   * The gun hand (right, or left with LeftHanded) holds the gun: gunPose = its aim pose pitched by the fit's angle.
//   * Foregrip: the other hand's grip squeezed at the gun's foregrip (the fit's foreFwd/foreUp from the gun hand)
//     makes it two-handed while held -- the gun then points from the gun hand through the other hand.
//   * The aim line: gunPose offset by the fit's aim line (up/right).
//   * Holsters: a grip squeezed at a body spot (relative to the head's heading) draws that weapon -- right shoulder
//     long gun 1, left shoulder long gun 2, right hip pistol, left hip grenade ([Holsters] in MOHAVR.ini).
//   * Reload gesture: the other hand's grip squeezed at the gun's magazine.
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

namespace mohavr::host {

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
    };
    struct Output {
        int         gunHand = 1;    // 0 left, 1 right: the hand that drew last (the pad's fire side follows it)
        bool        gunValid = false, twoHanded = false;
        XrPosef     gun{}, aimRay{};
        std::string command;        // to run in the game this frame ("" none)
        bool        consumed[2]{};  // grips kept from the pad
        bool        pulse[2]{};     // haptic pulse this frame
    };
    Output Update(const Input& in);
    // The fit without a menu: the shipped defaults ([Aim] RayUp, [Hands] ForeFwd/ForeUp).
    const shared::GunFit& DefaultFit() const { return defaultFit_; }

private:
    struct Zone {
        const wchar_t* key;
        float          x, y, z;  // metres from the head: right, up, forward (heading frame)
        std::string    command;
    };
    Zone  zones_[4];
    float zoneRadius_ = 0.16f;
    bool  holsters_ = true, foregrip_ = true, reloadGesture_ = true;
    float gripWas_[2]{};
    bool  held_[2]{};          // grip pressed (hysteresis)
    bool  consumed_[2]{};      // this press was used by a gesture
    bool  twoHanded_ = false;
    int   inZone_[2] = {-1, -1};
    bool  nearFore_ = false;
    int   gunHand_ = 1;
    int   lastStart_ = -1;       // the start setting last applied
    shared::GunFit defaultFit_{};
};

}  // namespace mohavr::host
