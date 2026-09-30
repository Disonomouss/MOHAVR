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
    // Start of the XR frame, before anything reads the controllers: the test file and the test state due now (so a
    // raw test's grips reach hands.cpp in the same frame the mapping sees them).
    void BeginFrame(double now);

    // M7: the controllers' aim poses (/input/aim/pose). CreateSpaces once the action sets are attached.
    bool CreateSpaces(XrSession session);
    // Both aim poses in `space` at `t`; the result's bit 0/1 = left/right valid. Test poses from pad_cmd.txt
    // replace a hand: "aim=yaw,pitch" (degrees) = the right hand 20 cm right, 30 cm below and 30 cm ahead of `head`,
    // turned by yaw/pitch from the head's heading; "hand=l|r,x,y,z,yaw,pitch[,roll]" = that hand at x right, y up, z ahead
    // (metres, heading frame); "hand=l|r,@mag|@pouch|@bolt[,dx,dy,dz[,yaw,pitch,roll]]" = that hand at a manual-reload
    // spot (SetTestTargets), offset in the heading frame; "aim=off" = both real again.
    std::uint32_t LocateHands(XrSpace space, XrTime t, const XrPosef& head, XrPosef (&out)[2]) const;
    // Hands.HoldLost: a hand that lost tracking keeps its last pose relative to the head's position and heading (not its
    // pitch: a held gun doesn't swing when you look up or down) until it's tracked again. Returns the held bits (OR them
    // into the valid bits). "lost=l|r|both|none" in pad_cmd.txt makes a hand lose tracking, for tests.
    std::uint32_t HoldLost(const XrPosef& head, XrPosef (&pose)[2], std::uint32_t valid);

    // M8 (hands.cpp): a hand's squeeze 0..1 (a raw test state's while one plays); grips a gesture used are kept
    // from the mapping until released; a short haptic pulse.
    float GripValue(XrSession s, int hand) const;
    float TriggerValue(XrSession s, int hand) const;  // likewise, the physical hand's trigger
    // A test throw (pad_cmd.txt "throwvel=x,y,z", m/s LOCAL): used for the next grenade release instead of the hand's.
    bool  TakeTestThrow(float (&v)[3]) {
        if (!testThrow_) return false;
        testThrow_ = false;
        for (int i = 0; i < 3; ++i) v[i] = testThrowVel_[i];
        return true;
    }
    // Manual reload test events (pad_cmd.txt "reload=eject|take|insert|rack|drop"), in order; taken once.
    std::vector<std::uint32_t> TakeTestReload() {
        std::vector<std::uint32_t> out;
        out.swap(testReload_);
        return out;
    }
    void  SetConsumed(bool left, bool right) { consumed_[0] = left; consumed_[1] = right; }
    void  Pulse(XrSession s, int hand) const;
    void  Pulse(XrSession s, int hand, float amplitude, float ms) const;
    // The manual reload's release button (D21): a physical hand's face button, upper = B (right) / Y (left), else A / X
    // (a raw test state's while one plays); kept from the mapping while masked. Set before Update.
    float FaceButton(XrSession s, int hand, bool upper) const;
    void  SetMaskedFace(int hand, bool upper, bool on) { maskedFace_[hand] = on ? FaceSrc(hand, upper) : kNone; }
    // Likewise a physical hand's trigger (the manual reload's flip of a held twin magazine).
    void  SetMaskedTrigger(int hand, bool on) { maskedTrig_[hand] = on; }
    // Tests: where "hand=l,@mag|@pouch|@bolt" puts a hand (LOCAL), from the last frame's hands.
    void  SetTestTargets(const XrVector3f (&p)[4], const bool (&ok)[4]) {
        for (int i = 0; i < 4; ++i) {
            testTarget_[i] = p[i];
            testTargetOk_[i] = ok[i];
        }
    }
    // Player options (the menu): right stick moves / left turns; left-handed (the triggers and grips swap sides).
    void  SetSwapSticks(bool on) { swapSticks_ = on; }
    void  SetLeftHanded(bool on) { leftHanded_ = on; }
    // [Controls] MoveDirection (round 29): head = the move stick's forward is where the head faces (its yaw from the body,
    // radians, left positive; set per XR frame), body = the game's own (the body's heading).
    void  SetMoveByHead(bool on) { moveByHead_ = on; }
    bool  MoveByHead() const { return moveByHead_; }
    void  SetHeadYaw(float yaw) { headYaw_ = yaw; headYawOk_ = true; }

private:
    // Controller inputs. kMenu is the tap of the menu button (main.cpp owns it); kRFlickDown/Up are short
    // pulses when the right stick is flicked down/up (derived from its Y, which isn't a stick axis by default).
    enum Src { kNone, kA, kB, kX, kY, kLGrip, kRGrip, kLTrig, kRTrig, kLThumb, kRThumb, kMenu, kRFlickDown, kRFlickUp,
               kSrcCount };
    enum Target { tA, tB, tX, tY, tLB, tRB, tLS, tRS, tStart, tBack, tUp, tDown, tLeft, tRight, tLT, tRT, tCount };
    static Src FaceSrc(int hand, bool upper) { return hand ? (upper ? kB : kA) : (upper ? kY : kX); }
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
    XrAction    haptic_[2]{};          // left, right vibration
    struct TestPose { bool on; float x, y, z, yaw, pitch, roll; int target = -1; };  // pad_cmd.txt "aim=" / "hand=" (heading frame)
    TestPose    testPose_[2]{};
    XrVector3f  testTarget_[4]{};      // "@mag", "@pouch", "@bolt", "@magin" (LOCAL)
    bool        testTargetOk_[4]{};
    Src         maskedFace_[2] = {kNone, kNone};  // the manual reload's release button, per physical hand
    bool        maskedDown_[2]{};
    bool        maskedTrig_[2]{};
    bool        testLost_[2]{};        // pad_cmd.txt "lost=": that hand reports no tracking
    bool        holdLost_ = true;      // [Hands] HoldLost
    XrPosef     heldRel_[2]{};         // the last tracked pose relative to the head's position and heading
    bool        haveRel_[2]{}, heldNow_[2]{};
    bool        consumed_[2]{};        // grips used by a gesture (hands.cpp)
    bool        testThrow_ = false;
    std::vector<std::uint32_t> testReload_;
    float       testThrowVel_[3]{};
    bool        swapSticks_ = false, leftHanded_ = false;
    bool        moveByHead_ = true, headYawOk_ = false;
    float       headYaw_ = 0.0f;
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
