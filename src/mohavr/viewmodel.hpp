// The first-person arms and gun (M8, [Weapon] ViewModel) -- game side.
//
// MOHA draws the arms and gun with their own FOV: the skeletal-mesh proxy bakes a flat-screen projection into
// their transform for every view (ENGINE-NOTES 5t). In stereo that projection is applied per eye with a symmetric
// frustum, so the gun is seen double. ViewModel=1 draws them where they really are (true 3D); ViewModel=2 also
// moves them from the game camera's frame onto the aiming controller's: LocalToWorld * D, D = inverse(camera) *
// hand. Only the drawing changes -- the game's arms, camera bone and weapon stay where the game put them.
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::viewmodel {

// Hooks the proxy's per-view transform (Weapon.ViewModel > 0). Called by view::Install.
bool Install(const Config& cfg);

// Per frame, from the player's own head-tracked view (game thread), after the aim.
void OnPlayerView();

}  // namespace mohavr::viewmodel
