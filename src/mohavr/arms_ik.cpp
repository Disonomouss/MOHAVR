#include "arms_ik.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "bridge.hpp"
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

const M4 kIdentity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};

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
V3 Row(const M4& a, int i) { return {a.m[i][0], a.m[i][1], a.m[i][2]}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Unit(V3 a) { const float l = Len(a); return l > 1e-6f ? Scale(a, 1.0f / l) : V3{0, 0, 0}; }

M4 Translate(V3 t) {
    M4 r = kIdentity;
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}
// A row-vector matrix from a column-vector 3x3 rotation `rc`, about `pivotOld`, putting that pivot at `pivotNew`.
M4 FromColumnRotation(const float (&rc)[3][3], V3 pivotOld, V3 pivotNew) {
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
// The rigid move that turns direction `from` onto `to` about `pivotOld` and puts that pivot at `pivotNew`.
M4 SegmentMove(V3 pivotOld, V3 from, V3 pivotNew, V3 to) {
    const V3 a = Unit(from), b = Unit(to);
    const V3 v = Cross(a, b);
    const float c = Dot(a, b);
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
    return FromColumnRotation(rc, pivotOld, pivotNew);
}
// A turn by `angle` about the line through `pivot` along `axis`.
// A segment turned onto a new direction AND a new hinge axis (the elbow's bend axis): the frame (dir, hinge,
// dir x hinge) old -> new, about `pivotOld`, which lands on `pivotNew`. The upper arm and the forearm share the hinge,
// so they meet at the elbow without a twist (round 20: each turned by its own shortest rotation, they didn't).
bool HingeMove(V3 pivotOld, V3 dirOld, V3 hingeOld, V3 pivotNew, V3 dirNew, V3 hingeNew, M4& out) {
    const V3 d0 = Unit(dirOld), d1 = Unit(dirNew);
    const V3 h0 = Unit(Sub(hingeOld, Scale(d0, Dot(hingeOld, d0)))), h1 = Unit(Sub(hingeNew, Scale(d1, Dot(hingeNew, d1))));
    if (Len(d0) < 0.5f || Len(d1) < 0.5f || Len(h0) < 0.5f || Len(h1) < 0.5f) return false;
    const V3 c0 = Cross(d0, h0), c1 = Cross(d1, h1);
    const V3 f0[3] = {d0, h0, c0}, f1[3] = {d1, h1, c1};
    float rc[3][3] = {};  // column-vector rotation: sum over k of f1[k] f0[k]^T
    for (int k = 0; k < 3; ++k) {
        const float a[3] = {f1[k].x, f1[k].y, f1[k].z}, b[3] = {f0[k].x, f0[k].y, f0[k].z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) rc[i][j] += a[i] * b[j];
    }
    out = FromColumnRotation(rc, pivotOld, pivotNew);
    return true;
}
M4 AxisAngle(V3 pivot, V3 axis, float angle) {
    const V3 a = Unit(axis);
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float rc[3][3] = {{c + t * a.x * a.x, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y},
                            {t * a.x * a.y + s * a.z, c + t * a.y * a.y, t * a.y * a.z - s * a.x},
                            {t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, c + t * a.z * a.z}};
    return FromColumnRotation(rc, pivot, pivot);
}
// A stretch by `k` along direction `axis` through `pivot`: x' = (x - p) S + p, S = I + (k-1) a^T a.
M4 Stretch(V3 pivot, V3 axis, float k) {
    const V3 a = Unit(axis);
    const float av[3] = {a.x, a.y, a.z};
    M4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = (i == j ? 1.0f : 0.0f) + (k - 1.0f) * av[i] * av[j];
    const float p[3] = {pivot.x, pivot.y, pivot.z};
    for (int j = 0; j < 3; ++j) r.m[3][j] = p[j] - (p[0] * r.m[0][j] + p[1] * r.m[1][j] + p[2] * r.m[2][j]);
    r.m[3][3] = 1.0f;
    return r;
}

Config           g_cfg;
SafetyHookInline g_hook;  // UMOHASkeletalMeshComponent::UpdateTransform: puts the game's pose back afterwards
SafetyHookMid    g_mid;   // just before MeshObject->Update: bakes the move (and the IK)

// Bone indices of the arms mesh, found by name (once per mesh).
struct Side {
    int hand = -1, fore = -1, arm = -1, clav = -1;
    std::vector<int> rolls;      // the forearm roll bones (they take part of the wrist's twist)
    std::vector<int> armGroup;   // the upper arm and its roll bone
    std::vector<int> handGroup;  // the hand and its fingers
};
struct Rig {
    std::uintptr_t mesh = 0;
    bool ok = false;
    Side side[2];            // right (the gun hand's), left (the support hand)
    std::vector<int> body;   // root .. legs (moved with the shoulders)
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
        sd.armGroup.push_back(sd.arm);
        for (int i = 0; i < num; ++i) {
            if (n[i].rfind(p + "ForeArmRoll", 0) == 0) sd.rolls.push_back(i);
            if (n[i] == p + "ArmRoll") sd.armGroup.push_back(i);
            if (n[i].rfind(p + "Hand", 0) == 0) sd.handGroup.push_back(i);  // the hand and its fingers
        }
    }
    // The body: everything that isn't a hand, a finger, an arm bone or a weapon prop.
    for (int i = 0; i < num; ++i) {
        const std::string& s = n[i];
        const bool handSide = s.find("Hand") != std::string::npos || s.find("Arm") != std::string::npos ||
                              s.find("Shoulder") != std::string::npos || s.find("Prop") != std::string::npos;
        if (!handSide) g_rig.body.push_back(i);
    }
    g_rig.ok = true;
    MLOG("armik: rig of %s -- %d bones; right hand %d (+%zu fingers) forearm %d (+%zu roll) arm %d (+%zu roll) clavicle %d, "
         "left %d %d %d %d, %zu body",
         names::Name(mesh).c_str(), num, g_rig.side[0].hand, g_rig.side[0].handGroup.size() - 1, g_rig.side[0].fore,
         g_rig.side[0].rolls.size(), g_rig.side[0].arm, g_rig.side[0].armGroup.size() - 1, g_rig.side[0].clav,
         g_rig.side[1].hand, g_rig.side[1].fore, g_rig.side[1].arm, g_rig.side[1].clav, g_rig.body.size());
    return true;
}

// The parts' own poses (restored after the render copy), per component (the arms, the gun).
struct Saved {
    std::uintptr_t comp = 0;
    std::vector<M4> bones;
};
Saved g_saved[4];

// Render thread: which parts carry the move this frame.
std::atomic<std::uintptr_t> g_bakedComp[4];
std::atomic<DWORD> g_bakedTick[4];

void MarkBaked(std::uintptr_t comp) {
    const DWORD now = GetTickCount();
    int slot = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_bakedComp[i].load(std::memory_order_relaxed) == comp) { slot = i; break; }
        if (now - g_bakedTick[i].load(std::memory_order_relaxed) > now - g_bakedTick[slot].load(std::memory_order_relaxed)) slot = i;
    }
    g_bakedComp[slot].store(comp, std::memory_order_relaxed);
    g_bakedTick[slot].store(now, std::memory_order_relaxed);
}

// The arms: body, shoulders and the two-bone solves on top of the baked move (bones already = saved * A * L2W^-1).
void SolveArms(M4* bones, const std::vector<M4>& saved, const M4& l2w, const M4& A, const M4& invL2W) {
    // The shoulders: anchored to the tracked head (Weapon.ShoulderWidth apart, ShoulderDrop below the eyes,
    // ShoulderBack behind, turned with the body) -- the game's rig has them at eye height behind the eye. The torso moves
    // with them (by their mean shift).
    V3 bodyShoulder[2], anchor[2];
    for (int s = 0; s < 2; ++s) bodyShoulder[s] = anchor[s] = Origin(Mul(saved[g_rig.side[s].arm], l2w));
    float head[3], yaw = 0.0f, upm = 100.0f;
    if (view::HeadInWorld(head, yaw, upm)) {
        const float k = upm / 100.0f;  // cm -> units
        const V3 fwd{std::cos(yaw), std::sin(yaw), 0.0f}, right{-std::sin(yaw), std::cos(yaw), 0.0f};
        const V3 base = Sub(Sub(V3{head[0], head[1], head[2]}, V3{0.0f, 0.0f, g_cfg.shoulderDrop * k}), Scale(fwd, g_cfg.shoulderBack * k));
        anchor[0] = Add(base, Scale(right, 0.5f * g_cfg.shoulderWidth * k));
        anchor[1] = Sub(base, Scale(right, 0.5f * g_cfg.shoulderWidth * k));
    }
    const V3 shift = Scale(Add(Sub(anchor[0], bodyShoulder[0]), Sub(anchor[1], bodyShoulder[1])), 0.5f);
    const M4 kBody = Mul(Mul(l2w, Translate(shift)), invL2W);
    for (int i : g_rig.body) bones[i] = Mul(saved[i], kBody);

    // The support hand off the foregrip: the mirror image of the gun hand's grip on its controller, put on the other
    // controller (round 17: following the game's animated left hand it sat wrong and, with the pistol, twisted).
    float gunF[16], offF[16];
    bool offValid = false, twoHanded = false;
    const bool framesOk = viewmodel::HandFrames(gunF, offF, offValid, twoHanded);
    const bool freeHand = g_cfg.freeOffHand && framesOk && offValid && !twoHanded;
    M4 supportDrawn{};  // saved support-side bone -> world
    if (freeHand) {
        M4 gunCtrl, offCtrl;
        std::memcpy(gunCtrl.m, gunF, sizeof(gunCtrl.m));
        std::memcpy(offCtrl.m, offF, sizeof(offCtrl.m));
        // The gun hand in its controller's frame -- taken from a long gun held still and kept (round 18: mirroring the
        // grenade grip put the free hand wrong; round 19: a frame of the long gun's put-away animation, or the
        // parachute's, had been kept -- the hand bent back with the grenade).
        static M4 longGunRel;
        static bool haveLongGunRel = false;
        static V3 lastOrigin{};
        static int steady = 0;
        const shared::Header* hdr = bridge::SharedHeader();
        M4 rel = Mul(Mul(saved[g_rig.side[0].hand], A), AffineInverse(gunCtrl));
        const bool longGun = hdr && hdr->weaponKind == 0 && std::strncmp(hdr->weaponKey, "Attachment_", 11) == 0;
        const V3 o = Origin(rel);
        steady = longGun && Len(Sub(o, lastOrigin)) < 0.2f * upm / 100.0f ? steady + 1 : 0;
        lastOrigin = o;
        if (steady >= 30) {
            if (!haveLongGunRel) MLOG("armik: free hand grip taken from %.47s (held still 30 frames)", hdr->weaponKey);
            longGunRel = rel;
            haveLongGunRel = true;
        }
        if (haveLongGunRel) rel = longGunRel;
        M4 mirror = kIdentity;
        mirror.m[1][1] = -1.0f;  // left <-> right in the controller's frame
        M4 relM = Mul(Mul(mirror, rel), mirror);
        // The player's adjustment (the menu's Free hand): yaw, pitch, roll about the wrist, then forward -- in the
        // controller's frame (X forward, Y right, Z up). Round 18: the mirrored grip looked bent back.
        if (hdr) {
            const float k = 0.0174533f;
            const V3 w = Origin(relM);
            const M4 turn = Mul(Mul(AxisAngle(w, V3{0, 0, 1}, hdr->freeHand[1] * k), AxisAngle(w, V3{0, 1, 0}, hdr->freeHand[0] * k)),
                                AxisAngle(w, V3{1, 0, 0}, hdr->freeHand[2] * k));
            relM = Mul(Mul(relM, turn), Translate(V3{hdr->freeHand[3] * upm / 100.0f, 0.0f, 0.0f}));
        }
        const M4 target = Mul(relM, offCtrl);
        supportDrawn = Mul(AffineInverse(saved[g_rig.side[1].hand]), target);
        static int logged = 0;
        if (logged < 2) {
            ++logged;
            const V3 t = Origin(target), c = Origin(offCtrl), r = Origin(rel);
            MLOG("armik: free hand at %.1f %.1f %.1f (its controller %.1f %.1f %.1f; the gun hand sits at %.1f %.1f %.1f in its "
                 "controller's frame)", t.x, t.y, t.z, c.x, c.y, c.z, r.x, r.y, r.z);
        }
    }
    static int seenFree = -1;
    if (seenFree != static_cast<int>(freeHand)) {
        seenFree = freeHand;
        MLOG("armik: support hand %s", freeHand ? "free (the mirror of the gun hand's grip, on the other controller)" : "on the gun");
    }

    for (int si = 0; si < 2; ++si) {
        const Side& s = g_rig.side[si];
        const bool moveHand = si == 1 && freeHand;
        const M4 ah = moveHand ? supportDrawn : A;  // how this side's hand chain is drawn
        if (moveHand) {
            const M4 kHand = Mul(ah, invL2W);
            for (int i : s.handGroup) bones[i] = Mul(saved[i], kHand);
        }
        const V3 wrist = Origin(Mul(saved[s.hand], ah));
        const V3 elbowOld = Origin(Mul(saved[s.fore], ah));
        const V3 shoulderOld = Origin(Mul(saved[s.arm], ah));
        V3 shoulder = anchor[si];
        const float lu = Len(Sub(elbowOld, shoulderOld)), lf = Len(Sub(wrist, elbowOld));
        if (lu < 1e-3f || lf < 1e-3f) continue;
        // Two-bone solve: shoulder -> elbow -> wrist, bending the way the game's pose bends (a little down). Just out of
        // reach the arm stretches (up to 30%); beyond that the shoulder follows the hand.
        const V3 u = Unit(Sub(wrist, shoulder));
        const float stretch = std::fmin(std::fmax(Len(Sub(wrist, shoulder)) / ((lu + lf) * 0.999f), 1.0f), 1.3f);
        const float lus = lu * stretch, lfs = lf * stretch;
        const float reach = (lus + lfs) * 0.999f;
        if (Len(Sub(wrist, shoulder)) > reach) shoulder = Sub(wrist, Scale(u, reach));
        float dist = Len(Sub(wrist, shoulder));
        dist = std::fmin(std::fmax(dist, std::fabs(lus - lfs) + 1e-3f), reach);
        bones[s.clav] = Mul(saved[s.clav], Mul(Mul(l2w, Translate(Sub(shoulder, bodyShoulder[si]))), invL2W));
        const float along = (lus * lus - lfs * lfs + dist * dist) / (2.0f * dist);
        const float out = std::sqrt(std::fmax(lus * lus - along * along, 0.0f));
        const V3 bodyElbow = Origin(Mul(saved[s.fore], l2w)), bodyWrist = Origin(Mul(saved[s.hand], l2w));
        const V3 u0 = Unit(Sub(bodyWrist, bodyShoulder[si]));
        const V3 e0 = Sub(bodyElbow, bodyShoulder[si]);
        V3 bend = Sub(e0, Scale(u0, Dot(e0, u0)));
        bend = Add(Unit(bend), V3{0.0f, 0.0f, -0.5f});
        bend = Unit(Sub(bend, Scale(u, Dot(bend, u))));
        if (Len(bend) < 0.5f) bend = Unit(Sub(V3{0, 0, -1}, Scale(u, -u.z)));
        const V3 elbow = Add(shoulder, Add(Scale(u, along), Scale(bend, out)));
        // The upper arm and the forearm bone: from the BODY's pose (moved with the shoulder), turned onto their new
        // segments -- no wrist twist (round 16: a 90-degree turn spun the bicep and shoulder around).
        const M4 bsh = Mul(l2w, Translate(Sub(shoulder, bodyShoulder[si])));
        const V3 elbowBody = Origin(Mul(saved[s.fore], bsh)), wristBody = Origin(Mul(saved[s.hand], bsh));
        // Weapon.ElbowHinge (round 20: each segment's own shortest turn left the elbow twisted):
        //   2 (default) the forearm is carried by the upper arm's turn, as its child, then bent onto its new direction
        //     from there -- the shoulder as before, the elbow without a fold;
        //   1 both turned about the elbow's hinge (shoulder -> elbow -> wrist) -- clean elbow, but the shoulder pinched;
        //   0 each turned on its own.
        const M4 tSwing = SegmentMove(shoulder, Sub(elbowBody, shoulder), shoulder, Sub(elbow, shoulder));
        M4 tArm = tSwing, tFore = SegmentMove(elbowBody, Sub(wristBody, elbowBody), elbow, Sub(wrist, elbow));
        if (g_cfg.elbowHinge == 2) {
            const V3 f = Sub(wristBody, elbowBody);
            const V3 f1 = Add(Add(Scale(Row(tSwing, 0), f.x), Scale(Row(tSwing, 1), f.y)), Scale(Row(tSwing, 2), f.z));
            tFore = Mul(tSwing, SegmentMove(Origin(Mul(Translate(elbowBody), tSwing)), f1, elbow, Sub(wrist, elbow)));
        } else if (g_cfg.elbowHinge == 1) {
            const V3 hingeBody = Cross(Sub(elbowBody, shoulder), Sub(wristBody, elbowBody));
            const V3 hingeNew = Cross(Sub(elbow, shoulder), Sub(wrist, elbow));
            M4 ha, hf;
            if (HingeMove(shoulder, Sub(elbowBody, shoulder), hingeBody, shoulder, Sub(elbow, shoulder), hingeNew, ha) &&
                HingeMove(elbowBody, Sub(wristBody, elbowBody), hingeBody, elbow, Sub(wrist, elbow), hingeNew, hf)) {
                tArm = ha;
                tFore = hf;
            }
        }
        const M4 kArm = Mul(Mul(Mul(bsh, tArm), Stretch(shoulder, Sub(elbow, shoulder), stretch)), invL2W);
        for (int i : s.armGroup) bones[i] = Mul(saved[i], kArm);
        const M4 foreTF = Mul(Mul(bsh, tFore), Stretch(elbow, Sub(wrist, elbow), stretch));
        bones[s.fore] = Mul(saved[s.fore], Mul(foreTF, invL2W));
        // The forearm roll bones: the twist-free forearm turned about its axis by 60% of the wrist's twist (round 17:
        // all of it twisted the arm too much with the pistol).
        const V3 axis = Unit(Sub(wrist, elbow));
        V3 p1 = Row(Mul(saved[s.hand], foreTF), 1), p2 = Row(Mul(saved[s.hand], ah), 1);
        p1 = Unit(Sub(p1, Scale(axis, Dot(p1, axis))));
        p2 = Unit(Sub(p2, Scale(axis, Dot(p2, axis))));
        const float twist = std::atan2(Dot(Cross(p1, p2), axis), Dot(p1, p2));
        const M4 kRoll = Mul(Mul(foreTF, AxisAngle(wrist, axis, 0.6f * twist)), invL2W);
        for (int i : s.rolls) bones[i] = Mul(saved[i], kRoll);
    }
}

// Just before MeshObject->Update (EBX = the component; its LocalToWorld is final): bake the move into a first-person
// part of the player's (and, for the arms, the IK).
void OnMeshUpdate(SafetyHookContext& ctx) {
    const std::uintptr_t comp = ctx.ebx;
    if (!comp) return;
    // This is the base class's UpdateTransform (every skinned mesh): only MOHASkeletalMeshComponents have the FOV.
    static std::uintptr_t mohaClass = 0;
    const std::uintptr_t cls = names::ReadPointer(comp + addr::kObjectClass);
    if (!cls) return;
    if (cls != mohaClass) {
        if (mohaClass || names::Name(cls) != "MOHASkeletalMeshComponent") return;
        mohaClass = cls;
    }
    const float fov = *reinterpret_cast<const float*>(comp + addr::kMohaSkelMeshFov);
    if (fov == 0.0f) return;  // not a first-person part
    float d[16], dInv[16];
    if (!viewmodel::CurrentMove(d, dInv)) return;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn) return;
    const bool arms = names::Outer(comp) == pawn;  // the arms' Outer is the pawn, the gun's its weapon
    const int sbo = names::PropertyOffset(comp, "SpaceBases"), l2wo = names::PropertyOffset(comp, "LocalToWorld"),
              smo = names::PropertyOffset(comp, "SkeletalMesh");
    if (sbo < 0 || l2wo < 0 || smo < 0) return;
    auto* bones = reinterpret_cast<M4*>(names::ReadPointer(comp + sbo));
    const int num = static_cast<int>(names::ReadPointer(comp + sbo + 4));
    if (!bones || num <= 0 || num > 512) return;
    if (arms) {
        const std::uintptr_t mesh = names::ReadPointer(comp + smo);
        if (mesh != g_rig.mesh && !Resolve(mesh, num)) {
            static int logged = 0;
            if (logged++ < 3) MLOG("armik: %s has no arm bones this rig expects -- the arms move with the gun", names::Name(mesh).c_str());
        }
    }
    Saved* sv = nullptr;
    for (Saved& s : g_saved)
        if (s.comp == comp || (!sv && s.comp == 0)) sv = &s;
    if (!sv) sv = &g_saved[0];
    sv->comp = comp;
    sv->bones.assign(bones, bones + num);

    M4 l2w, D;
    std::memcpy(l2w.m, reinterpret_cast<const void*>(comp + l2wo), sizeof(l2w.m));
    std::memcpy(D.m, d, sizeof(D.m));
    const M4 invL2W = AffineInverse(l2w);
    const M4 A = Mul(l2w, D);  // where the part is drawn: bone * A
    const M4 kMove = Mul(A, invL2W);
    for (int i = 0; i < num; ++i) bones[i] = Mul(sv->bones[i], kMove);
    if (arms && g_rig.ok && static_cast<int>(sv->bones.size()) == num) SolveArms(bones, sv->bones, l2w, A, invL2W);
    MarkBaked(comp);
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        MLOG("armik: baked the move into %s (%s, %d bones)", names::Name(comp).c_str(), arms ? "the arms" : "the gun", num);
    }
}

void __fastcall Hook_UpdateTransform(std::uintptr_t comp) {  // ECX = the component, no stack arguments
    g_hook.thiscall<void>(comp);
    // The game's own pose back (its Camera bone, sockets and later reads); the renderer has its copy.
    for (Saved& s : g_saved) {
        if (s.comp != comp) continue;
        const int sbo = names::PropertyOffset(comp, "SpaceBases");
        auto* bones = reinterpret_cast<M4*>(names::ReadPointer(comp + sbo));
        const int num = static_cast<int>(names::ReadPointer(comp + sbo + 4));
        if (bones && num == static_cast<int>(s.bones.size())) std::memcpy(bones, s.bones.data(), s.bones.size() * sizeof(M4));
        s.comp = 0;
    }
}

}  // namespace

bool IsBaked(std::uintptr_t comp) {
    const DWORD now = GetTickCount();
    for (int i = 0; i < 4; ++i)
        if (g_bakedComp[i].load(std::memory_order_relaxed) == comp && now - g_bakedTick[i].load(std::memory_order_relaxed) < 250)
            return true;
    return false;
}

bool Install(const Config& cfg) {
    g_cfg = cfg;
    if (!cfg.armIK || cfg.viewModel != 2) {
        MLOG("armik: Weapon.ArmIK=%d (needs Weapon.ViewModel=2) -- the arms move with the gun", cfg.armIK);
        return false;
    }
    if (!patch::BytesMatch(addr::kMohaSkelUpdateTransform, addr::kMohaSkelUpdateTransformBytes,
                           sizeof(addr::kMohaSkelUpdateTransformBytes)) ||
        !patch::BytesMatch(addr::kSkelMeshObjectUpdateCall, addr::kSkelMeshObjectUpdateCallBytes,
                           sizeof(addr::kSkelMeshObjectUpdateCallBytes))) {
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
    auto mid = safetyhook::MidHook::create(reinterpret_cast<void*>(addr::kSkelMeshObjectUpdateCall), OnMeshUpdate);
    if (!mid) {
        MLOG("armik: mid hook failed (error %d) -- standing down", static_cast<int>(mid.error().type));
        g_hook = {};
        return false;
    }
    g_mid = std::move(*mid);
    MLOG("armik: hooked UpdateTransform 0x%08X and the MeshObject update 0x%08X (ElbowHinge=%d)",
         static_cast<unsigned>(addr::kMohaSkelUpdateTransform), static_cast<unsigned>(addr::kSkelMeshObjectUpdateCall), cfg.elbowHinge);
    return true;
}

}  // namespace mohavr::armsik
