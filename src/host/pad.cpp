#include "pad.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

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
constexpr const wchar_t* kTargetDefaults[] = {L"a",    L"b",    L"x",    L"y",    L"lgrip", L"rgrip", L"lthumb", L"rthumb",
                                              L"menu", L"none", L"none", L"none", L"none",  L"none",  L"ltrigger", L"rtrigger"};
constexpr const wchar_t* kSrcNames[] = {L"none",     L"a",      L"b",      L"x",      L"y",    L"lgrip",
                                        L"rgrip",    L"ltrigger", L"rtrigger", L"lthumb", L"rthumb", L"menu"};

std::int16_t Axis(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(v < 0.0f ? v * 32768.0f : v * 32767.0f);
}
std::uint8_t Trigger(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

}  // namespace

bool Pad::Init(XrInstance instance, const std::wstring& ini) {
    // [Controls] -- unknown names fall back to the default (and say so).
    for (int t = 0; t < tCount; ++t) {
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"Controls", kTargetKeys[t], kTargetDefaults[t], v, 32, ini.c_str());
        int found = -1;
        for (int s = 0; s < kSrcCount; ++s)
            if (!_wcsicmp(v, kSrcNames[s])) found = s;
        if (found < 0) {
            MLOG("pad: [Controls] %ls=%ls is not a control -- using %ls", kTargetKeys[t], v, kTargetDefaults[t]);
            for (int s = 0; s < kSrcCount; ++s)
                if (!_wcsicmp(kTargetDefaults[t], kSrcNames[s])) found = s;
        }
        map_[t] = static_cast<Src>(found);
    }
    auto stick = [&](const wchar_t* key, const wchar_t* def) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", key, def, v, 16, ini.c_str());
        return !_wcsicmp(v, L"left") ? 0 : !_wcsicmp(v, L"right") ? 1 : -1;
    };
    leftStick_ = stick(L"LeftStick", L"left");
    rightStick_ = stick(L"RightStick", L"right");
    rightY_ = GetPrivateProfileIntW(L"Controls", L"RightStickY", 0, ini.c_str()) != 0;

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
    if (!ok) return false;

    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    testPath_ = std::wstring(tmp, n) + L"MOHAVR\\pad_cmd.txt";

    std::string m;
    for (int t = 0; t < tCount; ++t) {
        char b[48];
        snprintf(b, sizeof(b), "%s%ls=%ls", t ? " " : "", kTargetKeys[t], kSrcNames[map_[t]]);
        m += b;
    }
    MLOG("pad: gameplay actions ready -- %s LeftStick=%d RightStick=%d RightStickY=%d", m.c_str(), leftStick_,
         rightStick_, rightY_);
    return true;
}

void Pad::AppendBindings(const std::string& profile, const std::function<XrPath(const char*)>& path,
                         std::vector<XrActionSuggestedBinding>& out) const {
    auto add = [&](XrAction a, const char* p) { out.push_back({a, path(p)}); };
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

float Pad::Value(XrSession s, Src src) const {
    if (src == kNone) return 0.0f;
    if (src == kMenu) return now_ < startUntil_ ? 1.0f : 0.0f;  // the menu-button tap (main.cpp)
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = src_[src];
    if (src == kLGrip || src == kRGrip || src == kLTrig || src == kRTrig) {
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &f)) && f.isActive ? f.currentState : 0.0f;
    }
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &b)) && b.isActive && b.currentState ? 1.0f : 0.0f;
}

// Lines: "lx=0 ly=1 rx=0 ry=0 lt=0 rt=1 buttons=a,start dur=2" (unset = 0 / none).
void Pad::ReadTests(double now) {
    if (GetFileAttributesW(testPath_.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, testPath_.c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            Test t;
            t.dur = 0.5;
            char* ctx = nullptr;
            for (char* tok = strtok_s(line, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx)) {
                char* eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = 0;
                const char* v = eq + 1;
                if (!strcmp(tok, "lx")) t.state.thumbLX = Axis(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "ly")) t.state.thumbLY = Axis(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "rx")) t.state.thumbRX = Axis(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "ry")) t.state.thumbRY = Axis(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "lt")) t.state.leftTrigger = Trigger(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "rt")) t.state.rightTrigger = Trigger(static_cast<float>(atof(v)));
                else if (!strcmp(tok, "dur")) t.dur = atof(v);
                else if (!strcmp(tok, "buttons")) {
                    static const char* names[] = {"a", "b", "x", "y", "lb", "rb", "ls", "rs", "start", "back",
                                                  "up", "down", "left", "right"};
                    std::string list(v);
                    size_t p = 0;
                    while (p <= list.size()) {
                        const size_t c = std::min(list.find(',', p), list.size());
                        const std::string name = list.substr(p, c - p);
                        for (int i = 0; i < 14; ++i)
                            if (name == names[i]) t.state.buttons |= kBits[i];
                        p = c + 1;
                    }
                }
            }
            tests_.push_back(t);
        }
        fclose(f);
    }
    DeleteFileW(testPath_.c_str());
    MLOG("pad: %zu test state(s) queued", tests_.size());
    (void)now;
}

void Pad::Update(XrSession session, double now, bool neutral, shared::Header* hdr) {
    now_ = now;
    ReadTests(now);
    if (testActive_ && now >= testUntil_) testActive_ = false;
    if (!testActive_ && !tests_.empty()) {
        testState_ = tests_.front().state;
        testUntil_ = now + tests_.front().dur;
        tests_.pop_front();
        testActive_ = true;
        MLOG("pad: test state buttons 0x%04X L(%d,%d) R(%d,%d) LT %u RT %u for %.2f s", testState_.buttons,
             testState_.thumbLX, testState_.thumbLY, testState_.thumbRX, testState_.thumbRY, testState_.leftTrigger,
             testState_.rightTrigger, testUntil_ - now);
    }

    shared::PadState p{};
    if (testActive_) {
        p = testState_;
    } else if (!neutral) {
        for (int t = 0; t < tCount - 2; ++t)
            if (Value(session, map_[t]) > 0.5f) p.buttons |= kBits[t];
        p.leftTrigger = Trigger(Value(session, map_[tLT]));
        p.rightTrigger = Trigger(Value(session, map_[tRT]));
        auto read = [&](int which, std::int16_t& x, std::int16_t& y, bool useY) {
            if (which < 0) return;
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = stick_[which];
            XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive) {
                x = Axis(v.currentState.x);
                if (useY) y = Axis(v.currentState.y);
            }
        };
        read(leftStick_, p.thumbLX, p.thumbLY, true);
        read(rightStick_, p.thumbRX, p.thumbRY, rightY_);
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

}  // namespace mohavr::host
