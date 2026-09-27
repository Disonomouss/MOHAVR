// Throwing grenades by hand (M8, [Hands] Throw) -- game side.
//
// MOHA throws a grenade when the fire trigger lets go: EALAGrenade.ProjectileFire spawns the projectile
// (SpawnedExplosive) and sets its velocity -- yaw from the body, pitch from the aim, speed from how far the trigger was
// pressed (ENGINE-NOTES 5v). The host sends the gun hand's velocity at that release (hdr->throwVel); when the grenade
// weapon's SpawnedExplosive next appears near the player, its Velocity becomes the hand's (mapped into the world,
// times Hands.ThrowScale) plus the pawn's own. Properties are found by name (names::PropertyOffset).
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::throwing {

void Configure(const Config& cfg);
// Per frame, from the player's own head-tracked view (game thread).
void OnPlayerView();

}  // namespace mohavr::throwing
