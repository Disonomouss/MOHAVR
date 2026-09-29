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

// Per frame, from the player's own head-tracked view (game thread), BEFORE the aim (aim uses GunRay).
// Also names the weapon in the player's hands for the host's Gun fit page (hdr->weaponKey) and applies the
// host's fit for it.
void OnPlayerView();

// ViewModel=2: this frame's aim line along the gun in the hand (world: start, unit direction; the scale in use).
// False when the gun isn't in the hand.
bool GunRay(float (&pos)[3], float (&dir)[3], float& unitsPerMeter);

// Arm IK: the move the first-person parts are drawn with (world, row-major FMatrix: LocalToWorld' = LocalToWorld *
// d) and its inverse; false when they're drawn where the game put them (or the game's own way).
bool CurrentMove(float (&d)[16], float (&dInv)[16]);
// Arm IK: the gun hand's controller frame (the gun's axes, its origin at the controller) and the other controller's
// frame (world; rows forward, right, up, origin), and whether the other hand is on the foregrip. False when the gun
// isn't drawn in the hand.
bool HandFrames(float (&gun)[16], float (&off)[16], bool& offValid, bool& twoHanded);
// Weapon.LeftHandMirror with the gun in the left hand: CurrentMove and HandFrames are then in a mirror world (reflected
// across the body's centre plane), and the parts are drawn back through the mirror.
bool Mirrored();
// How the player's body (the pawn: location and yaw) moved since the player view CurrentMove and HandFrames come from
// (world, row-major: a point carried by the body then -> now = p * w). The next tick's bake is one move behind the eyes
// without it (Weapon.CatchUp). False when unknown (no view, another pawn, a jump of over 1 m).
bool BodyMoveSinceView(float (&w)[16]);

}  // namespace mohavr::viewmodel
