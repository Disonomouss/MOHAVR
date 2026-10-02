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

}  // namespace mohavr::carrier
