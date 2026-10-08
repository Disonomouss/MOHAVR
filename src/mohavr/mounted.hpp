// Mounted guns (GOAL D, MOUNTED-DESIGN.md): the simulator's way to the level's MG42 nests.
#pragma once
#include <cstdint>

#include "../common/shared_frame.hpp"

namespace mohavr::mounted {

// Tests (Debug.GameCommands): "mohavr mg list | goto [n] | use [n] | where" (mounted.cpp).
bool TestCommand(const wchar_t* line);

// GOAL D2 (D71): a mounted gun is the weapon in hand (MOHAMountedGunWeapon: manning a nest) and [Weapon] MountedGame is on --
// the gun then stays where the game puts it (on its mount; viewmodel's "no gun drawn") and the game aims it (aim.cpp).
void Configure(bool mountedGame, bool mountedHands = false);
bool GameHandles(std::uintptr_t pawn);

// D78 ([Weapon] MountedHands; the menu's hdr->mgMode): the gun hand aims a manned MG42. Per Draw (game thread): the hand's
// aim line, taken against the body's heading, written to the pawn's rMGRot (the gun against its mount) within the
// weapon's limits, and hdr->mgState published.
void OnDraw(shared::Header* hdr);
// The hands aim the MG42 manned now (the controller is then held level: the camera's pitch is the controller's plus the
// mount's, its yaw the controller's -- the arms' aim blend turns the gun).
bool HandsNow();
// A mounted gun manned now (its camera turns with the mount).
bool Manned();

}  // namespace mohavr::mounted
