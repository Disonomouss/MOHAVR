// The virtual Xbox pad (M6, Input.Controllers): the headset's controllers -> an XINPUT_GAMEPAD in the
// shared block, which the game-side XInputGetState hook hands to MOHA as pad 0.
//
// The mapping is [Controls] in MOHAVR.ini (next to the host): each Xbox control takes one or more
// controller inputs. Defaults (the player's layout from headset round 5, on MOHA's own pad bindings):
// A jump, B reload/use, right grip use, Y switch weapon, X grenade, right-stick flick down crouch
// (a stance toggle in MOHA), left-stick click sprint (latched), triggers aim/fire, left grip alt fire,
// right-stick click melee, a TAP of the left menu button Start (holding it opens the MOHAVR menu;
// main.cpp). Right stick Y is not an axis by default: the head drives pitch. While one of the game's menus is
// open (hdr->gameUiMenu) a menu layout applies instead ([ControlsMenu]: A select, B back, X, Y as labelled).
//
// Test channel (the simulator has no controllers): %TEMP%\MOHAVR\pad_cmd.txt, one state per line,
// played in order, each for its duration -- tools/pad_cmd.py writes it.
#pragma once
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class Pad {
public:
    // Creates the gameplay action set and reads [Controls] from `ini`.
    bool Init(XrInstance instance, const std::wstring& ini);
    XrActionSet Set() const { return set_; }
    // Adds this set's suggested bindings for `profile` (all of a profile's bindings go in one call).
    void AppendBindings(const std::string& profile, const std::function<XrPath(const char*)>& path,
                        std::vector<XrActionSuggestedBinding>& out) const;
    // Holds Start for a moment (a tap of the menu button; see main.cpp).
    void PulseStart() { startUntil_ = now_ + 0.15; }
    // Per XR frame after xrSyncActions with Set() active. `neutral` (the MOHAVR menu is open) publishes
    // a centred pad with nothing pressed, so the game neither moves nor sees a disconnect.
    // `snapDeg` > 0: the right stick's X snaps by that many degrees per flick (hdr->snapYawTotal) instead
    // of turning smoothly.
    void Update(XrSession session, double now, bool neutral, int snapDeg, shared::Header* hdr);

    // M7: the controllers' aim poses (/input/aim/pose). CreateSpaces once the action sets are attached.
    bool CreateSpaces(XrSession session);
    // Both aim poses in `space` at `t`; the result's bit 0/1 = left/right valid. A test aim from
    // pad_cmd.txt ("aim=yaw,pitch" in degrees; "aim=off") replaces the right hand with a pose 20 cm right,
    // 30 cm below and 30 cm ahead of `head`, turned by yaw/pitch from the head's heading.
    std::uint32_t LocateHands(XrSpace space, XrTime t, const XrPosef& head, XrPosef (&out)[2]) const;

private:
    // Controller inputs. kMenu is the tap of the menu button (main.cpp owns it); kRFlickDown/Up are short
    // pulses when the right stick is flicked down/up (derived from its Y, which isn't a stick axis by default).
    enum Src { kNone, kA, kB, kX, kY, kLGrip, kRGrip, kLTrig, kRTrig, kLThumb, kRThumb, kMenu, kRFlickDown, kRFlickUp,
               kSrcCount };
    enum Target { tA, tB, tX, tY, tLB, tRB, tLS, tRS, tStart, tBack, tUp, tDown, tLeft, tRight, tLT, tRT, tCount };
    static constexpr int kMaxSources = 4;  // per Xbox control ("A=b,rgrip")
    // What the controllers did this frame, before the mapping.
    struct Raw {
        float src[kSrcCount]{};
        float lx = 0, ly = 0, rx = 0, ry = 0;
    };
    struct Test {
        shared::PadState state{};  // an Xbox pad state, used as is ...
        bool             raw = false;
        Raw              rawIn{};  // ... or (raw=1) controller input that goes through the mapping
        double           dur = 0.0;
    };

    void  ReadRaw(XrSession s, Raw& r) const;
    shared::PadState Map(const Raw& r, bool menuLayout);
    void  ReadTests(double now);

    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction    stick_[2]{};           // left, right thumbstick
    XrAction    src_[kSrcCount]{};     // real controller actions (none for kNone, kMenu and the flicks)
    XrAction    aim_[2]{};             // left, right aim pose
    XrSpace     aimSpace_[2]{};
    bool        testAim_ = false;      // pad_cmd.txt "aim=yaw,pitch"
    float       testAimYaw_ = 0.0f, testAimPitch_ = 0.0f;  // degrees
    Src         map_[tCount][kMaxSources]{};
    Src         mapMenu_[tCount][kMaxSources]{};  // while a game menu is open ([ControlsMenu]: A selects, B backs out)
    bool        menuLayout_ = false;
    bool        blocked_[kSrcCount]{};  // held across a layout switch: ignored until released
    bool        sprintToggle_ = true;  // [Controls] SprintToggle: a click latches LS (sprint) until clicked again or you stop
    bool        sprintLatched_ = false, sprintSrcWas_ = false;
    double      sprintStillSince_ = -1.0;
    bool        flickArmed_[2] = {true, true};  // down, up
    double      flickUntil_[2] = {0.0, 0.0};
    int         leftStick_ = 0, rightStick_ = 1;  // which thumbstick feeds each Xbox stick (-1 = none)
    bool        rightY_ = false;
    double      now_ = 0.0, startUntil_ = 0.0;
    shared::PadState last_{};
    bool        published_ = false;
    std::wstring testPath_;
    std::deque<Test> tests_;
    double      testUntil_ = 0.0;
    Test        test_{};
    bool        testActive_ = false;
    bool        snapArmed_ = true;
};

}  // namespace mohavr::host
