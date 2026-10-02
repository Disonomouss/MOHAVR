// The off-hand pistol (the player, 2026-10-02: "Is it possible to build similar system for using the pistol with the off
// hand?") -- spike S1, the shot (work/research/offpistol/DESIGN.md 2.2, 7.1). Test commands only (Debug.GameCommands):
// nothing changes in play.
//
// The holstered pistol (the inventory manager's PistolWeapon, not the weapon in hand) fires ONE shot through the layer
// below the game's fire states: EALAWeapon.CalcWeaponFireNative (the bullets' own native trace; aim.cpp's bullet hook
// stands down meanwhile), EALASmallArms.ProcessInstantHit per impact (native ApplyDamage -> TakeDamage, the impact effects
// on the gun's attachment), the weapon's AI stimuli, the pawn's NoiseRadius, the stats' OnWeaponFire, a direct AmmoCount
// write and its report through PlaySoundAt. For the damage calls only, Pawn.Weapon is the pistol (kills and experience
// count for it). The gun in hand, its state, PendingFire, FlashCount and ammo are never written.
//
// "mohavr pistol" (a dump), "mohavr pistol fire <eye|hand|enemy> [head] [main]", "mohavr pistol upgrade",
// "mohavr pistol enemy", "mohavr pistol loop <n>".
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::offpistol {

void Configure(const Config& cfg);
// A test-channel line (game thread): true when it was an off-hand pistol command (handled here).
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::offpistol
