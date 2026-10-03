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

// A holster whose command draws a grenade: the type its off-hand take gets (0 frag, 1 Gammon, 2 stick, 0xFF the game's
// order), -1 for any other.
int GrenadeZoneType(const std::string& cmd) {
    std::string c;
    for (char ch : cmd) c += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    if (c.find("grenade") == std::string::npos && c.find("gammon") == std::string::npos && c.find("stick") == std::string::npos)
        return -1;
    return c.find("frag") != std::string::npos ? 0 : c.find("gammon") != std::string::npos ? 1 : c.find("stick") != std::string::npos ? 2 : 0xFF;
}

// A holster whose command draws the pistol (the off-hand pistol's draw spot).
bool PistolZone(const std::string& cmd) {
    std::string c;
    for (char ch : cmd) c += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    return c == "switchpistol";
}

// A holster that holds the off-hand knife (a host-only command: never run in the game).
bool KnifeZone(const std::string& cmd) { return !_stricmp(cmd.c_str(), "Knife"); }

}  // namespace

const wchar_t* Hands::SpotName(int i) {
    static const wchar_t* kNames[kSpots] = {L"RightShoulder", L"LeftShoulder", L"RightHip", L"LeftHip", L"Chest", L"LowerBack", L"MagPouch"};
    return i >= 0 && i < kSpots ? kNames[i] : L"";
}

void Hands::Init(const std::wstring& ini) {
    // Body spots in metres from the head (right, up, forward) in the head's heading frame. The command is the
    // game's own (MOHAPlayerController exec functions).
    zones_[0] = {L"RightShoulder", "SwitchPrimary"};
    zones_[1] = {L"LeftShoulder", "SwitchSecondary"};
    zones_[2] = {L"RightHip", "SwitchPistol"};
    zones_[3] = {L"LeftHip", "SwitchGrenade"};
    zones_[4] = {L"Chest", "SwitchPistol"};  // the player, 2026-10-02: "Add chest holster" (the off-hand pistol's cross-draw)
    zones_[5] = {L"LowerBack", "Knife"};     // the player, 2026-10-03: "Add a holster to lower back for it" (the off-hand knife)
    // Where (cm from the head: right, up, forward) and how big (radius, cm): [Holsters] <Name>Spot = x y z r. The last is
    // the manual reload's magazine pouch, at the middle of the belt (D21).
    const HolsterSpot builtIn[kSpots] = {{20, -22, -8, 16}, {-20, -22, -8, 16}, {22, -65, 0, 16}, {-22, -65, 0, 16}, {0, -34, 10, 12},
                                         {0, -58, -22, 16}, {0, -60, 14, 12}};
    for (int i = 0; i < kSpots; ++i) {
        wchar_t v[64] = L"";
        if (i < kHolsters) {
            Zone& z = zones_[i];
            GetPrivateProfileStringW(L"Holsters", z.key, L"", v, 64, ini.c_str());
            if (v[0] && !_wcsicmp(v, L"none")) z.command.clear();
            else if (v[0]) {
                std::string s;
                for (const wchar_t* p = v; *p; ++p) s += static_cast<char>(*p < 128 ? *p : '?');
                z.command = s;
            }
            defaultCommands_[i] = z.command;
        }
        HolsterSpot cm = builtIn[i];
        const std::wstring key = std::wstring(SpotName(i)) + L"Spot";
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
    pouchReload_ = GetPrivateProfileIntW(L"Hands", L"PouchReload", 0, ini.c_str()) != 0;
    mirrorLeft_ = GetPrivateProfileIntW(L"Weapon", L"LeftHandMirror", 1, ini.c_str()) != 0;
    MLOG("hands: holsters %d (%s / %s / %s / %s / %s / %s), foregrip %d, reload gesture %d, left hand mirrored %d", holsters_,
         zones_[0].command.c_str(), zones_[1].command.c_str(), zones_[2].command.c_str(), zones_[3].command.c_str(),
         zones_[4].command.c_str(), zones_[5].command.c_str(), foregrip_, reloadGesture_, mirrorLeft_);
}

void Hands::SetCommand(int i, const std::string& c) {
    if (i < 0 || i >= kHolsters || zones_[i].command == c) return;
    zones_[i].command = c;
    MLOG("hands: %ls holds '%s'", zones_[i].key, c.empty() ? "nothing" : c.c_str());
}

Hands::Output Hands::Update(const Input& in) {
    Output out;
    // The gun hand: the menu's starting hand (applied whenever that setting changes), then whichever hand draws.
    // (Not while the off hand holds a grenade or the pistol: the change waits for the throw or the put back, OFFHAND-DESIGN
    // 5.5.)
    const bool nadeHeld = nade_ && nade_->Holding();
    const bool pistolHeld = pistol_ && pistol_->Holding();
    const bool knifeHeld = knife_ && knife_->Holding();  // (as the frame starts: a draw or a put back below holds strikes off)
    const bool offBusy = nadeHeld || pistolHeld || knifeHeld;  // the off hand holds something: no foregrip, no reload spots
    // A holster in use: one that holds something (a knife holster only while the off-hand knife is on).
    auto zoneLive = [&](int z) { return !zones_[z].command.empty() && (!KnifeZone(zones_[z].command) || (knife_ && knife_->On())); };
    if (static_cast<int>(in.startLeft) != lastStart_ && !offBusy) {
        lastStart_ = in.startLeft ? 1 : 0;
        gunHand_ = in.startLeft ? 0 : 1;
        twoHanded_ = false;
        MLOG("hands: gun hand -> %s (the starting hand)", gunHand_ ? "right" : "left");
    }
    const int g = gunHand_, o = 1 - g;
    if (in.testThrow) {  // kept for the next release (the gun hand's grenade, or the off hand's)
        testThrow_ = true;
        for (int i = 0; i < 3; ++i) testThrowVel_[i] = in.testThrowVel[i];
    }
    // The manual reload drives the gun in hand (D21), or not (then the fixed-spot reload gesture, RELOAD-DESIGN 3.8).
    ManualReload::In rin;
    rin.gunOk = (in.valid & (1u << g)) != 0;
    rin.offOk = (in.valid & (1u << o)) != 0;
    rin.gestures = in.gestures;
    rin.hasView = in.hasView;
    rin.gunHand = g;
    rin.fitAngle = in.fit.angle;
    rin.offTrigger = in.trigger[o];
    rin.gunTrigger = in.trigger[g];
    rin.showSpots = in.reloadSpotsShown;
    rin.foregrip = foregrip_ && in.weaponKind == 0 && in.fit.foreFwd >= 15.0f;  // (GOAL A3: a pump gun's pump)
    rin.now = in.now;
    for (int h = 0; h < 2; ++h) rin.release[h] = in.release[h];
    if (reload_) reload_->SetRingScale(ringScale_);
    const bool reloadActive = reload_ && reload_->Begin(rin);
    // The foregrip is for long guns only, the reload gesture not for grenades (headset round 17).
    const bool foregripOk = foregrip_ && in.weaponKind == 0, reloadOk = reloadGesture_ && in.weaponKind != 2 && !reloadActive;
    if (twoHanded_ && !foregripOk) {
        twoHanded_ = false;
        MLOG("hands: foregrip let go (not a long gun)");
    }
    const bool gunOk = (in.valid & (1u << g)) != 0, offOk = (in.valid & (1u << o)) != 0;
    // Where each hand interacts (round 31: the white dot in the drawn hand): the aim point moved by the hand point, in
    // the controller's frame (x right, y up, z back); "in" is toward the palm, the right hand's left.
    V3 pt[2];
    for (int h = 0; h < 2; ++h) {
        const float side = h ? -handPoint_[2] : handPoint_[2];
        pt[h] = Add(P(in.aim[h].position), Rotate(in.aim[h].orientation, V3{side, handPoint_[1], -handPoint_[0]}));
    }

    // The gun's pose before the foregrip: the gun hand's aim pose, pitched by the fit's angle (+ = muzzle up).
    XrPosef gun{};
    if (gunOk) {
        const float a = in.fit.angle * 0.0174533f;
        gun.position = in.aim[g].position;
        gun.orientation = Mul(in.aim[g].orientation, XrQuaternionf{std::sin(a * 0.5f), 0.0f, 0.0f, std::cos(a * 0.5f)});
    }
    const V3 gunPos = P(gun.position);
    // The gun's frame: -Z forward; the fit's right / left mirrored for a left gun hand, like the aim line.
    const float foreRight = (g == 0 && mirrorLeft_) ? -in.fit.foreRight : in.fit.foreRight;
    const V3 foreOff = {foreRight / 100.0f, in.fit.foreUp / 100.0f, -in.fit.foreFwd / 100.0f};
    const V3 fore = Add(gunPos, Rotate(gun.orientation, foreOff));
    const V3 mag = Add(gunPos, Rotate(gun.orientation, V3{0.0f, -0.08f, -0.10f}));  // the magazine well

    // Holster spots in the head's heading frame.
    const auto& hq = in.head.orientation;
    const float fx = -(2.0f * (hq.x * hq.z + hq.w * hq.y)), fz = -(1.0f - 2.0f * (hq.x * hq.x + hq.y * hq.y));
    const float heading = std::atan2(-fx, -fz);
    const V3 right{std::cos(heading), 0.0f, -std::sin(heading)}, fwd{-std::sin(heading), 0.0f, -std::cos(heading)};
    const V3 head = P(in.head.position);
    V3 centre[kSpots];
    for (int z = 0; z < kSpots; ++z)
        centre[z] = Add(head, Add(Add(Scale(right, spots_[z].x), V3{0.0f, spots_[z].y, 0.0f}), Scale(fwd, spots_[z].z)));
    const V3 pouch = centre[kHolsters];
    const float pouchR = spots_[kHolsters].r;

    // The spots, for the rings: each holster, and the off hand's foregrip / magazine spots on the gun.
    auto addSpot = [&](SpotKind kind, V3 c, float r, bool onlyOffHand) {
        Spot& s = out.spots[out.spotCount++];
        s.kind = kind;
        s.pos = {c.x, c.y, c.z};
        s.radius = r;
        for (int h = 0; h < 2; ++h) {
            if (!(in.valid & (1u << h)) || (onlyOffHand && h != o)) continue;
            const float d = Len(Sub(pt[h], c));
            if (d < r) s.inside = true;
            if (d < 2.0f * r) s.close = true;
        }
    };
    if (holsters_)
        for (int z = 0; z < kHolsters; ++z)
            if (zoneLive(z) || in.pouchShown) addSpot(kHolster, centre[z], spots_[z].r, false);
    if (gunOk && foregripOk && in.fit.foreFwd >= 15.0f && !offBusy) addSpot(kForegrip, fore, foregripR_, true);
    if (gunOk && reloadOk && !offBusy) addSpot(kMagazine, mag, 0.10f * ringScale_, true);
    // The pouch: while the gun's magazine is out (a new one comes from it), or while the Holsters page moves it.
    if (in.pouchShown || (reloadActive && reload_->MagazineOut() && !offBusy)) addSpot(kPouch, pouch, pouchR, !in.pouchShown);
    else if (pouchReload_ && gunOk && in.weaponKind != 2) addSpot(kPouch, pouch, pouchR, false);  // (the pouch reload)
    out.targetOk[1] = true;
    out.target[1] = {pouch.x, pouch.y, pouch.z};
    out.offValid = offOk;
    out.offHand = {pt[o].x, pt[o].y, pt[o].z};
    // The off-hand grenade's view of the off hand.
    OffHandGrenade::In nin;
    nin.offHand = o;
    nin.offTracked = (in.tracked & (1u << o)) != 0 && offOk;
    nin.gripActive = in.gripActive[o];
    nin.gestures = in.gestures;
    nin.modMenu = in.modMenu;
    nin.gameMenu = in.gameMenu;
    nin.hasView = in.hasView;
    nin.gunOk = gunOk && in.weaponKind != 2;
    nin.trigger = in.trigger[o];
    nin.hand = {in.aim[o].orientation, {pt[o].x, pt[o].y, pt[o].z}};
    nin.testThrow = testThrow_;
    for (int i = 0; i < 3; ++i) nin.testVel[i] = testThrowVel_[i];
    nin.now = in.now;
    // The gun hand's grenade's view of the gun hand.
    OffHandGrenade::In gin = nin;
    gin.offHand = g;
    gin.offTracked = (in.tracked & (1u << g)) != 0 && gunOk;
    gin.gripActive = in.gripActive[g];
    gin.gunOk = gunOk && in.grenadeType >= 0;
    gin.type = in.grenadeType >= 0 ? static_cast<std::uint32_t>(in.grenadeType) : 0u;
    gin.trigger = in.trigger[g];
    gin.hand = {in.aim[g].orientation, {pt[g].x, pt[g].y, pt[g].z}};
    // The off-hand pistol's view of it.
    OffHandPistol::In pin;
    pin.offHand = o;
    pin.offValid = offOk;
    pin.offTracked = nin.offTracked;
    pin.gripActive = in.gripActive[o];
    pin.gestures = in.gestures;
    pin.hasView = in.hasView;
    pin.gunOk = gunOk;
    pin.nadeHeld = nadeHeld;
    pin.trigger = in.trigger[o];
    pin.offAim = in.aim[o];
    pin.fit = in.pistolFit;
    pin.now = in.now;

    for (int h = 0; h < 2; ++h) {
        const bool ok = (in.valid & (1u << h)) != 0;
        const V3 hp = pt[h];
        // Which holster spot the hand is in (a pulse when it enters one): the one it is deepest in, by its distance over the
        // radius (the lower back touches the left hip; the list's order mustn't decide).
        int zone = -1;
        if (ok && holsters_) {
            float best = 1.0f;
            for (int z = 0; z < kHolsters; ++z) {
                if (!zoneLive(z)) continue;
                const float depth = Len(Sub(hp, centre[z])) / spots_[z].r;
                if (depth < best) {
                    best = depth;
                    zone = z;
                }
            }
        }
        // The off hand at the foregrip and a holster at once (the chest spot, a long gun at low ready): whichever centre the
        // hand is closer to (the review: a squeeze meant for the foregrip drew a weapon).
        if (zone >= 0 && !offBusy && h == 1 - gunHand_ && foregrip_ && in.weaponKind == 0 && in.fit.foreFwd >= 15.0f && (in.valid & (1u << gunHand_)) &&
            Len(Sub(hp, fore)) < foregripR_ && Len(Sub(hp, fore)) < Len(Sub(hp, centre[zone])))
            zone = -1;
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
        // (Physical melee: a draw, a holster or the pouch is a fast move of the gun hand, not a strike.)
        if (h == g && (zone >= 0 || Len(Sub(hp, pouch)) < pouchR)) gunPressAt_ = in.now;
        if (!in.gestures || !ok) continue;
        // The pouch reload (the player, 2026-10-02): a hand holding a gun grips the ammo pouch -- reloaded at once, no
        // animation. The gun hand's gun (not a grenade), or the pistol the off hand holds.
        if (pouchReload_ && Len(Sub(hp, pouch)) < pouchR) {
            const bool gunHand = h == g && gunOk && in.weaponKind != 2 && !(gunNade_ && gunNade_->Live());
            const bool offPistol = h == o && pistol_ && pistol_->Holding();
            if (gunHand || offPistol) {
                out.command = gunHand ? "mohavr pouchreload gun" : "mohavr pouchreload off";
                consumed_[h] = true;
                out.pulse[h] = true;
                out.pulseAmp[h] = 0.4f;
                out.pulseMs[h] = 25.0f;
                MLOG("hands: %s hand at the pouch -- %s", h ? "right" : "left", gunHand ? "the gun reloaded" : "the pistol refilled");
                continue;
            }
        }
        // The off hand holds a grenade (pressed again after a freeze let the grip go): the grip is the grenade's.
        if (h == o && nade_ && nade_->Holding()) {
            nade_->HeldPress(nin, zone >= 0);
            consumed_[h] = true;
            continue;
        }
        // The gun hand's grenade with its pin out: the squeeze starts the throw (let go to throw).
        if (h == g && gunNade_ && gunNade_->HeldPress(gin, zone >= 0)) {
            consumed_[h] = true;
            continue;
        }
        // The off hand holds the pistol: its presses are the pistol's (with PistolHold=toggle a click at any holster puts it
        // back), out of reach of the reload's spots, the grenade and the foregrip.
        if (h == o && pistol_ && pistol_->Holding()) {
            pistol_->HeldPress(pin, zone >= 0);
            consumed_[h] = true;
            continue;
        }
        // The off hand holds the knife: likewise its presses are the knife's (a click at a holster puts it back).
        if (h == o && knifeHeld) {
            OffHandKnife::Pulse kp;
            knife_->HeldPress(zone >= 0, kp);
            if (kp.amp > 0.0f) {
                out.pulse[h] = true;
                out.pulseAmp[h] = kp.amp;
                out.pulseMs[h] = kp.ms;
            }
            consumed_[h] = true;
            continue;
        }
        // The manual reload's spots first (the pouch touches the hip spots; RELOAD-DESIGN 3.4).
        if (h == o && reloadActive && reload_->TakePress({hp.x, hp.y, hp.z}, {pouch.x, pouch.y, pouch.z}, pouchR)) {
            consumed_[h] = true;
            continue;
        }
        // The off-hand grenade (OFFHAND-DESIGN 5.5): the off hand at a grenade holster takes one while a gun is in the
        // other hand; the gun hand there is refused while one is held (it would make a grenade the main weapon).
        const int nadeType = zone >= 0 ? GrenadeZoneType(zones_[zone].command) : -1;
        if (nade_ && nadeType >= 0) {
            if (h == o && nade_->TakePress(nin, static_cast<std::uint32_t>(nadeType))) {
                consumed_[h] = true;
                continue;
            }
            if (h == g && nade_->Holding()) {
                consumed_[h] = true;
                out.pulse[h] = true;
                out.pulseAmp[h] = 0.2f;
                out.pulseMs[h] = 60.0f;
                MLOG("hands: the gun hand at %ls while the off hand holds a grenade -- refused", zones_[zone].key);
                continue;
            }
        }
        // The off-hand pistol (OFFPISTOL-DESIGN 4.1): the off hand at a pistol holster draws it while a gun is in the other
        // hand; the gun hand there is refused while it is held (SwitchPistol would equip the pistol the off hand holds).
        if (pistol_ && zone >= 0 && PistolZone(zones_[zone].command)) {
            if (h == o && pistol_->DrawPress(pin)) {
                consumed_[h] = true;
                continue;
            }
            if (h == g && pistol_->Holding()) {
                consumed_[h] = true;
                out.pulse[h] = true;
                out.pulseAmp[h] = 0.2f;
                out.pulseMs[h] = 60.0f;
                MLOG("hands: the gun hand at %ls while the off hand holds the pistol -- refused", zones_[zone].key);
                continue;
            }
        }
        // The off-hand knife (OFFKNIFE-DESIGN): the off hand at a knife holster draws it while a gun is in the other hand;
        // the gun hand there does nothing (the command is the host's, never the game's).
        if (zone >= 0 && KnifeZone(zones_[zone].command)) {
            OffHandKnife::Pulse kp;
            if (h == o && gunOk && knife_) knife_->DrawPress(kp);
            else kp = {0.2f, 60.0f};
            out.pulse[h] = true;
            out.pulseAmp[h] = kp.amp;
            out.pulseMs[h] = kp.ms;
            consumed_[h] = true;
            if (h != o || !gunOk) MLOG("hands: %s hand at %ls -- the knife is the off hand's, with a gun in the other", h ? "right" : "left",
                                       zones_[zone].key);
            continue;
        }
        if (zone >= 0) {
            out.command = zones_[zone].command;
            consumed_[h] = true;
            out.pulse[h] = true;
            MLOG("hands: %s hand at %ls -> '%s'", h ? "right" : "left", zones_[zone].key, out.command.c_str());
            if (h != gunHand_ && _strnicmp(out.command.c_str(), "Switch", 6) == 0 && !(gunNade_ && gunNade_->Live())) {
                // The hand that draws holds the gun; the other one becomes the foregrip / reload hand. (Pressed
                // this frame: the rest of this frame still works out the old gun hand's gun.) Only a draw does (a holster
                // may run Reload).
                gunHand_ = h;
                twoHanded_ = false;
                gunPressAt_ = in.now;  // (physical melee: the draw is the new gun hand's fast move, not a strike)
                MLOG("hands: gun hand -> %s (drew)", h ? "right" : "left");
            }
        } else if (h == o && gunOk) {
            if (foregripOk && Len(Sub(hp, fore)) < foregripR_) {
                twoHanded_ = true;
                consumed_[h] = true;
                out.pulse[h] = true;
                MLOG("hands: foregrip taken (%.0f cm from the point)", 100.0f * Len(Sub(hp, fore)));
            } else if (reloadOk && Len(Sub(hp, mag)) < 0.10f * ringScale_) {
                out.command = "Reload";
                consumed_[h] = true;
                out.pulse[h] = true;
                MLOG("hands: reload gesture");
            }
        }
    }
    // A pulse on the off hand as it comes to the foregrip (not while already holding it).
    if (gunOk && offOk && foregripOk && !twoHanded_ && in.gestures && !offBusy) {
        const bool atFore = Len(Sub(pt[o], fore)) < foregripR_;
        if (atFore && !nearFore_) out.pulse[o] = true;
        nearFore_ = atFore;
    }

    // Two-handed: the gun turns so that its foregrip point lies on the other hand (a long gun only: the point must
    // be well ahead of the gun hand for the direction to be steady).
    if (gunOk && offOk && twoHanded_ && in.fit.foreFwd >= 15.0f) {
        const V3 want = Sub(pt[o], gunPos);
        const V3 have = Rotate(gun.orientation, foreOff);
        if (Len(want) > 0.12f) {
            gun.orientation = Mul(FromTo(have, want), gun.orientation);
            out.turned = true;
        }
    }
    // The manual reload, with the gun's final pose.
    if (reload_) {
        rin.gun = gun;
        rin.off = {in.aim[o].orientation, {pt[o].x, pt[o].y, pt[o].z}};
        rin.offAim = in.aim[o];
        rin.offHeld = held_[o];
        rin.foreHeld = twoHanded_;
        ManualReload::Out rout;
        reload_->Frame(rin, rout);
        if (rout.releaseForegrip && twoHanded_) {
            twoHanded_ = false;  // the grip now holds the magazine (it stays consumed)
            MLOG("hands: foregrip let go (the hand took the magazine)");
        }
        for (int i = 0; i < rout.ringCount && out.spotCount < kHolsters + 5 && !offBusy; ++i) {
            Spot& sp = out.spots[out.spotCount++];
            sp = {kMagWell, rout.rings[i].pos, rout.rings[i].radius, rout.rings[i].inside, rout.rings[i].close};
        }
        for (int h = 0; h < 2; ++h) {
            if (rout.pulseAmp[h] > 0.0f) {
                out.pulse[h] = true;
                out.pulseAmp[h] = rout.pulseAmp[h];
                out.pulseMs[h] = rout.pulseMs[h];
            }
            out.maskFace[h] = rout.mask[h];
            out.maskTrigger[h] = rout.maskTrigger[h];
        }
        out.targetOk[0] = rout.targetOk[0];
        out.target[0] = rout.target[0];
        out.targetOk[2] = rout.targetOk[1];
        out.target[2] = rout.target[1];
        // Tests put the aim point at a target: the ones for a spot are moved by the hand point, so it lands there.
        const V3 hpOff = Sub(pt[o], P(in.aim[o].position));
        for (int i = 0; i < 3; ++i) out.target[i] = {out.target[i].x - hpOff.x, out.target[i].y - hpOff.y, out.target[i].z - hpOff.z};
        out.targetOk[3] = rout.targetOk[2];
        out.target[3] = rout.target[2];
        for (int i = 0; i < 2; ++i) {  // GOAL A2: the bolt's knob lifted / drawn back (spots: moved by the hand point too)
            out.targetOk[4 + i] = rout.targetOk[3 + i];
            const XrVector3f& t = rout.target[3 + i];
            out.target[4 + i] = {t.x - hpOff.x, t.y - hpOff.y, t.z - hpOff.z};
        }
        out.alignOk = rout.alignOk;
        out.align = rout.align;
        // GOAL A3: the foregrip point (a pump gun's pump), moved by the hand point like the other spots.
        out.targetOk[6] = gunOk;
        out.target[6] = {fore.x - hpOff.x, fore.y - hpOff.y, fore.z - hpOff.z};
    }
    // The off-hand grenade, every frame (after the reload: its masks and pulses add to the reload's).
    if (nade_) {
        nin.gripHeld = held_[o];
        OffHandGrenade::Out nout;
        nade_->Frame(nin, nout);
        if (nout.usedTest) testThrow_ = false;
        if (nout.maskTrigger) out.maskTrigger[o] = true;
        out.maskSwitch = nout.maskSwitch;
        for (int h = 0; h < 2; ++h)
            if (nout.pulseAmp[h] > 0.0f) {
                out.pulse[h] = true;
                out.pulseAmp[h] = std::fmax(out.pulseAmp[h], nout.pulseAmp[h]);
                out.pulseMs[h] = std::fmax(out.pulseMs[h], nout.pulseMs[h]);
            }
        // Tests (pad_cmd.txt hand=l,@grenade): the first grenade holster, moved by the hand point like the other spots.
        const V3 hpOff = Sub(pt[o], P(in.aim[o].position));
        for (int z = 0; z < kHolsters && holsters_; ++z)
            if (GrenadeZoneType(zones_[z].command) >= 0) {
                out.targetOk[7] = true;
                out.target[7] = {centre[z].x - hpOff.x, centre[z].y - hpOff.y, centre[z].z - hpOff.z};
                break;
            }
    }
    // The gun hand's grenade, every frame.
    if (gunNade_) {
        gin.gripHeld = held_[g];
        gin.testThrow = testThrow_;
        OffHandGrenade::Out gout;
        gunNade_->Frame(gin, gout);
        if (gout.usedTest) testThrow_ = false;
        if (gout.maskTrigger) out.maskTrigger[g] = true;
        for (int h = 0; h < 2; ++h)
            if (gout.pulseAmp[h] > 0.0f) {
                out.pulse[h] = true;
                out.pulseAmp[h] = std::fmax(out.pulseAmp[h], gout.pulseAmp[h]);
                out.pulseMs[h] = std::fmax(out.pulseMs[h], gout.pulseMs[h]);
            }
    }
    // The off-hand pistol, every frame (after the grenade: its masks and pulses add to the others').
    if (pistol_) {
        pin.gripHeld = held_[o];
        pin.nadeHeld = nade_ && nade_->Holding();
        OffHandPistol::Out pout;
        pistol_->Frame(pin, pout);
        if (pout.maskTrigger) out.maskTrigger[o] = true;
        out.maskSwitchB = pout.maskSwitch;
        for (int h = 0; h < 2; ++h)
            if (pout.pulseAmp[h] > 0.0f) {
                out.pulse[h] = true;
                out.pulseAmp[h] = std::fmax(out.pulseAmp[h], pout.pulseAmp[h]);
                out.pulseMs[h] = std::fmax(out.pulseMs[h], pout.pulseMs[h]);
            }
        // Tests (pad_cmd.txt hand=l,@pistol): the first pistol holster, moved by the hand point like the other spots;
        // hand=l,@chest: the chest holster.
        const V3 hpOff = Sub(pt[o], P(in.aim[o].position));
        for (int z = 0; z < kHolsters && holsters_; ++z)
            if (PistolZone(zones_[z].command)) {
                out.targetOk[8] = true;
                out.target[8] = {centre[z].x - hpOff.x, centre[z].y - hpOff.y, centre[z].z - hpOff.z};
                break;
            }
        out.targetOk[9] = true;
        out.target[9] = {centre[4].x - hpOff.x, centre[4].y - hpOff.y, centre[4].z - hpOff.z};
    }
    // The off-hand knife, every frame (grip mode: letting go puts it back).
    if (knife_) {
        OffHandKnife::Pulse kp;
        knife_->Frame(held_[o], kp);
        if (kp.amp > 0.0f) {
            out.pulse[o] = true;
            out.pulseAmp[o] = std::fmax(out.pulseAmp[o], kp.amp);
            out.pulseMs[o] = std::fmax(out.pulseMs[o], kp.ms);
        }
        // Tests (pad_cmd.txt hand=l,@back): the first knife holster, moved by the hand point like the other spots.
        const V3 hpOff = Sub(pt[o], P(in.aim[o].position));
        for (int z = 0; z < kHolsters && holsters_; ++z)
            if (KnifeZone(zones_[z].command)) {
                out.targetOk[10] = true;
                out.target[10] = {centre[z].x - hpOff.x, centre[z].y - hpOff.y, centre[z].z - hpOff.z};
                break;
            }
    }
    out.twoHanded = twoHanded_;
    out.gunHand = gunHand_;
    if (!in.gestures) gesturesOffAt_ = in.now;
    out.meleeBusy = (gunPressAt_ >= 0.0 && in.now - gunPressAt_ < 0.4) || (reload_ && reload_->GunHandBusy()) ||
                    (gesturesOffAt_ >= 0.0 && in.now - gesturesOffAt_ < 0.15);
    if (knife_ && knife_->Holding() != knifeHeld) offGripAt_ = in.now;
    out.knifeBusy = (offGripAt_ >= 0.0 && in.now - offGripAt_ < 0.4) || (gesturesOffAt_ >= 0.0 && in.now - gesturesOffAt_ < 0.15);
    if (gunOk && g == gunHand_) {
        out.gunValid = true;
        out.gun = gun;
        out.aimRay.orientation = gun.orientation;
        // The fit is tuned on the right hand; the game draws a left hand's gun mirrored (Weapon.LeftHandMirror), so its
        // barrel is as far to the left of the controller as the right hand's is to the right (round 26).
        const float rayRight = (g == 0 && mirrorLeft_) ? -in.fit.rayRight : in.fit.rayRight;
        const V3 start = Add(gunPos, Rotate(gun.orientation, V3{rayRight / 100.0f, in.fit.rayUp / 100.0f, 0.0f}));
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
    const float trig = in.trigger[g];
    if (!triggerHeld_ && trig > 0.5f) triggerHeld_ = true;
    if (triggerHeld_ && trig < 0.3f) {
        triggerHeld_ = false;
        if (in.grenade && gunOk && !(gunNade_ && gunNade_->On())) {  // (GrenadePin: the gun hand's grenade throws by its grip)
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
