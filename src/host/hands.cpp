#include "hands.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

struct V3 { float x, y, z; };
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 P(const XrVector3f& v) { return {v.x, v.y, v.z}; }

XrQuaternionf Mul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
V3 Rotate(const XrQuaternionf& q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 c = Cross(u, v), cc = Cross(u, c);
    return Add(v, Scale(Add(Scale(c, q.w), cc), 2.0f));
}
// The shortest rotation taking direction a onto direction b.
XrQuaternionf FromTo(V3 a, V3 b) {
    a = Scale(a, 1.0f / Len(a));
    b = Scale(b, 1.0f / Len(b));
    const V3 c = Cross(a, b);
    const float w = 1.0f + Dot(a, b);
    if (w < 1e-4f) return {0.0f, 1.0f, 0.0f, 0.0f};  // opposite: half a turn about up (never in practice)
    const float n = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z + w * w);
    return {c.x / n, c.y / n, c.z / n, w / n};
}

}  // namespace

const wchar_t* Hands::SpotName(int i) {
    static const wchar_t* kNames[kHolsters] = {L"RightShoulder", L"LeftShoulder", L"RightHip", L"LeftHip"};
    return i >= 0 && i < kHolsters ? kNames[i] : L"";
}

void Hands::Init(const std::wstring& ini) {
    // Body spots in metres from the head (right, up, forward) in the head's heading frame. The command is the
    // game's own (MOHAPlayerController exec functions).
    zones_[0] = {L"RightShoulder", "SwitchPrimary"};
    zones_[1] = {L"LeftShoulder", "SwitchSecondary"};
    zones_[2] = {L"RightHip", "SwitchPistol"};
    zones_[3] = {L"LeftHip", "SwitchGrenade"};
    // Where (cm from the head: right, up, forward) and how big (radius, cm): [Holsters] <Name>Spot = x y z r.
    const HolsterSpot builtIn[kHolsters] = {{20, -22, -8, 16}, {-20, -22, -8, 16}, {22, -65, 0, 16}, {-22, -65, 0, 16}};
    for (int i = 0; i < kHolsters; ++i) {
        Zone& z = zones_[i];
        wchar_t v[64] = L"";
        GetPrivateProfileStringW(L"Holsters", z.key, L"", v, 64, ini.c_str());
        if (v[0] && !_wcsicmp(v, L"none")) z.command.clear();
        else if (v[0]) {
            std::string s;
            for (const wchar_t* p = v; *p; ++p) s += static_cast<char>(*p < 128 ? *p : '?');
            z.command = s;
        }
        HolsterSpot cm = builtIn[i];
        const std::wstring key = std::wstring(z.key) + L"Spot";
        GetPrivateProfileStringW(L"Holsters", key.c_str(), L"", v, 64, ini.c_str());
        if (v[0]) swscanf_s(v, L"%f %f %f %f", &cm.x, &cm.y, &cm.z, &cm.r);
        defaultSpots_[i] = {cm.x / 100.0f, cm.y / 100.0f, cm.z / 100.0f, cm.r / 100.0f};
        spots_[i] = defaultSpots_[i];
    }
    auto iniFloat = [&](const wchar_t* sec, const wchar_t* key, float def) {
        wchar_t b[32] = L"";
        GetPrivateProfileStringW(sec, key, L"", b, 32, ini.c_str());
        return b[0] ? static_cast<float>(_wtof(b)) : def;
    };
    defaultFit_ = {{iniFloat(L"Weapon", L"GripX", 34.0f), iniFloat(L"Weapon", L"GripY", 11.0f), iniFloat(L"Weapon", L"GripZ", -17.0f)},
                   0.0f, iniFloat(L"Aim", L"RayUp", 8.0f), 0.0f, iniFloat(L"Hands", L"ForeFwd", 30.0f),
                   iniFloat(L"Hands", L"ForeUp", 0.0f)};
    holsters_ = GetPrivateProfileIntW(L"Holsters", L"Enabled", 1, ini.c_str()) != 0;
    foregrip_ = GetPrivateProfileIntW(L"Hands", L"Foregrip", 1, ini.c_str()) != 0;
    reloadGesture_ = GetPrivateProfileIntW(L"Hands", L"ReloadGesture", 1, ini.c_str()) != 0;
    MLOG("hands: holsters %d (%s / %s / %s / %s), foregrip %d, reload gesture %d", holsters_, zones_[0].command.c_str(),
         zones_[1].command.c_str(), zones_[2].command.c_str(), zones_[3].command.c_str(), foregrip_, reloadGesture_);
}

Hands::Output Hands::Update(const Input& in) {
    Output out;
    // The gun hand: the menu's starting hand (applied whenever that setting changes), then whichever hand draws.
    if (static_cast<int>(in.startLeft) != lastStart_) {
        lastStart_ = in.startLeft ? 1 : 0;
        gunHand_ = in.startLeft ? 0 : 1;
        twoHanded_ = false;
        MLOG("hands: gun hand -> %s (the starting hand)", gunHand_ ? "right" : "left");
    }
    const int g = gunHand_, o = 1 - g;
    const bool gunOk = (in.valid & (1u << g)) != 0, offOk = (in.valid & (1u << o)) != 0;

    // The gun's pose before the foregrip: the gun hand's aim pose, pitched by the fit's angle (+ = muzzle up).
    XrPosef gun{};
    if (gunOk) {
        const float a = in.fit.angle * 0.0174533f;
        gun.position = in.aim[g].position;
        gun.orientation = Mul(in.aim[g].orientation, XrQuaternionf{std::sin(a * 0.5f), 0.0f, 0.0f, std::cos(a * 0.5f)});
    }
    const V3 gunPos = P(gun.position);
    const V3 foreOff = {0.0f, in.fit.foreUp / 100.0f, -in.fit.foreFwd / 100.0f};  // the gun's frame: -Z forward
    const V3 fore = Add(gunPos, Rotate(gun.orientation, foreOff));
    const V3 mag = Add(gunPos, Rotate(gun.orientation, V3{0.0f, -0.08f, -0.10f}));  // the magazine well

    // Holster spots in the head's heading frame.
    const auto& hq = in.head.orientation;
    const float fx = -(2.0f * (hq.x * hq.z + hq.w * hq.y)), fz = -(1.0f - 2.0f * (hq.x * hq.x + hq.y * hq.y));
    const float heading = std::atan2(-fx, -fz);
    const V3 right{std::cos(heading), 0.0f, -std::sin(heading)}, fwd{-std::sin(heading), 0.0f, -std::cos(heading)};
    const V3 head = P(in.head.position);
    V3 centre[kHolsters];
    for (int z = 0; z < kHolsters; ++z)
        centre[z] = Add(head, Add(Add(Scale(right, spots_[z].x), V3{0.0f, spots_[z].y, 0.0f}), Scale(fwd, spots_[z].z)));

    // The spots, for the rings: each holster, and the off hand's foregrip / magazine spots on the gun.
    auto addSpot = [&](SpotKind kind, V3 c, float r, bool onlyOffHand) {
        Spot& s = out.spots[out.spotCount++];
        s.kind = kind;
        s.pos = {c.x, c.y, c.z};
        s.radius = r;
        for (int h = 0; h < 2; ++h) {
            if (!(in.valid & (1u << h)) || (onlyOffHand && h != o)) continue;
            const float d = Len(Sub(P(in.aim[h].position), c));
            if (d < r) s.inside = true;
            if (d < 2.0f * r) s.close = true;
        }
    };
    if (holsters_)
        for (int z = 0; z < kHolsters; ++z)
            if (!zones_[z].command.empty()) addSpot(kHolster, centre[z], spots_[z].r, false);
    if (gunOk && foregrip_ && in.fit.foreFwd >= 15.0f) addSpot(kForegrip, fore, 0.12f, true);
    if (gunOk && reloadGesture_) addSpot(kMagazine, mag, 0.10f, true);
    out.offValid = offOk;
    out.offHand = in.aim[o].position;

    for (int h = 0; h < 2; ++h) {
        const bool ok = (in.valid & (1u << h)) != 0;
        const V3 hp = P(in.aim[h].position);
        // Which holster spot the hand is in (a pulse when it enters one).
        int zone = -1;
        if (ok && holsters_) {
            for (int z = 0; z < kHolsters; ++z) {
                if (zones_[z].command.empty()) continue;
                if (Len(Sub(hp, centre[z])) < spots_[z].r) zone = z;
            }
        }
        if (zone >= 0 && zone != inZone_[h] && in.gestures) out.pulse[h] = true;
        inZone_[h] = zone;

        const float gr = in.grip[h];
        const bool press = !held_[h] && gr >= 0.6f, release = held_[h] && gr < 0.4f;
        if (release) {
            held_[h] = false;
            consumed_[h] = false;
            if (h == o && twoHanded_) {
                twoHanded_ = false;
                MLOG("hands: foregrip released");
            }
        }
        if (!press) continue;
        held_[h] = true;
        if (!in.gestures || !ok) continue;
        if (zone >= 0) {
            out.command = zones_[zone].command;
            consumed_[h] = true;
            out.pulse[h] = true;
            MLOG("hands: %s hand at %ls -> '%s'", h ? "right" : "left", zones_[zone].key, out.command.c_str());
            if (h != gunHand_) {
                // The hand that draws holds the gun; the other one becomes the foregrip / reload hand. (Pressed
                // this frame: the rest of this frame still works out the old gun hand's gun.)
                gunHand_ = h;
                twoHanded_ = false;
                MLOG("hands: gun hand -> %s (drew)", h ? "right" : "left");
            }
        } else if (h == o && gunOk) {
            if (foregrip_ && Len(Sub(hp, fore)) < 0.12f) {
                twoHanded_ = true;
                consumed_[h] = true;
                out.pulse[h] = true;
                MLOG("hands: foregrip taken (%.0f cm from the point)", 100.0f * Len(Sub(hp, fore)));
            } else if (reloadGesture_ && Len(Sub(hp, mag)) < 0.10f) {
                out.command = "Reload";
                consumed_[h] = true;
                out.pulse[h] = true;
                MLOG("hands: reload gesture");
            }
        }
    }
    // A pulse on the off hand as it comes to the foregrip (not while already holding it).
    if (gunOk && offOk && foregrip_ && !twoHanded_ && in.gestures) {
        const bool atFore = Len(Sub(P(in.aim[o].position), fore)) < 0.12f;
        if (atFore && !nearFore_) out.pulse[o] = true;
        nearFore_ = atFore;
    }

    // Two-handed: the gun turns so that its foregrip point lies on the other hand (a long gun only: the point must
    // be well ahead of the gun hand for the direction to be steady).
    if (gunOk && offOk && twoHanded_ && in.fit.foreFwd >= 15.0f) {
        const V3 want = Sub(P(in.aim[o].position), gunPos);
        const V3 have = Rotate(gun.orientation, foreOff);
        if (Len(want) > 0.12f) gun.orientation = Mul(FromTo(have, want), gun.orientation);
    }
    out.twoHanded = twoHanded_;
    out.gunHand = gunHand_;
    if (gunOk && g == gunHand_) {
        out.gunValid = true;
        out.gun = gun;
        out.aimRay.orientation = gun.orientation;
        const V3 start = Add(gunPos, Rotate(gun.orientation, V3{in.fit.rayRight / 100.0f, in.fit.rayUp / 100.0f, 0.0f}));
        out.aimRay.position = {start.x, start.y, start.z};
    }
    for (int h = 0; h < 2; ++h) out.consumed[h] = held_[h] && consumed_[h];

    // Throwing: remember where each hand was; when the gun hand's trigger lets go of a grenade, its velocity over the
    // last ~0.1 s is the throw (the game gives it to the grenade the release spawns).
    for (int h = 0; h < 2; ++h) {
        if (!(in.valid & (1u << h))) continue;
        Sample& s = hist_[h][histNext_[h]];
        histNext_[h] = (histNext_[h] + 1) % 16;
        s.t = in.now;
        s.p[0] = in.aim[h].position.x;
        s.p[1] = in.aim[h].position.y;
        s.p[2] = in.aim[h].position.z;
    }
    if (in.testThrow) {  // kept for the next release
        testThrow_ = true;
        for (int i = 0; i < 3; ++i) testThrowVel_[i] = in.testThrowVel[i];
    }
    const float trig = in.trigger[g];
    if (!triggerHeld_ && trig > 0.5f) triggerHeld_ = true;
    if (triggerHeld_ && trig < 0.3f) {
        triggerHeld_ = false;
        if (in.grenade && gunOk) {
            float v[3] = {0.0f, 0.0f, 0.0f};
            if (testThrow_) {
                testThrow_ = false;
                for (int i = 0; i < 3; ++i) v[i] = testThrowVel_[i];
            } else {
                // The newest sample and the one closest to 0.1 s before it.
                const Sample& now = hist_[g][(histNext_[g] + 15) % 16];
                const Sample* then = nullptr;
                for (int k = 2; k < 16; ++k) {
                    const Sample& c = hist_[g][(histNext_[g] + 16 - k) % 16];
                    if (c.t <= 0.0 || now.t - c.t > 0.2) break;
                    then = &c;
                    if (now.t - c.t >= 0.1) break;
                }
                if (then && now.t > then->t)
                    for (int i = 0; i < 3; ++i) v[i] = static_cast<float>((now.p[i] - then->p[i]) / (now.t - then->t));
            }
            const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            MLOG("hands: grenade released at %.1f m/s (%.1f %.1f %.1f)%s", speed, v[0], v[1], v[2],
                 speed >= minThrowSpeed_ ? " -> thrown by the hand" : " -> too slow: the game's own throw");
            if (speed >= minThrowSpeed_) {
                out.thrown = true;
                for (int i = 0; i < 3; ++i) out.throwVel[i] = v[i];
            }
        }
    }
    return out;
}

}  // namespace mohavr::host
