// Mounted guns (GOAL D, MOUNTED-DESIGN.md): the simulator's way to the level's MG42 nests.
#pragma once
#include <cstdint>

namespace mohavr::mounted {

// Tests (Debug.GameCommands): "mohavr mg list | goto [n] | use [n] | where" (mounted.cpp).
bool TestCommand(const wchar_t* line);

// GOAL D2 (D71): a mounted gun is the weapon in hand (MOHAMountedGunWeapon: manning a nest) and [Weapon] MountedGame is on --
// the gun then stays where the game puts it (on its mount; viewmodel's "no gun drawn") and the game aims it (aim.cpp).
void Configure(bool mountedGame);
bool GameHandles(std::uintptr_t pawn);

}  // namespace mohavr::mounted
