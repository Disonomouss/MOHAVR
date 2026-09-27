// Aiming (M7, [Aim] Mode) -- game side.
//
// The player's shots take their base aim from Pawn.GetBaseAimRotation() (ENGINE-NOTES 5o/5s) and start at
// the game's own, untracked eye. Mode 1 aims with the head, 2/3 with the left/right controller: each view
// frame the chosen pose's ray is traced into the world (UWorld::SingleLineCheck), and the player's
// GetBaseAimRotation then returns the direction from the shot's start to that hit point -- so the shot lands
// exactly where the ray points, although it starts at the eye. The hit distance goes to the host for the
// reticle (hdr->aimDistance / aimSource).
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::aim {

// Hooks execGetBaseAimRotation (Aim.Mode > 0). Called by view::Install.
bool Install(const Config& cfg);

// Per frame, from the player's own head-tracked view (game thread): `ctrl` = the local PlayerController,
// `shotStart` = the game's untracked view location (where its shots start).
void OnPlayerView(std::uintptr_t ctrl, const float (&shotStart)[3]);

// The local player's pawn (checked both ways, addresses.hpp), or 0. Game thread.
std::uintptr_t LocalPlayerPawn();

}  // namespace mohavr::aim
