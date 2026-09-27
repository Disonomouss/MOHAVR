#include "throwing.hpp"

#include <windows.h>

#include <cmath>

#include "aim.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "vr_view.hpp"

namespace mohavr::throwing {
namespace {

Config g_cfg;
std::uint32_t g_seenSeq = 0;
bool  g_pending = false;
DWORD g_pendingTick = 0;
float g_vel[3]{};                // the throw, world units/s
std::uintptr_t g_lastProj = 0;
float g_lastWritten[3]{};
std::uintptr_t g_follow = 0;     // the thrown grenade, logged once a second later
DWORD g_followAt = 0;

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

void Configure(const Config& cfg) { g_cfg = cfg; }

void OnPlayerView() {
    if (!g_cfg.throwByHand) return;
    const shared::Header* hdr = bridge::SharedHeader();
    if (!hdr) return;
    const DWORD now = GetTickCount();
    // A new release from the host: remember it for the grenade it spawns (the throw animation takes a moment).
    const std::uint32_t seq = hdr->throwSeq;
    if (seq != g_seenSeq) {
        g_seenSeq = seq;
        const float xr[3] = {hdr->throwVel[0], hdr->throwVel[1], hdr->throwVel[2]};
        float ue[3];
        if (view::VectorToWorld(xr, ue)) {
            for (int i = 0; i < 3; ++i) g_vel[i] = ue[i] * g_cfg.throwScale;
            g_pending = true;
            g_pendingTick = now;
            MLOG("throw: release %.1f %.1f %.1f m/s -> %.0f %.0f %.0f units/s (x%.2f)", xr[0], xr[1], xr[2], g_vel[0], g_vel[1],
                 g_vel[2], g_cfg.throwScale);
        }
    }
    if (g_follow && static_cast<LONG>(now - g_followAt) >= 0) {
        float loc[3], vel[3];
        const int lo = names::PropertyOffset(g_follow, "Location"), vo = names::PropertyOffset(g_follow, "Velocity");
        if (lo >= 0 && vo >= 0 && names::ReadVector(g_follow + lo, loc) && names::ReadVector(g_follow + vo, vel))
            MLOG("throw: 1 s later the grenade is at %.0f %.0f %.0f, velocity %.0f %.0f %.0f", loc[0], loc[1], loc[2], vel[0],
                 vel[1], vel[2]);
        g_follow = 0;
    }
    if (g_pending && now - g_pendingTick > 2500) {
        g_pending = false;
        MLOG("throw: no grenade appeared within 2.5 s of the release");
    }
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn) return;
    const int wo = names::PropertyOffset(pawn, "Weapon");
    const std::uintptr_t weapon = wo >= 0 ? names::ReadPointer(pawn + wo) : 0;
    if (!weapon) return;
    const int so = names::PropertyOffset(weapon, "SpawnedExplosive");  // only the grenade weapons have it
    if (so < 0) return;
    const std::uintptr_t proj = names::ReadPointer(weapon + so);
    if (!proj || !g_pending) {
        g_lastProj = proj;
        return;
    }
    const int lo = names::PropertyOffset(proj, "Location"), vo = names::PropertyOffset(proj, "Velocity");
    float loc[3], vel[3], cam[3], pitch = 0.0f, yaw = 0.0f;
    if (lo < 0 || vo < 0 || !names::ReadVector(proj + lo, loc) || !names::ReadVector(proj + vo, vel) ||
        !view::GameCamera(cam, pitch, yaw))
        return;
    // A new grenade: another projectile, or the pooled one relaunched (a velocity we didn't write), near the player.
    const bool fresh = proj != g_lastProj || Dist(vel, g_lastWritten) > 1.0f;
    g_lastProj = proj;
    if (!fresh || Dist(loc, cam) > 300.0f) return;
    float pawnVel[3] = {0.0f, 0.0f, 0.0f};
    const int pvo = names::PropertyOffset(pawn, "Velocity");
    if (pvo >= 0) names::ReadVector(pawn + pvo, pawnVel);
    float* v = reinterpret_cast<float*>(proj + vo);
    for (int i = 0; i < 3; ++i) {
        v[i] = g_vel[i] + pawnVel[i];
        g_lastWritten[i] = v[i];
    }
    g_pending = false;
    g_follow = proj;
    g_followAt = now + 1000;
    MLOG("throw: grenade %s (%s) velocity %.0f %.0f %.0f -> %.0f %.0f %.0f, %.0f units from the eye", names::Name(proj).c_str(),
         names::ClassName(proj).c_str(), vel[0], vel[1], vel[2], v[0], v[1], v[2], Dist(loc, cam));
}

}  // namespace mohavr::throwing
