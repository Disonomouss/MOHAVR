#include "pad.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf QConj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf r = QMul(QMul(q, XrQuaternionf{v.x, v.y, v.z, 0.0f}), QConj(q));
    return {r.x, r.y, r.z};
}
// The head's heading only (a turn about LOCAL's up, +Y): R_y(heading) * (0,0,-1) = the head's forward, flattened.
XrQuaternionf Heading(const XrQuaternionf& q) {
    const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float h = std::atan2(-fx, -fz);
    return {0.0f, std::sin(h * 0.5f), 0.0f, std::cos(h * 0.5f)};
}

// XINPUT_GAMEPAD_* button bits.
constexpr std::uint16_t kBits[] = {
    0x1000, 0x2000, 0x4000, 0x8000,  // A B X Y
    0x0100, 0x0200,                  // LB RB
    0x0040, 0x0080,                  // LS RS (thumb clicks)
    0x0010, 0x0020,                  // Start Back
    0x0001, 0x0002, 0x0004, 0x0008,  // DPad up down left right
};
constexpr const wchar_t* kTargetKeys[] = {L"A",     L"B",    L"X",      L"Y",        L"LB",       L"RB",
                                          L"LS",    L"RS",   L"Start",  L"Back",     L"DPadUp",   L"DPadDown",
                                          L"DPadLeft", L"DPadRight", L"LT", L"RT"};
// Defaults (headset round 5, the player's choice) on top of MOHA's own pad layout (MOHAPlayerInput.uc):
// Xbox A = reload/use/flare -> B and the right grip; Xbox B = switch weapon -> Y; Xbox X = crouch (a press
// toggles the stance) -> a flick of the right stick down; Xbox Y = jump -> A; RB = grenade -> X;
// LB = alt fire (a weapon attachment on/off) -> nothing (round 24: the left grip is for the foregrip and holsters);
// LS = sprint (latched, SprintToggle) -> left stick click; RS = melee.
constexpr const wchar_t* kTargetDefaults[] = {L"b,rgrip", L"y",    L"rflickdown", L"a",    L"none",  L"x",
                                              L"lthumb",  L"rthumb", L"menu",     L"none", L"none",  L"none",
                                              L"none",    L"none",   L"ltrigger", L"rtrigger"};
constexpr const wchar_t* kSrcNames[] = {L"none",   L"a",      L"b",        L"x",        L"y",      L"lgrip",
                                        L"rgrip",  L"ltrigger", L"rtrigger", L"lthumb", L"rthumb", L"menu",
                                        L"rflickdown", L"rflickup"};
static_assert(sizeof(kSrcNames) / sizeof(kSrcNames[0]) == 14, "one name per Src");

std::int16_t Axis(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(v < 0.0f ? v * 32768.0f : v * 32767.0f);
}
std::uint8_t Trigger(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

int SrcByName(const std::wstring& n) {
    for (int s = 0; s < static_cast<int>(sizeof(kSrcNames) / sizeof(kSrcNames[0])); ++s)
        if (!_wcsicmp(n.c_str(), kSrcNames[s])) return s;
    return -1;
}

}  // namespace

bool Pad::Init(XrInstance instance, const std::wstring& ini) {
    holdLost_ = GetPrivateProfileIntW(L"Hands", L"HoldLost", 1, ini.c_str()) != 0;
    // [Controls] Target=src[,src...] -- unknown names fall back to the default (and say so).
    auto parse = [&](const wchar_t* list, Src (&out)[kMaxSources]) {
        for (auto& o : out) o = kNone;
        std::wstring s(list);
        int n = 0;
        size_t p = 0;
        while (p <= s.size() && n < kMaxSources) {
            const size_t c = std::min(s.find(L',', p), s.size());
            std::wstring name = s.substr(p, c - p);
            name.erase(0, name.find_first_not_of(L' '));
            name.erase(name.find_last_not_of(L' ') + 1);
            const int src = SrcByName(name);
            if (src < 0) return false;
            if (src != kNone) out[n++] = static_cast<Src>(src);
            p = c + 1;
        }
        return true;
    };
    for (int t = 0; t < tCount; ++t) {
        wchar_t v[64] = L"";
        GetPrivateProfileStringW(L"Controls", kTargetKeys[t], kTargetDefaults[t], v, 64, ini.c_str());
        if (!parse(v, map_[t])) {
            MLOG("pad: [Controls] %ls=%ls has an unknown input -- using %ls", kTargetKeys[t], v, kTargetDefaults[t]);
            parse(kTargetDefaults[t], map_[t]);
        }
    }
    // The menu layout: the gameplay mapping with the face buttons as labelled (the game's menus select with
    // Xbox A and back out with Xbox B).
    static const wchar_t* kMenuKeys[] = {L"A", L"B", L"X", L"Y"};
    static const wchar_t* kMenuDefaults[] = {L"a", L"b", L"x", L"y"};
    for (int t = 0; t < tCount; ++t)
        for (int i = 0; i < kMaxSources; ++i) mapMenu_[t][i] = map_[t][i];
    for (int t = tA; t <= tY; ++t) {
        wchar_t v[64] = L"";
        GetPrivateProfileStringW(L"ControlsMenu", kMenuKeys[t], kMenuDefaults[t], v, 64, ini.c_str());
        if (!parse(v, mapMenu_[t])) {
            MLOG("pad: [ControlsMenu] %ls=%ls has an unknown input -- using %ls", kMenuKeys[t], v, kMenuDefaults[t]);
            parse(kMenuDefaults[t], mapMenu_[t]);
        }
    }
    auto stick = [&](const wchar_t* key, const wchar_t* def) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", key, def, v, 16, ini.c_str());
        return !_wcsicmp(v, L"left") ? 0 : !_wcsicmp(v, L"right") ? 1 : -1;
    };
    leftStick_ = stick(L"LeftStick", L"left");
    rightStick_ = stick(L"RightStick", L"right");
    rightY_ = GetPrivateProfileIntW(L"Controls", L"RightStickY", 0, ini.c_str()) != 0;
    sprintToggle_ = GetPrivateProfileIntW(L"Controls", L"SprintToggle", 1, ini.c_str()) != 0;
    {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", L"MoveDirection", L"head", v, 16, ini.c_str());
        moveByHead_ = _wcsicmp(v, L"body") != 0;
    }

    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "mohavr_play");
    strcpy_s(asci.localizedActionSetName, "MOHAVR gameplay");
    if (XR_FAILED(xrCreateActionSet(instance, &asci, &set_))) return false;
    auto make = [&](XrAction& a, const char* name, const char* loc, XrActionType type) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(aci.actionName, name);
        strcpy_s(aci.localizedActionName, loc);
        aci.actionType = type;
        return XR_SUCCEEDED(xrCreateAction(set_, &aci, &a));
    };
    bool ok = make(stick_[0], "left_stick", "Left stick", XR_ACTION_TYPE_VECTOR2F_INPUT) &&
              make(stick_[1], "right_stick", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    const struct { Src s; const char* name; const char* loc; XrActionType type; } defs[] = {
        {kA, "button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {kB, "button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {kX, "button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {kY, "button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {kLGrip, "left_grip", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {kRGrip, "right_grip", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {kLTrig, "left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {kRTrig, "right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {kLThumb, "left_thumb_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {kRThumb, "right_thumb_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    };
    for (const auto& d : defs) ok = ok && make(src_[d.s], d.name, d.loc, d.type);
    ok = ok && make(aim_[0], "left_aim", "Left hand aim", XR_ACTION_TYPE_POSE_INPUT) &&
         make(aim_[1], "right_aim", "Right hand aim", XR_ACTION_TYPE_POSE_INPUT) &&
         make(haptic_[0], "left_haptic", "Left vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT) &&
         make(haptic_[1], "right_haptic", "Right vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    if (!ok) return false;

    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    testPath_ = std::wstring(tmp, n) + L"MOHAVR\\pad_cmd.txt";

    std::string m;
    for (int t = 0; t < tCount; ++t) {
        m += t ? " " : "";
        char b[16];
        snprintf(b, sizeof(b), "%ls=", kTargetKeys[t]);
        m += b;
        bool first = true;
        for (Src s : map_[t]) {
            if (s == kNone) continue;
            snprintf(b, sizeof(b), "%s%ls", first ? "" : ",", kSrcNames[s]);
            m += b;
            first = false;
        }
        if (first) m += "none";
    }
    MLOG("pad: gameplay actions ready -- %s LeftStick=%d RightStick=%d RightStickY=%d SprintToggle=%d MoveDirection=%s",
         m.c_str(), leftStick_, rightStick_, rightY_, sprintToggle_, moveByHead_ ? "head" : "body");
    return true;
}

void Pad::AppendBindings(const std::string& profile, const std::function<XrPath(const char*)>& path,
                         std::vector<XrActionSuggestedBinding>& out) const {
    auto add = [&](XrAction a, const char* p) { out.push_back({a, path(p)}); };
    if (profile == "/interaction_profiles/oculus/touch_controller" || profile == "/interaction_profiles/valve/index_controller") {
        add(aim_[0], "/user/hand/left/input/aim/pose");
        add(aim_[1], "/user/hand/right/input/aim/pose");
        add(haptic_[0], "/user/hand/left/output/haptic");
        add(haptic_[1], "/user/hand/right/output/haptic");
    }
    if (profile == "/interaction_profiles/oculus/touch_controller") {
        add(stick_[0], "/user/hand/left/input/thumbstick");
        add(stick_[1], "/user/hand/right/input/thumbstick");
        add(src_[kA], "/user/hand/right/input/a/click");
        add(src_[kB], "/user/hand/right/input/b/click");
        add(src_[kX], "/user/hand/left/input/x/click");
        add(src_[kY], "/user/hand/left/input/y/click");
        add(src_[kLGrip], "/user/hand/left/input/squeeze/value");
        add(src_[kRGrip], "/user/hand/right/input/squeeze/value");
        add(src_[kLTrig], "/user/hand/left/input/trigger/value");
        add(src_[kRTrig], "/user/hand/right/input/trigger/value");
        add(src_[kLThumb], "/user/hand/left/input/thumbstick/click");
        add(src_[kRThumb], "/user/hand/right/input/thumbstick/click");
    } else if (profile == "/interaction_profiles/valve/index_controller") {
        add(stick_[0], "/user/hand/left/input/thumbstick");
        add(stick_[1], "/user/hand/right/input/thumbstick");
        add(src_[kA], "/user/hand/right/input/a/click");
        add(src_[kB], "/user/hand/right/input/b/click");
        add(src_[kX], "/user/hand/left/input/a/click");
        add(src_[kY], "/user/hand/left/input/b/click");
        add(src_[kLGrip], "/user/hand/left/input/squeeze/value");
        add(src_[kRGrip], "/user/hand/right/input/squeeze/value");
        add(src_[kLTrig], "/user/hand/left/input/trigger/value");
        add(src_[kRTrig], "/user/hand/right/input/trigger/value");
        add(src_[kLThumb], "/user/hand/left/input/thumbstick/click");
        add(src_[kRThumb], "/user/hand/right/input/thumbstick/click");
    }
}

// The controllers as they are this frame (buttons 0/1, grips and triggers 0..1, sticks -1..1).
void Pad::ReadRaw(XrSession s, Raw& r) const {
    for (int i = 0; i < kSrcCount; ++i) {
        if (!src_[i]) continue;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = src_[i];
        if (i == kLGrip || i == kRGrip || i == kLTrig || i == kRTrig) {
            XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
            r.src[i] = XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &f)) && f.isActive ? f.currentState : 0.0f;
        } else {
            XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
            r.src[i] = XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &b)) && b.isActive && b.currentState ? 1.0f : 0.0f;
        }
    }
    for (int w = 0; w < 2; ++w) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_[w];
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &v)) && v.isActive) {
            (w ? r.rx : r.lx) = v.currentState.x;
            (w ? r.ry : r.ly) = v.currentState.y;
        }
    }
}

// Controller input -> Xbox pad, through [Controls].
shared::PadState Pad::Map(const Raw& in, bool menuLayout) {
    Raw r = in;
    // Grips a hand gesture used (holster, foregrip, reload) don't also press their mapped button.
    if (consumed_[0]) r.src[kLGrip] = 0.0f;
    if (consumed_[1]) r.src[kRGrip] = 0.0f;
    // Nor does the manual reload's release button while it drives the gun (D21).
    for (int h = 0; h < 2; ++h) {
        const Src m = maskedFace_[h];
        if (m == kNone) continue;
        const bool down = r.src[m] > 0.5f;
        if (down && !maskedDown_[h]) MLOG("pad: %ls kept from the game (the manual reload's release button)", kSrcNames[m]);
        maskedDown_[h] = down;
        r.src[m] = 0.0f;
    }
    if (maskedTrig_[0]) r.src[kLTrig] = 0.0f;  // physical hands, before the left-handed swap below
    if (maskedTrig_[1]) r.src[kRTrig] = 0.0f;
    // Player options: right stick moves, left turns (the flicks go with the turning stick); left-handed: the gun
    // hand's trigger fires and its grip uses, as the right ones do by default.
    if (swapSticks_) {
        std::swap(r.lx, r.rx);
        std::swap(r.ly, r.ry);
    }
    if (leftHanded_) {
        std::swap(r.src[kLTrig], r.src[kRTrig]);
        std::swap(r.src[kLGrip], r.src[kRGrip]);
    }
    // Switching layouts (a game menu opened or closed): anything still held is ignored until released, so
    // the A that selected "Resume" doesn't also jump once the menu is gone.
    if (menuLayout != menuLayout_) {
        menuLayout_ = menuLayout;
        for (int i = 0; i < kSrcCount; ++i) blocked_[i] = r.src[i] > 0.5f;
        MLOG("pad: %s layout", menuLayout ? "menu" : "gameplay");
    }
    for (int i = 0; i < kSrcCount; ++i) {
        if (blocked_[i] && r.src[i] <= 0.5f) blocked_[i] = false;
        if (blocked_[i]) r.src[i] = 0.0f;
    }
    const auto& map = menuLayout ? mapMenu_ : map_;
    r.src[kMenu] = now_ < startUntil_ ? 1.0f : 0.0f;  // the menu-button tap (main.cpp)
    // Right-stick flicks: a push past 70% down/up = a 0.15 s press; re-armed once back under 30%.
    const float fy[2] = {-r.ry, r.ry};
    for (int d = 0; d < 2; ++d) {
        if (!flickArmed_[d] && fy[d] < 0.3f) flickArmed_[d] = true;
        if (flickArmed_[d] && fy[d] > 0.7f) {
            flickArmed_[d] = false;
            flickUntil_[d] = now_ + 0.15;
        }
    }
    r.src[kRFlickDown] = now_ < flickUntil_[0] ? 1.0f : 0.0f;
    r.src[kRFlickUp] = now_ < flickUntil_[1] ? 1.0f : 0.0f;

    auto value = [&](int t) {
        float v = 0.0f;
        for (Src s : map[t])
            if (s != kNone) v = std::max(v, r.src[s]);
        return v;
    };
    shared::PadState p{};
    for (int t = 0; t < tCount - 2; ++t)
        if (value(t) > 0.5f) p.buttons |= kBits[t];
    p.leftTrigger = Trigger(value(tLT));
    p.rightTrigger = Trigger(value(tRT));
    const float sx[2] = {r.lx, r.rx}, sy[2] = {r.ly, r.ry};
    if (leftStick_ >= 0) {
        float x = sx[leftStick_], y = sy[leftStick_];
        // MoveDirection=head: the game moves along the body's heading, the view adds the head's yaw -- so the stick,
        // meant in the head's frame, turns by that yaw into the body's (x right, y forward; left turns are positive).
        if (moveByHead_ && headYawOk_ && !menuLayout) {
            const float c = std::cos(headYaw_), s = std::sin(headYaw_);
            const float bx = x * c - y * s, by = x * s + y * c;
            x = bx;
            y = by;
        }
        p.thumbLX = Axis(x);
        p.thumbLY = Axis(y);
    }
    if (rightStick_ >= 0) {
        p.thumbRX = Axis(sx[rightStick_]);
        if (rightY_) p.thumbRY = Axis(sy[rightStick_]);
    }

    // Sprint toggle: a click latches LS (MOHA's sprint is hold-to-sprint); another click, or letting the
    // move stick rest for 0.3 s, releases it.
    if (sprintToggle_) {
        const bool src = (p.buttons & kBits[tLS]) != 0;
        if (src && !sprintSrcWas_) {
            sprintLatched_ = !sprintLatched_;
            MLOG("pad: sprint %s", sprintLatched_ ? "on" : "off");
        }
        sprintSrcWas_ = src;
        const float mx = static_cast<float>(p.thumbLX) / 32767.0f, my = static_cast<float>(p.thumbLY) / 32767.0f;
        if (sprintLatched_ && mx * mx + my * my < 0.04f) {
            if (sprintStillSince_ < 0.0) sprintStillSince_ = now_;
            if (now_ - sprintStillSince_ > 0.3) {
                sprintLatched_ = false;
                MLOG("pad: sprint off (stopped moving)");
            }
        } else {
            sprintStillSince_ = -1.0;
        }
        p.buttons = static_cast<std::uint16_t>((p.buttons & ~kBits[tLS]) | (sprintLatched_ ? kBits[tLS] : 0));
    }
    return p;
}

// Lines: "lx=0 ly=1 rx=0 ry=0 lt=0 rt=1 buttons=a,start dur=2" = an Xbox pad state as is (unset = 0 / none);
// with raw=1 the values are controller input instead ("raw=1 ry=-1 press=b,rgrip dur=0.2") and go through
// the mapping, so [Controls], the flicks and the sprint toggle can be tested.
void Pad::ReadTests(double now) {
    if (GetFileAttributesW(testPath_.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    // Take the file first (a rename), then read and delete the taken copy: a command written while the host reads can't
    // be deleted unread, and a file caught mid-replace is left for the next frame (GOAL A2: a lost "@boltback" line --
    // the open failed during the writer's replace, and the delete then took the new command).
    const std::wstring taken = testPath_ + L".taken";
    if (!MoveFileExW(testPath_.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, taken.c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            Test t;
            t.dur = 0.5;
            float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
            std::string buttons, press;
            bool aimLine = false;
            char* ctx = nullptr;
            for (char* tok = strtok_s(line, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx)) {
                char* eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = 0;
                const char* v = eq + 1;
                if (!strcmp(tok, "aim")) {
                    // "aim=yaw,pitch" (degrees) or "aim=off": a test right-hand aim pose, until changed.
                    aimLine = true;
                    float y = 0.0f, p = 0.0f;
                    if (strcmp(v, "off") != 0 && sscanf_s(v, "%f,%f", &y, &p) >= 1) {
                        testPose_[1] = {true, 0.2f, -0.3f, 0.3f, y, p, 0.0f};
                        MLOG("pad: test aim yaw %.1f pitch %.1f deg (right hand)", y, p);
                    } else {
                        testPose_[0].on = testPose_[1].on = false;
                        MLOG("pad: test poses off");
                    }
                } else if (!strcmp(tok, "lost")) {
                    // "lost=l|r|both|none": those hands report no tracking (the runtime dropping idle controllers).
                    aimLine = true;
                    testLost_[0] = !strcmp(v, "l") || !strcmp(v, "both");
                    testLost_[1] = !strcmp(v, "r") || !strcmp(v, "both");
                    MLOG("pad: test tracking loss: left %s, right %s", testLost_[0] ? "lost" : "tracked",
                         testLost_[1] ? "lost" : "tracked");
                } else if (!strcmp(tok, "reload")) {
                    aimLine = true;
                    // (GOAL A2: 7-10 the bolt's steps; 6, the taped pair's other half, has no name here. GOAL A3: a
                    // pump's strokes are the bolt's back / forward.)
                    static const char* kNames[] = {"", "eject", "insert", "rack", "take", "drop", "", "boltup", "boltback",
                                                   "boltfwd", "boltdown"};
                    std::uint32_t pumpE = !strcmp(v, "pumpback") ? 8u : !strcmp(v, "pumpfwd") ? 9u : 0u;
                    if (pumpE) {
                        testReload_.push_back(pumpE);
                        MLOG("pad: test reload event %s", v);
                    }
                    for (std::uint32_t e = 1; e < 11; ++e)
                        if (kNames[e][0] && !strcmp(v, kNames[e])) {
                            testReload_.push_back(e);
                            MLOG("pad: test reload event %s", v);
                        }
                } else if (!strcmp(tok, "throwvel")) {
                    aimLine = true;
                    if (sscanf_s(v, "%f,%f,%f", &testThrowVel_[0], &testThrowVel_[1], &testThrowVel_[2]) == 3) {
                        testThrow_ = true;
                        MLOG("pad: test throw velocity %.1f %.1f %.1f m/s (the next grenade release)", testThrowVel_[0],
                             testThrowVel_[1], testThrowVel_[2]);
                    }
                } else if (!strcmp(tok, "hand")) {
                    // "hand=l|r,x,y,z,yaw,pitch": that hand at x right, y up, z ahead of the head (m), turned yaw/pitch.
                    aimLine = true;
                    char which = 0;
                    TestPose tp{true, 0, 0, 0, 0, 0, 0};
                    if (v[0] && v[1] == ',' && v[2] == '@') {
                        // "hand=l,@mag|@pouch|@bolt[,dx,dy,dz[,yaw,pitch,roll]]": at a manual-reload spot, offset.
                        static const char* kTargets[] = {"mag", "pouch", "bolt", "magin", "boltup", "boltback", "fore", "grenade",
                                                         "pistol", "chest"};
                        const char* name = v + 3;
                        const size_t len = strcspn(name, ",");
                        for (int i = 0; i < 10; ++i)
                            if (strlen(kTargets[i]) == len && !strncmp(name, kTargets[i], len)) tp.target = i;
                        if (name[len] == ',')
                            sscanf_s(name + len + 1, "%f,%f,%f,%f,%f,%f", &tp.x, &tp.y, &tp.z, &tp.yaw, &tp.pitch, &tp.roll);
                        // "...,align" (only @magin): the hand turned so the held magazine sits as seated (any grip).
                        tp.align = tp.target == 3 && strstr(name, ",align") != nullptr;
                        const int h = (v[0] == 'l' || v[0] == 'L') ? 0 : 1;
                        testPose_[h] = tp;
                        MLOG("pad: test %s hand at @%.*s %+.2f %+.2f %+.2f m, yaw %.1f pitch %.1f%s", h ? "right" : "left",
                             static_cast<int>(len), name, tp.x, tp.y, tp.z, tp.yaw, tp.pitch, tp.target < 0 ? " (unknown spot)" : "");
                    } else if (sscanf_s(v, "%c,%f,%f,%f,%f,%f,%f", &which, 1, &tp.x, &tp.y, &tp.z, &tp.yaw, &tp.pitch, &tp.roll) >= 4) {
                        const int h = (which == 'l' || which == 'L') ? 0 : 1;
                        testPose_[h] = tp;
                        MLOG("pad: test %s hand at %.2f %.2f %.2f m, yaw %.1f pitch %.1f", h ? "right" : "left", tp.x, tp.y,
                             tp.z, tp.yaw, tp.pitch);
                    }
                }
                else if (!strcmp(tok, "lx")) lx = static_cast<float>(atof(v));
                else if (!strcmp(tok, "ly")) ly = static_cast<float>(atof(v));
                else if (!strcmp(tok, "rx")) rx = static_cast<float>(atof(v));
                else if (!strcmp(tok, "ry")) ry = static_cast<float>(atof(v));
                else if (!strcmp(tok, "lt")) lt = static_cast<float>(atof(v));
                else if (!strcmp(tok, "rt")) rt = static_cast<float>(atof(v));
                else if (!strcmp(tok, "dur")) t.dur = atof(v);
                else if (!strcmp(tok, "raw")) t.raw = atoi(v) != 0;
                else if (!strcmp(tok, "buttons")) buttons = v;
                else if (!strcmp(tok, "press")) press = v;
            }
            if (aimLine) continue;  // an aim line sets the test pose only; it is not a pad state
            auto each = [](const std::string& list, auto fn) {
                size_t p = 0;
                while (p < list.size()) {
                    const size_t c = std::min(list.find(',', p), list.size());
                    fn(list.substr(p, c - p));
                    p = c + 1;
                }
            };
            if (t.raw) {
                t.rawIn.lx = lx; t.rawIn.ly = ly; t.rawIn.rx = rx; t.rawIn.ry = ry;
                t.rawIn.src[kLTrig] = lt;
                t.rawIn.src[kRTrig] = rt;
                each(press, [&](const std::string& n) {
                    const int s = SrcByName(std::wstring(n.begin(), n.end()));
                    if (s > 0) t.rawIn.src[s] = 1.0f;
                });
            } else {
                t.state.thumbLX = Axis(lx); t.state.thumbLY = Axis(ly);
                t.state.thumbRX = Axis(rx); t.state.thumbRY = Axis(ry);
                t.state.leftTrigger = Trigger(lt);
                t.state.rightTrigger = Trigger(rt);
                static const char* names[] = {"a", "b", "x", "y", "lb", "rb", "ls", "rs", "start", "back",
                                              "up", "down", "left", "right"};
                each(buttons, [&](const std::string& n) {
                    for (int i = 0; i < 14; ++i)
                        if (n == names[i]) t.state.buttons |= kBits[i];
                });
            }
            tests_.push_back(t);
        }
        fclose(f);
    }
    DeleteFileW(taken.c_str());
    MLOG("pad: %zu test state(s) queued", tests_.size());
    (void)now;
}

void Pad::BeginFrame(double now) {
    now_ = now;
    ReadTests(now);
    if (testActive_ && now >= testUntil_) testActive_ = false;
    if (!testActive_ && !tests_.empty()) {
        test_ = tests_.front();
        testUntil_ = now + test_.dur;
        tests_.pop_front();
        testActive_ = true;
        if (test_.raw)
            MLOG("pad: raw test input L(%.2f,%.2f) R(%.2f,%.2f) for %.2f s", test_.rawIn.lx, test_.rawIn.ly, test_.rawIn.rx,
                 test_.rawIn.ry, test_.dur);
        else
            MLOG("pad: test state buttons 0x%04X L(%d,%d) R(%d,%d) LT %u RT %u for %.2f s", test_.state.buttons,
                 test_.state.thumbLX, test_.state.thumbLY, test_.state.thumbRX, test_.state.thumbRY,
                 test_.state.leftTrigger, test_.state.rightTrigger, test_.dur);
    }
}

void Pad::Update(XrSession session, double now, bool neutral, int snapDeg, shared::Header* hdr) {
    if (now != now_) BeginFrame(now);  // a caller that didn't call BeginFrame this frame
    shared::PadState p{};
    if (testActive_ && !test_.raw) {
        p = test_.state;
    } else if (testActive_ || !neutral) {
        Raw r{};
        if (testActive_) r = test_.rawIn;
        else ReadRaw(session, r);
        // The player (2026-10-01: "when I pressed A on the option the menu closed and my character jumped"): what is
        // held as the menu closes stays from the game until it is let go.
        for (int i = 0; i < kSrcCount; ++i) {
            if (wasNeutral_ && r.src[i] >= 0.5f) {
                heldOverMenu_[i] = true;
                MLOG("pad: input %d held as the menu closed -- kept from the game until let go", i);
            }
            if (heldOverMenu_[i] && r.src[i] < 0.3f) heldOverMenu_[i] = false;
            if (heldOverMenu_[i]) r.src[i] = 0.0f;
        }
        p = Map(r, hdr->gameUiMenu != 0);
    }
    wasNeutral_ = neutral && !testActive_;
    // After "Give all weapons": B (switch weapon) in gameplay is the engine's NextWeapon, sent by main.cpp.
    if (allWeapons_ && !hdr->gameUiMenu) {
        const bool down = (p.buttons & 0x2000) != 0;
        if (down && !allBDown_) nextWeaponReq_ = true;
        allBDown_ = down;
        p.buttons &= ~0x2000;
    } else {
        allBDown_ = false;
    }
    // The off-hand pistol (PistolKeep): B would take the held pistol to the gun hand -- the press is taken here instead.
    if (redirectB_ && !redirectWas_) redirectBDown_ = (p.buttons & 0x2000) != 0;  // down already: the game's press
    redirectWas_ = redirectB_;
    if (redirectB_ && !hdr->gameUiMenu) {
        const bool down = (p.buttons & 0x2000) != 0;
        if (down && !redirectBDown_) {
            redirectReq_ = true;
            MLOG("pad: switch weapon kept from the pistol in the off hand -- the primary instead");
        }
        redirectBDown_ = down;
        p.buttons &= ~0x2000;
    } else if (redirectBDown_ && (p.buttons & 0x2000)) {
        p.buttons &= ~0x2000;  // still down from then: kept until let go
    } else {
        redirectBDown_ = false;
    }
    // Buttons held back in gameplay (the off-hand grenade: RB, the game's own grenade switch, while one is held) -- each
    // until it is let go, or it would reach the game as a fresh press when the mask ends.
    const std::uint16_t mask = hdr->gameUiMenu ? 0 : static_cast<std::uint16_t>(maskedButtons_ | maskedHeld_);
    if (mask) {
        const std::uint16_t down = p.buttons & mask;
        if (down & ~maskedHeld_) MLOG("pad: buttons 0x%04X kept from the game (a grenade in the off hand)", down & ~maskedHeld_);
        maskedHeld_ = down;
        p.buttons &= static_cast<std::uint16_t>(~mask);
    } else {
        maskedHeld_ = 0;
    }

    // Snap turn: a flick past 70% = one step; the stick must come back under 30% before the next.
    if (snapDeg > 0) {
        const float x = static_cast<float>(p.thumbRX) / 32767.0f;
        if (!snapArmed_ && std::fabs(x) < 0.3f) snapArmed_ = true;
        if (snapArmed_ && std::fabs(x) > 0.7f) {
            snapArmed_ = false;
            // UE yaw grows clockwise seen from above, so a flick right (+x) is a positive step.
            const LONG step = static_cast<LONG>(std::lround(snapDeg * 65536.0 / 360.0)) * (x > 0.0f ? 1 : -1);
            const LONG total = InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&hdr->snapYawTotal), step) + step;
            MLOG("pad: snap %s %d deg (total %ld)", x > 0.0f ? "right" : "left", snapDeg, total);
        }
        p.thumbRX = 0;
    }

    if (published_ && !memcmp(&p, &last_, sizeof(p))) return;  // XInput's packet number changes only with the state
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->padSeq));  // odd: writing
    hdr->padButtons = p.buttons;
    hdr->padLeftTrigger = p.leftTrigger;
    hdr->padRightTrigger = p.rightTrigger;
    hdr->padThumbLX = p.thumbLX;
    hdr->padThumbLY = p.thumbLY;
    hdr->padThumbRX = p.thumbRX;
    hdr->padThumbRY = p.thumbRY;
    hdr->padActive = 1;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->padSeq));  // even: done
    if (!published_) MLOG("pad: driving the game's pad 0");
    last_ = p;
    published_ = true;
}

bool Pad::CreateSpaces(XrSession session) {
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_[h];
        ci.poseInActionSpace.orientation.w = 1.0f;
        if (!aim_[h] || XR_FAILED(xrCreateActionSpace(session, &ci, &aimSpace_[h]))) {
            MLOG("pad: aim pose space %d failed -- no hand aiming", h);
            return false;
        }
    }
    MLOG("pad: aim pose spaces ready");
    return true;
}

std::uint32_t Pad::LocateHands(XrSpace space, XrTime t, const XrPosef& head, XrPosef (&out)[2]) const {
    std::uint32_t valid = 0;
    for (int h = 0; h < 2; ++h) {
        out[h] = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        if (!aimSpace_[h]) continue;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(aimSpace_[h], space, t, &loc)) && (loc.locationFlags & need) == need) {
            out[h] = loc.pose;
            valid |= 1u << h;
        }
    }
    for (int h = 0; h < 2; ++h) {
        const TestPose& tp = testPose_[h];
        if (!tp.on) continue;
        // The head's heading (yaw only), then the test pose in that frame.
        const auto& q = head.orientation;
        const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float heading = std::atan2(-fx, -fz);  // R_y(heading) * (0,0,-1) = (fx, 0, fz)
        const float sh = std::sin(heading), ch = std::cos(heading);
        // right = (cos, 0, -sin), forward = (-sin, 0, -cos)
        XrVector3f base = head.position;
        if (tp.target >= 0) {
            if (!testTargetOk_[tp.target]) continue;  // not known yet: the hand as tracked
            base = testTarget_[tp.target];
        }
        if (tp.align && testAlignOk_) {  // @magin,align: the pose that seats the held magazine, offset in the heading frame
            const XrVector3f& a = testAlign_.position;
            out[h].position = {a.x + tp.x * ch - tp.z * sh, a.y + tp.y, a.z - tp.x * sh - tp.z * ch};
            out[h].orientation = testAlign_.orientation;
            valid |= 1u << h;
            continue;
        }
        out[h].position = {base.x + tp.x * ch - tp.z * sh, base.y + tp.y, base.z - tp.x * sh - tp.z * ch};
        const float yaw = heading - tp.yaw * 0.0174533f;  // positive test yaw = to the right
        const float pitch = tp.pitch * 0.0174533f;        // positive = up
        const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cx = std::cos(pitch * 0.5f),
                    sx = std::sin(pitch * 0.5f);
        const XrQuaternionf yp{cy * sx, cx * sy, -sy * sx, cy * cx};  // R_y(yaw) * R_x(pitch)
        const float rz = -tp.roll * 0.0174533f * 0.5f, sz = std::sin(rz), cz = std::cos(rz);  // + roll = clockwise
        out[h].orientation = {yp.x * cz + yp.y * sz, yp.y * cz - yp.x * sz, yp.z * cz + yp.w * sz, yp.w * cz - yp.z * sz};
        valid |= 1u << h;
    }
    for (int h = 0; h < 2; ++h)
        if (testLost_[h]) valid &= ~(1u << h);
    return valid;
}

std::uint32_t Pad::HoldLost(const XrPosef& head, XrPosef (&pose)[2], std::uint32_t valid) {
    std::uint32_t held = 0;
    const XrQuaternionf qy = Heading(head.orientation), qyi = QConj(qy);
    static const char* kSide[] = {"left", "right"};
    for (int h = 0; h < 2; ++h) {
        if (valid & (1u << h)) {
            const XrVector3f d{pose[h].position.x - head.position.x, pose[h].position.y - head.position.y,
                               pose[h].position.z - head.position.z};
            heldRel_[h] = {QMul(qyi, pose[h].orientation), QRot(qyi, d)};
            haveRel_[h] = true;
            if (heldNow_[h]) {
                heldNow_[h] = false;
                MLOG("pad: the %s hand is tracked again", kSide[h]);
            }
        } else if (holdLost_ && haveRel_[h]) {
            const XrVector3f d = QRot(qy, heldRel_[h].position);
            pose[h] = {QMul(qy, heldRel_[h].orientation), {head.position.x + d.x, head.position.y + d.y, head.position.z + d.z}};
            held |= 1u << h;
            if (!heldNow_[h]) {
                heldNow_[h] = true;
                MLOG("pad: the %s hand lost tracking -- held where it was until it's back (Hands.HoldLost)", kSide[h]);
            }
        }
    }
    return held;
}

float Pad::GripValue(XrSession s, int hand) const {
    const Src src = hand ? kRGrip : kLGrip;
    if (testActive_ && test_.raw) return test_.rawIn.src[src];
    if (!src_[src]) return 0.0f;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = src_[src];
    XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &f)) && f.isActive ? f.currentState : 0.0f;
}

float Pad::TriggerValue(XrSession s, int hand) const {
    const Src src = hand ? kRTrig : kLTrig;
    if (testActive_ && test_.raw) return test_.rawIn.src[src];
    if (!src_[src]) return 0.0f;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = src_[src];
    XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &f)) && f.isActive ? f.currentState : 0.0f;
}

bool Pad::GripActive(XrSession s, int hand) const {
    const Src src = hand ? kRGrip : kLGrip;
    if (testActive_ && test_.raw) return true;
    if (!src_[src]) return false;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = src_[src];
    XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &f)) && f.isActive;
}

float Pad::FaceButton(XrSession s, int hand, bool upper) const {
    const Src src = FaceSrc(hand, upper);
    if (testActive_ && test_.raw) return test_.rawIn.src[src];
    if (!src_[src]) return 0.0f;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = src_[src];
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &b)) && b.isActive && b.currentState ? 1.0f : 0.0f;
}

void Pad::Pulse(XrSession s, int hand, float amplitude, float ms) const {
    if (!haptic_[hand]) return;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_[hand];
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = static_cast<XrDuration>(ms * 1e6f);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = amplitude;
    xrApplyHapticFeedback(s, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
}

void Pad::Pulse(XrSession s, int hand) const {
    if (!haptic_[hand]) return;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_[hand];
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = 30000000;  // 30 ms
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = 0.5f;
    xrApplyHapticFeedback(s, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
}

}  // namespace mohavr::host
