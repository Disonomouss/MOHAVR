#include "scope.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "carrier.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::scope {
using namespace script;  // Call, Obj, Int, Float (script_call.hpp)
namespace {

#include "scope_points.inc"

Config g_cfg;
bool   g_bake = false;

// The real scopes (SCOPE-DESIGN 2.6): the Springfield's M73B1 (a Weaver 330) 2.5x with a crosshair; the G43's ZF4 4x and
// the StG44's ZF4 4x with the German post and bars.
struct RealScope {
    const char*   gun;
    float         mag;
    std::uint32_t reticle;  // 0 a crosshair, 1 a post and bars
};
constexpr RealScope kReal[] = {{"Attachment_Springfield", 2.5f, 0}, {"Attachment_G43", 4.0f, 1}, {"Attachment_Stg44", 4.0f, 1}};

// The gun in hand and its scope's tube in the host's gun frame (metres: x right, y up, z back), taken while quiet.
struct State {
    std::uintptr_t gun = 0, att = 0;
    int            level = -2;
    std::string    key;
    const ScopeTube* tube = nullptr;
    bool           have = false;
    float          eye[3]{}, obj[3]{}, radius = 0.0f;
    float          minFov = 0.0f, maxFov = 0.0f;
    bool           present = false;
} g;
std::uint32_t g_lastCaps = 0xFFFFFFFFu;
// Quiet: the weapon Active for 0.3 s, no shot for 0.5 s (its kick moves the drawn gun against the hand).
int         g_flashSeen = -1;
DWORD       g_flashAt = 0, g_quietSince = 0;
std::string g_stateSeen;

bool SafeCopy(void* dst, std::uintptr_t src, std::size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// p x m (row vectors: rows X, Y, Z, origin).
void Xform(const float* p, const float* m, float* out) {
    float r[3];
    for (int j = 0; j < 3; ++j) r[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    std::memcpy(out, r, sizeof(r));
}

void RigidInverse(const float (&m)[16], float (&out)[16]) {
    float r[16] = {m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1};
    for (int j = 0; j < 3; ++j) r[12 + j] = -(m[12] * r[j] + m[13] * r[4 + j] + m[14] * r[8 + j]);
    std::memcpy(out, r, sizeof(out));
}

const ScopeTube* Tube(const std::string& key, int level) {
    for (const ScopeTube& t : kScopeTubes)
        if (key == t.gun && level >= t.level) return &t;
    return nullptr;
}

// Points on a bone of the drawn gun (in the bone's frame) in the host's gun frame -- as physical melee takes its levers
// (melee.cpp TakeLevers): the bone's live pose (SpaceBases: the game's own -- the bake puts it back after the renderer's
// copy; the StG44's scope bone mounts) x the mesh's LocalToWorld x the bake's move, the left hand's mirror and the body's
// catch-up undone, into the gun hand's controller frame (forward, right, up), un-mirrored.
bool GunFrame(std::uintptr_t att, const char* bone, const float (*local)[3], int n, float (*out)[3], const shared::Header* hdr) {
    const std::uintptr_t comp = Obj(att, "Mesh");
    const int lo = comp ? names::PropertyOffset(comp, "LocalToWorld") : -1;
    float D[16], ctrl[16], off[16], R[16], Wm[16], l2w[16], G[16], ctrlInv[16], Winv[16], B[16];
    bool offValid = false, two = false;
    if (lo < 0 || !armsik::BakedMove(comp, D) || !viewmodel::HandFrames(ctrl, off, offValid, two)) return false;
    if (!SafeCopy(l2w, comp + lo, sizeof(l2w))) return false;
    {
        const std::uintptr_t mesh = Obj(comp, "SkeletalMesh");
        const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
        const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
        const int so = names::PropertyOffset(comp, "SpaceBases");
        const std::uintptr_t bases = so >= 0 ? names::ReadPointer(comp + so) : 0;
        const int nb = so >= 0 ? static_cast<int>(ReadU32(comp + so + 4)) : 0;
        int bi = -1;
        for (int i = 0; data && i < num && i < nb && i < 256 && bi < 0; ++i)
            if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) == bone) bi = i;
        if (bi < 0 || !SafeCopy(B, bases + 64u * static_cast<std::uintptr_t>(bi), sizeof(B))) return false;
    }
    carrier::Mul16(l2w, D, G);
    const bool mirrored = viewmodel::DrawMirror(R);
    if (mirrored) {
        float t[16];
        carrier::Mul16(G, R, t);
        std::memcpy(G, t, sizeof(G));
    }
    const float live = hdr ? hdr->unitsPerMeter : 0.0f;
    const float upm = live > 1.0f && live < 1000.0f ? live : g_cfg.unitsPerMeter;
    const bool carry = g_cfg.catchUp && viewmodel::BodyMoveSinceView(Wm);
    if (carry) RigidInverse(Wm, Winv);
    RigidInverse(ctrl, ctrlInv);
    for (int i = 0; i < n; ++i) {
        float c[3], w[3], r[3];
        Xform(local[i], B, c);  // the bone's frame -> the component (mesh) space
        Xform(c, G, w);
        if (mirrored) Xform(w, R, w);
        if (carry) Xform(w, Winv, w);
        Xform(w, ctrlInv, r);  // forward, right, up (units)
        out[i][0] = (mirrored ? -r[1] : r[1]) / upm;
        out[i][1] = r[2] / upm;
        out[i][2] = -r[0] / upm;
    }
    return true;
}

bool ScopeOn(std::uintptr_t gun) {
    if (!gun || !names::IsA(gun, "EALASmallArms")) return false;
    Call c(gun, "IsScopeEnabled", true);
    return c.ok && c.Run() && c.ReturnBool();
}

void ReadParams(std::uintptr_t gun, float& mn, float& mx) {
    mn = mx = 0.0f;
    const std::uintptr_t sc = Obj(gun, "ScopeComponent");
    const int po = sc ? names::PropertyOffset(sc, "ScopeParams") : -1;
    float p[2];
    if (po >= 0 && SafeCopy(p, sc + po, sizeof(p))) {
        mn = p[0];  // ScopeTuning: MinFOV, MaxFOV, FOVChangeRate, ... (MOHAIncludeClass.uc)
        mx = p[1];
    }
}

void Publish(shared::Header* hdr, std::uint32_t caps) {
    const RealScope* real = nullptr;
    for (const RealScope& r : kReal)
        if (g.key == r.gun) real = &r;
    const std::uint32_t s = hdr->scopeSeq;
    hdr->scopeSeq = s | 1u;
    MemoryBarrier();
    hdr->scopeCaps = caps;
    std::memset(hdr->scopeKey, 0, sizeof(hdr->scopeKey));
    strncpy_s(hdr->scopeKey, sizeof(hdr->scopeKey), g.key.c_str(), _TRUNCATE);
    for (int i = 0; i < 3; ++i) {
        hdr->scopeOcular[i] = g.eye[i];
        hdr->scopeObjective[i] = g.obj[i];
    }
    hdr->scopeRadius = g.radius;
    hdr->scopeGameFov[0] = g.minFov;
    hdr->scopeGameFov[1] = g.maxFov;
    hdr->scopeRealMag = real ? real->mag : 0.0f;
    hdr->scopeReticle = real ? real->reticle : 0u;
    MemoryBarrier();
    hdr->scopeSeq = (s | 1u) + 1u;
}

}  // namespace

void Configure(const Config& cfg, bool bake) {
    g_cfg = cfg;
    g_bake = bake;
    MLOG("scope: a %d px column for the scope view while a scope is at an eye ([Scope] Enable, the menu's, is the host's)%s", cfg.scopeColumn,
         bake ? "" : "; no arm bake (Weapon.ArmIK, ViewModel=2): no scope geometry");
}

void OnDraw(shared::Header* hdr) {
    if (!hdr) return;
    // ([Scope] Column=0: no scope view, nothing for the host. [Scope] Enable is the host's switch -- the menu's.)
    if (g_cfg.scopeColumn <= 0 || !g_cfg.stereo) {
        if (g_lastCaps != 0) {
            g_lastCaps = 0;
            Publish(hdr, 0);
        }
        return;
    }
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t gun = pawn ? Obj(pawn, "Weapon") : 0, att = pawn ? Obj(pawn, "CurrentWeaponAttachment") : 0;
    const int level = gun ? Int(gun, "CurrentUpgradeLevel", -1) : -1;
    if (gun != g.gun || att != g.att || level != g.level) {
        g.gun = gun;
        g.att = att;
        g.level = level;
        g.key = att ? names::ClassName(att) : std::string();
        g.tube = Tube(g.key, level);
        g.have = false;
        ReadParams(gun, g.minFov, g.maxFov);
        if (g.tube || (gun && Obj(gun, "ScopeComponent")))
            MLOG("scope: %s at level %d -- %s, the game's zoom FOV %.0f..%.0f deg%s", g.key.c_str(), level,
                 g.tube ? "a scope tube" : "no scope tube known", g.minFov, g.maxFov, ScopeOn(gun) ? "; the scope on" : "");
    }
    std::uint32_t caps = 1u;  // (the column comes while the scope view renders)
    const bool wasPresent = g.present;
    g.present = g.tube && ScopeOn(gun);
    if (g.present) caps |= 2u;
    {
        // Taken again from a quiet gun after the scope comes back on (the StG44 re-mounts it) or the hand changes (the mirror:
        // its sideways sign).
        float R[16];
        static bool lastMirrored = false;
        const bool mirrored = viewmodel::DrawMirror(R);
        if ((g.present && !wasPresent) || mirrored != lastMirrored) g.have = false;
        lastMirrored = mirrored;
    }
    // The tube in the host's gun frame, while the gun is quiet (Active for 0.3 s, no shot for 0.5 s).
    if (g.present && g_bake) {
        const DWORD tick = GetTickCount();
        const int fo = names::PropertyOffset(pawn, "FlashCount");
        const int flash = fo >= 0 ? static_cast<int>(ReadU32(pawn + fo) & 0xFF) : -1;
        const std::string state = names::StateName(gun);
        if (flash != g_flashSeen) g_flashSeen = flash, g_flashAt = tick;
        if (state != g_stateSeen) g_stateSeen = state, g_quietSince = tick;
        const bool quiet = state == "Active" && tick - g_flashAt > 500 && tick - g_quietSince > 300;
        if (quiet) {
            const float local[2][3] = {{g.tube->eyeLocal[0], g.tube->eyeLocal[1], g.tube->eyeLocal[2]},
                                       {g.tube->objLocal[0], g.tube->objLocal[1], g.tube->objLocal[2]}};
            float out[2][3];
            if (GunFrame(att, g.tube->bone, local, 2, out, hdr)) {
                const bool first = !g.have;
                std::memcpy(g.eye, out[0], sizeof(g.eye));
                std::memcpy(g.obj, out[1], sizeof(g.obj));
                const float live = hdr->unitsPerMeter;
                g.radius = g.tube->radius / (live > 1.0f && live < 1000.0f ? live : g_cfg.unitsPerMeter);
                g.have = true;
                if (first)
                    MLOG("scope: %s's tube in the gun frame -- eyepiece %.3f %.3f %.3f m (r %.1f cm), objective %.3f %.3f %.3f m",
                         g.key.c_str(), g.eye[0], g.eye[1], g.eye[2], g.radius * 100.0f, g.obj[0], g.obj[1], g.obj[2]);
            }
        }
    }
    if (g.present && g.have) caps |= 4u;
    if (caps != g_lastCaps) {
        static int logged = 0;
        if (logged++ < 40) MLOG("scope: caps %u (column %d, scope on %d, geometry %d) -- %s", caps, caps & 1u, (caps >> 1) & 1u,
                                (caps >> 2) & 1u, g.key.empty() ? "no gun" : g.key.c_str());
        g_lastCaps = caps;
    }
    Publish(hdr, caps);
}

bool TestCommand(const wchar_t* line) {
    if (wcsncmp(line, L"mohavr scope", 12) != 0) return false;
    float mn = 0, mx = 0;
    ReadParams(g.gun, mn, mx);
    const std::uintptr_t sc = Obj(g.gun, "ScopeComponent");
    MLOG("scope: info -- %s at level %d (%s): IsScopeEnabled %d, ScopeComponent %s, ScopeParams FOV %.1f..%.1f (current %.1f), "
         "tube %s, geometry %s (eyepiece %.3f %.3f %.3f, objective %.3f %.3f %.3f m, r %.1f cm)",
         g.key.c_str(), g.level, g.gun ? names::ClassName(g.gun).c_str() : "-", ScopeOn(g.gun) ? 1 : 0,
         sc ? names::ClassName(sc).c_str() : "none", mn, mx, Float(sc, "fCurrentFOV", 0.0f), g.tube ? g.tube->bone : "none",
         g.have ? "taken" : "not yet", g.eye[0], g.eye[1], g.eye[2], g.obj[0], g.obj[1], g.obj[2], g.radius * 100.0f);
    return true;
}

}  // namespace mohavr::scope
