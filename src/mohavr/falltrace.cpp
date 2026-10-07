#include "falltrace.hpp"

#include <cmath>
#include <cwchar>

#include "addresses.hpp"
#include "aim.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"

namespace mohavr::falltrace {
namespace {

void ToReal(const float* m, const float (&p)[3], float (&out)[3]) {
    if (!m) {
        out[0] = p[0], out[1] = p[1], out[2] = p[2];
        return;
    }
    for (int j = 0; j < 3; ++j) out[j] = p[0] * m[0 + j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
}

}  // namespace

Result Trace(std::uintptr_t pawn, const float (&p0)[3], const float (&v0)[3], float g, float floorZ, const float* mirror) {
    Result r;
    r.floorZ = floorZ;
    if (!pawn || !(g > 0.0f) || p0[2] <= floorZ) return r;  // (from at or under the floor: it rests there, as before)
    constexpr float kDt = 0.02f, kMaxT = 3.0f;
    float p[3] = {p0[0], p0[1], p0[2]};
    for (float t = 0.0f; t < kMaxT; t += kDt) {
        const float t1 = t + kDt;
        float q[3] = {p0[0] + v0[0] * t1, p0[1] + v0[1] * t1, p0[2] + v0[2] * t1 - 0.5f * g * t1 * t1};
        const bool last = q[2] <= floorZ;
        if (last) q[2] = floorZ;
        float a[3], b[3], hit[3];
        ToReal(mirror, p, a);
        ToReal(mirror, q, b);
        if (aim::WorldTrace(pawn, a, b, hit)) {
            const float sx = b[0] - a[0], sy = b[1] - a[1], sz = b[2] - a[2];
            const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
            const float frac = len > 1e-3f ? std::sqrt((hit[0] - a[0]) * (hit[0] - a[0]) + (hit[1] - a[1]) * (hit[1] - a[1]) +
                                                       (hit[2] - a[2]) * (hit[2] - a[2])) / len
                                           : 0.0f;
            const float horiz = std::sqrt(sx * sx + sy * sy), drop = a[2] - b[2];
            if (drop > horiz) {  // landed on top of something
                r.top = true;
                r.stopT = t + frac * kDt;
                r.floorZ = std::fmax(floorZ, hit[2] + 1.0f);
                return r;
            }
            // A wall: the sideways motion stops just short of it; then straight down to what is under that point.
            r.wall = true;
            r.stopT = t + frac * kDt;
            const float back = len > 1e-3f ? 3.0f / len : 0.0f;
            const float s[3] = {hit[0] - sx * back, hit[1] - sy * back, hit[2] - sz * back};
            const float down[3] = {s[0], s[1], floorZ - 5.0f};
            float under[3];
            if (aim::WorldTrace(pawn, s, down, under) && under[2] > floorZ) r.floorZ = under[2] + 1.0f;
            return r;
        }
        p[0] = q[0], p[1] = q[1], p[2] = q[2];
        if (last) break;
    }
    return r;
}

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr falltrace", 16) != 0) return false;
    float speed = 3.0f, up = 1.0f;
    swscanf_s(line + 16, L"%f %f", &speed, &up);
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t ctrl = pawn ? script::Obj(pawn, "Controller") : 0;
    if (!pawn || !ctrl) {
        MLOG("falltrace: test -- no pawn");
        return true;
    }
    const float* loc = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    const std::uintptr_t cyl = script::Obj(pawn, "CylinderComponent");
    const float half = script::Float(cyl, "CollisionHeight", 96.0f);
    const float eye = script::Float(pawn, "EyeHeight", 64.0f);
    const int yaw = *reinterpret_cast<const int*>(ctrl + addr::kActorRotation + 4);
    const float a = static_cast<float>(yaw & 0xFFFF) * 6.2831853f / 65536.0f;
    constexpr float upm = 100.0f;
    const float p0[3] = {loc[0], loc[1], loc[2] + eye};
    const float v0[3] = {std::cos(a) * speed * upm, std::sin(a) * speed * upm, up * upm};
    const Result r = Trace(pawn, p0, v0, 9.8f * upm, loc[2] - half, nullptr);
    MLOG("falltrace: test -- %.1f m/s ahead (+%.1f up) from the eye at %.0f %.0f %.0f: %s, the sideways motion stops after %.2f s, "
         "rests at %.0f (the feet at %.0f)", speed, up, p0[0], p0[1], p0[2], r.wall ? "a WALL" : r.top ? "landed on TOP of something" : "nothing in the way",
         r.stopT > 100.0f ? -1.0f : r.stopT, r.floorZ, loc[2] - half);
    return true;
}

}  // namespace mohavr::falltrace
