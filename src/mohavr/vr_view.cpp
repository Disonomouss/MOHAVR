#include "vr_view.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>

#include "addresses.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr::view {
namespace {

constexpr float kPi = 3.14159265358979f;

struct Vec3 { float x, y, z; };

// Unreal rotation units: 65536 = 360 degrees.
inline float UnrToRad(int u) { return static_cast<float>(u) * (kPi / 32768.0f); }
inline int RadToUnr(float r) { return static_cast<int>(std::lround(r * (32768.0f / kPi))); }

// OpenXR (right-handed, +X right, +Y up, -Z forward) -> Unreal (left-handed, +X forward,
// +Y right, +Z up):  ue = ( -xr.z, xr.x, xr.y ).
inline Vec3 XrToUe(float x, float y, float z) { return {-z, x, y}; }

// Rotate an OpenXR vector by quaternion q (x,y,z,w).
Vec3 QuatRotate(const shared::Pose& q, float vx, float vy, float vz) {
    // v' = v + 2*w*(u x v) + 2*(u x (u x v)), u = (qx,qy,qz)
    const float ux = q.qx, uy = q.qy, uz = q.qz, w = q.qw;
    const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    const float ccx = uy * cz - uz * cy, ccy = uz * cx - ux * cz, ccz = ux * cy - uy * cx;
    return {vx + 2.0f * (w * cx + ccx), vy + 2.0f * (w * cy + ccy), vz + 2.0f * (w * cz + ccz)};
}

inline Vec3 YawRotate(const Vec3& v, float yaw) {  // about Unreal +Z: X toward Y
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// --- state --------------------------------------------------------------------------------------
Config g_cfg;
SafetyHookMid g_viewHook;
SafetyHookMid g_projHookNormal;
SafetyHookMid g_projHookConstrained;

// The view the game thread applied in the current CalcSceneView, and the previous one (render
// thread lag, ENGINE-NOTES 5e). Written on the game thread, read on the render thread.
struct AppliedView {
    shared::Pose pose;      // head pose used (OpenXR, LOCAL)
    shared::Fov  fov;       // projection actually used (after widening)
    DWORD        thread;    // game thread that computed it
    bool         valid;
};
CRITICAL_SECTION g_lock;
AppliedView g_current{}, g_previous{};
bool g_thisViewActive = false;   // head tracking applied in the CalcSceneView in progress
shared::Fov g_mono{};            // union FOV for this view (before widening)
long g_views = 0;
bool g_loggedProj = false;

// --- the view merge hook ------------------------------------------------------------------------
void OnViewPoint(SafetyHookContext& ctx) {
    g_thisViewActive = false;
    shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !g_cfg.headTracking) return;

    shared::Pose head, eye[2];
    shared::Fov fov[2];
    if (!shared::ReadViews(hdr, head, eye, fov)) return;

    auto* loc = *reinterpret_cast<float**>(ctx.ebp + 0x10);
    auto* rot = *reinterpret_cast<int**>(ctx.ebp + 0x14);
    if (!loc || !rot) return;

    const float gameYaw = UnrToRad(rot[1]);

    // Head basis in Unreal axes, then turned by the game's yaw (body heading).
    const Vec3 fXr = QuatRotate(head, 0, 0, -1), rXr = QuatRotate(head, 1, 0, 0), uXr = QuatRotate(head, 0, 1, 0);
    const Vec3 F = YawRotate(XrToUe(fXr.x, fXr.y, fXr.z), gameYaw);
    const Vec3 R = YawRotate(XrToUe(rXr.x, rXr.y, rXr.z), gameYaw);
    const Vec3 U = YawRotate(XrToUe(uXr.x, uXr.y, uXr.z), gameYaw);

    // FMatrix::Rotator (UE3): pitch/yaw from X, roll from Y/Z against the unrolled Y axis.
    const float yaw = std::atan2(F.y, F.x);
    const float pitch = std::atan2(F.z, std::sqrt(F.x * F.x + F.y * F.y));
    const Vec3 SY{-std::sin(yaw), std::cos(yaw), 0.0f};
    const float roll = std::atan2(Dot(U, SY), Dot(R, SY));
    rot[0] = RadToUnr(pitch);
    rot[1] = RadToUnr(yaw);
    rot[2] = RadToUnr(roll);

    if (g_cfg.headPosition) {
        // Translation is relative to where the head was when tracking started (runtimes differ:
        // the simulator's LOCAL puts the head at y = 1.7 m, real runtimes near 0).
        static bool haveOrigin = false;
        static float ox = 0, oy = 0, oz = 0;
        if (!haveOrigin) {
            haveOrigin = true;
            ox = head.px; oy = head.py; oz = head.pz;
            MLOG("view: head position origin set at (%.3f %.3f %.3f) m", ox, oy, oz);
        }
        const Vec3 p = YawRotate(XrToUe(head.px - ox, head.py - oy, head.pz - oz), gameYaw);
        const float s = g_cfg.unitsPerMeter;
        loc[0] += p.x * s;
        loc[1] += p.y * s;
        loc[2] += p.z * s;
    }

    // Mono: one render that covers both eyes.
    g_mono.tanLeft = fov[0].tanLeft < fov[1].tanLeft ? fov[0].tanLeft : fov[1].tanLeft;
    g_mono.tanRight = fov[0].tanRight > fov[1].tanRight ? fov[0].tanRight : fov[1].tanRight;
    g_mono.tanUp = fov[0].tanUp > fov[1].tanUp ? fov[0].tanUp : fov[1].tanUp;
    g_mono.tanDown = fov[0].tanDown < fov[1].tanDown ? fov[0].tanDown : fov[1].tanDown;

    EnterCriticalSection(&g_lock);
    g_previous = g_current;
    g_current.pose = head;
    g_current.fov = g_mono;  // projection hook refines this with the widened FOV
    g_current.thread = GetCurrentThreadId();
    g_current.valid = true;
    LeaveCriticalSection(&g_lock);
    g_thisViewActive = true;

    if (++g_views == 1 || g_views % 2000 == 0) {
        MLOG("view #%ld: head q(%.3f %.3f %.3f %.3f) p(%.3f %.3f %.3f) -> rot P%d Y%d R%d (game yaw %d)", g_views,
             head.qx, head.qy, head.qz, head.qw, head.px, head.py, head.pz, rot[0], rot[1], rot[2], RadToUnr(gameYaw));
    }
}

// --- the projection hooks -----------------------------------------------------------------------
void OnProjection(SafetyHookContext& ctx) {
    if (!g_thisViewActive || !g_cfg.headsetProjection) return;
    shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !hdr->width || !hdr->height) return;
    auto* m = reinterpret_cast<float*>(ctx.eax);  // 4x4 row-major, row vectors (ENGINE-NOTES 5g)
    if (!m) return;

    // Widen the union FOV to the viewport's aspect, so the whole backbuffer is used and nothing
    // inside the eyes' FOV is cut: grow the short axis around its centre.
    shared::Fov f = g_mono;
    const float aspect = static_cast<float>(hdr->width) / static_cast<float>(hdr->height);
    const float w = f.tanRight - f.tanLeft, h = f.tanUp - f.tanDown;
    if (w < aspect * h) {
        const float cx = 0.5f * (f.tanRight + f.tanLeft), hw = 0.5f * aspect * h;
        f.tanLeft = cx - hw;
        f.tanRight = cx + hw;
    } else {
        const float cy = 0.5f * (f.tanUp + f.tanDown), hh = 0.5f * w / aspect;
        f.tanDown = cy - hh;
        f.tanUp = cy + hh;
    }

    // Asymmetric perspective; depth terms (m[10], m[11], m[14]) stay as the engine built them
    // (near 5.0, infinite far) so culling and depth precision are unchanged.
    m[0] = 2.0f / (f.tanRight - f.tanLeft);
    m[1] = 0.0f;
    m[4] = 0.0f;
    m[5] = 2.0f / (f.tanUp - f.tanDown);
    m[8] = -(f.tanRight + f.tanLeft) / (f.tanRight - f.tanLeft);
    m[9] = -(f.tanUp + f.tanDown) / (f.tanUp - f.tanDown);

    EnterCriticalSection(&g_lock);
    g_current.fov = f;
    LeaveCriticalSection(&g_lock);

    if (!g_loggedProj) {
        g_loggedProj = true;
        MLOG("projection: headset FOV L%.3f R%.3f U%.3f D%.3f (union), widened to %.3f aspect -> L%.3f R%.3f U%.3f D%.3f",
             g_mono.tanLeft, g_mono.tanRight, g_mono.tanUp, g_mono.tanDown, aspect, f.tanLeft, f.tanRight, f.tanUp, f.tanDown);
    }
}

bool CheckCall(std::uintptr_t va, std::uintptr_t target) {
    if (*reinterpret_cast<const std::uint8_t*>(va) != 0xE8) return false;
    const auto rel = *reinterpret_cast<const std::int32_t*>(va + 1);
    return va + 5 + static_cast<std::uintptr_t>(rel) == target;
}

bool Hook(SafetyHookMid& slot, std::uintptr_t va, safetyhook::MidHookFn fn, const char* what) {
    auto res = safetyhook::MidHook::create(reinterpret_cast<void*>(va), fn);
    if (!res) {
        MLOG("view: MidHook %s at 0x%08X failed (error %d) -- standing down", what, static_cast<unsigned>(va),
             static_cast<int>(res.error().type));
        return false;
    }
    slot = std::move(*res);
    MLOG("view: MidHook %s at 0x%08X installed", what, static_cast<unsigned>(va));
    return true;
}

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
    InitializeCriticalSection(&g_lock);
    if (!cfg.headTracking) {
        MLOG("view: Camera.HeadTracking=0 -- no view hooks");
        return false;
    }
    // Standing rule 4: the build check already verified every signature in addresses.hpp; check
    // the call targets once more right here, next to the code that depends on them.
    if (!CheckCall(addr::kViewPointMerge + 3, 0x10BEDFF0) || !CheckCall(addr::kProjCallNormal, addr::kPerspectiveMatrix) ||
        !CheckCall(addr::kProjCallConstrained, addr::kPerspectiveMatrix)) {
        MLOG("view: call targets in CalcSceneView don't match -- standing down");
        return false;
    }
    if (!Hook(g_viewHook, addr::kViewPointMerge, OnViewPoint, "view merge")) return false;
    if (cfg.headsetProjection) {
        Hook(g_projHookNormal, addr::kProjAfterNormal, OnProjection, "projection (normal)");
        Hook(g_projHookConstrained, addr::kProjAfterConstrained, OnProjection, "projection (constrained)");
    }
    return true;
}

bool MetaForPresentedFrame(shared::SlotMeta& meta) {
    EnterCriticalSection(&g_lock);
    // With UE3's render thread the frame being presented was computed one game frame earlier.
    const AppliedView& v = (g_current.valid && g_current.thread == GetCurrentThreadId()) ? g_current : g_previous;
    const bool ok = v.valid;
    if (ok) {
        meta.pose = v.pose;
        meta.fov = v.fov;
    }
    LeaveCriticalSection(&g_lock);
    meta.hasView = ok ? 1u : 0u;
    return ok;
}

}  // namespace mohavr::view
