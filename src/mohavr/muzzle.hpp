// The player's muzzle flash and ejected brass ([Weapon] MuzzleFlash and Brass, rounds 26-27) -- game side.
//
// The game places them at its first-person gun's sockets (Barrel_Player, ShellEject_Player), in the gun's own
// flat-screen pose: the mod moves the gun only where it is drawn (arms_ik bakes the move into the render copy), so they
// showed half a metre in front of the face. SmallArmsAttachment.StartMuzzleFlash / StartShellEjectParticles write the
// game's socket into the particle component (SetTranslation / SetRotation: the fields, applied later) and activate it;
// the transform is applied inside ActivateSystem, before any particle spawns. A hook on its exec moves the fields by the
// gun's drawn move first (and through the left hand's mirror), or suppresses the spawning afterwards (hide).
// ENGINE-NOTES 5aj.
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::muzzle {

// Verifies execActivateSystem's bytes and hooks it (unless both switches are `game`). Called by view::Install.
bool Install(const Config& cfg);
// Debug.MuzzleFreeze ([S] tool): once per Draw, with the game's first ULocalPlayer (game thread): traces the first
// flash each Draw, then pauses the world.
void OnDraw(std::uintptr_t localPlayer);

}  // namespace mohavr::muzzle
