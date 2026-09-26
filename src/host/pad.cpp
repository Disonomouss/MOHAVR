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
// Defaults (headset round 5, the player's choice) on top of MOHA's own pad layout (MOHAPlayerInput.uc):
// Xbox A = reload/use/flare -> B and the right grip; Xbox B = switch weapon -> Y; Xbox X = crouch (a press
// toggles the stance) -> a flick of the right stick down; Xbox Y = jump -> A; RB = grenade -> X;
// LB = alt fire -> left grip; LS = sprint (latched, SprintToggle) -> left stick click; RS = melee.
constexpr const wchar_t* kTargetDefaults[] = {L"b,rgrip", L"y",    L"rflickdown", L"a",    L"lgrip", L"x",
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
    auto stick = [&](const wchar_t* key, const wchar_t* def) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", key, def, v, 16, ini.c_str());
        return !_wcsicmp(v, L"left") ? 0 : !_wcsicmp(v, L"right") ? 1 : -1;
    };
    leftStick_ = stick(L"LeftStick", L"left");
    rightStick_ = stick(L"RightStick", L"right");
    rightY_ = GetPrivateProfileIntW(L"Controls", L"RightStickY", 0, ini.c_str()) != 0;
    sprintToggle_ = GetPrivateProfileIntW(L"Controls", L"SprintToggle", 1, ini.c_str()) != 0;

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
    MLOG("pad: gameplay actions ready -- %s LeftStick=%d RightStick=%d RightStickY=%d SprintToggle=%d", m.c_str(),
         leftStick_, rightStick_, rightY_, sprintToggle_);
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
shared::PadState Pad::Map(const Raw& in) {
    Raw r = in;
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
        for (Src s : map_[t])
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
        p.thumbLX = Axis(sx[leftStick_]);
        p.thumbLY = Axis(sy[leftStick_]);
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
    FILE* f = nullptr;
    if (_wfopen_s(&f, testPath_.c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            Test t;
            t.dur = 0.5;
            float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
            std::string buttons, press;
            char* ctx = nullptr;
            for (char* tok = strtok_s(line, " \t\r\n", &ctx); tok; tok = strtok_s(nullptr, " \t\r\n", &ctx)) {
                char* eq = strchr(tok, '=');
                if (!eq) continue;
                *eq = 0;
                const char* v = eq + 1;
                if (!strcmp(tok, "lx")) lx = static_cast<float>(atof(v));
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
    DeleteFileW(testPath_.c_str());
    MLOG("pad: %zu test state(s) queued", tests_.size());
    (void)now;
}

void Pad::Update(XrSession session, double now, bool neutral, int snapDeg, shared::Header* hdr) {
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

    shared::PadState p{};
    if (testActive_ && !test_.raw) {
        p = test_.state;
    } else if (testActive_ || !neutral) {
        Raw r{};
        if (testActive_) r = test_.rawIn;
        else ReadRaw(session, r);
        p = Map(r);
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

}  // namespace mohavr::host
