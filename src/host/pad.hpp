// The virtual Xbox pad (M6, Input.Controllers): the headset's controllers -> an XINPUT_GAMEPAD in the
// shared block, which the game-side XInputGetState hook hands to MOHA as pad 0.
//
// The mapping is [Controls] in MOHAVR.ini (next to the host). Defaults follow MOHA's own pad layout
// (MOHAPlayerInput.uc Bindings_Default) on Touch 1:1: sticks -> sticks, triggers -> LT (aim) / RT
// (fire), grips -> LB / RB, A B X Y -> A B X Y, stick clicks -> LS (sprint) / RS (melee), a TAP of
// the left menu button -> Start (holding it opens the MOHAVR menu instead; main.cpp). Right stick Y
// is off by default: the head already drives pitch.
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

private:
    enum Src { kNone, kA, kB, kX, kY, kLGrip, kRGrip, kLTrig, kRTrig, kLThumb, kRThumb, kMenu, kSrcCount };
    enum Target { tA, tB, tX, tY, tLB, tRB, tLS, tRS, tStart, tBack, tUp, tDown, tLeft, tRight, tLT, tRT, tCount };
    struct Test {
        shared::PadState state{};
        double           dur = 0.0;
    };

    float Value(XrSession s, Src src) const;
    void  ReadTests(double now);

    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction    stick_[2]{};           // left, right thumbstick
    XrAction    src_[kSrcCount]{};     // boolean/float sources (kMenu has none: main.cpp owns the menu button)
    Src         map_[tCount]{};
    int         leftStick_ = 0, rightStick_ = 1;  // which thumbstick feeds each Xbox stick (-1 = none)
    bool        rightY_ = false;
    double      now_ = 0.0, startUntil_ = 0.0;
    shared::PadState last_{};
    bool        published_ = false;
    std::wstring testPath_;
    std::deque<Test> tests_;
    double      testUntil_ = 0.0;
    shared::PadState testState_{};
    bool        testActive_ = false;
    bool        snapArmed_ = true;
};

}  // namespace mohavr::host
