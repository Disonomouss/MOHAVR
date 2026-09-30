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
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"

namespace mohavr::reload {
namespace {

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
    bool                     open = false;
    int                      minUpgrade = 0;
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
};
std::vector<GunLine> g_lines;

const GunLine* LineFor(const std::string& key) {
    for (const GunLine& l : g_lines)
        if (l.key == key) return &l;
    return nullptr;
}

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
    std::vector<wchar_t> keys(8192);
    const DWORD n = GetPrivateProfileStringW(L"ManualReload", nullptr, L"", keys.data(), static_cast<DWORD>(keys.size()), ini.c_str());
    std::vector<std::pair<std::string, std::string>> refs;
    for (const wchar_t* k = keys.data(); k < keys.data() + n && *k; k += wcslen(k) + 1) {
        const std::string key = Narrow(k);
        if (key.rfind("Attachment_", 0) != 0) continue;
        wchar_t v[1024] = L"";
        GetPrivateProfileStringW(L"ManualReload", k, L"", v, 1024, ini.c_str());
        const std::string line = Narrow(v);
        const size_t dot = key.find('.');
        if (dot != std::string::npos) {
            if (key.substr(dot) == ".Ref") refs.push_back({key.substr(0, dot), line});
            continue;
        }
        GunLine g;
        g.key = key;
        g.open = Token(line, "Action") == "open";
        const std::string mu = Token(line, "MinUpgrade");
        g.minUpgrade = mu.empty() ? 0 : atoi(mu.c_str());
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
        g_lines.push_back(g);
    }
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
    const GunLine* l = LineFor(AttachKey(w));
    if (!l) return nullptr;
    for (const std::string& k : g_failedKeys)
        if (k == l->key) return nullptr;
    const int* up = Field(w, "CurrentUpgradeLevel");
    if (up && *up < l->minUpgrade) return nullptr;
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

const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP"};

void Apply(std::uintptr_t pawn, std::uintptr_t w, WState& s, const GunLine& l, std::uint32_t type) {
    int* clipP = Field(w, "AmmoCount");
    const int* maxP = Field(w, "MaxAmmoCount");
    if (!clipP || !maxP) return;
    const int c0 = *clipP, m = maxP[0];
    const int r0 = ReserveAvailable(pawn, w);
    int& c = *clipP;
    switch (type) {
    case shared::kReloadEject: {
        if (!s.magIn) break;
        const int keep = (!l.open && g_cfg.keepChambered) ? std::min(c, 1) : 0;
        const int k = c;
        c = keep;
        ToReserve(pawn, w, k - keep);
        s.magIn = false;
        s.pending = false;
        s.heldRounds = k - keep;
        break;
    }
    case shared::kReloadTake:
        if (s.magIn) break;
        s.heldRounds = Bit(w, "bInfiniteAmmo") ? m : std::min(m, ReserveAvailable(pawn, w));
        break;
    case shared::kReloadInsert:
        if (s.magIn) break;
        s.magIn = true;
        if (Ready(s, l, c)) {
            c += FromReserve(pawn, w, std::max(m - c, 0));
            s.pending = false;
        } else {
            s.pending = Bit(w, "bInfiniteAmmo") || ReserveAvailable(pawn, w) > 0;
        }
        s.heldRounds = 0;
        break;
    case shared::kReloadRack:
        if (!l.open) {
            if (c == 0 && s.magIn && s.pending) {
                c = FromReserve(pawn, w, m);
                s.pending = false;
            }
        } else if (!s.cocked) {
            s.cocked = true;
            if (s.magIn && s.pending) {
                c = FromReserve(pawn, w, m);
                s.pending = false;
            }
        }
        break;
    case shared::kReloadDrop:
        s.heldRounds = 0;
        break;
    default:
        break;
    }
    MLOG("reload: %s %s: clip %d -> %d, reserve %d -> %d (owed %d); magazine %s%s%s", type < 6 ? kEventName[type] : "?",
         l.key.c_str(), c0, c, r0, ReserveAvailable(pawn, w), OwedFor(AmmoClassOf(w)), s.magIn ? "in" : "out",
         s.pending ? ", pending (rack to feed)" : "", l.open ? (s.cocked ? ", cocked" : ", bolt forward") : "");
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
        if (pn != "Root" && pn != "RootOffset") {
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
    if (!l.topRound.empty()) {
        r.top = find(l.topRound);
        if (r.top < 0 && why.empty()) why = "no top-round bone " + l.topRound;
    }
    r.ok = why.empty();
    if (r.ok) {
        MLOG("reload: %s on %s -- %zu magazine bone(s), %zu action bone(s)%s; the RefSkeleton check passed", l.key.c_str(),
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
    int         mag = -1, hold = -1, top = -1, rack = -1;
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
    if (magState == 2 && !rf.magValid) magState = 3;
    // The visible variant (hidden upgrade parts have a zeroed 3x3) and its data.
    int v = -1;
    for (size_t k = 0; k < r->mag.size() && v < 0; ++k) {
        const int j = r->mag[k];
        if (j >= 0 && j < num && std::fabs(Det3(saved + 16 * j)) > 1e-3f) v = static_cast<int>(k);
    }
    const Vec3 outMesh = Norm(PerVariant(l->magOut, v, Vec3{0, 1, 0}));
    const Vec3 grabMesh = PerVariant(l->magGrab, v, Vec3{0, 0, 0});
    const float magR = PerVariant(l->magR, v, 7.0f);
    const Vec3 grabW = Xform(grabMesh, 1.0f, a);
    float pullCm = 0.0f, heldGap = -1.0f, heldTurn = -1.0f;
    if (magState == 1) {
        // Grabbed: the group slides out along the magazine's way out (mesh space), by the host's pull.
        const float scale = Len(Xform(outMesh, 0.0f, a));
        const float dist = scale > 1e-4f ? rf.view.magPull * upm / scale : 0.0f;
        pullCm = rf.view.magPull * 100.0f;
        auto slide = [&](int j) {
            if (j < 0 || j >= num) return;
            float m[16];
            std::memcpy(m, saved + 16 * j, sizeof(m));
            m[12] += outMesh.x * dist;
            m[13] += outMesh.y * dist;
            m[14] += outMesh.z * dist;
            Draw(bones, j, m, kMove);
        };
        for (int j : r->mag) slide(j);
        slide(r->top);  // the top round sits in the magazine's lips
    } else if (magState == 2) {
        // In the off hand: moved rigidly from the in-gun grab frame (G's axes at the grab point) to the held one.
        float fgrab[16], fgrabInv[16], fheld[16], invL2W[16], m1[16], m2[16], hold[16];
        std::memcpy(fgrab, G, sizeof(fgrab));
        fgrab[12] = grabW.x;
        fgrab[13] = grabW.y;
        fgrab[14] = grabW.z;
        AffineInverse(fgrab, fgrabInv);
        Mul(rf.magFrame, carry, fheld);
        AffineInverse(l2w, invL2W);
        Mul(a, fgrabInv, m1);
        Mul(m1, fheld, m2);
        Mul(m2, invL2W, hold);
        for (int j : r->mag)
            if (j >= 0 && j < num) Mul(saved + 16 * j, hold, bones + 16 * j);
        const float dx = fheld[12] - off[12], dy = fheld[13] - off[13], dz = fheld[14] - off[14];
        heldGap = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0f / upm;
        // How far the held frame is turned from the seated one (0 when the off hand points like the gun hand).
        float tr = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) tr += fgrab[i * 4 + k] * fheld[i * 4 + k];
        heldTurn = std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
    } else if (magState == 3) {
        for (int j : r->mag)
            if (j >= 0 && j < num) Collapse(bones, j, saved, kMove);
    }
    // The action: held at its empty position, and drawn back by the host's rack while the off hand holds it
    // (RELOAD-DESIGN 5.3).
    const bool hold = l->emptyCue && (l->open ? !s.cocked : c == 0);
    const bool racking = hostState && (rf.view.flags & 8u);
    float zGame = 0.0f, zDrawn = 0.0f, zHeld = 0.0f;
    const int b = !r->bolt.empty() ? r->bolt[0] : -1;
    if (b >= 0 && b < num) {
        const float zs = saved[16 * b + 14], zIdle = l->boltZ[0], zEmpty = l->boltZ[1];
        const float zh = hold ? (zEmpty < zIdle ? std::min(zs, zEmpty) : std::max(zs, zEmpty)) : zs;
        const float zd = racking ? zh + std::clamp(rf.view.rack, 0.0f, 1.0f) * (l->boltZ[2] - zh) : zh;
        zGame = zs;
        zHeld = zh;
        zDrawn = zd;
        if (zd != zs) {
            float m[16];
            std::memcpy(m, saved + 16 * b, sizeof(m));
            m[14] = zd;
            Draw(bones, b, m, kMove);
            if (r->bolt.size() > 1 && r->bolt[1] >= 0 && r->bolt[1] < num && l->haveBolt2Z) {
                float m2[16];
                std::memcpy(m2, saved + 16 * r->bolt[1], sizeof(m2));
                m2[14] = SecondZ(*l, zd);
                Draw(bones, r->bolt[1], m2, kMove);
            }
        }
    }
    // The top round: only while the magazine is in and has rounds (RELOAD-DESIGN 5.4); it slides with a grabbed one.
    int topShown = -1;
    if (r->top >= 0 && r->top < num) {
        const bool shown = (magState == 0 || magState == 1) && (l->open ? (c >= 1 || s.pending) : (c >= 2 || s.pending));
        if (!shown) Collapse(bones, r->top, saved, kMove);
        topShown = shown ? 1 : 0;
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
        g_geo.haveBolt = b >= 0 && b < num;
        if (g_geo.haveBolt) {
            const Vec3 p{saved[16 * b + 12] + l->boltGrab.x, saved[16 * b + 13] + l->boltGrab.y, zHeld + l->boltGrab.z};
            g_geo.boltGrab = hostPoint(Xform(p, 1.0f, a));
            g_geo.boltBack = Norm(hostDir(Xform(Vec3{0, 0, -1}, 0.0f, a)));
            g_geo.boltTravel = Len(Xform(Vec3{0, 0, l->boltZ[2] - zHeld}, 0.0f, a)) / upm;
            g_geo.heldBack = hold && std::fabs(l->boltZ[2] - zHeld) < 2.0f;  // the empty hold only (not a shot's recoil)
        }
    }
    const DWORD now = GetTickCount();
    if (g_cfg.debugReloadTrace && (key != g_trace.key || magState != g_trace.mag || (hold ? 1 : 0) != g_trace.hold ||
                                   topShown != g_trace.top || (racking ? 1 : 0) != g_trace.rack ||
                                   ((magState == 1 || magState == 2 || racking) && static_cast<LONG>(now - g_trace.next) >= 0))) {
        g_trace.key = key;
        g_trace.mag = magState;
        g_trace.hold = hold ? 1 : 0;
        g_trace.top = topShown;
        g_trace.rack = racking ? 1 : 0;
        g_trace.next = now + 1000;
        static const char* kMag[] = {"in the gun", "grabbed", "in the off hand", "hidden (out)"};
        char extra[96] = "";
        if (magState == 1) snprintf(extra, sizeof(extra), " (pulled %.1f cm)", pullCm);
        if (magState == 2)
            snprintf(extra, sizeof(extra), " (its grab point %.1f cm from the off controller, turned %.0f deg from seated)",
                     heldGap, heldTurn);
        MLOG("reload: trace -- drawn %s: magazine %s%s, action %s%s (game Z %.2f -> drawn %.2f), top round %s; host %s", key.c_str(),
             kMag[magState], extra, hold ? "held empty" : "the game's", racking ? ", racked by the off hand" : "", zGame, zDrawn,
             topShown < 0 ? "none" : topShown ? "shown" : "hidden", hostState ? "drives it" : "not driving");
        if (v >= 0 && r->mag[v] >= 0 && r->mag[v] < num) {
            const float* m = saved + 16 * r->mag[v];
            MLOG("reload: trace -- the visible magazine %s (variant %d): axes %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f, at %.2f %.2f %.2f",
                 l->mag[v].c_str(), v, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
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

void OnGunBake(std::uintptr_t comp, const float* saved, float* bones, int num, const float* l2w, const float* a,
               const float* kMove, const float* carry) {
    g_lastGunBake = GetTickCount();
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

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (g_cfg.debugReloadProbe) ProbeDraw(pawn);
    if (!g_cfg.manualReload) return;
    g_lastDraw = GetTickCount();
    g_flags = hdr ? hdr->reloadFlags : 0;
    if (pawn != g_pawn) {  // a new local pawn (death, a level load): every state starts over
        g_pawn = pawn;
        g_ws.clear();
        g_owed.clear();
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
                MLOG("reload: %s's clip rose %d -> %d without the mod -- magazine in, ready", line->key.c_str(), s->lastClip, c);
            } else if (c == 0 && line->open && s->cocked) {
                s->cocked = false;  // the last shot: an open bolt closes on the empty chamber
                MLOG("reload: %s fired empty -- the bolt is forward (rack after a new magazine)", line->key.c_str());
            }
        }
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
                MLOG("reload: %s rejected -- %s", type < 6 ? kEventName[type] : "?",
                     !line ? "no converted gun in hand" : "it was meant for another gun");
                continue;
            }
            Apply(pawn, w, *s, *line, type);
        }
        hdr->reloadEvtAck = g_seen;
    }
    if (s && clipP) s->lastClip = *clipP;
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
                      (w && Bit(w, "bAlternateFireMode") ? 8u : 0u) | (w && Blocking(w) ? 16u : 0u);
    if (geoOk) {
        const Vec3* pts[4] = {&g_geo.magGrab, &g_geo.magOut, &g_geo.boltGrab, &g_geo.boltBack};
        float* dst[4] = {hdr->magGrab, hdr->magOut, hdr->boltGrab, hdr->boltBack};
        for (int i = 0; i < 4; ++i) {
            dst[i][0] = pts[i]->x;
            dst[i][1] = pts[i]->y;
            dst[i][2] = pts[i]->z;
        }
        hdr->magGrabR = g_geo.magGrabR;
        hdr->boltTravel = g_geo.haveBolt ? g_geo.boltTravel : 0.0f;
    }
    hdr->ammoClip = clipP ? *clipP : 0;
    hdr->ammoMax = maxP ? maxP[0] : 0;
    hdr->ammoReserve = w ? ReserveAvailable(pawn, w) : 0;
    std::uint32_t st = 0;
    if (s && line) {
        const int c = clipP ? *clipP : 0;
        const bool ready = Ready(*s, *line, c);
        const bool rackNeeded = line->open ? !s->cocked : (c == 0 && s->magIn && s->pending);
        st = (s->magIn ? 1u : 0u) | (s->pending ? 2u : 0u) | (ready ? 4u : 0u) | (geoOk && g_geo.heldBack ? 8u : 0u) |
             (rackNeeded ? 16u : 0u) | (line->open ? 32u : 0u) | (Bit(w, "bInfiniteAmmo") ? 64u : 0u);
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
