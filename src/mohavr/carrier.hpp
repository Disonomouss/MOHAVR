// The off hand's drawn items (the off-hand grenade, the off-hand pistol): a clone of a weapon's first-person pickup mesh
// (DroppedPickupMesh -- the same skeletal mesh as its attachment, which exists only while the weapon is in the hand),
// attached to the arms at their Camera bone and placed by the arm bake (arms_ik.cpp BakeCarrier; collapsed -- nothing
// drawn -- when there is no off-hand frame). Factored from the grenade's spike S2 (ENGINE-NOTES 5bb).
#pragma once
#include <cstdint>

namespace mohavr::carrier {

struct Slot {
    std::uintptr_t comp = 0, arms = 0, pawn = 0;
};

// Clones `weapon`'s pickup mesh into `slot` and attaches it to the pawn's arms, drawn as the arms are (their depth group,
// light environment, LOD, first-person FOV and shadows; no collision; no physics asset -- a pistol's pickup mesh has one,
// which would keep the bake from it). The slot holds the clone before AttachComponent, so the bake knows it at its first
// update. `who` prefixes the log lines; `trace` logs the success too. False, the slot empty, on any failure.
bool Attach(Slot& slot, std::uintptr_t pawn, std::uintptr_t weapon, const char* who, bool trace);
// Detaches the slot's clone (when its pawn is still the local one) and empties the slot.
void Detach(Slot& slot, const char* who, bool trace);
// The slot's clone while its pawn is the local pawn, else 0.
std::uintptr_t Component(const Slot& slot);

// A weapon's fit ([GunFit] <attachment class> = grip forward right up, angle ...): the player's ini, else the shipped one,
// else [Weapon] GripX/Y/Z and 0 deg. `from` says which.
void FitFromIni(const char* key, float (&fit)[4], const char*& from);
// The off hand's hold of a weapon, mirrored from the gun hand's: the weapon's mesh in the off controller's frame (rows X, Y,
// Z, origin; Unreal row vectors), from its mesh in the game camera's frame at the gun hand's idle (`camRows`: X, Y, Z,
// origin; forward, right, up) and the fit: M_left = S x M_right x M_y (S the mesh's X mirror, M_y the controller's), M_right
// = the camera pose with its origin moved back by the fit's grip, then pitched by the fit's angle as the host's gunPose.
void MirroredHold(const float (&camRows)[12], const float (&fit)[4], float (&out)[16]);
// out = a x b (row-major 4x4).
void Mul16(const float* a, const float* b, float* out);

}  // namespace mohavr::carrier
