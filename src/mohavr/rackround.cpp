#include "rackround.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "falltrace.hpp"
#include "carrier.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "vr_view.hpp"

namespace mohavr::rackround {
using namespace script;  // Call, Obj, ReadU32 (script_call.hpp)
namespace {

Config g_cfg;
bool   g_bake = false;
std::uintptr_t g_pawn = 0;

// A round in flight (or at rest at the feet).
struct Round {
    carrier::Slot slot;
    bool        on = false;
    int         bone = -1;
    double      start = 0.0;                 // seconds (QPC)
    float       rows0[9] = {};               // the round bone's axes at the throw (X, Y, Z in the world, scaled)
    float       c0[3] = {}, v0[3] = {};      // its centre at the throw; its velocity (units/s)
    float       axis[3] = {0, 0, 1};         // the tumble's axis (unit)
    float       len = 0.0f, spin = 0.0f, g = 0.0f, floorC = 0.0f;  // floorC: the centre's height at rest
    float       tLand = 0.0f, tEnd = 0.0f;   // seconds after the throw (the slow motion applied)
    float       stopT = 1e9f;                // GOAL C3: the sideways motion stops (a wall)
    bool        landed = false;              // its impact sound played
    bool        drawnLogged = false;         // (the first bake that draws it, logged)
    std::string what;
};
Round g_round[2];

// The class defaults' meshes, found once per pawn (a level's packages decide which attachment classes are loaded).
struct Tmpl {
    std::string    key;
    std::uintptr_t comp = 0;
};
std::vector<Tmpl> g_tmpl;

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
}

// Object.FindObject(path, cls) through `ctx` (an unnumbered static native), as knife.cpp's.
std::uintptr_t FindByPath(std::uintptr_t ctx, const std::wstring& path, std::uintptr_t cls) {
    if (!ctx || !cls) return 0;
    struct FString {
        const wchar_t* data;
        int            num, max;
    } s{path.c_str(), static_cast<int>(path.size()) + 1, static_cast<int>(path.size()) + 1};
    Call find(ctx, "FindObject");
    if (!find.Set("ObjectName", &s, sizeof(s)) || !find.Set("ObjectClass", &cls, sizeof(cls)) || !find.Run()) return 0;
    return find.ReturnObject();
}

// The attachment class's default WeaponMeshComponent (MOHAGameNonNative.Default__<Attachment>.WeaponMeshComponent, a
// MOHASkeletalMeshComponent: the class defaults in the decompiled scripts, work/research/uc/Attachment_*.uc).
std::uintptr_t TemplateFor(std::uintptr_t pawn, const std::string& att) {
    for (const Tmpl& t : g_tmpl)
        if (t.key == att) return t.comp;
    const std::uintptr_t arms = Obj(pawn, "FPArms");
    const std::uintptr_t cls = arms ? names::ReadPointer(arms + addr::kObjectClass) : 0;
    const std::wstring path = L"MOHAGameNonNative.Default__" + std::wstring(att.begin(), att.end()) + L".WeaponMeshComponent";
    const std::uintptr_t t = FindByPath(pawn, path, cls);
    MLOG("rackround: the template Default__%s.WeaponMeshComponent %s (%s), mesh %s", att.c_str(), t ? "found" : "NOT FOUND",
         t ? names::ClassName(t).c_str() : "-", t ? names::Name(Obj(t, "SkeletalMesh")).c_str() : "-");
    g_tmpl.push_back({att, t});
    return t;
}

int BoneIndex(std::uintptr_t comp, const std::string& name) {
    const std::uintptr_t mesh = Obj(comp, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    for (int i = 0; data && i < num && i < 512; ++i)
        if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) == name) return i;
    return -1;
}

float Len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
void Norm3(float* v) {
    const float l = Len3(v);
    if (l > 1e-6f)
        for (int i = 0; i < 3; ++i) v[i] /= l;
}
// v turned by `ang` about the unit axis u (Rodrigues).
void Turn(const float* u, float ang, const float* v, float* out) {
    const float c = std::cos(ang), s = std::sin(ang), d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    const float x[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] * c + x[i] * s + u[i] * d * (1.0f - c);
}

// The round's bone frame (world) `t` seconds after the throw: ballistic, tumbling about its axis until it lands; at rest
// it lies flat (its length level, as it came down).
void FrameAt(const Round& r, float t, float (&m)[16]) {
    const float tt = std::min(t, r.tLand);
    float rows[9];
    for (int k = 0; k < 3; ++k) Turn(r.axis, r.spin * tt, r.rows0 + 3 * k, rows + 3 * k);
    float c[3];
    const float ts = std::min(tt, r.stopT);  // (GOAL C3: stopped by a wall)
    for (int i = 0; i < 2; ++i) c[i] = r.c0[i] + r.v0[i] * ts;
    c[2] = r.c0[2] + r.v0[2] * tt;
    c[2] -= 0.5f * r.g * tt * tt;
    if (t >= r.tLand) {
        c[2] = r.floorC;
        // Flat: its length level (the direction it had), the other two axes about it, sized as they were.
        const float lx = Len3(rows), ly = Len3(rows + 3), lz = Len3(rows + 6);
        float z[3] = {rows[6], rows[7], 0.0f};
        if (Len3(z) < 1e-4f) z[0] = 1.0f;
        Norm3(z);
        float x[3] = {-z[1], z[0], 0.0f};          // level, across it
        float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};  // z x x
        // The same handedness as thrown (the mirror world's frames may be reflected).
        const float* r0 = r.rows0;
        const float det0 = r0[0] * (r0[4] * r0[8] - r0[5] * r0[7]) - r0[1] * (r0[3] * r0[8] - r0[5] * r0[6]) +
                           r0[2] * (r0[3] * r0[7] - r0[4] * r0[6]);
        if (det0 < 0.0f)
            for (float& e : y) e = -e;
        for (int i = 0; i < 3; ++i) {
            rows[i] = x[i] * lx;
            rows[3 + i] = y[i] * ly;
            rows[6 + i] = z[i] * lz;
        }
    }
    const float o[16] = {rows[0], rows[1], rows[2], 0, rows[3], rows[4], rows[5], 0, rows[6], rows[7], rows[8], 0,
                         c[0] - 0.5f * r.len * rows[6], c[1] - 0.5f * r.len * rows[7], c[2] - 0.5f * r.len * rows[8], 1};
    std::memcpy(m, o, sizeof(m));
}

float Elapsed(const Round& r) { return static_cast<float>(Now() - r.start) / g_cfg.debugReloadSlowMo; }

void Reset() {
    for (Round& r : g_round) r = Round{};  // (the old pawn's clones go with it)
    g_tmpl.clear();
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
}

bool Throw(std::uintptr_t pawn, const Source& src, const float* rows, const float* port, const float* vel, float floorZ,
           float upm) {
    if (!g_bake || !pawn || src.attachment.empty()) {
        MLOG("rackround: no round drawn (%s)", !g_bake ? "no arm bake" : !pawn ? "no pawn" : "no source");
        return false;
    }
    if (pawn != g_pawn) {
        g_pawn = pawn;
        Reset();
    }
    const std::uintptr_t t = TemplateFor(pawn, src.attachment);
    if (!t) return false;
    const int bone = BoneIndex(t, src.bone);
    if (bone < 0) {
        MLOG("rackround: %s's mesh %s has no bone %s", src.attachment.c_str(), names::Name(Obj(t, "SkeletalMesh")).c_str(),
             src.bone.c_str());
        return false;
    }
    // A free slot, else the oldest round goes.
    Round* r = nullptr;
    for (Round& q : g_round)
        if (!q.on && !r) r = &q;
    if (!r) r = g_round[0].start <= g_round[1].start ? &g_round[0] : &g_round[1];
    carrier::Detach(r->slot, "rackround", false);
    Round n;
    n.bone = bone;
    n.what = src.attachment + "." + src.bone;
    n.len = src.len;
    // Chambered: along the gun's bore (mesh +Z), base to the back; sized as the drawn gun (and the source's scale).
    for (int k = 0; k < 3; ++k) {
        n.rows0[k] = rows[k] * src.scaleWidth;
        n.rows0[3 + k] = rows[4 + k] * src.scaleWidth;
        n.rows0[6 + k] = rows[8 + k] * src.scaleLen;
    }
    for (int i = 0; i < 3; ++i) n.c0[i] = port[12 + i];
    // Out of the port: to the gun's right, RackRoundUp of that up (1 = 45 deg), a little back; plus the port's own speed.
    float fwd[3] = {port[0], port[1], port[2]}, right[3] = {port[4], port[5], port[6]}, up[3] = {port[8], port[9], port[10]};
    Norm3(fwd);
    Norm3(right);
    Norm3(up);
    float dir[3];
    for (int i = 0; i < 3; ++i) dir[i] = right[i] + g_cfg.rackRoundUp * up[i] - 0.2f * fwd[i];
    Norm3(dir);
    for (int i = 0; i < 3; ++i) n.v0[i] = dir[i] * g_cfg.rackRoundSpeed * upm + (vel ? vel[i] : 0.0f);
    // It tumbles end over end about the port's up (tilted a little forward).
    for (int i = 0; i < 3; ++i) n.axis[i] = up[i] + 0.3f * fwd[i];
    Norm3(n.axis);
    n.spin = g_cfg.rackRoundSpin;
    n.g = 9.8f * upm;
    // At rest its centre is its half-width above the floor (a round's radius is ~0.6 of its bone's units across).
    // GOAL C3: the throw's path traced into the world (the round lives in the real world): a table or a wall stops it.
    if (g_cfg.fallTrace) {
        const float p0[3] = {n.c0[0], n.c0[1], n.c0[2]}, v0[3] = {n.v0[0], n.v0[1], n.v0[2]};
        const falltrace::Result ft = falltrace::Trace(pawn, p0, v0, n.g, floorZ, nullptr);
        if (ft.wall || ft.top)
            MLOG("rackround: the round's flight meets %s after %.2f s -- it rests at %.0f (the feet at %.0f)",
                 ft.top ? "something under it" : "a wall", ft.stopT, ft.floorZ, floorZ);
        floorZ = ft.floorZ;
        n.stopT = ft.stopT;
    }
    n.floorC = floorZ + 0.6f * Len3(n.rows0);
    const float z0 = n.c0[2], vz = n.v0[2];
    const float disc = vz * vz + 2.0f * n.g * (z0 - n.floorC);
    n.tLand = z0 > n.floorC && disc >= 0.0f ? (vz + std::sqrt(disc)) / n.g : 0.0f;
    n.tLand = std::min(n.tLand, 3.0f);
    n.tEnd = n.tLand + g_cfg.rackRoundRest;
    n.start = Now();
    n.on = true;
    *r = n;  // (known to the bake before its first update)
    if (!carrier::AttachTemplate(r->slot, pawn, t, pawn, "rackround", false, true, 600.0f)) {
        MLOG("rackround: %s -- the carrier could not be attached", n.what.c_str());
        r->on = false;
        return false;
    }
    float head[3] = {0, 0, 0}, yaw = 0.0f, hu = 0.0f;
    view::HeadInWorld(head, yaw, hu);
    MLOG("rackround: a live round (%s, bone #%d, %.2f u long, x%.2f / %.2f) thrown from %.1f %.1f %.1f at %.0f %.0f %.0f u/s "
         "(the port's own %.0f %.0f %.0f; the port's right %.2f %.2f %.2f, up %.2f %.2f %.2f; the head at %.1f %.1f %.1f); it "
         "lands %.2f s later at z %.1f, gone %.1f s after that (carrier %s)",
         n.what.c_str(), bone, n.len, src.scaleLen, src.scaleWidth, n.c0[0], n.c0[1], n.c0[2], n.v0[0], n.v0[1], n.v0[2],
         vel ? vel[0] : 0.0f, vel ? vel[1] : 0.0f, vel ? vel[2] : 0.0f, right[0], right[1], right[2], up[0], up[1], up[2], head[0],
         head[1], head[2], n.tLand * g_cfg.debugReloadSlowMo, n.floorC, g_cfg.rackRoundRest * g_cfg.debugReloadSlowMo,
         names::Name(r->slot.comp).c_str());
    return true;
}

void OnDraw() {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (pawn != g_pawn) {
        g_pawn = pawn;
        Reset();
        return;
    }
    for (Round& r : g_round) {
        if (!r.on) continue;
        const float t = Elapsed(r);
        if (!r.landed && t >= r.tLand) {
            r.landed = true;
            // The case's landing sound of the gun in hand (SmallArmsAttachment.PlayShellImpactSound: the floor's material
            // at the feet).
            const std::uintptr_t att = Obj(pawn, "CurrentWeaponAttachment");
            Call snd(att, "PlayShellImpactSound", true);
            const bool played = att && snd.Run();
            MLOG("rackround: %s landed (%s)", r.what.c_str(), played ? "its impact sound played" : "silent: no attachment sound");
        }
        if (t >= r.tEnd) {
            carrier::Detach(r.slot, "rackround", false);
            r.on = false;
            MLOG("rackround: %s gone", r.what.c_str());
        }
    }
}

bool IsCarrier(std::uintptr_t comp) {
    if (!comp) return false;
    for (const Round& r : g_round)
        if (carrier::Component(r.slot) == comp) return true;
    return false;
}

bool BoneFrame(std::uintptr_t comp, int& bone, float (&world)[16]) {
    for (Round& r : g_round) {
        if (!comp || carrier::Component(r.slot) != comp) continue;
        if (!r.on) return false;
        const float t = Elapsed(r);
        if (t >= r.tEnd) return false;
        bone = r.bone;
        FrameAt(r, t, world);
        if (!r.drawnLogged) {
            r.drawnLogged = true;
            MLOG("rackround: the bake draws %s (bone #%d) at %.1f %.1f %.1f, its length along %.2f %.2f %.2f", r.what.c_str(), bone,
                 world[12], world[13], world[14], world[8], world[9], world[10]);
        }
        return true;
    }
    return false;
}

std::string CheckSource(std::uintptr_t pawn, const Source& src) {
    const std::uintptr_t arms = Obj(pawn, "FPArms");
    const std::uintptr_t cls = arms ? names::ReadPointer(arms + addr::kObjectClass) : 0;
    const std::wstring path = L"MOHAGameNonNative.Default__" + std::wstring(src.attachment.begin(), src.attachment.end()) + L".WeaponMeshComponent";
    const std::uintptr_t t = FindByPath(pawn, path, cls);
    if (!t) return "NOT FOUND (the template)";
    if (!Obj(t, "SkeletalMesh")) return "NOT FOUND (no mesh on the template)";
    if (BoneIndex(t, src.bone) < 0) return "NOT FOUND (no bone " + src.bone + ")";
    return "found (" + names::Name(Obj(t, "SkeletalMesh")) + ")";
}

}  // namespace mohavr::rackround
