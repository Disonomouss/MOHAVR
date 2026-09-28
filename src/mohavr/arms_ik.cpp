#include "arms_ik.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"
#include "viewmodel.hpp"
#include "vr_view.hpp"

namespace mohavr::armsik {
namespace {

// Unreal FMatrix: row vectors (p' = p * M), rows = X/Y/Z axes then the origin.
struct M4 { float m[4][4]; };
struct V3 { float x, y, z; };

M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
// Inverse of an affine matrix (a 3x3 block, possibly scaled, and a translation row).
M4 AffineInverse(const M4& a) {
    const float (&m)[4][4] = a.m;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    M4 r{};
    if (std::fabs(det) < 1e-12f) return r;
    const float id = 1.0f / det;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    for (int j = 0; j < 3; ++j) r.m[3][j] = -(m[3][0] * r.m[0][j] + m[3][1] * r.m[1][j] + m[3][2] * r.m[2][j]);
    r.m[3][3] = 1.0f;
    return r;
}
V3 Origin(const M4& a) { return {a.m[3][0], a.m[3][1], a.m[3][2]}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Unit(V3 a) { const float l = Len(a); return l > 1e-6f ? Scale(a, 1.0f / l) : V3{0, 0, 0}; }

// The rigid move (row-vector matrix) that turns direction `from` onto `to` about `pivotOld` and puts that pivot at
// `pivotNew`: x' = (x - pivotOld) R + pivotNew.
M4 SegmentMove(V3 pivotOld, V3 from, V3 pivotNew, V3 to) {
    const V3 a = Unit(from), b = Unit(to);
    const V3 v = Cross(a, b);
    const float c = Dot(a, b);
    // Column-vector Rodrigues for a -> b, then transposed for row vectors.
    float rc[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (c > -0.9999f) {
        const float k = 1.0f / (1.0f + c);
        const float vx[3][3] = {{0, -v.z, v.y}, {v.z, 0, -v.x}, {-v.y, v.x, 0}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float vv = 0.0f;
                for (int t = 0; t < 3; ++t) vv += vx[i][t] * vx[t][j];
                rc[i][j] += vx[i][j] + vv * k;
            }
    }
    M4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = rc[j][i];
    const V3 po{pivotOld.x * r.m[0][0] + pivotOld.y * r.m[1][0] + pivotOld.z * r.m[2][0],
                pivotOld.x * r.m[0][1] + pivotOld.y * r.m[1][1] + pivotOld.z * r.m[2][1],
                pivotOld.x * r.m[0][2] + pivotOld.y * r.m[1][2] + pivotOld.z * r.m[2][2]};
    r.m[3][0] = pivotNew.x - po.x;
    r.m[3][1] = pivotNew.y - po.y;
    r.m[3][2] = pivotNew.z - po.z;
    r.m[3][3] = 1.0f;
    return r;
}

Config g_cfg;
SafetyHookInline g_hook;

// Bone indices of the arms mesh, found by name (once per mesh).
struct Side {
    int hand = -1, fore = -1, arm = -1, clav = -1;
    std::vector<int> foreGroup, armGroup;  // bones moved with the forearm / the upper arm
};
struct Rig {
    std::uintptr_t mesh = 0;
    bool ok = false;
    Side side[2];            // right, left
    std::vector<int> body;   // bones put back at the body (root .. legs, the clavicles)
} g_rig;

bool Resolve(std::uintptr_t mesh, int bones) {
    g_rig = Rig{};
    g_rig.mesh = mesh;
    const std::uintptr_t data = names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton);
    const int num = static_cast<int>(names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton + 4));
    if (!data || num != bones) return false;
    std::vector<std::string> n(num);
    for (int i = 0; i < num; ++i) n[i] = names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride);
    auto find = [&](const std::string& s) {
        for (int i = 0; i < num; ++i)
            if (n[i] == s) return i;
        return -1;
    };
    const char* prefix[2] = {"Right", "Left"};
    for (int s = 0; s < 2; ++s) {
        Side& sd = g_rig.side[s];
        const std::string p = prefix[s];
        sd.hand = find(p + "Hand");
        sd.fore = find(p + "ForeArm");
        sd.arm = find(p + "Arm");
        sd.clav = find(p + "Shoulder");
        if (sd.hand < 0 || sd.fore < 0 || sd.arm < 0 || sd.clav < 0) return false;
        sd.foreGroup.push_back(sd.fore);
        sd.armGroup.push_back(sd.arm);
        for (int i = 0; i < num; ++i) {
            if (n[i].rfind(p + "ForeArmRoll", 0) == 0) sd.foreGroup.push_back(i);
            if (n[i] == p + "ArmRoll") sd.armGroup.push_back(i);
        }
        g_rig.body.push_back(sd.clav);
    }
    // The body: everything that isn't a hand, a finger, an arm bone or a weapon prop.
    for (int i = 0; i < num; ++i) {
        const std::string& s = n[i];
        const bool handSide = s.find("Hand") != std::string::npos || s.find("Arm") != std::string::npos ||
                              s.find("Shoulder") != std::string::npos || s.find("Prop") != std::string::npos;
        if (!handSide) g_rig.body.push_back(i);
    }
    g_rig.ok = true;
    MLOG("armik: rig of %s -- %d bones; right hand %d forearm %d arm %d clavicle %d (+%zu/+%zu roll), left %d %d %d %d, %zu body",
         names::Name(mesh).c_str(), num, g_rig.side[0].hand, g_rig.side[0].fore, g_rig.side[0].arm, g_rig.side[0].clav,
         g_rig.side[0].foreGroup.size() - 1, g_rig.side[0].armGroup.size() - 1, g_rig.side[1].hand, g_rig.side[1].fore,
         g_rig.side[1].arm, g_rig.side[1].clav, g_rig.body.size());
    return true;
}

// The arms' pose before we touched it (restored after the render copy).
std::vector<M4> g_saved;

bool Apply(std::uintptr_t comp) {
    if (!g_cfg.armIK) return false;
    const float fov = *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov);
    if (fov == 0.0f) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn || names::Outer(comp) != pawn) return false;  // only the player's arms (the gun's Outer is the weapon)
    float d[16], dInv[16];
    if (!viewmodel::CurrentMove(d, dInv)) return false;
    const int sbo = names::PropertyOffset(comp, "SpaceBases"), l2wo = names::PropertyOffset(comp, "LocalToWorld"),
              smo = names::PropertyOffset(comp, "SkeletalMesh");
    if (sbo < 0 || l2wo < 0 || smo < 0) return false;
    auto* bones = reinterpret_cast<M4*>(names::ReadPointer(comp + sbo));
    const int num = static_cast<int>(names::ReadPointer(comp + sbo + 4));
    const std::uintptr_t mesh = names::ReadPointer(comp + smo);
    if (!bones || num <= 0 || !mesh) return false;
    if (mesh != g_rig.mesh && !Resolve(mesh, num)) {
        static int logged = 0;
        if (logged++ < 3) MLOG("armik: %s has no arm bones this rig expects -- no IK", names::Name(mesh).c_str());
        return false;
    }
    if (!g_rig.ok) return false;

    M4 l2w, D, Dinv;
    std::memcpy(l2w.m, reinterpret_cast<const void*>(comp + l2wo), sizeof(l2w.m));
    std::memcpy(D.m, d, sizeof(D.m));
    std::memcpy(Dinv.m, dInv, sizeof(Dinv.m));
    const M4 A = Mul(l2w, D);             // drawn: bone * A
    const M4 Ainv = AffineInverse(A);
    g_saved.assign(bones, bones + num);

    // The shoulders: the game's rig has them at eye height ~25 units behind the eye and off-centre (fine flat, wrong
    // in VR), so they're anchored to the tracked head instead: Weapon.ShoulderWidth apart, ShoulderDrop below the
    // eyes, ShoulderBack behind, turned with the body. The torso moves with them (translated by their mean shift).
    V3 bodyShoulder[2], anchor[2];
    for (int s = 0; s < 2; ++s) bodyShoulder[s] = anchor[s] = Origin(Mul(g_saved[g_rig.side[s].arm], l2w));
    float head[3], yaw = 0.0f, upm = 100.0f;
    if (view::HeadInWorld(head, yaw, upm)) {
        const float k = upm / 100.0f;  // cm -> units
        const V3 fwd{std::cos(yaw), std::sin(yaw), 0.0f}, right{-std::sin(yaw), std::cos(yaw), 0.0f};
        const V3 base = Sub(Sub(V3{head[0], head[1], head[2]}, V3{0.0f, 0.0f, g_cfg.shoulderDrop * k}), Scale(fwd, g_cfg.shoulderBack * k));
        anchor[0] = Add(base, Scale(right, 0.5f * g_cfg.shoulderWidth * k));
        anchor[1] = Sub(base, Scale(right, 0.5f * g_cfg.shoulderWidth * k));
    }
    const V3 shift = Scale(Add(Sub(anchor[0], bodyShoulder[0]), Sub(anchor[1], bodyShoulder[1])), 0.5f);
    auto translate = [](V3 t) {
        M4 r{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {t.x, t.y, t.z, 1}}};
        return r;
    };
    // Body: drawn where the game has it, moved with the shoulders: K = L2W * T(shift) * A^-1 (clavicles below).
    const M4 kBody = Mul(Mul(l2w, translate(shift)), Ainv);
    for (int i : g_rig.body) bones[i] = Mul(g_saved[i], kBody);

    for (int si = 0; si < 2; ++si) {
        const Side& s = g_rig.side[si];
        const V3 wrist = Origin(Mul(g_saved[s.hand], A));      // with the gun
        const V3 elbowOld = Origin(Mul(g_saved[s.fore], A));
        const V3 shoulderOld = Origin(Mul(g_saved[s.arm], A));
        V3 shoulder = anchor[si];
        const float lu = Len(Sub(elbowOld, shoulderOld)), lf = Len(Sub(wrist, elbowOld));
        if (lu < 1e-3f || lf < 1e-3f) continue;
        static DWORD nextLog[2] = {0, 0};
        if (static_cast<LONG>(GetTickCount() - nextLog[si]) >= 0) {
            nextLog[si] = GetTickCount() + 5000;
            MLOG("armik: %s arm: upper %.1f fore %.1f, shoulder->wrist %.1f units (shoulder %.1f %.1f %.1f, wrist %.1f %.1f %.1f)",
                 si ? "left" : "right", lu, lf, Len(Sub(wrist, shoulder)), shoulder.x, shoulder.y, shoulder.z, wrist.x, wrist.y,
                 wrist.z);
        }
        // Two-bone solve: shoulder -> elbow (lu) -> wrist (lf), bending the way the game's pose bends. Out of reach, the
        // shoulder follows the hand (the arm never stretches).
        const float reach = (lu + lf) * 0.999f;
        const V3 u = Unit(Sub(wrist, shoulder));
        if (Len(Sub(wrist, shoulder)) > reach) shoulder = Sub(wrist, Scale(u, reach));
        float dist = Len(Sub(wrist, shoulder));
        dist = std::fmin(std::fmax(dist, std::fabs(lu - lf) + 1e-3f), reach);
        // The clavicle goes with its shoulder joint.
        const M4 kClav = Mul(Mul(l2w, translate(Sub(shoulder, bodyShoulder[si]))), Ainv);
        bones[s.clav] = Mul(g_saved[s.clav], kClav);
        const float along = (lu * lu - lf * lf + dist * dist) / (2.0f * dist);
        const float out = std::sqrt(std::fmax(lu * lu - along * along, 0.0f));
        const V3 bodyElbow = Origin(Mul(g_saved[s.fore], l2w)), bodyWrist = Origin(Mul(g_saved[s.hand], l2w));
        const V3 u0 = Unit(Sub(bodyWrist, bodyShoulder[si]));
        const V3 e0 = Sub(bodyElbow, bodyShoulder[si]);
        V3 bend = Sub(e0, Scale(u0, Dot(e0, u0)));  // the game's bend
        bend = Add(Unit(bend), V3{0.0f, 0.0f, -0.5f});                                          // and a little down
        bend = Unit(Sub(bend, Scale(u, Dot(bend, u))));
        if (Len(bend) < 0.5f) bend = Unit(Sub(V3{0, 0, -1}, Scale(u, -u.z)));
        const V3 elbow = Add(shoulder, Add(Scale(u, along), Scale(bend, out)));
        // Rigid moves of the two segments (drawn space), then back into the arms' component space.
        const M4 tArm = SegmentMove(shoulderOld, Sub(elbowOld, shoulderOld), shoulder, Sub(elbow, shoulder));
        const M4 tFore = SegmentMove(wrist, Sub(elbowOld, wrist), wrist, Sub(elbow, wrist));
        const M4 kArm = Mul(Mul(A, tArm), Ainv), kFore = Mul(Mul(A, tFore), Ainv);
        for (int i : s.armGroup) bones[i] = Mul(g_saved[i], kArm);
        for (int i : s.foreGroup) bones[i] = Mul(g_saved[i], kFore);
    }
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        const V3 sw = Origin(Mul(bones[g_rig.side[0].arm], A)), sb = Origin(Mul(g_saved[g_rig.side[0].arm], l2w));
        MLOG("armik: applied -- right shoulder joint drawn at %.1f %.1f %.1f (body %.1f %.1f %.1f)", sw.x, sw.y, sw.z, sb.x,
             sb.y, sb.z);
    }
    return true;
}

void __fastcall Hook_UpdateTransform(std::uintptr_t comp) {  // ECX = the component, no stack arguments
    const bool changed = Apply(comp);
    g_hook.thiscall<void>(comp);
    if (changed) {
        // The game's own pose back (its Camera bone, sockets and later reads); the renderer has its copy.
        const int sbo = names::PropertyOffset(comp, "SpaceBases");
        auto* bones = reinterpret_cast<M4*>(names::ReadPointer(comp + sbo));
        const int num = static_cast<int>(names::ReadPointer(comp + sbo + 4));
        if (bones && num == static_cast<int>(g_saved.size())) std::memcpy(bones, g_saved.data(), g_saved.size() * sizeof(M4));
    }
}

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
    if (!cfg.armIK || cfg.viewModel != 2) {
        MLOG("armik: Weapon.ArmIK=%d (needs Weapon.ViewModel=2) -- the arms move with the gun", cfg.armIK);
        return false;
    }
    if (!patch::BytesMatch(addr::kMohaSkelUpdateTransform, addr::kMohaSkelUpdateTransformBytes,
                           sizeof(addr::kMohaSkelUpdateTransformBytes))) {
        MLOG("armik: UpdateTransform bytes differ -- standing down");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kMohaSkelUpdateTransform),
                                              reinterpret_cast<void*>(&Hook_UpdateTransform));
    if (!res) {
        MLOG("armik: inline hook failed (error %d)", static_cast<int>(res.error().type));
        return false;
    }
    g_hook = std::move(*res);
    MLOG("armik: UMOHASkeletalMeshComponent::UpdateTransform hooked at 0x%08X",
         static_cast<unsigned>(addr::kMohaSkelUpdateTransform));
    return true;
}

}  // namespace mohavr::armsik
