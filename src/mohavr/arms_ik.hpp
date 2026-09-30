// Arm IK and the gun's move (M8, [Weapon] ArmIK) -- game side.
//
// The first-person parts (the arms and the gun) are moved into the aiming hand by baking the move D into their bone
// matrices just before their pose is copied for the renderer (inside USkeletalMeshComponent::UpdateTransform, with
// LocalToWorld final): the arms and the gun then use the same D in the same frame (no jitter against each other), and
// the renderer draws them where they are (viewmodel.cpp skips them). The arms rig is hand-driven (ENGINE-NOTES 5x):
// here the hands stay on the gun, the body and clavicles go back under the player's head-anchored shoulders, and each
// arm is a two-bone solve (the upper arm and forearm without the wrist's twist; the forearm roll bones take part of
// it). Off the foregrip the support hand follows the other controller, as the mirror of the gun hand's grip. The
// game's own pose is put back right after the copy.
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::armsik {

bool Install(const Config& cfg);

// Render thread: whether this first-person part's bone matrices already carry the move (drawn as is).
bool IsBaked(std::uintptr_t component);
// Game thread: the move this part was last baked with (world, row-major: drawn = the game's pose x d), if within 250 ms.
bool BakedMove(std::uintptr_t component, float (&d)[16]);
// Game thread (round 31): the free support hand's frame in its controller's frame (world axes; the mirror world's when
// mirrored), as the last arms bake placed it -- the drawn hand = this x the controller frame.
bool FreeHandRel(float (&rel)[16]);

}  // namespace mohavr::armsik
