// Arm IK (M8, [Weapon] ArmIK) -- game side.
//
// The first-person arms rig is hand-driven (ENGINE-NOTES 5x): the forearm, upper arm and clavicle hang off each hand,
// so drawing the arms with the gun in the controller (viewmodel.cpp: LocalToWorld * D) drags whole arms and the body
// along. Just before the arms' pose is copied for the renderer (UMOHASkeletalMeshComponent::UpdateTransform) this
// rewrites their SpaceBases so that, drawn with D, the hands stay on the gun while the body and the clavicles stay
// where the game has them, and each arm is a two-bone solve from its shoulder joint to its wrist (the elbow bending
// the way the game's pose bends it). The game's own pose is put back right after the copy.
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::armsik {

bool Install(const Config& cfg);

}  // namespace mohavr::armsik
