// Manual reload (D21, RELOAD-DESIGN.md) -- game side ([Weapon] ManualReload).
// M1, the game rules: a hook on execHasReserveAmmo keeps the game from reloading a converted gun by itself (only while
// the host's pipeline is engaged); the host's events (eject, take, insert, rack, drop; shared block v14) move the rounds
// between the clip and the reserve without losing any (RELOAD-DESIGN 2.2); the state goes back to the host every Draw.
// M0, the probe (Debug.ReloadProbe): logs the drawn guns' bones, the component scale, the gun bakes per Draw, the
// weapon's ammo fields and the reserve, and the hook's calls.
#pragma once
#include <cstdint>
#include <string>

namespace mohavr {
struct Config;
namespace shared {
struct Header;
}
}

namespace mohavr::reload {

// Called by view::Install after armsik::Install; `pipelineHooked` = the Draw hook and the arm bake are installed (the
// game's own reload is only ever blocked then).
bool Install(const Config& cfg, bool pipelineHooked);
// Game thread, once per Draw (vr_view Hook_Draw).
void OnDraw(shared::Header* hdr);
// The last first-person gun update's LocalToWorld (the game's) and its attachment class (for the arms' bake).
bool LastGun(float (&l2w)[16], std::string& key);
// Round 31: the off hand's reload grip this frame (from the game's reload animations): where the support hand goes
// (world, row-major) and its 15 fingers' frames in the hand's (4x3 each, in `names` order); false = no grip now.
bool GripNow(float (&target)[16], const float*& fingers, const char* const*& names);
// GOAL A3: a pump gun's pump drawn back by the off hand -- its move in the world this frame (the bake's world: the
// mirror world while mirrored), for the support hand riding it while two-handed; false = none now.
bool PumpShift(float (&d)[3]);
// Game thread, every first-person gun update, right after the bake (arms_ik OnMeshUpdate): `saved` = the game's own
// pose (num row-major 4x4 matrices, component space), `bones` = the drawn ones (saved x kMove; overridden here for the
// magazine, the action and the top round), `l2w` = the component's LocalToWorld, `a` = L2W x D, kMove = a x inv(L2W),
// `carry` = the body's move since the player view the hand frames come from (the IK's targets x carry; Weapon.CatchUp).
// M3: also samples the geometry the host needs (the magazine's grab point and way out, the action's), published in OnDraw.
void OnGunBake(std::uintptr_t comp, const float* saved, float* bones, int num, const float* l2w, const float* a,
               const float* kMove, const float* carry);

}  // namespace mohavr::reload
