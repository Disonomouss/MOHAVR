// Manual reload (D21, RELOAD-DESIGN.md) -- game side. Milestone M0: the probe (Debug.ReloadProbe), which only logs:
// the drawn guns' bones (the game's pose, the reference pose, |det|), the component scale, the gun bakes per Draw, the
// weapon's ammo fields and the reserve, and the calls of execHasReserveAmmo through a count-only hook (it never changes
// the result).
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::reload {

// Called by view::Install after armsik::Install.
bool Install(const Config& cfg);
// Game thread, once per Draw (vr_view Hook_Draw).
void OnDraw();
// Game thread, every first-person gun update, right after the bake (arms_ik OnMeshUpdate): `saved` = the game's own
// pose (num row-major 4x4 matrices, component space), `l2w` = the component's LocalToWorld, `a` = L2W x D (where a bone
// is drawn: bone x a).
void OnGunBake(std::uintptr_t comp, const float* saved, int num, const float* l2w, const float* a);

}  // namespace mohavr::reload
