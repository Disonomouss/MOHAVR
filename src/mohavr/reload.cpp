#include "reload.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"

namespace mohavr::reload {
namespace {

#include "reload_grips.inc"

Config           g_cfg;
SafetyHookInline g_hook;
bool             g_hooked = false;        // execHasReserveAmmo hooked (count-only for the probe, or blocking)
bool             g_blockAllowed = false;  // Weapon.ManualReload + [ManualReload] Hook + the pipeline's hooks installed

// --- the per-gun lines ([ManualReload] Attachment_<Gun>=Action=open|closed MinUpgrade=N Mag=... ; RELOAD-DESIGN 1.3) ---
struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct RefBone {
    std::string name;
    Vec3        pos;
};
struct GunLine {
    std::string              key;
    // GOAL A4: a gun may have a line per range of upgrade levels -- "Attachment_Mauser@0=... MinUpgrade=-1 MaxUpgrade=0"
    // beside "Attachment_Mauser=... MinUpgrade=1" (the C96's clip-loaded fixed magazine below its 20-round box). `key` is
    // the attachment class; gripKey = the ini key (the reload grips are per line).
    std::string              gripKey;
    int                      maxUpgrade = 99;
    bool                     open = false;
    int                      minUpgrade = -1;       // (-1: every level -- an un-upgraded gun is at -1; GOAL A2 found the 0
                                                    // default had left every Step 1 gun unconverted before its first upgrade)
    bool                     emptyCue = true;   // Empty=none: no hold (RELOAD-DESIGN 0.4)
    std::vector<std::string> mag;               // the magazine bones (variants move together)
    std::vector<Vec3>        magOut, magGrab;   // per variant (one value = all)
    std::vector<float>       magR;              // cm
    std::vector<std::string> bolt;              // the action bone, then a second one moved with it (MP40 chamber_slide)
    float                    boltZ[3] = {};     // idle, empty, full back
    float                    bolt2Z[3] = {};    // the second bone's Z at those three
    bool                     haveBolt2Z = false;
    Vec3                     boltGrab;
    std::string              topRound;
    int                      refBones = 0;      // the RefSkeleton check (the .Ref line)
    std::vector<RefBone>     ref;
    std::vector<std::string> sndOut, sndIn, sndRack;  // the arms' reload cues ("group.name"), per variant (M7)
    std::string              taped;             // twin magazines: the second magazine's bone (taped when it shows)
    Vec3                     tapedA, tapedB;    // the pair's bone at rest in taped state A and B (mesh space)
    float                    tapedRot = 0.0f;   // B is turned this far about Z from A (degrees)
    bool                     grabTrigger = false;  // round 32: the magazine is grabbed only with the off hand's trigger held
    bool                     triggerRack = false;  // round 32: the gun hand's trigger releases a locked-back action
    // GOAL A1 (the Garand's en-bloc clip):
    bool                     ejectOnEmpty = false;  // EjectOnEmpty=1: the game throws the empty clip at the last shot (its own
                                                    // ping and projectile): the magazine is out, and no copy falls
    bool                     feedOnInsert = false;  // Feed=insert: an empty gun loads and closes as the clip goes in (no rack)
    bool                     noGrab = false;        // NoGrab=1: the seated magazine can't be pulled out by hand
    bool                     latch = true;          // Latch=0: the release button does nothing on this gun
    std::vector<std::string> sndClose;              // SndClose: the action closing on its own (Feed=insert), per variant
    int                      keepChambered = -1;    // KeepChambered=0|1: per gun, over [ManualReload] KeepChambered
                                                    // (the Garand: 0 -- a kept round would make the game throw a
                                                    // second empty clip at its shot)
    float                    ejectSpeed = 0.4f;     // EjectSpeed: m/s an ejected magazine leaves along its way out
    // GOAL A5 (the Panzerschreck's rocket): Insert=slide -- the held magazine's front meets the well's mouth, then it
    // slides in along its way out; MagLen = from the grab point to the front, MagSeat = the seated grab point's depth
    // inside the mouth (mesh units along MagOut).
    bool                     slideInsert = false;
    float                    magLen = 0.0f, magSeat = 0.0f;
    // GOAL A2 (the bolt actions, Action=bolt): a two-stage action the off hand works by its knob -- stage 1 turns the bolt
    // up (BoltLift degrees about mesh +Z through each BoltTurn bone's own origin), stage 2 draws it back BoltTravel units
    // (the BoltSlide bone, and the turned bones with it). After a shot the trigger is held until the bolt has been
    // cycled; the game's own rechamber is off for the gun while the manual reload drives it. It loads through the open
    // action: the "magazine" is the loading item (Mag=clip;round -- the visible variant, as the game's upgrade shows it),
    // parked by the game and drawn seated at MagRest; a stripper clip strips its rounds when seated and stays in the
    // guides (ClipIn) until the bolt closes; TopRoundAt = the top round in the open action.
    bool                     boltAction = false;
    std::vector<std::string> boltTurn;
    std::string              boltSlide;
    float                    boltLift = 0.0f, boltTravel = 0.0f;
    Vec3                     knob;
    bool                     haveTopRoundAt = false, haveClipIn = false;
    Vec3                     topRoundAt, clipIn;
    std::vector<Vec3>        magRest;
    std::vector<std::vector<float>> magRestRot;  // GOAL A3: a MagRest of 12 floats (the rows X, Y, Z, then the point) is
                                                 // turned too (the M12's shell nose-up through the port); else empty
    std::vector<std::string> sndUp, sndBack, sndFwd, sndDown, sndRound, sndClip, sndCase;
    // GOAL A3 (the M12, Action=pump): the off hand works the pump after every shot -- back (BOLT BACK: the case out) and
    // forward (BOLT FORWARD: a shell from the tube into the chamber); the trigger is held meanwhile and the game's own
    // rechamber is off while the manual reload drives the gun. Shells go one at a time through the loading port into the
    // tube (the action closed); the chamber is live, spent or empty, and loading an empty gun leaves it empty (pump once).
    // The pump and its second bone move along their parent's Z row (the M12's RootOffset is tilted 1 degree). SndIn2: the
    // shell's second cue, 60 ms after SndIn.
    bool                     pump = false;
    std::vector<std::string> sndIn2;
    // GOAL A5 (the M18's breech, Action=bolt with a swing): stage 2 swings instead of drawing back -- BoltSwing = the hinge
    // bone, swung SwingDeg degrees about SwingAxis through SwingAt (mesh; right-handed), the turned bones with it. The
    // "magazine" is the round in the chamber (the game's pose): shown while a round or a spent case is in, else hidden;
    // opening on a spent case throws it out (it falls). HoldOpen=0|1: per gun, over [ManualReload] HoldOpen.
    std::string              boltSwing;
    Vec3                     swingAxis, swingAt;
    float                    swingDeg = 0.0f;
    int                      holdOpen = -1;
};
std::vector<GunLine> g_lines;
std::string          g_sndTake;  // [ManualReload] SndTake: a magazine from the pouch

std::string Token(const std::string& line, const char* name) {
    const std::string k = std::string(name) + "=";
    size_t p = 0;
    while ((p = line.find(k, p)) != std::string::npos) {
        if (p == 0 || line[p - 1] == ' ') {
            const size_t e = line.find(' ', p);
            return line.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
        }
        p += k.size();
    }
    return {};
}

std::vector<std::string> Split(const std::string& v, char sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= v.size()) {
        const size_t e = v.find(sep, p);
        out.push_back(v.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return out;
}
Vec3 ParseVec(const std::string& v) {
    Vec3 r;
    sscanf_s(v.c_str(), "%f,%f,%f", &r.x, &r.y, &r.z);
    return r;
}
std::vector<Vec3> ParseVecs(const std::string& v) {
    std::vector<Vec3> out;
    if (v.empty()) return out;
    for (const std::string& t : Split(v, ';')) out.push_back(ParseVec(t));
    return out;
}
std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += static_cast<char>(*w);  // the ini's keys and values are ASCII
    return s;
}

void ParseLines(const std::wstring& ini) {
    // GOAL rule 3: [ManualReload] Off = the guns (attachment keys, comma-separated) that keep the game's own reload.
    wchar_t offW[1024] = L"";
    GetPrivateProfileStringW(L"ManualReload", L"Off", L"", offW, 1024, ini.c_str());
    const std::vector<std::string> off = Split(Narrow(offW), ',');
    std::vector<wchar_t> keys(8192);
    const DWORD n = GetPrivateProfileStringW(L"ManualReload", nullptr, L"", keys.data(), static_cast<DWORD>(keys.size()), ini.c_str());
    std::vector<std::pair<std::string, std::string>> refs;
    for (const wchar_t* k = keys.data(); k < keys.data() + n && *k; k += wcslen(k) + 1) {
        const std::string key = Narrow(k);
        if (key.rfind("Attachment_", 0) != 0) continue;
        const std::string base = key.substr(0, key.find_first_of("@."));  // (GOAL A4: Key@N, a per-level line)
        if (std::find(off.begin(), off.end(), key) != off.end() || std::find(off.begin(), off.end(), base) != off.end()) {
            MLOG("reload: %s is off ([ManualReload] Off) -- the game's own reload", key.c_str());
            continue;
        }
        wchar_t v[1024] = L"";
        GetPrivateProfileStringW(L"ManualReload", k, L"", v, 1024, ini.c_str());
        const std::string line = Narrow(v);
        const size_t dot = key.find('.');
        if (dot != std::string::npos) {
            if (key.substr(dot) == ".Ref") refs.push_back({key.substr(0, dot), line});
            continue;
        }
        GunLine g;
        g.key = key.substr(0, key.find('@'));
        g.gripKey = key;
        g.open = Token(line, "Action") == "open";
        const std::string mu = Token(line, "MinUpgrade"), xu = Token(line, "MaxUpgrade");
        g.minUpgrade = mu.empty() ? -1 : atoi(mu.c_str());
        g.maxUpgrade = xu.empty() ? 99 : atoi(xu.c_str());
        g.emptyCue = Token(line, "Empty") != "none";
        for (const std::string& b : Split(Token(line, "Mag"), ';'))
            if (!b.empty()) g.mag.push_back(b);
        g.magOut = ParseVecs(Token(line, "MagOut"));
        g.magGrab = ParseVecs(Token(line, "MagGrab"));
        for (const std::string& r : Split(Token(line, "MagR"), ';'))
            if (!r.empty()) g.magR.push_back(static_cast<float>(atof(r.c_str())));
        for (const std::string& b : Split(Token(line, "Bolt"), ','))
            if (!b.empty()) g.bolt.push_back(b);
        sscanf_s(Token(line, "BoltZ").c_str(), "%f,%f,%f", &g.boltZ[0], &g.boltZ[1], &g.boltZ[2]);
        g.haveBolt2Z = sscanf_s(Token(line, "Bolt2Z").c_str(), "%f,%f,%f", &g.bolt2Z[0], &g.bolt2Z[1], &g.bolt2Z[2]) == 3;
        g.boltGrab = ParseVec(Token(line, "BoltGrab"));
        g.topRound = Token(line, "TopRound");
        g.sndOut = Split(Token(line, "SndOut"), ';');
        g.sndIn = Split(Token(line, "SndIn"), ';');
        g.sndRack = Split(Token(line, "SndRack"), ';');
        g.taped = Token(line, "Taped");
        g.tapedA = ParseVec(Token(line, "TapedA"));
        g.tapedB = ParseVec(Token(line, "TapedB"));
        const std::string tr = Token(line, "TapedRot");
        g.tapedRot = tr.empty() ? 0.0f : static_cast<float>(atof(tr.c_str()));
        g.grabTrigger = Token(line, "GrabTrigger") == "1";
        g.triggerRack = Token(line, "TriggerRack") == "1";
        g.ejectOnEmpty = Token(line, "EjectOnEmpty") == "1";
        g.feedOnInsert = Token(line, "Feed") == "insert";
        g.noGrab = Token(line, "NoGrab") == "1";
        g.latch = Token(line, "Latch") != "0";
        g.sndClose = Split(Token(line, "SndClose"), ';');
        const std::string kc = Token(line, "KeepChambered");
        g.keepChambered = kc.empty() ? -1 : atoi(kc.c_str());
        const std::string es = Token(line, "EjectSpeed");
        if (!es.empty()) g.ejectSpeed = static_cast<float>(atof(es.c_str()));
        g.slideInsert = Token(line, "Insert") == "slide";
        g.boltAction = Token(line, "Action") == "bolt";
        for (const std::string& b : Split(Token(line, "BoltTurn"), ','))
            if (!b.empty()) g.boltTurn.push_back(b);
        g.boltSlide = Token(line, "BoltSlide");
        g.boltLift = static_cast<float>(atof(Token(line, "BoltLift").c_str()));
        g.boltTravel = static_cast<float>(atof(Token(line, "BoltTravel").c_str()));
        g.knob = ParseVec(Token(line, "Knob"));
        const std::string tra = Token(line, "TopRoundAt"), ci = Token(line, "ClipIn");
        g.haveTopRoundAt = !tra.empty();
        if (g.haveTopRoundAt) g.topRoundAt = ParseVec(tra);
        g.haveClipIn = !ci.empty();
        if (g.haveClipIn) g.clipIn = ParseVec(ci);
        for (const std::string& t : Split(Token(line, "MagRest"), ';')) {
            if (t.empty()) continue;
            std::vector<float> f;
            for (const std::string& x : Split(t, ',')) f.push_back(static_cast<float>(atof(x.c_str())));
            if (f.size() == 12) {  // a frame: the rows, then the point
                g.magRest.push_back({f[9], f[10], f[11]});
                g.magRestRot.push_back(std::vector<float>(f.begin(), f.begin() + 9));
            } else {
                g.magRest.push_back(ParseVec(t));
                g.magRestRot.push_back({});
            }
        }
        g.pump = Token(line, "Action") == "pump";
        g.sndIn2 = Split(Token(line, "SndIn2"), ';');
        g.boltSwing = Token(line, "BoltSwing");
        g.swingAxis = ParseVec(Token(line, "SwingAxis"));
        g.swingAt = ParseVec(Token(line, "SwingAt"));
        g.swingDeg = static_cast<float>(atof(Token(line, "SwingDeg").c_str()));
        const std::string ho = Token(line, "HoldOpen");
        g.holdOpen = ho.empty() ? -1 : atoi(ho.c_str());
        g.sndUp = Split(Token(line, "SndUp"), ';');
        g.sndBack = Split(Token(line, "SndBack"), ';');
        g.sndFwd = Split(Token(line, "SndFwd"), ';');
        g.sndDown = Split(Token(line, "SndDown"), ';');
        g.sndRound = Split(Token(line, "SndRound"), ';');
        g.sndClip = Split(Token(line, "SndClip"), ';');
        g.sndCase = Split(Token(line, "SndCase"), ';');
        g.magLen = static_cast<float>(atof(Token(line, "MagLen").c_str()));
        g.magSeat = static_cast<float>(atof(Token(line, "MagSeat").c_str()));
        g_lines.push_back(g);
    }
    wchar_t take[128] = L"";
    GetPrivateProfileStringW(L"ManualReload", L"SndTake", L"", take, 128, ini.c_str());
    g_sndTake = Narrow(take);
    for (const auto& r : refs)
        for (GunLine& g : g_lines) {
            if (g.key != r.first) continue;
            for (const std::string& t : Split(r.second, ' ')) {
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                const std::string name = t.substr(0, eq), val = t.substr(eq + 1);
                if (name == "Bones") g.refBones = atoi(val.c_str());
                else g.ref.push_back({name, ParseVec(val)});
            }
        }
}

// --- the game-side model (RELOAD-DESIGN 2.1) ---
struct WState {
    std::uintptr_t w = 0;
    int            index = -1;       // UObject Index: a new weapon at a freed one's address is a new state
    bool           magIn = true, pending = false, cocked = true;
    int            lastClip = -1;    // the clip at the end of the last Draw
    int            heldRounds = 0;   // the magazine in the hand (visual only)
    int            pendingRounds = 0;  // what a rack feeds (a taped half's own count)
    // Twin magazines: the half in the gun (0 = taped state A, 1 = B; -1 = the game's TapedMagMode), and each half's rounds
    // while out of the gun (-1 = in the gun, or full).
    int            half = -1;
    int            halfCount[2] = {-1, -1};
    bool           gameEjected = false;  // EjectOnEmpty: the game threw the clip out (its own projectile; ours doesn't fall)
    bool           emptyPending = false; // EjectOnEmpty: the last shot is out; the clip goes when the firing state ends
    // GOAL A2 (bolt actions): the bolt 0 closed, 1 lifted, 2 drawn back (open), 3 pushed forward (not yet turned down).
    int            bolt = 0;
    bool           spent = false;        // a fired case in the chamber: the trigger is held until the bolt is worked
    bool           clipSeated = false;   // a stripped clip in the guides until the bolt closes
    bool           gated = false;        // the trigger held (FiringStatesArray[0] = None)
    bool           chamberEmpty = false; // GOAL A3 (a pump gun): nothing in the chamber (the case out, not yet pumped closed on
                                         // a shell; or an empty gun loaded) -- the clip's rounds are all in the tube
    bool           caseFall = false;     // GOAL A5: the breech opened on a spent case -- the bake throws it out (it falls)
    std::uint32_t  rechamber[2] = {}, fire0[2] = {};  // the game's FNames set to None (to put back)
    bool           rechamberOff = false, fireOff = false;
};
std::vector<WState> g_ws;
struct Owed {
    std::uintptr_t cls;
    int            n;
};
std::vector<Owed> g_owed;           // per pawn: rounds an EJECT could not put back (the reserve at its cap)
std::uintptr_t   g_pawn = 0;
DWORD            g_lastDraw = 0, g_lastGunBake = 0;
std::uint32_t    g_flags = 0;       // hdr->reloadFlags as read this Draw
std::uint32_t    g_seen = 0;
bool             g_seenInit = false;
std::uint32_t    g_pawnSeq = 0;
std::string      g_lastState;       // Debug.ReloadTrace: the weapon's state, logged on change

int ObjIndex(std::uintptr_t o) { return o ? *reinterpret_cast<const int*>(o + 4) : -1; }

std::uintptr_t PawnWeapon(std::uintptr_t pawn) {
    const int wo = pawn ? names::PropertyOffset(pawn, "Weapon") : -1;
    return wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
}

// The weapon's fields (reflection; the static offsets are in ENGINE-NOTES 5am).
int* Field(std::uintptr_t w, const char* n) {
    const int o = names::PropertyOffset(w, n);
    return o >= 0 ? reinterpret_cast<int*>(w + o) : nullptr;
}
bool Bit(std::uintptr_t w, const char* n) {
    int o = -1;
    std::uint32_t m = 0;
    return names::BoolProperty(w, n, o, m) && (*reinterpret_cast<const std::uint32_t*>(w + o) & m) != 0;
}
std::string AttachKey(std::uintptr_t w) {  // the class object's own name (e.g. Attachment_Stg44), RELOAD-DESIGN X4
    const int o = names::PropertyOffset(w, "AttachmentClass");
    const std::uintptr_t cls = o >= 0 ? names::ReadPointer(w + o) : 0;
    return cls ? names::Name(cls) : std::string();
}
std::uintptr_t AmmoClassOf(std::uintptr_t w) {
    const int o = names::PropertyOffset(w, "AmmoClass");
    return o >= 0 ? names::ReadPointer(w + o) : 0;
}

std::vector<std::string> g_failedKeys;  // guns whose RefSkeleton check failed: the game's own reload

// The converted line for this weapon now, or null (no line, below MinUpgrade, alt mode, the check failed).
const GunLine* Converted(std::uintptr_t w) {
    const std::string key = AttachKey(w);
    const int* up = Field(w, "CurrentUpgradeLevel");
    // The line whose upgrade levels hold the gun's (GOAL A4: a gun may have one per range, Key@N).
    const GunLine* l = nullptr;
    for (const GunLine& g : g_lines)
        if (!l && g.key == key && (!up || (*up >= g.minUpgrade && *up <= g.maxUpgrade))) l = &g;
    if (!l) return nullptr;
    for (const std::string& k : g_failedKeys)
        if (k == l->key) return nullptr;
    if (Bit(w, "bAlternateFireMode")) return nullptr;
    return l;
}

WState& StateFor(std::uintptr_t w) {
    const int idx = ObjIndex(w);
    for (WState& s : g_ws)
        if (s.w == w && s.index == idx) return s;
    if (g_ws.size() >= 16) g_ws.erase(g_ws.begin());
    WState s;
    s.w = w;
    s.index = idx;
    g_ws.push_back(s);
    return g_ws.back();
}

int& OwedFor(std::uintptr_t cls) {
    for (Owed& o : g_owed)
        if (o.cls == cls) return o.n;
    g_owed.push_back({cls, 0});
    return g_owed.back().n;
}

// The reserve entry for the weapon's ammo class (the exact match, as AddReserveAmmo writes; M0: the native's subclass
// match picks the same entry); null if none.
int* ReserveOf(std::uintptr_t pawn, std::uintptr_t cls, int& cap) {
    const int io = names::PropertyOffset(pawn, "InvManager");
    const std::uintptr_t inv = io >= 0 ? names::ReadPointer(pawn + io) : 0;
    if (!inv || !cls) return nullptr;
    const int so = names::PropertyOffset(inv, "AmmoStorage"), no = names::PropertyOffset(inv, "NumAmmoClasses");
    if (so < 0 || no < 0) return nullptr;
    const int n = *reinterpret_cast<const int*>(inv + no);
    for (int i = 0; i < n && i < 10; ++i) {
        const std::uintptr_t e = inv + so + 12 * i;
        if (names::ReadPointer(e) != cls) continue;
        cap = *reinterpret_cast<const int*>(e + 8);
        return reinterpret_cast<int*>(e + 4);
    }
    return nullptr;
}

// RELOAD-DESIGN 2.2: no round is ever lost (the overflow at the cap is carried as `owed`).
void ToReserve(std::uintptr_t pawn, std::uintptr_t w, int k) {
    if (k <= 0 || Bit(w, "bInfiniteAmmo")) return;
    const std::uintptr_t cls = AmmoClassOf(w);
    int cap = 0;
    int* r = ReserveOf(pawn, cls, cap);
    int put = 0;
    if (r) {
        put = std::min(k, std::max(cap - *r, 0));
        *r += put;
    }
    OwedFor(cls) += k - put;
}
int FromReserve(std::uintptr_t pawn, std::uintptr_t w, int n) {
    if (n <= 0) return 0;
    if (Bit(w, "bInfiniteAmmo")) return n;
    const std::uintptr_t cls = AmmoClassOf(w);
    int& o = OwedFor(cls);
    const int a = std::min(n, o);
    o -= a;
    int cap = 0;
    int* r = ReserveOf(pawn, cls, cap);
    const int b = r ? std::min(n - a, *r) : 0;
    if (r) *r -= b;
    return a + b;
}
int ReserveAvailable(std::uintptr_t pawn, std::uintptr_t w) {
    if (Bit(w, "bInfiniteAmmo")) return 9999;
    const std::uintptr_t cls = AmmoClassOf(w);
    int cap = 0;
    const int* r = ReserveOf(pawn, cls, cap);
    return (r ? *r : 0) + OwedFor(cls);
}

bool Ready(const WState& s, const GunLine& l, int clip) { return l.open ? s.cocked : clip >= 1; }
// GOAL A2 / A5: an emptied bolt stays open (per gun: HoldOpen=0|1 over [ManualReload] HoldOpen; the M18's breech has no
// follower).
bool HoldOpenFor(const GunLine& l) { return l.holdOpen >= 0 ? l.holdOpen != 0 : g_cfg.holdOpen; }

// --- M7, the reload sounds: the arms' own cues (the AnimNotify_Sounds of the reload animations the manual reload no
// longer plays), played at the events through the weapon's script function WeaponPlaySound (-> Instigator.PlaySound). ---
struct Cue {
    std::string    name;  // "group.name", lower case
    std::uintptr_t cue;
};
std::vector<Cue> g_cues;
std::uintptr_t   g_cuesPawn = 0;
bool             g_soundsOff = false;  // a fault in ProcessEvent: no more sounds this session
struct KeyVariant {
    std::string key;
    int         v;
    bool        taped;  // the taped pair shows (twin magazines)
};
std::vector<KeyVariant> g_variant;     // each gun's visible magazine variant (from the bake)

std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return s;
}
int VariantOf(const std::string& key) {
    for (const KeyVariant& k : g_variant)
        if (k.key == key) return k.v;
    return 0;
}
bool TapedNow(const std::string& key) {
    for (const KeyVariant& k : g_variant)
        if (k.key == key) return k.taped;
    return false;
}
int* TapedModeField(std::uintptr_t w) {
    const int o = names::PropertyOffset(w, "TapedMagMode");
    return o >= 0 ? reinterpret_cast<int*>(w + o) : nullptr;  // a byte enum: read and write the low byte
}
int TapedMode(std::uintptr_t w) {
    const int* f = TapedModeField(w);
    return f ? (*reinterpret_cast<const std::uint8_t*>(f) & 1) : 0;
}

// Every SoundCue the arms' AnimSets name in an AnimNotify_Sound (FPArms.AnimSets[].Sequences[].Notifies[].Notify).
void BuildCues(std::uintptr_t pawn) {
    g_cues.clear();
    g_cuesPawn = pawn;
    const int ao = names::PropertyOffset(pawn, "FPArms");
    const std::uintptr_t arms = ao >= 0 ? names::ReadPointer(pawn + ao) : 0;
    const int so = arms ? names::PropertyOffset(arms, "AnimSets") : -1;
    if (so < 0) {
        MLOG("reload: sounds -- no FPArms.AnimSets on the pawn: silent");
        return;
    }
    const std::uintptr_t sets = names::ReadPointer(arms + so);
    const int nSets = static_cast<int>(names::ReadPointer(arms + so + 4));
    int nSeq = 0, nNotify = 0;
    for (int i = 0; i < nSets && i < 64; ++i) {
        const std::uintptr_t set = names::ReadPointer(sets + 4 * i);
        const int qo = set ? names::PropertyOffset(set, "Sequences") : -1;
        if (qo < 0) continue;
        const std::uintptr_t seqs = names::ReadPointer(set + qo);
        const int n = static_cast<int>(names::ReadPointer(set + qo + 4));
        for (int k = 0; k < n && k < 4096; ++k) {
            const std::uintptr_t seq = names::ReadPointer(seqs + 4 * k);
            const int no = seq ? names::PropertyOffset(seq, "Notifies") : -1;
            if (no < 0) continue;
            ++nSeq;
            const std::uintptr_t evs = names::ReadPointer(seq + no);
            const int ne = static_cast<int>(names::ReadPointer(seq + no + 4));
            for (int e = 0; e < ne && e < 256; ++e) {
                // AnimNotifyEvent { float Time; AnimNotify* Notify; FName Comment; } = 16 bytes
                const std::uintptr_t notify = names::ReadPointer(evs + 16 * e + 4);
                if (!notify || names::ClassName(notify) != "AnimNotify_Sound") continue;
                ++nNotify;
                const int co = names::PropertyOffset(notify, "SoundCue");
                const std::uintptr_t cue = co >= 0 ? names::ReadPointer(notify + co) : 0;
                if (!cue) continue;
                const std::string name = Lower(names::Name(names::Outer(cue)) + "." + names::Name(cue));
                bool have = false;
                for (const Cue& q : g_cues) have = have || q.cue == cue;
                if (!have) g_cues.push_back({name, cue});
            }
        }
    }
    MLOG("reload: sounds -- %zu cues from %d sound notifies in %d sequences of %d arm animsets", g_cues.size(), nNotify, nSeq, nSets);
    for (const GunLine& l : g_lines)
        for (const auto* list : {&l.sndOut, &l.sndIn, &l.sndRack, &l.sndIn2, &l.sndUp, &l.sndBack, &l.sndFwd, &l.sndDown,
                                 &l.sndRound, &l.sndClip})
            for (const std::string& want : *list) {
                bool found = want.empty();
                for (const Cue& q : g_cues) found = found || q.name == Lower(want);
                if (!found) MLOG("reload: sounds -- %s's cue %s is not among them (silent)", l.key.c_str(), want.c_str());
            }
}

// No C++ objects here: SEH only.
bool CallProcessEvent(std::uintptr_t pe, std::uintptr_t obj, std::uintptr_t fn, void* parms) {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*)>(pe)(obj, nullptr, fn, parms, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Debug.ReloadTrace: the audio component the last cue made, looked for among WorldInfo's components for a second.
struct CueCheck {
    std::uintptr_t cue = 0, world = 0;
    DWORD          until = 0;
    std::string    name;
} g_cueCheck;

void CheckCue() {
    if (!g_cueCheck.cue) return;
    const std::uintptr_t wi = g_cueCheck.world;
    const int co = wi ? names::PropertyOffset(wi, "Components") : -1;
    const std::uintptr_t arr = co >= 0 ? names::ReadPointer(wi + co) : 0;
    const int n = co >= 0 ? static_cast<int>(names::ReadPointer(wi + co + 4)) : 0;
    for (int i = 0; i < n && i < 4096; ++i) {
        const std::uintptr_t ac = names::ReadPointer(arr + 4 * i);
        if (!ac || names::ClassName(ac) != "AudioComponent") continue;
        const int so = names::PropertyOffset(ac, "SoundCue"), wo = names::PropertyOffset(ac, "WaveInstances"),
                  po = names::PropertyOffset(ac, "PlaybackTime");
        if (so < 0 || names::ReadPointer(ac + so) != g_cueCheck.cue) continue;
        const int waves = wo >= 0 ? static_cast<int>(names::ReadPointer(ac + wo + 4)) : -1;
        float t = 0.0f;
        if (po >= 0) std::memcpy(&t, reinterpret_cast<const void*>(ac + po), sizeof(t));
        if (waves > 0 || t > 0.0f) {
            MLOG("reload: sound %s is playing (an AudioComponent of WorldInfo: %d wave instance(s), %.2f s in)",
                 g_cueCheck.name.c_str(), waves, t);
            g_cueCheck.cue = 0;
            return;
        }
    }
    if (static_cast<LONG>(GetTickCount() - g_cueCheck.until) >= 0) {
        MLOG("reload: sound %s -- no playing AudioComponent found within a second", g_cueCheck.name.c_str());
        g_cueCheck.cue = 0;
    }
}

// The cue at the gun: the weapon's script function PlaySoundAt(Sound, Location) (Actor.uc: WorldInfo.CreateAudioComponent
// at that spot, auto-destroyed, Play) through AActor::ProcessEvent.
void PlayCue(std::uintptr_t pawn, std::uintptr_t w, const std::string& want, const char* what) {
    if (!g_cfg.reloadSounds || g_soundsOff || want.empty() || !pawn || !w) return;
    if (pawn != g_cuesPawn) BuildCues(pawn);
    std::uintptr_t cue = 0;
    const std::string lw = Lower(want);
    for (const Cue& q : g_cues)
        if (q.name == lw) cue = q.cue;
    if (!cue) return;
    const std::uintptr_t cls = names::ReadPointer(w + addr::kObjectClass);
    const std::uintptr_t fn = cls ? names::FindFieldProbe(cls, "PlaySoundAt") : 0;
    const std::uintptr_t pSound = fn ? names::FindFieldProbe(fn, "ASound") : 0;
    const std::uintptr_t pLoc = fn ? names::FindFieldProbe(fn, "SourceLocation") : 0;
    const std::uintptr_t vt = names::ReadPointer(w);
    const std::uintptr_t pe = vt ? names::ReadPointer(vt + addr::kVtProcessEvent) : 0;
    if (!fn || names::ClassName(fn) != "Function" || !pSound || !pLoc || pe != addr::kActorProcessEvent) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            MLOG("reload: sounds -- no PlaySoundAt to call (function %s, ProcessEvent 0x%08X): silent",
                 fn ? names::ClassName(fn).c_str() : "none", static_cast<unsigned>(pe));
        }
        return;
    }
    const int oSound = static_cast<int>(names::ReadPointer(pSound + addr::kPropertyOffset));
    const int oLoc = static_cast<int>(names::ReadPointer(pLoc + addr::kPropertyOffset));
    alignas(16) std::uint8_t parms[64] = {};
    if (oSound < 0 || oSound > 60 || oLoc < 0 || oLoc > 52) return;
    // Where the gun is drawn (the aim line's start), else the pawn.
    float at[3] = {0, 0, 0}, dir[3], upm = 100.0f;
    if (!viewmodel::GunRay(at, dir, upm)) {
        const int lo = names::PropertyOffset(pawn, "Location");
        if (lo >= 0) names::ReadVector(pawn + lo, at);
    }
    std::memcpy(parms + oSound, &cue, sizeof(cue));
    std::memcpy(parms + oLoc, at, sizeof(at));
    if (!CallProcessEvent(pe, w, fn, parms)) {
        g_soundsOff = true;
        MLOG("reload: sounds -- PlaySoundAt faulted: no more reload sounds this session");
        return;
    }
    if (g_cfg.debugReloadTrace) {
        MLOG("reload: sound %s (%s) at %.0f %.0f %.0f", want.c_str(), what, at[0], at[1], at[2]);
        const int wo = names::PropertyOffset(w, "WorldInfo");
        g_cueCheck = {cue, wo >= 0 ? names::ReadPointer(w + wo) : 0, GetTickCount() + 1000, want};
    }
}

const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP", "INSERT (the other half)", "BOLT UP", "BOLT BACK",
                            "BOLT FORWARD", "BOLT DOWN"};
constexpr std::uint32_t kEventCount = 11;
const char* kBoltName[] = {"closed", "lifted", "open", "forward"};

// GOAL A2: a fired case leaves as the bolt comes back -- the attachment's EjectRechamberedShell (a final call to
// SmallArmsAttachment.EjectShell: the case particles, which the mod already moves to the drawn gun), through ProcessEvent.
void EjectCase(std::uintptr_t pawn) {
    const int ao = pawn ? names::PropertyOffset(pawn, "CurrentWeaponAttachment") : -1;
    const std::uintptr_t att = ao >= 0 ? names::ReadPointer(pawn + ao) : 0;
    if (!att) return;
    const std::uintptr_t cls = names::ReadPointer(att + addr::kObjectClass);
    const std::uintptr_t fn = cls ? names::FindFieldProbe(cls, "EjectRechamberedShell") : 0;
    const std::uintptr_t vt = names::ReadPointer(att);
    const std::uintptr_t pe = vt ? names::ReadPointer(vt + addr::kVtProcessEvent) : 0;
    if (!fn || names::ClassName(fn) != "Function" || pe != addr::kActorProcessEvent) {
        MLOG("reload: no EjectRechamberedShell to call on %s -- the case stays", names::Name(att).c_str());
        return;
    }
    alignas(16) std::uint8_t parms[16] = {};
    const bool ok = CallProcessEvent(pe, att, fn, parms);
    if (g_cfg.debugReloadTrace || !ok)
        MLOG("reload: %s.EjectRechamberedShell %s", names::Name(att).c_str(), ok ? "called (the case)" : "FAULTED");
}

// A cue played a moment later (GOAL A1: the Garand's op-rod slams home ~0.35 s after the clip goes in).
struct DelayedCue {
    bool           on = false;
    std::uintptr_t w = 0;
    std::string    name;
    const char*    what = "";
    DWORD          due = 0;
};
DelayedCue g_delayedCue;

void Apply(std::uintptr_t pawn, std::uintptr_t w, WState& s, const GunLine& l, std::uint32_t type) {
    int* clipP = Field(w, "AmmoCount");
    const int* maxP = Field(w, "MaxAmmoCount");
    if (!clipP || !maxP) return;
    const int c0 = *clipP, m = maxP[0];
    const int r0 = ReserveAvailable(pawn, w);
    const bool wasIn = s.magIn;
    bool closes = false;  // Feed=insert: the action closed on its own
    const char* refused = nullptr;  // (A2) the event didn't apply, and why
    bool caseOut = false, clipOut = false, loadedClip = false, fed = false;
    const bool taped = TapedNow(l.key);
    if (taped && s.half < 0) s.half = TapedMode(w);
    int& c = *clipP;
    switch (type) {
    case shared::kReloadEject: {
        if (!s.magIn) break;
        const bool keepOne = l.keepChambered >= 0 ? l.keepChambered != 0 : g_cfg.keepChambered;
        const int keep = (!l.open && keepOne) ? std::min(c, 1) : 0;
        const int k = c;
        c = keep;
        ToReserve(pawn, w, k - keep);
        s.magIn = false;
        s.pending = false;
        s.heldRounds = k - keep;
        if (taped) {  // the half that was in keeps its rounds; the other one is full unless it was used
            s.halfCount[s.half] = k - keep;
            if (s.halfCount[1 - s.half] < 0) s.halfCount[1 - s.half] = m;
        }
        break;
    }
    case shared::kReloadTake:
        if (l.pump) {  // GOAL A3: a shell, while the tube has room
            if (c >= m) refused = "the tube is full";
            else s.heldRounds = 1;
            break;
        }
        if (l.boltAction) {  // GOAL A2: a clip or a round, into the open action with room
            if (s.bolt != 2 || c >= m) refused = s.bolt != 2 ? "the bolt is not open" : "the magazine is full";
            else s.heldRounds = 1;
            break;
        }
        if (s.magIn) break;
        s.heldRounds = Bit(w, "bInfiniteAmmo") ? m : std::min(m, ReserveAvailable(pawn, w));
        if (taped) s.halfCount[0] = s.halfCount[1] = m;  // a new pair
        break;
    case shared::kReloadInsert:
    case shared::kReloadInsertOther: {
        if (l.pump) {  // GOAL A3: a shell through the loading port into the tube; an empty gun's chamber stays empty
            if (c >= m) {
                refused = "the tube is full";
                break;
            }
            if (c == 0 && !s.spent) s.chamberEmpty = true;
            c += FromReserve(pawn, w, 1);
            s.heldRounds = 0;
            break;
        }
        if (l.boltAction) {  // GOAL A2: through the open action -- a stripper clip strips its rounds, a round goes in
            if (s.bolt != 2 || c >= m) {
                refused = s.bolt != 2 ? "the bolt is not open" : "the magazine is full";
                break;
            }
            loadedClip = l.haveClipIn && VariantOf(l.key) == 0;
            c += FromReserve(pawn, w, loadedClip ? std::min(5, m - c) : 1);
            if (loadedClip) s.clipSeated = true;
            s.heldRounds = 0;
            break;
        }
        if (s.magIn) break;
        s.magIn = true;
        int rounds = m;
        if (taped) {
            const int h = type == shared::kReloadInsertOther ? 1 - s.half : s.half;
            if (s.halfCount[h] >= 0) rounds = s.halfCount[h];
            s.halfCount[h] = -1;
            s.half = h;
            if (int* f = TapedModeField(w)) *reinterpret_cast<std::uint8_t*>(f) = static_cast<std::uint8_t>(h);
        }
        // GOAL A1 (Feed=insert, the Garand): the clip loads an empty gun at once and the action closes on its own.
        if (Ready(s, l, c) || l.feedOnInsert) {
            c += FromReserve(pawn, w, std::min(rounds, std::max(m - c, 0)));
            s.pending = false;
            closes = l.feedOnInsert && c0 == 0 && c > 0;
        } else {
            s.pending = rounds > 0 && (Bit(w, "bInfiniteAmmo") || ReserveAvailable(pawn, w) > 0);
            s.pendingRounds = rounds;
        }
        s.heldRounds = 0;
        s.gameEjected = false;
        break;
    }
    case shared::kReloadRack:
        if (!l.open) {
            if (c == 0 && s.magIn && s.pending) {
                c = FromReserve(pawn, w, std::min(s.pendingRounds, m));
                s.pending = false;
            }
        } else if (!s.cocked) {
            s.cocked = true;
            if (s.magIn && s.pending) {
                c = FromReserve(pawn, w, std::min(s.pendingRounds, m));
                s.pending = false;
            }
        }
        break;
    case shared::kReloadDrop:
        s.heldRounds = 0;
        break;
    case shared::kReloadBoltUp:
        if (!l.boltAction || s.bolt != 0) refused = "the bolt is not closed";
        else s.bolt = 1;
        break;
    case shared::kReloadBoltBack:
        if (l.pump) {  // GOAL A3: the pump back -- a spent case comes out; a live shell stays (a press check loses nothing)
            if (s.bolt != 0) {
                refused = "the pump is not forward";
            } else {
                s.bolt = 2;
                if (s.spent) {
                    EjectCase(pawn);
                    caseOut = true;
                    s.spent = false;
                    s.chamberEmpty = true;
                }
            }
            break;
        }
        if (!l.boltAction || (s.bolt != 1 && s.bolt != 3)) {
            refused = "the bolt is not lifted";
        } else {
            s.bolt = 2;
            if (s.spent) {  // the case comes out with the extractor (GOAL A5: the M18's is thrown out of the breech)
                if (l.boltSwing.empty()) EjectCase(pawn);
                else s.caseFall = true;
                caseOut = true;
                s.spent = false;
            }
        }
        break;
    case shared::kReloadBoltForward:
        if (l.pump) {  // GOAL A3: the pump forward -- a shell from the tube into an empty chamber
            if (s.bolt != 2) {
                refused = "the pump is not back";
            } else {
                s.bolt = 0;
                if (s.chamberEmpty && c >= 1) {
                    s.chamberEmpty = false;
                    fed = true;
                }
            }
            break;
        }
        if (!l.boltAction || s.bolt != 2) {
            refused = "the bolt is not back";
        } else if (HoldOpenFor(l) && c == 0) {
            refused = "held open: the magazine is empty";
        } else {
            s.bolt = 3;
            if (s.clipSeated) {  // the bolt pushes the empty clip out of the guides
                s.clipSeated = false;
                clipOut = true;
            }
        }
        break;
    case shared::kReloadBoltDown:
        if (!l.boltAction || (s.bolt != 1 && s.bolt != 3)) refused = "the bolt is not forward";
        else s.bolt = 0;
        break;
    default:
        break;
    }
    // M7: the gun's own cue (per visible magazine variant).
    const int v = VariantOf(l.key);
    auto pick = [&](const std::vector<std::string>& list) { return list.empty() ? std::string() : list[v < static_cast<int>(list.size()) ? v : 0]; };
    if (type == shared::kReloadEject && wasIn) PlayCue(pawn, w, pick(l.sndOut), "magazine out");
    else if ((type == shared::kReloadInsert || type == shared::kReloadInsertOther) && !wasIn) PlayCue(pawn, w, pick(l.sndIn), "magazine in");
    else if (type == shared::kReloadRack && !(l.ejectOnEmpty && !l.open && c == 0)) PlayCue(pawn, w, pick(l.sndRack), "rack");
    else if (type == shared::kReloadTake && !wasIn) PlayCue(pawn, w, g_sndTake, "from the pouch");
    if (closes && !pick(l.sndClose).empty()) g_delayedCue = {true, w, pick(l.sndClose), "the action closes", GetTickCount() + 350};
    if (l.boltAction && !refused) {  // GOAL A2: the bolt's own cues (the blocked rechamber / reload animations')
        if (type == shared::kReloadBoltUp) PlayCue(pawn, w, pick(l.sndUp), "bolt up");
        else if (type == shared::kReloadBoltBack) PlayCue(pawn, w, pick(l.sndBack), "bolt back");
        else if (type == shared::kReloadBoltForward) PlayCue(pawn, w, pick(l.sndFwd), "bolt forward");
        else if (type == shared::kReloadBoltDown) PlayCue(pawn, w, pick(l.sndDown), "bolt down");
        else if (type == shared::kReloadInsert) PlayCue(pawn, w, pick(loadedClip ? l.sndClip : l.sndRound), loadedClip ? "the clip in" : "a round in");
        if (caseOut) g_delayedCue = {true, w, pick(l.sndCase), "the case", GetTickCount() + 120};
    }
    if (l.pump && !refused) {  // GOAL A3: the pump's and the shell's cues (the blocked rechamber / looping reload's)
        if (type == shared::kReloadBoltBack) PlayCue(pawn, w, pick(l.sndBack), "pump back");
        else if (type == shared::kReloadBoltForward) PlayCue(pawn, w, pick(l.sndFwd), "pump forward");
        else if (type == shared::kReloadTake) PlayCue(pawn, w, g_sndTake, "from the pouch");
        else if (type == shared::kReloadInsert) {
            PlayCue(pawn, w, pick(l.sndIn), "a shell in");
            if (!pick(l.sndIn2).empty()) g_delayedCue = {true, w, pick(l.sndIn2), "the shell home", GetTickCount() + 60};
        }
    }
    if (l.pump) {
        const char* ev = type == shared::kReloadBoltBack ? "PUMP BACK" : type == shared::kReloadBoltForward ? "PUMP FORWARD" :
                         type < kEventCount ? kEventName[type] : "?";
        MLOG("reload: %s %s: clip %d -> %d, reserve %d -> %d (owed %d); pump %s, chamber %s%s%s%s%s", ev, l.key.c_str(), c0, c,
             r0, ReserveAvailable(pawn, w), OwedFor(AmmoClassOf(w)), s.bolt == 2 ? "back" : "forward",
             s.spent ? "spent" : (s.chamberEmpty || c == 0) ? "empty" : "live", caseOut ? "; the case ejected" : "",
             fed ? "; a shell chambered" : "", type == shared::kReloadInsert && !refused ? "; a shell into the tube" : "",
             refused ? (std::string("; REFUSED -- ") + refused).c_str() : "");
        return;
    }
    char twin[80] = "";
    if (taped)
        snprintf(twin, sizeof(twin), "; taped: half %c in the gun%s, half A %d, B %d", s.half ? 'B' : 'A', s.magIn ? "" : " (out)",
                 s.halfCount[0], s.halfCount[1]);
    if (l.boltAction) {
        MLOG("reload: %s %s: clip %d -> %d, reserve %d -> %d (owed %d); bolt %s%s%s%s%s%s%s", type < kEventCount ? kEventName[type] : "?",
             l.key.c_str(), c0, c, r0, ReserveAvailable(pawn, w), OwedFor(AmmoClassOf(w)), kBoltName[s.bolt & 3],
             s.spent ? ", a spent case in" : "", s.clipSeated ? ", a clip in the guides" : "", caseOut ? "; the case ejected" : "",
             clipOut ? "; the clip pushed out" : "", loadedClip ? "; a stripper clip" : (type == shared::kReloadInsert && !refused ? "; a round" : ""),
             refused ? (std::string("; REFUSED -- ") + refused).c_str() : "");
        return;
    }
    MLOG("reload: %s %s: clip %d -> %d, reserve %d -> %d (owed %d); magazine %s%s%s%s%s", type < kEventCount ? kEventName[type] : "?",
         l.key.c_str(), c0, c, r0, ReserveAvailable(pawn, w), OwedFor(AmmoClassOf(w)), s.magIn ? "in" : "out",
         s.pending ? ", pending (rack to feed)" : "", l.open ? (s.cocked ? ", cocked" : ", bolt forward") : "", twin,
         closes ? "; the action closed on its own" : "");
}

// RELOAD-DESIGN 2.5: whether the game's own reload is blocked for `self` right now.
bool Blocking(std::uintptr_t self) {
    if (!g_blockAllowed || !self) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn || self != PawnWeapon(pawn)) return false;  // pointer first: the folded GameInfo call never gets further
    if (!(g_flags & 1u) || !(g_flags & 16u)) return false;  // the player's toggle, and the host engaged
    const DWORD now = GetTickCount();
    if (now - g_lastDraw > 250 || now - g_lastGunBake > 250) return false;  // the pipeline alive
    if (!Converted(self)) return false;
    float gf[16], of[16];
    bool ov = false, th = false;
    return viewmodel::HandFrames(gf, of, ov, th);
}

// --- the bake (RELOAD-DESIGN 5): overrides of the gun's drawn bones; the game's pose is put back after the render copy ---
struct Resolved {
    std::uintptr_t   mesh = 0;
    int              num = 0;
    const GunLine*   line = nullptr;
    bool             ok = false;
    std::vector<int> mag, bolt;
    int              top = -1;
    int              taped = -1;  // the second magazine of a taped pair
    std::vector<int> boltTurn;    // GOAL A2: the turning bolt bones
    int              boltSlide = -1;
    int              boltParent = -1;  // GOAL A3: the action bone's parent (a pump moves along its Z row)
    int              boltSwing = -1;   // GOAL A5: the hinge bone of a breech that swings open
};
std::deque<Resolved> g_resolved;

float Det3(const float* m);

// Row vectors (FMatrix): out = a x b, 4x4 row-major.
void Mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(out, r, sizeof(r));
}
// The inverse of an affine row-vector matrix (a 3x3 with a translation row; the component may carry a scale).
void AffineInverse(const float* a, float* out) {
    const float det = a[0] * (a[5] * a[10] - a[6] * a[9]) - a[1] * (a[4] * a[10] - a[6] * a[8]) + a[2] * (a[4] * a[9] - a[5] * a[8]);
    const float k = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
    float r[16] = {};
    r[0] = (a[5] * a[10] - a[6] * a[9]) * k;
    r[1] = (a[2] * a[9] - a[1] * a[10]) * k;
    r[2] = (a[1] * a[6] - a[2] * a[5]) * k;
    r[4] = (a[6] * a[8] - a[4] * a[10]) * k;
    r[5] = (a[0] * a[10] - a[2] * a[8]) * k;
    r[6] = (a[2] * a[4] - a[0] * a[6]) * k;
    r[8] = (a[4] * a[9] - a[5] * a[8]) * k;
    r[9] = (a[1] * a[8] - a[0] * a[9]) * k;
    r[10] = (a[0] * a[5] - a[1] * a[4]) * k;
    for (int j = 0; j < 3; ++j) r[12 + j] = -(a[12] * r[j] + a[13] * r[4 + j] + a[14] * r[8 + j]);
    r[15] = 1.0f;
    std::memcpy(out, r, sizeof(r));
}
// A point (w = 1) or a direction (w = 0) through a row-vector matrix.
Vec3 Xform(const Vec3& v, float w, const float* m) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8] + w * m[12], v.x * m[1] + v.y * m[5] + v.z * m[9] + w * m[13],
            v.x * m[2] + v.y * m[6] + v.z * m[10] + w * m[14]};
}
float Len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 Norm(const Vec3& v) {
    const float l = Len(v);
    return l > 1e-6f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0, 0, 0};
}

// A bone's drawn matrix from a modified game pose: bones[j] = m x kMove.
void Draw(float* bones, int j, const float* m, const float* kMove) { Mul(m, kMove, bones + 16 * j); }
// Hidden like the game hides upgrade parts: the 3x3 zeroed, the origin kept (M0: ENGINE-NOTES 5am).
void Collapse(float* bones, int j, const float* saved, const float* kMove) {
    float m[16];
    std::memcpy(m, saved + 16 * j, sizeof(m));
    for (int i = 0; i < 12; ++i) m[i] = 0.0f;
    Draw(bones, j, m, kMove);
}

// Finds the line's bones in the mesh and runs the RefSkeleton check (RELOAD-DESIGN 1.3): the bone count, every listed
// bone present, a child of the root, and at its reference position (within 0.05 u; FMeshBone position at +28, M0).
const Resolved* Resolve(std::uintptr_t comp, std::uintptr_t mesh, int num, const GunLine& l) {
    for (const Resolved& r : g_resolved)
        if (r.mesh == mesh && r.num == num && r.line == &l) return &r;
    Resolved r;
    r.mesh = mesh;
    r.num = num;
    r.line = &l;
    const std::uintptr_t data = names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton);
    const int refNum = static_cast<int>(names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton + 4));
    auto find = [&](const std::string& name) {
        for (int i = 0; data && i < refNum; ++i)
            if (names::NameAt(data + i * addr::kMeshBoneStride) == name) return i;
        return -1;
    };
    std::string why;
    char buf[160];
    if (!data || refNum != num) {
        snprintf(buf, sizeof(buf), "the RefSkeleton has %d bones, the pose %d", refNum, num);
        why = buf;
    } else if (l.refBones && refNum != l.refBones) {
        snprintf(buf, sizeof(buf), "%d bones, the line expects %d", refNum, l.refBones);
        why = buf;
    }
    for (size_t k = 0; why.empty() && k < l.ref.size(); ++k) {
        const int i = find(l.ref[k].name);
        if (i < 0) {
            why = "no bone " + l.ref[k].name;
            break;
        }
        const std::uintptr_t b = data + i * addr::kMeshBoneStride;
        const int parent = *reinterpret_cast<const int*>(b + 56);
        const std::string pn = parent >= 0 && parent < refNum ? names::NameAt(data + parent * addr::kMeshBoneStride) : "";
        // The listed position is the bone's local one, so the parent must be a root -- or a bone at the origin directly
        // under one (GOAL A1: the Garand's parts hang off `stock`, which sits at 0,0,0 under RootOffset).
        bool rootLike = pn == "Root" || pn == "RootOffset";
        if (!rootLike && parent >= 0 && parent < refNum) {
            const std::uintptr_t pb = data + parent * addr::kMeshBoneStride;
            const int grand = *reinterpret_cast<const int*>(pb + 56);
            const std::string gn = grand >= 0 && grand < refNum ? names::NameAt(data + grand * addr::kMeshBoneStride) : "";
            const float* pp = reinterpret_cast<const float*>(pb + 28);
            rootLike = (gn == "Root" || gn == "RootOffset") && std::fabs(pp[0]) < 0.05f && std::fabs(pp[1]) < 0.05f &&
                       std::fabs(pp[2]) < 0.05f;
        }
        if (!rootLike) {
            why = l.ref[k].name + "'s parent is " + pn;
            break;
        }
        const float* pos = reinterpret_cast<const float*>(b + 28);
        if (std::fabs(pos[0] - l.ref[k].pos.x) > 0.05f || std::fabs(pos[1] - l.ref[k].pos.y) > 0.05f ||
            std::fabs(pos[2] - l.ref[k].pos.z) > 0.05f) {
            snprintf(buf, sizeof(buf), "%s at %.3f %.3f %.3f, expected %.3f %.3f %.3f", l.ref[k].name.c_str(), pos[0], pos[1],
                     pos[2], l.ref[k].pos.x, l.ref[k].pos.y, l.ref[k].pos.z);
            why = buf;
        }
    }
    for (const std::string& m : l.mag) {
        const int i = find(m);
        if (i < 0 && why.empty()) why = "no magazine bone " + m;
        r.mag.push_back(i);
    }
    for (const std::string& b : l.bolt) {
        const int i = find(b);
        if (i < 0 && why.empty()) why = "no action bone " + b;
        r.bolt.push_back(i);
    }
    if (!r.bolt.empty() && r.bolt[0] >= 0 && data)
        r.boltParent = *reinterpret_cast<const int*>(data + r.bolt[0] * addr::kMeshBoneStride + 56);
    if (!l.taped.empty()) r.taped = find(l.taped);  // missing: no twin handling (not a failure)
    for (const std::string& t : l.boltTurn) {
        const int i = find(t);
        if (i < 0 && why.empty()) why = "no turning bolt bone " + t;
        r.boltTurn.push_back(i);
    }
    if (!l.boltSlide.empty()) {
        r.boltSlide = find(l.boltSlide);
        if (r.boltSlide < 0 && why.empty()) why = "no bolt slide bone " + l.boltSlide;
    }
    if (!l.boltSwing.empty()) {
        r.boltSwing = find(l.boltSwing);
        if (r.boltSwing < 0 && why.empty()) why = "no hinge bone " + l.boltSwing;
    }
    if (!l.topRound.empty()) {
        r.top = find(l.topRound);
        if (r.top < 0 && why.empty()) why = "no top-round bone " + l.topRound;
    }
    r.ok = why.empty();
    if (r.ok) {
        MLOG("reload: %s on %s -- %zu magazine bone(s), %zu action bone(s)%s; the RefSkeleton check passed", l.gripKey.c_str(),
             names::Name(mesh).c_str(), r.mag.size(), r.bolt.size(), r.top >= 0 ? ", a top round" : "");
    } else {
        g_failedKeys.push_back(l.key);
        MLOG("reload: %s on %s -- the RefSkeleton check FAILED (%s): the gun keeps the game's own reload", l.key.c_str(),
             names::Name(mesh).c_str(), why.c_str());
    }
    (void)comp;
    g_resolved.push_back(r);
    return &g_resolved.back();
}

// Twin magazines: the pair's bone at rest in taped state 0 (A) or 1 (B), and the mesh-space move from one state to the
// other (inv(M_from) x M_to).
// Round 31: the pose part-way (u 0 = A .. 1 = B), so the flip can be seen: a turn about Z goes round the move's own fixed
// axis (the pair's centre line: c with c - c.Rz = b - a.Rz, in XY) while Z moves evenly; no turn = an even slide.
void PairPoseAt(const GunLine& l, float u, float* m) {
    const float full = l.tapedRot * 0.0174533f, ang = full * u, ca = std::cos(ang), sa = std::sin(ang);
    const Vec3& A = l.tapedA;
    const Vec3& B = l.tapedB;
    Vec3 p{A.x + (B.x - A.x) * u, A.y + (B.y - A.y) * u, A.z + (B.z - A.z) * u};
    if (std::fabs(full) > 1e-3f) {
        // Row vectors: v.Rz = (x cos - y sin, x sin + y cos).
        const float cf = std::cos(full), sf = std::sin(full);
        const float rx = B.x - (A.x * cf - A.y * sf), ry = B.y - (A.x * sf + A.y * cf);
        // (I - Rz) c = r: [[1 - cf, sf], [-sf, 1 - cf]] (as c.(I - Rz) with row vectors), solved.
        const float m00 = 1.0f - cf, m01 = -sf, m10 = sf, m11 = 1.0f - cf;  // c.(I - Rz) = (c.x m00 + c.y m10, c.x m01 + c.y m11)
        const float det = m00 * m11 - m01 * m10;
        if (std::fabs(det) > 1e-6f) {
            const float cx = (rx * m11 - ry * m10) / det, cy = (ry * m00 - rx * m01) / det;
            const float dx = A.x - cx, dy = A.y - cy;
            p.x = cx + dx * ca - dy * sa;
            p.y = cy + dx * sa + dy * ca;
        }
    }
    const float r[16] = {ca, sa, 0, 0, -sa, ca, 0, 0, 0, 0, 1, 0, p.x, p.y, p.z, 1};
    std::memcpy(m, r, sizeof(r));
}
void PairPose(const GunLine& l, int state, float* m) { PairPoseAt(l, state ? 1.0f : 0.0f, m); }
void PairMove(const GunLine& l, int from, float to, float* out) {
    float mf[16], mt[16], inv[16];
    PairPose(l, from, mf);
    PairPoseAt(l, to, mt);
    AffineInverse(mf, inv);
    Mul(inv, mt, out);
}

// Round 31, the reload grips (reload_grips.inc): a 4x3 row-major frame as a 4x4.
void Mat12(const float* m, float* out) {
    const float r[16] = {m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0, m[9], m[10], m[11], 1};
    std::memcpy(out, r, sizeof(r));
}
const GripData* FindGrip(const std::string& key, const char* kind) {
    for (const GripData& g : kGrips)
        if (key == g.gun && !std::strcmp(kind, g.kind)) return &g;
    return nullptr;
}
struct GripState {
    float           target[16];
    const GripData* grip = nullptr;
    DWORD           tick = 0;
} g_gripNow;
struct PumpShiftState {  // GOAL A3: the pump's move in the world, the last bake (PumpShift)
    float d[3] = {};
    DWORD tick = 0;
    bool  on = false;
} g_pumpShift;
// A rotation (columns: the frame's x, y, z axes) as a quaternion (x, y, z, w).
void MatQuat(const float (&c)[3][3], float (&q)[4]) {
    // c[col][row]: m(row, col)
    auto m = [&](int r, int k) { return c[k][r]; };
    const float tr = m(0, 0) + m(1, 1) + m(2, 2);
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m(2, 1) - m(1, 2)) / s;
        q[1] = (m(0, 2) - m(2, 0)) / s;
        q[2] = (m(1, 0) - m(0, 1)) / s;
    } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
        const float s = std::sqrt(1.0f + m(0, 0) - m(1, 1) - m(2, 2)) * 2.0f;
        q[3] = (m(2, 1) - m(1, 2)) / s;
        q[0] = 0.25f * s;
        q[1] = (m(0, 1) + m(1, 0)) / s;
        q[2] = (m(0, 2) + m(2, 0)) / s;
    } else if (m(1, 1) > m(2, 2)) {
        const float s = std::sqrt(1.0f + m(1, 1) - m(0, 0) - m(2, 2)) * 2.0f;
        q[3] = (m(0, 2) - m(2, 0)) / s;
        q[0] = (m(0, 1) + m(1, 0)) / s;
        q[1] = 0.25f * s;
        q[2] = (m(1, 2) + m(2, 1)) / s;
    } else {
        const float s = std::sqrt(1.0f + m(2, 2) - m(0, 0) - m(1, 1)) * 2.0f;
        q[3] = (m(1, 0) - m(0, 1)) / s;
        q[0] = (m(0, 2) + m(2, 0)) / s;
        q[1] = (m(1, 2) + m(2, 1)) / s;
        q[2] = 0.25f * s;
    }
}

// Round 31: a dropped magazine falls (from the well, or from the hand with its speed), tumbling a little, lands at the
// feet's height, rests a moment and is gone. World frames (the mirror world while mirrored, like everything baked).
struct Fall {
    std::string key;
    int         lastMag = -1;
    float       last[16] = {}, prev[16] = {};  // the magazine's frame in the last two bakes (in the gun or in the hand)
    DWORD       lastTick = 0, prevTick = 0;
    bool        have = false;
    bool        on = false;
    DWORD       start = 0;
    float       f0[16] = {}, v0[3] = {}, pre[16] = {};
    float       floorZ = 0.0f, upm = 100.0f;
} g_fall;
float g_flipU = 0.0f;       // the held pair's drawn flip (0 = A .. 1 = B), eased toward the half the host says
std::string g_flipKey;
DWORD g_flipTick = 0;

// The MP40's chamber slide follows its bolt, piecewise-linear through the three (bolt, slide) pairs of the line.
float SecondZ(const GunLine& l, float z) {
    float bz[3] = {l.boltZ[0], l.boltZ[1], l.boltZ[2]}, sz[3] = {l.bolt2Z[0], l.bolt2Z[1], l.bolt2Z[2]};
    for (int i = 0; i < 2; ++i)  // sort by the bolt's Z
        for (int j = 0; j < 2 - i; ++j)
            if (bz[j] > bz[j + 1]) {
                std::swap(bz[j], bz[j + 1]);
                std::swap(sz[j], sz[j + 1]);
            }
    if (z <= bz[0]) return sz[0];
    if (z >= bz[2]) return sz[2];
    const int k = z < bz[1] ? 0 : 1;
    const float t = (z - bz[k]) / (bz[k + 1] - bz[k]);
    return sz[k] + t * (sz[k + 1] - sz[k]);
}

struct TraceState {
    std::string key;
    int         mag = -1, hold = -1, top = -1, rack = -1, half = -1;
    DWORD       next = 0;
} g_trace;

// RELOAD-DESIGN 5.5: what the host needs, from the last gun bake before a Draw, in the host's gun frame (x right, y up,
// z back, metres; un-mirrored). Published in OnDraw only after a fresh bake of the gun in hand.
struct Geo {
    bool        ok = false;       // sampled for the converted gun in hand
    std::string key;
    Vec3        magGrab, magOut, boltGrab, boltBack;
    float       magGrabR = 0.07f, boltTravel = 0.0f;
    bool        haveBolt = false, heldBack = false;
    bool        haveHeld = false;      // round 31: where a held magazine sits in the drawn hand (the hold grip)
    float       heldPos[3] = {}, heldQuat[4] = {0, 0, 0, 1};
    float       magLen = 0.0f, magSeat = 0.0f;  // v17: the slide insert, metres (0 = snap)
    int         actN = 0;                        // v18: the two-stage action's knob path (host frame) and its s values
    Vec3        actPath[9];
    float       actS[9] = {};
};
Geo  g_geo;
bool g_bakeFresh = false;  // a gun bake since the last Draw (the pipeline is alive)

// The value for variant v of a per-variant list (one value = all).
template <class T>
T PerVariant(const std::vector<T>& list, int v, T def) {
    if (list.empty()) return def;
    return list[v >= 0 && v < static_cast<int>(list.size()) ? v : 0];
}

void OverrideBones(std::uintptr_t comp, const float* saved, float* bones, int num, const float* l2w, const float* a,
                   const float* kMove, const float* carry) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t w = PawnWeapon(pawn);
    if (!w) return;
    const std::string key = AttachKey(w);
    if (key.empty() || names::ClassName(names::Outer(comp)) != key) return;  // mid-switch: the old gun as the game has it
    const GunLine* l = Converted(w);
    if (!l) return;
    const int smo = names::PropertyOffset(comp, "SkeletalMesh");
    const std::uintptr_t mesh = smo >= 0 ? names::ReadPointer(comp + smo) : 0;
    const Resolved* r = mesh ? Resolve(comp, mesh, num, *l) : nullptr;
    if (!r || !r->ok) return;
    WState& s = StateFor(w);
    const int* clipP = Field(w, "AmmoCount");
    const int c = clipP ? *clipP : 0;
    // The host's inputs of this player view, and the gun's controller frame the bake is drawn from (G = gun x carry).
    viewmodel::ReloadFrame rf{};
    float gf[16], of[16];
    bool ov = false, th = false;
    const bool haveRf = viewmodel::ReloadInputs(rf) && viewmodel::HandFrames(gf, of, ov, th);
    float G[16], off[16];
    if (haveRf) {
        Mul(gf, carry, G);
        Mul(of, carry, off);
    }
    const float upm = haveRf && rf.upm > 1.0f ? rf.upm : 100.0f;
    // The magazine (RELOAD-DESIGN 5.2): the host's state when its flags are for this gun and it drives it (engaged),
    // else the game's (in or out).
    const bool hostState = haveRf && rf.view.keyHash == shared::KeyHash(key.c_str()) && (rf.view.flags & 1u) && (rf.view.flags & 16u);
    int magState = hostState ? static_cast<int>((rf.view.flags >> 1) & 3u) : (s.magIn ? 0 : 3);
    if (magState == 2 && (!rf.magValid || !haveRf)) magState = 3;
    // The visible variant (hidden upgrade parts have a zeroed 3x3) and its data.
    int v = -1;
    for (size_t k = 0; k < r->mag.size() && v < 0; ++k) {
        const int j = r->mag[k];
        if (j >= 0 && j < num && std::fabs(Det3(saved + 16 * j)) > 1e-3f) v = static_cast<int>(k);
    }
    // Twin magazines: taped when the pair's second magazine shows; the game's pose now (A or B, the nearer rest point),
    // the half in the gun (ours), and the one toward the well in the hand (flipped by the host).
    const bool taped = !l->taped.empty() && r->taped >= 0 && r->taped < num && std::fabs(Det3(saved + 16 * r->taped)) > 1e-3f;
    {
        bool found = false;
        for (KeyVariant& k : g_variant)
            if (k.key == key) {
                k.v = v < 0 ? 0 : v;
                k.taped = taped;
                found = true;
            }
        if (!found) g_variant.push_back({key, v < 0 ? 0 : v, taped});
    }
    float pre[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Round 34: the pair as if half A were in the well -- the frame every grip is taken against, so a hand holding the
    // half in the well holds it the same whichever half that is (round 33: after a flip the grab turned the wrist over and
    // the next magazine sat upside down).
    float preCanon[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool havePre = false;
    int poseNow = 0, halfDrawn = 0;
    if (taped && v >= 0 && r->mag[v] >= 0 && r->mag[v] < num) {
        const float* m = saved + 16 * r->mag[v];
        auto d2 = [&](const Vec3& p) { return (m[12] - p.x) * (m[12] - p.x) + (m[13] - p.y) * (m[13] - p.y) + (m[14] - p.z) * (m[14] - p.z); };
        poseNow = d2(l->tapedB) < d2(l->tapedA) ? 1 : 0;
        if (s.half < 0) s.half = TapedMode(w);
        halfDrawn = s.half;
        if (magState == 2 && (rf.view.flags & 32u)) halfDrawn = 1 - s.half;
        // The flip is seen (round 31): while held, the drawn pair turns to the new half over 0.35 s; otherwise it is at it.
        const DWORD tick = GetTickCount();
        const float target = static_cast<float>(halfDrawn);
        if (key != g_flipKey || magState != 2) {
            g_flipU = target;
        } else {
            const float step = std::min(static_cast<float>(tick - g_flipTick), 100.0f) / (350.0f * g_cfg.debugReloadSlowMo);
            g_flipU = g_flipU < target ? std::min(target, g_flipU + step) : std::max(target, g_flipU - step);
        }
        g_flipKey = key;
        g_flipTick = tick;
        if (poseNow != 0) PairMove(*l, poseNow, 0.0f, preCanon);
        const float u = g_flipU * g_flipU * (3.0f - 2.0f * g_flipU);  // ease in and out
        if (std::fabs(u - static_cast<float>(poseNow)) > 1e-4f) {
            PairMove(*l, poseNow, u, pre);
            havePre = true;
        }
    }
    auto posed = [&](int j, float* out) {  // the game's pose of bone j, with the pair moved to the half drawn
        if (!l->magRest.empty() && v >= 0 && v < static_cast<int>(r->mag.size()) && j == r->mag[v]) {
            // GOAL A2: the loading item is parked by the game (in the stock); seated it sits at MagRest (unturned -- or, GOAL
            // A3, turned as its 12 floats say: the M12's shell nose-up through the loading port; GOAL A4, the C96's clip in
            // the guides, where the hand seats it).
            const Vec3 at = PerVariant(l->magRest, v, Vec3{saved[16 * j + 12], saved[16 * j + 13], saved[16 * j + 14]});
            float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at.x, at.y, at.z, 1};
            const std::vector<float> rot = PerVariant(l->magRestRot, v, std::vector<float>{});
            if (rot.size() == 9)
                for (int i = 0; i < 3; ++i)
                    for (int k = 0; k < 3; ++k) id[i * 4 + k] = rot[i * 3 + k];
            std::memcpy(out, id, sizeof(id));
            return;
        }
        if (havePre) Mul(saved + 16 * j, pre, out);
        else std::memcpy(out, saved + 16 * j, 16 * sizeof(float));
    };
    // The seated frame the grips of a held item are taken against: a taped pair's canonical frame (half A in the well), or
    // (GOAL A3) an item the game parks -- a bolt gun's clip, the M12's shell -- at MagRest, as posed() draws it.
    auto seatOf = [&](int j, float* out) {
        if (!l->magRest.empty() && v >= 0 && v < static_cast<int>(r->mag.size()) && j == r->mag[v])
            posed(j, out);
        else
            Mul(saved + 16 * j, preCanon, out);
    };
    // GOAL A2 / A5: bone j of a two-stage action at s (0 closed, 1 the first stage done, 2 open), from the game's pose: a
    // turned bone turned BoltLift degrees about mesh +Z at its own origin (row vectors: (x, y) -> (x c - y s, x s + y c)),
    // then, in the second stage, drawn back BoltTravel along -Z (slide) or swung SwingDeg about the hinge (swing:
    // right-handed about SwingAxis through SwingAt -- the rows as directions, the origin as a point).
    auto actPose = [&](int j, float sv, bool turn, bool slide, bool swing, float* m) {
        std::memcpy(m, saved + 16 * j, 16 * sizeof(float));
        const float ang = std::min(sv, 1.0f) * l->boltLift * 0.0174533f, t2 = std::max(sv - 1.0f, 0.0f);
        if (turn && ang != 0.0f) {
            const float ct = std::cos(ang), st = std::sin(ang);
            for (int row = 0; row < 3; ++row) {
                const float x = m[row * 4], y = m[row * 4 + 1];
                m[row * 4] = x * ct - y * st;
                m[row * 4 + 1] = x * st + y * ct;
            }
        }
        if (slide) m[14] -= t2 * l->boltTravel;
        if (swing && t2 > 0.0f && l->swingDeg != 0.0f) {
            const Vec3 u = Norm(l->swingAxis);
            const float th = t2 * l->swingDeg * 0.0174533f, cs = std::cos(th), sn = std::sin(th);
            auto rot = [&](float& x, float& y, float& z) {  // Rodrigues
                const float dp = u.x * x + u.y * y + u.z * z;
                const float qx = u.y * z - u.z * y, qy = u.z * x - u.x * z, qz = u.x * y - u.y * x;
                const float nx = x * cs + qx * sn + u.x * dp * (1.0f - cs), ny = y * cs + qy * sn + u.y * dp * (1.0f - cs),
                            nz = z * cs + qz * sn + u.z * dp * (1.0f - cs);
                x = nx;
                y = ny;
                z = nz;
            };
            for (int row = 0; row < 3; ++row) rot(m[row * 4], m[row * 4 + 1], m[row * 4 + 2]);
            float ox = m[12] - l->swingAt.x, oy = m[13] - l->swingAt.y, oz = m[14] - l->swingAt.z;
            rot(ox, oy, oz);
            m[12] = l->swingAt.x + ox;
            m[13] = l->swingAt.y + oy;
            m[14] = l->swingAt.z + oz;
        }
    };
    const Vec3 outMesh = Norm(PerVariant(l->magOut, v, Vec3{0, 1, 0}));
    const Vec3 grabMesh = PerVariant(l->magGrab, v, Vec3{0, 0, 0});
    const float magR = PerVariant(l->magR, v, 7.0f);
    const Vec3 grabW = Xform(grabMesh, 1.0f, a);
    float pullCm = 0.0f, heldGap = -1.0f, heldTurn = -1.0f, grabSlide = 0.0f;
    // The in-gun grab frame (G's axes at the grab point) and, held, the magazine's frame: where a drop falls from.
    float fgrab[16], fheld[16], invL2W[16], inHandMove[16];
    bool haveInHandMove = false;
    AffineInverse(l2w, invL2W);
    const DWORD nowTick = GetTickCount();
    if (haveRf) {
        std::memcpy(fgrab, G, sizeof(fgrab));
        fgrab[12] = grabW.x;
        fgrab[13] = grabW.y;
        fgrab[14] = grabW.z;
        if (magState == 2) Mul(rf.magFrame, carry, fheld);
    }
    if (key != g_fall.key) {
        g_fall = Fall{};
        g_fall.key = key;
    }
    auto feetZ = [&]() {  // the floor: the pawn's feet (its location less the collision cylinder's half height)
        float loc[3] = {0, 0, 0};
        const int lo = names::PropertyOffset(pawn, "Location"), co = names::PropertyOffset(pawn, "CylinderComponent");
        if (lo >= 0) names::ReadVector(pawn + lo, loc);
        const std::uintptr_t cyl = co >= 0 ? names::ReadPointer(pawn + co) : 0;
        const int ho = cyl ? names::PropertyOffset(cyl, "CollisionHeight") : -1;
        float h = 0.0f;
        if (ho >= 0) std::memcpy(&h, reinterpret_cast<const void*>(cyl + ho), sizeof(h));
        return loc[2] - (h > 1.0f && h < 200.0f ? h : 50.0f);
    };
    // Round 31 (and GOAL A5's case): the magazine falling -- ballistic from f0 until it reaches the floor, then at rest; a
    // tumble about its own right axis while it falls. False (and the fall off) once it is over.
    auto fallDraw = [&]() {
        const float t = static_cast<float>(nowTick - g_fall.start) / (1000.0f * g_cfg.debugReloadSlowMo);
        if (!g_fall.on || !haveRf || t >= 2.0f) {
            g_fall.on = false;
            return false;
        }
        const float gz = -9.8f * g_fall.upm;
        const float z0 = g_fall.f0[14], vz = g_fall.v0[2], floor = g_fall.floorZ + 2.0f;
        float tl = t;  // the landing time (z0 + vz t + gz t^2 / 2 = floor)
        const float disc = vz * vz - 2.0f * gz * (z0 - floor);
        if (z0 > floor && disc >= 0.0f) tl = std::min(t, (-vz - std::sqrt(disc)) / gz);
        else if (z0 <= floor) tl = 0.0f;
        float f[16];
        const float ang = 2.5f * tl, ca = std::cos(ang), sa = std::sin(ang);
        for (int k = 0; k < 4; ++k) {  // forward and up turned about the frame's right (row 1)
            f[0 + k] = g_fall.f0[0 + k] * ca + g_fall.f0[8 + k] * sa;
            f[4 + k] = g_fall.f0[4 + k];
            f[8 + k] = g_fall.f0[8 + k] * ca - g_fall.f0[0 + k] * sa;
        }
        f[12] = g_fall.f0[12] + g_fall.v0[0] * tl;
        f[13] = g_fall.f0[13] + g_fall.v0[1] * tl;
        f[14] = std::max(floor, z0 + vz * tl + 0.5f * gz * tl * tl);
        f[15] = 1.0f;
        float fgrabInv[16], m1[16], m2[16], move[16];
        AffineInverse(fgrab, fgrabInv);
        Mul(a, fgrabInv, m1);
        Mul(m1, f, m2);
        Mul(m2, invL2W, move);
        for (int j : r->mag)
            if (j >= 0 && j < num) {
                float m[16];
                Mul(saved + 16 * j, g_fall.pre, m);
                Mul(m, move, bones + 16 * j);
            }
        return true;
    };
    // (EjectOnEmpty: the clip the game threw at the last shot flies as its own projectile -- ours doesn't fall too.)
    if (haveRf && magState == 3 && (g_fall.lastMag == 0 || g_fall.lastMag == 2) && g_fall.have && g_cfg.dropFall &&
        !(g_fall.lastMag == 0 && s.gameEjected) && !l->boltAction && !l->pump) {
        // Dropped just now: fall from where it was drawn, with its speed (and a push out of the well when ejected).
        g_fall.on = true;
        g_fall.start = nowTick;
        std::memcpy(g_fall.f0, g_fall.last, sizeof(g_fall.f0));
        std::memcpy(g_fall.pre, pre, sizeof(g_fall.pre));
        const float dt = static_cast<float>(g_fall.lastTick - g_fall.prevTick) / 1000.0f;
        for (int i = 0; i < 3; ++i) g_fall.v0[i] = dt > 0.004f && dt < 0.1f ? (g_fall.last[12 + i] - g_fall.prev[12 + i]) / dt : 0.0f;
        const float sp = std::sqrt(g_fall.v0[0] * g_fall.v0[0] + g_fall.v0[1] * g_fall.v0[1] + g_fall.v0[2] * g_fall.v0[2]);
        if (sp > 5.0f * upm)
            for (float& x : g_fall.v0) x *= 5.0f * upm / sp;
        if (g_fall.lastMag == 0) {
            const Vec3 o = Norm(Xform(outMesh, 0.0f, a));
            const float push = l->ejectSpeed * upm;  // EjectSpeed (the Garand's clip pops up out of the receiver)
            g_fall.v0[0] += o.x * push;
            g_fall.v0[1] += o.y * push;
            g_fall.v0[2] += o.z * push;
        }
        g_fall.upm = upm;
        g_fall.floorZ = feetZ();
        if (g_cfg.debugReloadTrace)
            MLOG("reload: trace -- the magazine falls from %.0f %.0f %.0f at %.0f %.0f %.0f u/s to the floor at %.0f",
                 g_fall.f0[12], g_fall.f0[13], g_fall.f0[14], g_fall.v0[0], g_fall.v0[1], g_fall.v0[2], g_fall.floorZ);
    }
    if (magState != 3) g_fall.on = false;
    if (haveRf && (magState == 0 || magState == 1 || magState == 2)) {
        std::memcpy(g_fall.prev, g_fall.last, sizeof(g_fall.prev));
        g_fall.prevTick = g_fall.lastTick;
        std::memcpy(g_fall.last, magState == 2 ? fheld : fgrab, sizeof(g_fall.last));
        g_fall.lastTick = nowTick;
        g_fall.have = true;
    }
    g_fall.lastMag = magState;
    if (magState == 0 && havePre) {
        for (int j : r->mag)
            if (j >= 0 && j < num) {
                float m[16];
                posed(j, m);
                Draw(bones, j, m, kMove);
            }
    }
    if (magState == 1) {
        // Grabbed: the group slides out along the magazine's way out (mesh space), by the host's pull.
        const float scale = Len(Xform(outMesh, 0.0f, a));
        const float dist = scale > 1e-4f ? rf.view.magPull * upm / scale : 0.0f;
        grabSlide = dist;
        pullCm = rf.view.magPull * 100.0f;
        auto slide = [&](int j) {
            if (j < 0 || j >= num) return;
            float m[16];
            posed(j, m);
            m[12] += outMesh.x * dist;
            m[13] += outMesh.y * dist;
            m[14] += outMesh.z * dist;
            Draw(bones, j, m, kMove);
        };
        for (int j : r->mag) slide(j);
        slide(r->top);  // the top round sits in the magazine's lips
    } else if (magState == 2) {
        // In the off hand: moved rigidly from the in-gun grab frame (G's axes at the grab point) to the held one.
        float fgrabInv[16], m1[16], m2[16], hold[16];
        AffineInverse(fgrab, fgrabInv);
        Mul(a, fgrabInv, m1);
        Mul(m1, fheld, m2);
        Mul(m2, invL2W, hold);
        std::memcpy(inHandMove, hold, sizeof(inHandMove));
        haveInHandMove = true;
        for (int j : r->mag)
            if (j >= 0 && j < num && (!l->boltAction || (v >= 0 && j == r->mag[v]))) {
                float m[16];
                posed(j, m);
                Mul(m, hold, bones + 16 * j);
            }
        const float dx = fheld[12] - off[12], dy = fheld[13] - off[13], dz = fheld[14] - off[14];
        heldGap = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0f / upm;
        // How far the held frame is turned from the seated one (0 when the off hand points like the gun hand).
        float tr = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) tr += fgrab[i * 4 + k] * fheld[i * 4 + k];
        heldTurn = std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
    } else if (magState == 3 && (l->boltAction || l->pump)) {
        // GOAL A2: nothing loading in the hand -- the items stay parked (the game's pose, inside the stock), except a
        // stripped clip, drawn in the guides until the bolt closes. (GOAL A3: the M12's shell likewise.)
        if (s.clipSeated && l->haveClipIn && v == 0 && r->mag[0] >= 0 && r->mag[0] < num) {
            const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, l->clipIn.x, l->clipIn.y, l->clipIn.z, 1};
            Draw(bones, r->mag[0], id, kMove);
        }
        // GOAL A5: the M18's round in the chamber -- the game's pose while a round or a spent case is in; a spent case
        // thrown out as the breech opens falls from it (backwards along the bore at EjectSpeed); an empty chamber shows
        // nothing.
        if (r->boltSwing >= 0) {
            if (s.caseFall && haveRf && g_cfg.dropFall) {
                g_fall.on = true;
                g_fall.start = nowTick;
                std::memcpy(g_fall.f0, fgrab, sizeof(g_fall.f0));
                const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                std::memcpy(g_fall.pre, id, sizeof(g_fall.pre));
                const Vec3 o = Norm(Xform(outMesh, 0.0f, a));
                const float push = l->ejectSpeed * upm;
                g_fall.v0[0] = o.x * push;
                g_fall.v0[1] = o.y * push;
                g_fall.v0[2] = o.z * push;
                g_fall.upm = upm;
                g_fall.floorZ = feetZ();
                if (g_cfg.debugReloadTrace)
                    MLOG("reload: trace -- the spent case falls from the breech at %.0f %.0f %.0f to the floor at %.0f",
                         g_fall.f0[12], g_fall.f0[13], g_fall.f0[14], g_fall.floorZ);
            }
            s.caseFall = false;
            if (!fallDraw() && c == 0 && !s.spent)
                for (int j : r->mag)
                    if (j >= 0 && j < num) Collapse(bones, j, saved, kMove);
        }
    } else if (magState == 3) {
        if (!fallDraw())
            for (int j : r->mag)
                if (j >= 0 && j < num) Collapse(bones, j, saved, kMove);
    }
    // The action: held at its empty position, and drawn back by the host's rack while the off hand holds it
    // (RELOAD-DESIGN 5.3).
    const bool hold = l->emptyCue && (l->open ? !s.cocked : c == 0);
    const bool racking = hostState && (rf.view.flags & 8u);
    float zGame = 0.0f, zDrawn = 0.0f, zHeld = 0.0f;
    const int b = !r->bolt.empty() ? r->bolt[0] : -1;
    float sDrawn = 0.0f;  // GOAL A2: the bolt's s (0 closed, 1 lifted, 2 drawn back)
    if (l->boltAction) {
        sDrawn = s.bolt == 2 ? 2.0f : (s.bolt == 1 || s.bolt == 3) ? 1.0f : 0.0f;
        if (racking) sDrawn = std::clamp(rf.view.rack, 0.0f, 1.0f) * 2.0f;  // the host's knob, as the off hand has it
        if (sDrawn > 0.0f) {
            for (int j : r->boltTurn) {
                if (j < 0 || j >= num) continue;
                float m[16];
                actPose(j, sDrawn, true, true, true, m);
                Draw(bones, j, m, kMove);
            }
            if (r->boltSlide >= 0 && r->boltSlide < num) {
                float m[16];
                actPose(r->boltSlide, sDrawn, false, true, false, m);
                Draw(bones, r->boltSlide, m, kMove);
            }
            if (r->boltSwing >= 0 && r->boltSwing < num) {  // GOAL A5: the M18's hinge arm
                float m[16];
                actPose(r->boltSwing, sDrawn, false, false, true, m);
                Draw(bones, r->boltSwing, m, kMove);
            }
        }
        // The top round, seen through the open action (the game parks the bone in the stock).
        if (l->haveTopRoundAt && r->top >= 0 && r->top < num && sDrawn >= 1.5f && c >= 1) {
            const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, l->topRoundAt.x, l->topRoundAt.y, l->topRoundAt.z, 1};
            Draw(bones, r->top, m, kMove);
        }
    }
    // GOAL A3: a pump moves along its parent's Z row (the M12's RootOffset is tilted 1 degree: along mesh Z alone the
    // pump would sit 0.2 u high at the back); the Step 1 actions along mesh Z, as proven.
    Vec3 ax{0, 0, 1};
    if (l->pump && r->boltParent >= 0 && r->boltParent < num) {
        const float* p = saved + 16 * r->boltParent;
        const Vec3 z = Norm(Vec3{p[8], p[9], p[10]});
        if (z.z > 0.5f) ax = z;
    }
    auto zTo = [&](float* m, float z) {  // a bone's matrix moved so its Z is z
        if (!l->pump) {
            m[14] = z;
            return;
        }
        const float k = (z - m[14]) / ax.z;
        m[12] += ax.x * k;
        m[13] += ax.y * k;
        m[14] += ax.z * k;
    };
    g_pumpShift.on = false;
    if (b >= 0 && b < num && !l->boltAction) {
        const float zs = saved[16 * b + 14], zIdle = l->boltZ[0], zEmpty = l->boltZ[1];
        const float zh = hold ? (zEmpty < zIdle ? std::min(zs, zEmpty) : std::max(zs, zEmpty)) : zs;
        const float zd = racking ? zh + std::clamp(rf.view.rack, 0.0f, 1.0f) * (l->boltZ[2] - zh) : zh;
        zGame = zs;
        zHeld = zh;
        zDrawn = zd;
        if (zd != zs) {
            float m[16];
            std::memcpy(m, saved + 16 * b, sizeof(m));
            zTo(m, zd);
            Draw(bones, b, m, kMove);
            if (r->bolt.size() > 1 && r->bolt[1] >= 0 && r->bolt[1] < num && l->haveBolt2Z) {
                float m2[16];
                std::memcpy(m2, saved + 16 * r->bolt[1], sizeof(m2));
                zTo(m2, SecondZ(*l, zd));
                Draw(bones, r->bolt[1], m2, kMove);
            }
        }
        if (l->pump && racking) {  // the pump's move in the world: the support hand rides it (arms_ik, two-handed)
            const float k = (zd - zs) / ax.z;
            const Vec3 dW = Xform(Vec3{ax.x * k, ax.y * k, ax.z * k}, 0.0f, a);
            g_pumpShift = {{dW.x, dW.y, dW.z}, nowTick, true};
        }
    }
    // The top round: only while the magazine is in and has rounds (RELOAD-DESIGN 5.4); it slides with a grabbed one.
    int topShown = -1;
    if (r->top >= 0 && r->top < num && !l->boltAction) {
        const bool shown = (magState == 0 || magState == 1) && (l->open ? (c >= 1 || s.pending) : (c >= 2 || s.pending));
        if (!shown) Collapse(bones, r->top, saved, kMove);
        topShown = shown ? 1 : 0;
    }
    // Round 31, the reload grips: the off hand takes the game's own grip -- on the magazine while grabbed, on the one it
    // holds, on the handle while racking -- the part's drawn frame x the grip (the hand in the part's frame).
    g_gripNow.grip = nullptr;
    // Round 32: the player's adjustments of this gun's grips (the menu's Reload grip page).
    float adj[3][6] = {};
    std::uint32_t gripFlags = 0;
    shared::Header* hdrA = bridge::SharedHeader();
    if (hdrA && !shared::ReadGripAdj(hdrA, key.c_str(), adj, gripFlags)) {
        std::memset(adj, 0, sizeof(adj));
        gripFlags = 0;
    }
    auto gripOf = [&](const GripData* gd, int which, float* out) {  // the hand in the part's frame, adjusted
        Mat12(gd->hand, out);
        const float* d = adj[which];
        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0 && d[4] == 0 && d[5] == 0) return;
        // Turned at the wrist about the gun's axes (mesh: X left, Y down, Z forward), then moved along them.
        const float k = 0.0174533f, t = d[3] * k, y = d[4] * k, r2 = d[5] * k;
        const float rx[16] = {1, 0, 0, 0, 0, std::cos(t), std::sin(t), 0, 0, -std::sin(t), std::cos(t), 0, 0, 0, 0, 1};
        const float ry[16] = {std::cos(y), 0, -std::sin(y), 0, 0, 1, 0, 0, std::sin(y), 0, std::cos(y), 0, 0, 0, 0, 1};
        const float rz[16] = {std::cos(r2), std::sin(r2), 0, 0, -std::sin(r2), std::cos(r2), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float rot[16], tmp[16], m[16];
        Mul(rx, ry, tmp);
        Mul(tmp, rz, rot);
        const float hx = out[12], hy = out[13], hz = out[14];
        out[12] = out[13] = out[14] = 0.0f;
        Mul(out, rot, m);
        m[12] = hx - d[2];  // right = -X
        m[13] = hy - d[1];  // up = -Y
        m[14] = hz + d[0];  // forward = +Z
        std::memcpy(out, m, sizeof(m));
    };
    // Round 35: "hold like grab" (the player's, per gun) -- the magazine in the hand (pulled out, or from the pouch) held
    // with the grab grip and its adjustments, so it looks the same coming out as going in (round 34 had it the other way
    // round). Guns whose reload animation has no grab grip (the Colt, the C96) keep the hold.
    const GripData* grabGd = FindGrip(l->gripKey, "mag");
    const bool holdLikeGrab = (gripFlags & 1u) && grabGd;
    if (g_cfg.reloadGrips && haveRf) {
        const char* kind = magState == 1 ? "mag" : magState == 2 ? (holdLikeGrab ? "mag" : "hold") : racking ? "bolt" : nullptr;
        const int which = magState == 1 ? 0 : magState == 2 ? (holdLikeGrab ? 0 : 1) : 2;
        const GripData* gd = kind ? FindGrip(l->gripKey, kind) : nullptr;
        const int part = !gd ? -1 : racking && magState != 1 && magState != 2 ? b : (v >= 0 ? r->mag[v] : -1);
        if (gd && part >= 0 && part < num) {
            float partW[16], hand[16], m[16], m2[16];
            if (magState == 2 && haveInHandMove) {
                // Held: the pair's canonical frame carried into the hand (a flip turns the drawn pair, not the hand).
                seatOf(part, m);
                Mul(m, inHandMove, m2);
                Mul(m2, l2w, partW);
            } else if (magState == 1) {
                // Grabbed: the canonical frame, slid out as drawn.
                Mul(saved + 16 * part, preCanon, m);
                m[12] += outMesh.x * grabSlide;
                m[13] += outMesh.y * grabSlide;
                m[14] += outMesh.z * grabSlide;
                Mul(m, kMove, m2);
                Mul(m2, l2w, partW);
            } else {
                Mul(bones + 16 * part, l2w, partW);
            }
            gripOf(gd, which, hand);
            Mul(hand, partW, g_gripNow.target);
            g_gripNow.grip = gd;
            g_gripNow.tick = nowTick;
        }
    }
    // Where a held magazine sits in the drawn hand (the hold grip): the free hand's frame (arms_ik) x inv(grip) = the
    // magazine bone, carried to its grab-point frame (as seated: the in-gun grab frame on the in-gun bone); published
    // to the host in the off controller's aim frame (un-mirrored), which then holds the magazine there.
    g_geo.haveHeld = false;
    if (g_cfg.reloadGrips && haveRf && v >= 0 && r->mag[v] >= 0 && r->mag[v] < num) {
        const GripData* gd = holdLikeGrab ? grabGd : FindGrip(l->gripKey, "hold");
        float relM[16];
        if (gd && armsik::FreeHandRel(relM)) {
            float handT[16], grip[16], gripInv[16], boneW[16], rest[16], restInv[16], grabInBone[16], fheldW[16], offInv[16], rel[16];
            Mul(relM, off, handT);
            gripOf(gd, holdLikeGrab ? 0 : 1, grip);
            AffineInverse(grip, gripInv);
            Mul(gripInv, handT, boneW);
            seatOf(r->mag[v], rest);  // the canonical pair (half A in the well); a parked item's seat
            Mul(rest, a, rest);
            AffineInverse(rest, restInv);
            Mul(fgrab, restInv, grabInBone);
            Mul(grabInBone, boneW, fheldW);
            AffineInverse(off, offInv);
            Mul(fheldW, offInv, rel);
            if (rf.mirrored) {  // the real controller's: y (right) flipped on both sides
                for (int i = 0; i < 4; ++i)
                    for (int k = 0; k < 4; ++k)
                        if ((i == 1) != (k == 1)) rel[i * 4 + k] = -rel[i * 4 + k];
            }
            // Host convention (x right, y up, z back; metres): the held frame's axes in the off frame's.
            const float cols[3][3] = {{rel[5], rel[6], -rel[4]}, {rel[9], rel[10], -rel[8]}, {-rel[1], -rel[2], rel[0]}};
            MatQuat(cols, g_geo.heldQuat);
            g_geo.heldPos[0] = rel[13] / upm;
            g_geo.heldPos[1] = rel[14] / upm;
            g_geo.heldPos[2] = -rel[12] / upm;
            g_geo.haveHeld = true;
        }
    }
    // The geometry for the host (RELOAD-DESIGN 5.5): points relative to G on its rows (forward, right, up) -> the host's
    // (right, up, -forward) in metres; the right component negated in the mirror world (5.6).
    if (haveRf) {
        const float mir = rf.mirrored ? -1.0f : 1.0f;
        auto hostDir = [&](const Vec3& d) {
            const float f = d.x * G[0] + d.y * G[1] + d.z * G[2], rr = d.x * G[4] + d.y * G[5] + d.z * G[6],
                        u = d.x * G[8] + d.y * G[9] + d.z * G[10];
            return Vec3{mir * rr, u, -f};
        };
        auto hostPoint = [&](const Vec3& p) {
            const Vec3 h = hostDir(Vec3{p.x - G[12], p.y - G[13], p.z - G[14]});
            return Vec3{h.x / upm, h.y / upm, h.z / upm};
        };
        g_geo.ok = true;
        g_geo.key = key;
        g_geo.magGrab = hostPoint(grabW);
        g_geo.magOut = Norm(hostDir(Xform(outMesh, 0.0f, a)));
        g_geo.magGrabR = magR / 100.0f;
        // v17 (Insert=slide): the magazine's length and seat depth along its way out, in metres at the gun's scale.
        g_geo.magLen = l->slideInsert ? Len(Xform(Vec3{outMesh.x * l->magLen, outMesh.y * l->magLen, outMesh.z * l->magLen}, 0.0f, a)) / upm : 0.0f;
        g_geo.magSeat = l->slideInsert ? Len(Xform(Vec3{outMesh.x * l->magSeat, outMesh.y * l->magSeat, outMesh.z * l->magSeat}, 0.0f, a)) / upm : 0.0f;
        // v18 (GOAL A2): the knob's path -- its point in the first turning bone, turned up in quarters, then drawn back.
        g_geo.actN = 0;
        int jt = -1;  // the first turning bone that shows (a hidden upgrade variant has a zero 3x3: its knob is nowhere)
        for (int j : r->boltTurn)
            if (jt < 0 && j >= 0 && j < num && std::fabs(Det3(saved + 16 * j)) > 1e-3f) jt = j;
        if (l->boltAction && jt >= 0) {
            // (GOAL A5: a swing is an arc -- four more samples along it.)
            static const float kS[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f};
            static const float kSwing[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
            const bool swings = r->boltSwing >= 0 && l->swingDeg != 0.0f;
            const float* ss = swings ? kSwing : kS;
            const int n = swings ? 9 : 6;
            for (int i = 0; i < n; ++i) {
                float m[16];
                actPose(jt, ss[i], true, true, true, m);
                g_geo.actPath[g_geo.actN] = hostPoint(Xform(Xform(l->knob, 1.0f, m), 1.0f, a));
                g_geo.actS[g_geo.actN] = ss[i];
                ++g_geo.actN;
            }
        }
        g_geo.haveBolt = b >= 0 && b < num;
        if (g_geo.haveBolt) {
            const Vec3 p{saved[16 * b + 12] + l->boltGrab.x, saved[16 * b + 13] + l->boltGrab.y, zHeld + l->boltGrab.z};
            g_geo.boltGrab = hostPoint(Xform(p, 1.0f, a));
            g_geo.boltBack = Norm(hostDir(Xform(Vec3{-ax.x, -ax.y, -ax.z}, 0.0f, a)));  // (GOAL A3: a pump's tilted way)
            const float k = (l->boltZ[2] - zHeld) / ax.z;
            g_geo.boltTravel = Len(Xform(Vec3{ax.x * k, ax.y * k, ax.z * k}, 0.0f, a)) / upm;
            g_geo.heldBack = hold && std::fabs(l->boltZ[2] - zHeld) < 2.0f;  // the empty hold only (not a shot's recoil)
        }
    }
    const DWORD now = GetTickCount();
    // Debug.ReloadTrace: the taped pair's pose 1.5 s after the game's TapedMagMode changes (its idle by then).
    static std::string tapedKey;
    static int tapedMode = -1;
    static DWORD tapedLogAt = 0;
    if (g_cfg.debugReloadTrace) {
        const int to = names::PropertyOffset(w, "TapedMagMode");
        const int mode = to >= 0 ? *reinterpret_cast<const std::uint8_t*>(w + to) : -1;
        if (key != tapedKey || mode != tapedMode) {
            tapedKey = key;
            tapedMode = mode;
            tapedLogAt = now + 1500;
        }
        if (tapedLogAt && static_cast<LONG>(now - tapedLogAt) >= 0) {
            tapedLogAt = 0;
            g_trace.key.clear();  // log the bones below now
        }
    }
    if (g_cfg.debugReloadTrace && (key != g_trace.key || magState != g_trace.mag || (hold ? 1 : 0) != g_trace.hold ||
                                   topShown != g_trace.top || (racking ? 1 : 0) != g_trace.rack || halfDrawn != g_trace.half ||
                                   ((magState == 1 || magState == 2 || racking) && static_cast<LONG>(now - g_trace.next) >= 0))) {
        g_trace.key = key;
        g_trace.mag = magState;
        g_trace.hold = hold ? 1 : 0;
        g_trace.top = topShown;
        g_trace.rack = racking ? 1 : 0;
        g_trace.half = halfDrawn;
        g_trace.next = now + 1000;
        static const char* kMag[] = {"in the gun", "grabbed", "in the off hand", "hidden (out)"};
        char extra[96] = "";
        if (magState == 1) snprintf(extra, sizeof(extra), " (pulled %.1f cm)", pullCm);
        if (magState == 2)
            snprintf(extra, sizeof(extra), " (its grab point %.1f cm from the off controller, turned %.0f deg from seated)",
                     heldGap, heldTurn);
        if (taped)
            MLOG("reload: trace -- taped pair: the game's pose %c, half %c in the gun, half %c drawn%s", poseNow ? 'B' : 'A',
                 s.half ? 'B' : 'A', halfDrawn ? 'B' : 'A', havePre ? " (moved)" : "");
        MLOG("reload: trace -- drawn %s: magazine %s%s, action %s%s (game Z %.2f -> drawn %.2f), top round %s; host %s", key.c_str(),
             kMag[magState], extra, hold ? "held empty" : "the game's", racking ? ", racked by the off hand" : "", zGame, zDrawn,
             topShown < 0 ? "none" : topShown ? "shown" : "hidden", hostState ? "drives it" : "not driving");
        for (size_t k = 0; k < r->mag.size(); ++k) {
            const int j = r->mag[k];
            if (j < 0 || j >= num) continue;
            const float* m = saved + 16 * j;
            MLOG("reload: trace -- magazine bone %s%s: axes %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f, at %.2f %.2f %.2f",
                 l->mag[k].c_str(), static_cast<int>(k) == v ? " (visible)" : "", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9],
                 m[10], m[12], m[13], m[14]);
        }
        {
            const int to = names::PropertyOffset(w, "TapedMagMode");
            if (to >= 0) MLOG("reload: trace -- TapedMagMode %d", *reinterpret_cast<const std::uint8_t*>(w + to));
        }
        if (haveRf)
            MLOG("reload: trace -- geometry (cm, the gun frame: right up back): magazine grab %.1f %.1f %.1f, out %.2f %.2f %.2f, "
                 "r %.0f; action grab %.1f %.1f %.1f, back %.2f %.2f %.2f, travel %.1f%s", g_geo.magGrab.x * 100.0f,
                 g_geo.magGrab.y * 100.0f, g_geo.magGrab.z * 100.0f, g_geo.magOut.x, g_geo.magOut.y, g_geo.magOut.z,
                 g_geo.magGrabR * 100.0f, g_geo.boltGrab.x * 100.0f, g_geo.boltGrab.y * 100.0f, g_geo.boltGrab.z * 100.0f,
                 g_geo.boltBack.x, g_geo.boltBack.y, g_geo.boltBack.z, g_geo.boltTravel * 100.0f,
                 rf.mirrored ? " (mirrored)" : "");
    }
}

// --- M0 probe state (game thread) ---
std::vector<std::uintptr_t> g_meshSeen;
std::uintptr_t   g_bakeMesh = 0;
int              g_bakeCount = 0;
int              g_bakesThisDraw = 0;
struct BakeStats { long draws = 0, bakes = 0; int max = 0; } g_still, g_moving;
DWORD            g_nextStats = 0, g_nextWeaponLog = 0;
std::uintptr_t   g_lastWeapon = 0;
long             g_callsWeapon = 0, g_callsOther = 0, g_resultTrue = 0, g_blocks = 0;
std::uintptr_t   g_otherClasses[8] = {};
long             g_otherCounts[8] = {};

// UEALAWeapon::execHasReserveAmmo -- folded with AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent (the same
// code; RELOAD-DESIGN 2.5): `self` may be the GameInfo. Only the pawn's own weapon is ever looked at further.
void __fastcall Hook_ExecHasReserveAmmo(std::uintptr_t self, void* /*edx*/, void* stack, void* result) {
    g_hook.thiscall<void>(self, stack, result);
    if (Blocking(self)) {
        if (result && *static_cast<std::uint32_t*>(result)) {
            *static_cast<std::uint32_t*>(result) = 0;
            ++g_blocks;
        }
        return;
    }
    if (!g_cfg.debugReloadProbe) return;
    const std::uintptr_t w = PawnWeapon(aim::LocalPlayerPawn());
    if (self && self == w) {
        ++g_callsWeapon;
        if (result && *static_cast<const std::uint32_t*>(result)) ++g_resultTrue;
        return;
    }
    ++g_callsOther;
    const std::uintptr_t cls = self ? names::ReadPointer(self + addr::kObjectClass) : 0;
    for (int i = 0; i < 8; ++i) {
        if (g_otherClasses[i] == cls || !g_otherClasses[i]) {
            if (!g_otherClasses[i]) {
                g_otherClasses[i] = cls;
                MLOG("reload: probe -- execHasReserveAmmo called for a %s (not the pawn's weapon; result %u, left as is)",
                     cls ? names::Name(cls).c_str() : "null", result ? *static_cast<const std::uint32_t*>(result) : 0u);
            }
            ++g_otherCounts[i];
            break;
        }
    }
}

std::string StateName(std::uintptr_t obj) {
    const std::uintptr_t frame = names::ReadPointer(obj + 0x18);
    const std::uintptr_t node = frame ? names::ReadPointer(frame + 0x2C) : 0;
    if (!node) return "?";
    if (node == names::ReadPointer(obj + addr::kObjectClass)) return "(none)";
    return names::NameAt(node + 0x2C);
}

bool Derives(std::uintptr_t c, std::uintptr_t want) {
    for (int depth = 0; c && depth < 64; c = names::ReadPointer(c + addr::kFieldSuper), ++depth)
        if (c == want) return true;
    return false;
}

float Det3(const float* m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
float RowNorm(const float* m, int r) { return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]); }

void LogWeapon(std::uintptr_t pawn, std::uintptr_t w) {
    auto off = [&](const char* n) { return names::PropertyOffset(w, n); };
    const int ac = off("AmmoCount"), mc = off("MaxAmmoCount"), cl = off("AmmoClass"), at = off("AttachmentClass"),
              up = off("CurrentUpgradeLevel"), tm = off("TapedMagMode");
    int ao = -1, io = -1;
    std::uint32_t am = 0, im = 0;
    const bool haveAlt = names::BoolProperty(w, "bAlternateFireMode", ao, am), haveInf = names::BoolProperty(w, "bInfiniteAmmo", io, im);
    const int* ammo = ac >= 0 ? reinterpret_cast<const int*>(w + ac) : nullptr;
    const int* maxa = mc >= 0 ? reinterpret_cast<const int*>(w + mc) : nullptr;
    const std::uintptr_t ammoClass = cl >= 0 ? names::ReadPointer(w + cl) : 0;
    MLOG("reload: probe -- weapon %s (%s), attachment %s, state %s; AmmoCount %d %d %d (at 0x%X, static 0x2D4), MaxAmmoCount %d %d %d "
         "(0x%X, 0x2E0), alt %d / infinite %d (bits at 0x%X mask 0x%X / 0x%X mask 0x%X, static 0x2EC 0x400 / 0x1), AmmoClass %s "
         "(0x%X, 0x2FC), CurrentUpgradeLevel %d, TapedMagMode %d",
         names::Name(w).c_str(), names::ClassName(w).c_str(), at >= 0 ? names::Name(names::ReadPointer(w + at)).c_str() : "?",
         StateName(w).c_str(), ammo ? ammo[0] : -1, ammo ? ammo[1] : -1, ammo ? ammo[2] : -1, ac, maxa ? maxa[0] : -1,
         maxa ? maxa[1] : -1, maxa ? maxa[2] : -1, mc,
         haveAlt ? ((*reinterpret_cast<const std::uint32_t*>(w + ao) & am) ? 1 : 0) : -1,
         haveInf ? ((*reinterpret_cast<const std::uint32_t*>(w + io) & im) ? 1 : 0) : -1, ao, am, io, im,
         ammoClass ? names::Name(ammoClass).c_str() : "none", cl, up >= 0 ? *reinterpret_cast<const int*>(w + up) : -99,
         tm >= 0 ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(w + tm)) : -1);
    const int io2 = names::PropertyOffset(pawn, "InvManager");
    const std::uintptr_t inv = io2 >= 0 ? names::ReadPointer(pawn + io2) : 0;
    if (!inv) return;
    const int so = names::PropertyOffset(inv, "AmmoStorage"), no = names::PropertyOffset(inv, "NumAmmoClasses");
    const int n = no >= 0 ? *reinterpret_cast<const int*>(inv + no) : -1;
    MLOG("reload: probe -- inventory %s: AmmoStorage at 0x%X (static 0x228), NumAmmoClasses %d at 0x%X (static 0x2A0)",
         names::Name(inv).c_str(), so, n, no);
    if (so < 0 || n < 0 || n > 10) return;
    int exact = -1, sub = -1;
    for (int i = 0; i < n; ++i) {
        const std::uintptr_t e = inv + so + 12 * i;
        const std::uintptr_t c = names::ReadPointer(e);
        const int amount = *reinterpret_cast<const int*>(e + 4), cap = *reinterpret_cast<const int*>(e + 8);
        if (c == ammoClass && exact < 0) exact = i;
        if (sub < 0 && ammoClass && Derives(c, ammoClass)) sub = i;
        MLOG("reload: probe --   reserve[%d] %s amount %d cap %d", i, c ? names::Name(c).c_str() : "none", amount, cap);
    }
    MLOG("reload: probe --   the weapon's AmmoClass: exact match [%d], subclass match (the native read) [%d]%s", exact, sub,
         exact == sub ? "" : " -- DIFFERENT");
}

void LogBones(std::uintptr_t comp, std::uintptr_t mesh, const float* saved, int num, const float* l2w, const float* a) {
    const std::uintptr_t data = names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton);
    const int refNum = static_cast<int>(names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton + 4));
    MLOG("reload: probe -- gun %s (%s of %s): %d bones (RefSkeleton %d); L2W rows %.4f %.4f %.4f, A rows %.4f %.4f %.4f",
         names::Name(mesh).c_str(), names::Name(comp).c_str(), names::Name(names::Outer(comp)).c_str(), num, refNum,
         RowNorm(l2w, 0), RowNorm(l2w, 1), RowNorm(l2w, 2), RowNorm(a, 0), RowNorm(a, 1), RowNorm(a, 2));
    for (int i = 0; i < num && i < refNum && data; ++i) {
        const std::uintptr_t b = data + i * addr::kMeshBoneStride;
        const int parent = *reinterpret_cast<const int*>(b + 56);
        const float* ref = reinterpret_cast<const float*>(b + 28);
        const float* m = saved + 16 * i;
        MLOG("reload: probe --   [%2d] %-28s parent %2d  ref %8.3f %8.3f %8.3f  pose %8.3f %8.3f %8.3f  |det| %.3f", i,
             names::NameAt(b).c_str(), parent, ref[0], ref[1], ref[2], m[12], m[13], m[14], std::fabs(Det3(m)));
    }
}

void ProbeDraw(std::uintptr_t pawn) {
    float speed = 0.0f;
    if (pawn) {
        float v[3];
        const int vo = names::PropertyOffset(pawn, "Velocity");
        if (vo >= 0 && names::ReadVector(pawn + vo, v)) speed = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    }
    BakeStats& s = speed > 50.0f ? g_moving : g_still;
    ++s.draws;
    s.bakes += g_bakesThisDraw;
    if (g_bakesThisDraw > s.max) s.max = g_bakesThisDraw;
    g_bakesThisDraw = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - g_nextStats) >= 0) {
        g_nextStats = now + 10000;
        if (g_still.draws || g_moving.draws)
            MLOG("reload: probe -- gun bakes per Draw: standing %.2f (max %d, %ld Draws), moving %.2f (max %d, %ld Draws); "
                 "execHasReserveAmmo: %ld for the pawn's weapon (%ld true), %ld for other objects",
                 g_still.draws ? static_cast<double>(g_still.bakes) / g_still.draws : 0.0, g_still.max, g_still.draws,
                 g_moving.draws ? static_cast<double>(g_moving.bakes) / g_moving.draws : 0.0, g_moving.max, g_moving.draws,
                 g_callsWeapon, g_resultTrue, g_callsOther);
        g_still = g_moving = BakeStats{};
    }
    const std::uintptr_t w = PawnWeapon(pawn);
    if (w && (w != g_lastWeapon || static_cast<LONG>(now - g_nextWeaponLog) >= 0)) {
        g_lastWeapon = w;
        g_nextWeaponLog = now + 15000;
        LogWeapon(pawn, w);
    }
}

}  // namespace

bool Install(const Config& cfg, bool pipelineHooked) {
    g_cfg = cfg;
    if (!cfg.manualReload && !cfg.debugReloadProbe) return false;
    ParseLines(cfg.iniPath);
    if (cfg.manualReload)
        MLOG("reload: Weapon.ManualReload=1 -- %zu guns with [ManualReload] lines; KeepChambered=%d, Hook=%d", g_lines.size(),
             cfg.keepChambered ? 1 : 0, cfg.reloadHook ? 1 : 0);
    const bool wantHook = cfg.debugReloadProbe || (cfg.manualReload && cfg.reloadHook && pipelineHooked);
    if (cfg.manualReload && cfg.reloadHook && !pipelineHooked)
        MLOG("reload: the Draw hook or the arm bake is missing -- the game keeps its own reload (no hook)");
    if (!wantHook) return true;
    // Standing rule 4: the 59 bytes through the result's store (RELOAD-DESIGN 2.5).
    if (!patch::BytesMatch(addr::kExecHasReserveAmmo, addr::kExecHasReserveAmmoBytes, sizeof(addr::kExecHasReserveAmmoBytes))) {
        MLOG("reload: execHasReserveAmmo bytes differ -- no hook: the game keeps its own reload");
        return true;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecHasReserveAmmo),
                                              reinterpret_cast<void*>(&Hook_ExecHasReserveAmmo));
    if (!res) {
        MLOG("reload: inline hook failed (error %d) -- the game keeps its own reload", static_cast<int>(res.error().type));
        return true;
    }
    g_hook = std::move(*res);
    g_hooked = true;
    g_blockAllowed = cfg.manualReload && cfg.reloadHook && pipelineHooked;
    MLOG("reload: execHasReserveAmmo hooked at 0x%08X (%s)", static_cast<unsigned>(addr::kExecHasReserveAmmo),
         g_blockAllowed ? "blocks the game's own reload for a converted gun while the manual reload is engaged" : "count-only");
    return true;
}

float       g_gunL2W[16];
std::string g_gunKey;

bool GripNow(float (&target)[16], const float*& fingers, const char* const*& names) {
    if (!g_gripNow.grip || GetTickCount() - g_gripNow.tick > 100) return false;
    std::memcpy(target, g_gripNow.target, sizeof(target));
    fingers = &g_gripNow.grip->fingers[0][0];
    names = kGripFingers;
    return true;
}

bool PumpShift(float (&d)[3]) {
    if (!g_pumpShift.on || GetTickCount() - g_pumpShift.tick > 100) return false;
    for (int i = 0; i < 3; ++i) d[i] = g_pumpShift.d[i];
    return true;
}

bool LastGun(float (&l2w)[16], std::string& key) {
    if (g_gunKey.empty() || GetTickCount() - g_lastGunBake > 250) return false;
    std::memcpy(l2w, g_gunL2W, sizeof(l2w));
    key = g_gunKey;
    return true;
}

void OnGunBake(std::uintptr_t comp, const float* saved, float* bones, int num, const float* l2w, const float* a,
               const float* kMove, const float* carry) {
    g_lastGunBake = GetTickCount();
    std::memcpy(g_gunL2W, l2w, sizeof(g_gunL2W));
    g_gunKey = names::ClassName(names::Outer(comp));
    g_bakeFresh = true;
    if (g_cfg.manualReload) OverrideBones(comp, saved, bones, num, l2w, a, kMove, carry);
    if (!g_cfg.debugReloadProbe) return;
    ++g_bakesThisDraw;
    const int smo = names::PropertyOffset(comp, "SkeletalMesh");
    const std::uintptr_t mesh = smo >= 0 ? names::ReadPointer(comp + smo) : 0;
    if (!mesh) return;
    if (mesh != g_bakeMesh) {
        g_bakeMesh = mesh;
        g_bakeCount = 0;
    }
    if (++g_bakeCount != 10) return;
    for (std::uintptr_t m : g_meshSeen)
        if (m == mesh) return;
    g_meshSeen.push_back(mesh);
    LogBones(comp, mesh, saved, num, l2w, a);
}

// GOAL A2: an FName property of `w` (or element `elem` of a TArray<FName> property) set to None while `on`, the game's
// own value kept to put back when it goes off. Never persisted: weapons are re-created from their defaults on a load.
void SetNameNone(std::uintptr_t w, const char* prop, int elem, bool on, std::uint32_t (&saved)[2], bool& isOff,
                 const std::string& key, bool log) {
    if (on == isOff) return;
    const int o = names::PropertyOffset(w, prop);
    if (o < 0) return;
    std::uintptr_t at = w + o;
    if (elem >= 0) {
        const std::uintptr_t data = names::ReadPointer(w + o);
        const int count = *reinterpret_cast<const int*>(w + o + 4);
        if (!data || elem >= count) return;
        at = data + 8 * elem;
    }
    auto* p = reinterpret_cast<std::uint32_t*>(at);
    if (on) {
        saved[0] = p[0];
        saved[1] = p[1];
        const std::string was = names::NameAt(at);
        p[0] = 0;
        p[1] = 0;
        isOff = true;
        if (log) MLOG("reload: %s's %s%s -> %s (was %s)", key.c_str(), prop, elem >= 0 ? "[0]" : "", names::NameAt(at).c_str(), was.c_str());
    } else {
        p[0] = saved[0];
        p[1] = saved[1];
        isOff = false;
        if (log) MLOG("reload: %s's %s%s back to %s", key.c_str(), prop, elem >= 0 ? "[0]" : "", names::NameAt(at).c_str());
    }
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (g_cfg.debugReloadProbe) ProbeDraw(pawn);
    if (!g_cfg.manualReload) return;
    CheckCue();
    g_lastDraw = GetTickCount();
    g_flags = hdr ? hdr->reloadFlags : 0;
    if (pawn != g_pawn) {  // a new local pawn (death, a level load): every state starts over
        g_pawn = pawn;
        g_ws.clear();
        g_owed.clear();
        g_cuesPawn = 0;
        ++g_pawnSeq;
        if (hdr) hdr->reloadPawnSeq = g_pawnSeq;
        MLOG("reload: a new local pawn -- the manual reload's state reset");
    }
    const std::uintptr_t w = PawnWeapon(pawn);
    const GunLine* line = w ? Converted(w) : nullptr;
    WState* s = w ? &StateFor(w) : nullptr;
    int* clipP = w ? Field(w, "AmmoCount") : nullptr;
    if (s && line && clipP) {
        const int c = *clipP;
        if (s->lastClip >= 0 && c != s->lastClip) {
            if (c > s->lastClip) {  // RELOAD-DESIGN 2.3: the game filled it (an upgrade, a cheat, a load, its own reload)
                s->magIn = true;
                s->pending = false;
                s->cocked = true;
                s->half = -1;  // the game's own reload flipped its taped state (TapedMagMode)
                s->halfCount[0] = s->halfCount[1] = -1;
                s->bolt = 0;  // (a bolt action: the game rechambered and loaded it)
                s->spent = false;
                s->clipSeated = false;
                s->chamberEmpty = false;  // (a pump gun likewise)
                MLOG("reload: %s's clip rose %d -> %d without the mod -- magazine in, ready", line->key.c_str(), s->lastClip, c);
            } else if (c == 0 && line->open && s->cocked) {
                s->cocked = false;  // the last shot: an open bolt closes on the empty chamber
                MLOG("reload: %s fired empty -- the bolt is forward (rack after a new magazine)", line->key.c_str());
            } else if (c == 0 && line->ejectOnEmpty && s->magIn) {
                // GOAL A1 (the Garand): the last shot. The game throws the empty clip when its firing state ends (0.15 s
                // later, WeaponSingleFire.EndState -> EjectClip); the clip goes then (below).
                s->emptyPending = true;
            }
        }
        // GOAL A2: a bolt action's shot leaves the case in the chamber: the trigger is held until the bolt is worked. (GOAL
        // A3: a pump gun's likewise, until it is pumped. Only while the manual reload drives the gun: otherwise the game's
        // own rechamber, on then, takes the case out.)
        if (s->lastClip >= 0 && c < s->lastClip && (line->boltAction || line->pump) && !Bit(w, "bAlternateFireMode") &&
            Blocking(w)) {
            s->spent = true;
            s->chamberEmpty = false;
            if (g_cfg.debugReloadTrace)
                MLOG("reload: %s fired (clip %d -> %d) -- a spent case in: the %s must be worked", line->key.c_str(), s->lastClip,
                     c, line->pump ? "pump" : "bolt");
        }
        // (A new state for an EjectOnEmpty gun that is already empty -- after a load or a pick-up -- has no clip.)
        if (s->lastClip < 0 && c == 0 && line->ejectOnEmpty) s->magIn = false;
        if (s->emptyPending && StateName(w) != "WeaponSingleFire") {
            s->emptyPending = false;
            if (s->magIn && c == 0) {
                s->magIn = false;
                s->pending = false;
                s->gameEjected = true;
                // The ping is the blocked reload animation's first cue (the attachment has none): the mod plays it.
                const int v = VariantOf(line->key);
                const std::vector<std::string>& so = line->sndOut;
                if (!so.empty()) PlayCue(pawn, w, so[v < static_cast<int>(so.size()) ? v : 0], "the clip pings out");
                MLOG("reload: %s fired its last round -- %s", line->key.c_str(),
                     so.empty() || so[0].empty() ? "it is gone (magazine out)"
                                                : "the game threw the clip out (magazine out, the action locked back)");
            }
        }
    }
    if (g_delayedCue.on && static_cast<LONG>(GetTickCount() - g_delayedCue.due) >= 0) {
        g_delayedCue.on = false;
        if (w == g_delayedCue.w) PlayCue(pawn, w, g_delayedCue.name, g_delayedCue.what);
    }
    if (g_cfg.debugReloadTrace && w) {
        const std::string st = StateName(w);
        if (st != g_lastState) {
            MLOG("reload: trace -- %s is in state %s, clip %d", AttachKey(w).c_str(), st.c_str(), clipP ? *clipP : -1);
            g_lastState = st;
        }
    }
    // The host's events, in order (RELOAD-DESIGN 4).
    if (hdr) {
        const std::uint32_t seq = hdr->reloadEvtSeq;
        if (!g_seenInit) {
            g_seen = seq;
            g_seenInit = true;
        }
        if (seq - g_seen > 8) {
            MLOG("reload: %u events overran the ring -- skipped", seq - g_seen);
            g_seen = seq;
        }
        const std::uint32_t myHash = w ? (shared::KeyHash(AttachKey(w).c_str()) & 0xFFFFFFu) : 0;
        for (int n = 0; g_seen != seq && n < 8; ++n) {
            const std::uint32_t e = hdr->reloadEvt[g_seen % 8];
            ++g_seen;
            const std::uint32_t type = e & 0xFFu;
            if (!s || !line || (e >> 8) != myHash) {
                MLOG("reload: %s rejected -- %s", type < kEventCount ? kEventName[type] : "?",
                     !line ? "no converted gun in hand" : "it was meant for another gun");
                continue;
            }
            Apply(pawn, w, *s, *line, type);
        }
        hdr->reloadEvtAck = g_seen;
    }
    if (s && clipP) s->lastClip = *clipP;
    // GOAL A2: while the manual reload drives a bolt action, the game's own rechamber is off (the off hand works the bolt),
    // and the trigger does nothing (FiringStatesArray[0] = None) until the bolt is back down on a fresh round.
    // (GOAL A3: a pump gun likewise, until it is pumped closed on a shell; empty, the trigger is the game's: its dry click.)
    if (s && line && (line->boltAction || line->pump)) {
        const bool driven = Blocking(w);
        // Switched off (the player's toggle), the game's own rechamber and reload have the gun: our chamber and action
        // start over (a spent case of ours would hold the trigger once it is switched on again).
        if (!(g_flags & 1u) && (s->spent || s->chamberEmpty || s->bolt != 0 || s->clipSeated)) {
            s->spent = s->chamberEmpty = s->clipSeated = false;
            s->bolt = 0;
            MLOG("reload: %s -- the manual reload is off: its chamber and action start over", line->key.c_str());
        }
        SetNameNone(w, "WeaponRechamberAnim", -1, driven, s->rechamber, s->rechamberOff, line->key, true);
        const int c = clipP ? *clipP : 0;
        s->gated = driven && (line->pump ? c >= 1 && (s->spent || s->chamberEmpty || s->bolt != 0) : (s->spent || s->bolt != 0));
        SetNameNone(w, "FiringStatesArray", 0, s->gated, s->fire0, s->fireOff, line->key, g_cfg.debugReloadTrace);
    }
    // Publish (RELOAD-DESIGN 4, 5.5) only after a fresh bake of a gun: the host then counts the game's side as alive.
    const bool fresh = g_bakeFresh;
    g_bakeFresh = false;
    const std::string key = w ? AttachKey(w) : std::string();
    const bool geoOk = g_geo.ok && g_geo.key == key;
    g_geo.ok = false;
    if (!hdr || !fresh) return;
    ++hdr->reloadGeoSeq;
    _ReadWriteBarrier();
    const int* maxP = w ? Field(w, "MaxAmmoCount") : nullptr;
    std::memset(hdr->reloadKey, 0, sizeof(hdr->reloadKey));
    std::memcpy(hdr->reloadKey, key.c_str(), std::min(key.size(), sizeof(hdr->reloadKey) - 1));
    // Converted only with this Draw's geometry (a gun mid-switch, or one whose data failed, is not).
    hdr->reloadCaps = (line && geoOk ? 1u : 0u) | (geoOk && g_geo.haveBolt ? 2u : 0u) | (g_blockAllowed ? 4u : 0u) |
                      (w && Bit(w, "bAlternateFireMode") ? 8u : 0u) | (w && Blocking(w) ? 16u : 0u) |
                      (geoOk && line && TapedNow(key) ? 32u : 0u) | (geoOk && g_geo.haveHeld ? 64u : 0u) |
                      (geoOk && line && line->grabTrigger ? 128u : 0u) | (geoOk && line && line->triggerRack ? 256u : 0u) |
                      (geoOk && line && line->noGrab ? 512u : 0u) | (geoOk && line && !line->latch ? 1024u : 0u) |
                      (geoOk && line && line->boltAction && g_geo.actN >= 2 ? 4096u : 0u) |
                      (geoOk && line && line->pump && g_geo.haveBolt ? 2048u : 0u);
    if (geoOk) {
        const Vec3* pts[4] = {&g_geo.magGrab, &g_geo.magOut, &g_geo.boltGrab, &g_geo.boltBack};
        float* dst[4] = {hdr->magGrab, hdr->magOut, hdr->boltGrab, hdr->boltBack};
        for (int i = 0; i < 4; ++i) {
            dst[i][0] = pts[i]->x;
            dst[i][1] = pts[i]->y;
            dst[i][2] = pts[i]->z;
        }
        hdr->magGrabR = g_geo.magGrabR;
        hdr->magLen = g_geo.magLen;
        hdr->magSeat = g_geo.magSeat;
        hdr->actPathN = static_cast<std::uint32_t>(g_geo.actN);
        for (int i = 0; i < g_geo.actN && i < 9; ++i) {
            hdr->actPath[i][0] = g_geo.actPath[i].x;
            hdr->actPath[i][1] = g_geo.actPath[i].y;
            hdr->actPath[i][2] = g_geo.actPath[i].z;
            hdr->actPathS[i] = g_geo.actS[i];
        }
        hdr->boltTravel = g_geo.haveBolt ? g_geo.boltTravel : 0.0f;
        if (g_geo.haveHeld)
            hdr->magHeld = {g_geo.heldPos[0], g_geo.heldPos[1], g_geo.heldPos[2], g_geo.heldQuat[0], g_geo.heldQuat[1],
                            g_geo.heldQuat[2], g_geo.heldQuat[3]};
    }
    hdr->ammoClip = clipP ? *clipP : 0;
    hdr->ammoMax = maxP ? maxP[0] : 0;
    hdr->ammoReserve = w ? ReserveAvailable(pawn, w) : 0;
    std::uint32_t st = 0;
    if (s && line) {
        const int c = clipP ? *clipP : 0;
        const bool ready = Ready(*s, *line, c) && !(line->pump && (s->spent || s->chamberEmpty));
        const bool rackNeeded = line->open ? !s->cocked : (c == 0 && s->magIn && s->pending);
        st = (s->magIn && !line->boltAction && !line->pump ? 1u : 0u) | (s->pending ? 2u : 0u) | (ready ? 4u : 0u) |
             (geoOk && g_geo.heldBack ? 8u : 0u) | (rackNeeded ? 16u : 0u) | (line->open ? 32u : 0u) | (Bit(w, "bInfiniteAmmo") ? 64u : 0u);
        if (line->boltAction) {  // GOAL A2 (reloadState bits 7-15, shared_frame.hpp)
            const int m = maxP ? maxP[0] : 0;
            st |= (s->spent ? 128u : 0u) | (s->bolt == 2 ? 256u : 0u) | (c < m ? 512u : 0u) |
                  (HoldOpenFor(*line) && s->bolt == 2 && c == 0 ? 1024u : 0u) | (s->gated ? 2048u : 0u) | (s->clipSeated ? 4096u : 0u) |
                  (s->bolt == 1 ? 16384u : 0u) | (s->bolt == 3 ? 32768u : 0u);
        }
        if (line->pump) {  // GOAL A3 (reloadState: 7 spent, 8 the pump back, 9 room, 11 trigger held, 13 the chamber empty)
            const int m = maxP ? maxP[0] : 0;
            st |= (s->spent ? 128u : 0u) | (s->bolt == 2 ? 256u : 0u) | (c < m ? 512u : 0u) | (s->gated ? 2048u : 0u) |
                  (s->chamberEmpty ? 8192u : 0u);
        }
    }
    hdr->reloadState = st;
    _ReadWriteBarrier();
    ++hdr->reloadGeoSeq;
    // Blocks, now and then.
    static DWORD nextLog = 0;
    const DWORD now = GetTickCount();
    if (g_blocks && static_cast<LONG>(now - nextLog) >= 0) {
        nextLog = now + 10000;
        MLOG("reload: the game's own reload blocked %ld time(s) so far", g_blocks);
    }
}

}  // namespace mohavr::reload
