// D89: the spare magazine in the belt pouch. See pouchmag.hpp.
#include "pouchmag.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "carrier.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "vr_view.hpp"

namespace mohavr::pouchmag {
using namespace script;
namespace {

bool g_on = true, g_bake = false;

// What the reload bake wants (refreshed every update of the gun; stale after 250 ms).
struct Want_ {
    bool        show = false;
    DWORD       at = 0;
    std::string att, bone;
    float       boneComp[16] = {};
    float       grab[3] = {};
    float       scale = 1.0f;
} g_want;

// The carrier.
struct Spare {
    carrier::Slot  slot;
    std::string    att;
    int            bone = -1;
    bool           on = false;
    float          world[16] = {};
    std::uintptr_t pawn = 0;
} g_spare;

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

int BoneIndex(std::uintptr_t comp, const std::string& name) {
    const std::uintptr_t mesh = Obj(comp, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    for (int i = 0; data && i < num && i < 512; ++i)
        if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) == name) return i;
    return -1;
}

void Drop(const char* why) {
    if (!g_spare.on && !carrier::Component(g_spare.slot)) return;
    carrier::Detach(g_spare.slot, "pouchmag", false);
    if (g_spare.on) MLOG("pouchmag: the spare gone from the pouch (%s)", why);
    g_spare.on = false;
}

}  // namespace

void Configure(bool on, bool bake) {
    g_on = on;
    g_bake = bake;
}

void Want(bool show, const std::string& attachment, const std::string& bone, const float* boneComp, const float (&grab)[3],
          float scale) {
    g_want.show = show;
    g_want.at = GetTickCount();
    if (!show) return;
    g_want.att = attachment;
    g_want.bone = bone;
    std::memcpy(g_want.boneComp, boneComp, sizeof(g_want.boneComp));
    std::memcpy(g_want.grab, grab, sizeof(g_want.grab));
    g_want.scale = scale;
}

void OnDraw(shared::Header* hdr) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (pawn != g_spare.pawn) {
        g_spare = Spare{};  // (the old pawn's clone goes with it)
        g_spare.pawn = pawn;
    }
    const std::uint32_t mode = hdr ? hdr->pouchMagMode : 0u;
    const bool on = g_bake && (mode == 2u || (mode == 0u && g_on));
    const bool want = on && pawn && g_want.show && GetTickCount() - g_want.at < 250 && hdr && hdr->pouchPosOk;
    if (!want) {
        Drop(!on ? "switched off" : "the magazine isn't out");
        return;
    }
    // The carrier: the gun's class default mesh (as the rack eject's round), re-made for another gun.
    if (!g_spare.on || g_spare.att != g_want.att) {
        Drop("another gun");
        const std::uintptr_t arms = Obj(pawn, "FPArms");
        const std::uintptr_t cls = arms ? names::ReadPointer(arms + addr::kObjectClass) : 0;
        const std::wstring path = L"MOHAGameNonNative.Default__" + std::wstring(g_want.att.begin(), g_want.att.end()) + L".WeaponMeshComponent";
        const std::uintptr_t t = FindByPath(pawn, path, cls);
        const int bone = t ? BoneIndex(t, g_want.bone) : -1;
        static int logged = 0;
        if (!t || bone < 0) {
            if (logged++ < 10) MLOG("pouchmag: no spare for %s (%s)", g_want.att.c_str(), !t ? "no template" : "no such bone");
            g_want.show = false;
            return;
        }
        g_spare.att = g_want.att;
        g_spare.bone = bone;
        g_spare.on = true;  // (known to the bake before its first update)
        if (!carrier::AttachTemplate(g_spare.slot, pawn, t, pawn, "pouchmag", false, true, 600.0f)) {
            g_spare.on = false;
            if (logged++ < 10) MLOG("pouchmag: the carrier could not be attached");
            return;
        }
        if (logged++ < 20) MLOG("pouchmag: a spare %s.%s (bone #%d) in the pouch", g_want.att.c_str(), g_want.bone.c_str(), bone);
    }
    // Its frame: the gun as it is held upright and facing the body's way (the first-person mesh: X left, Y down, Z ahead),
    // scaled as drawn, the magazine centred in the pouch: halfway from its seated bone's origin (its top) to its grab point
    // (D91: the grab point alone stood it up out of the pouch).
    shared::Pose p{};
    p.px = hdr->pouchPos[0];
    p.py = hdr->pouchPos[1];
    p.pz = hdr->pouchPos[2];
    p.qw = 1.0f;
    float pos[3], fwd[3], upm = 100.0f, head[3], yaw = 0.0f, hu = 100.0f;
    if (!view::PoseToWorld(p, pos, fwd, upm) || !view::HeadInWorld(head, yaw, hu)) {
        Drop("no view");
        return;
    }
    const float s = g_want.scale, c = std::cos(yaw), n = std::sin(yaw);
    const float U[9] = {n * s, -c * s, 0.0f,   // mesh X -> left
                        0.0f, 0.0f, -s,        // mesh Y -> down
                        c * s, n * s, 0.0f};   // mesh Z -> ahead
    const float* b = g_want.boneComp;
    float mid[3], o[3];
    for (int j = 0; j < 3; ++j) mid[j] = 0.5f * (g_want.grab[j] + b[12 + j]);
    for (int j = 0; j < 3; ++j) o[j] = pos[j] - (mid[0] * U[j] + mid[1] * U[3 + j] + mid[2] * U[6 + j]);
    // world = boneComp x [U, o] (row vectors).
    for (int r = 0; r < 4; ++r)
        for (int j = 0; j < 3; ++j) g_spare.world[r * 4 + j] = b[r * 4] * U[j] + b[r * 4 + 1] * U[3 + j] + b[r * 4 + 2] * U[6 + j] +
                                                             (r == 3 ? o[j] : 0.0f);
    for (int r = 0; r < 3; ++r) g_spare.world[r * 4 + 3] = 0.0f;
    g_spare.world[15] = 1.0f;
    // (Where it is against the head, when that moves past 2 cm: the pouch is the body's, not the hands'.)
    static float last[3] = {1e9f, 1e9f, 1e9f};
    static int moves = 0;
    const float rel[3] = {pos[0] - head[0], pos[1] - head[1], pos[2] - head[2]};
    const float d = std::sqrt((rel[0] - last[0]) * (rel[0] - last[0]) + (rel[1] - last[1]) * (rel[1] - last[1]) +
                              (rel[2] - last[2]) * (rel[2] - last[2]));
    if (d > 2.0f && moves < 30) {
        ++moves;
        std::memcpy(last, rel, sizeof(last));
        MLOG("pouchmag: the pouch at %.1f %.1f %.1f units from the head", rel[0], rel[1], rel[2]);
    }
}

bool IsCarrier(std::uintptr_t comp) { return comp && carrier::Component(g_spare.slot) == comp; }

bool BoneFrame(std::uintptr_t comp, int& bone, float (&world)[16]) {
    if (!IsCarrier(comp) || !g_spare.on) return false;
    bone = g_spare.bone;
    std::memcpy(world, g_spare.world, sizeof(world));
    return true;
}

}  // namespace mohavr::pouchmag
