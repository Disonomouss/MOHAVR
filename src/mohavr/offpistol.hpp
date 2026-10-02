// The off-hand pistol (the player, 2026-10-02: "Is it possible to build similar system for using the pistol with the off
// hand?") -- game side. OFFPISTOL-DESIGN.md.
//
// The holstered pistol (the inventory manager's PistolWeapon, or the other pistol of a Colt + C96 pair: not the weapon in
// hand) fires through the layer below the game's fire states: EALAWeapon.CalcWeaponFireNative (the bullets' own native
// trace; aim.cpp's bullet hook stands down meanwhile), EALASmallArms.ProcessInstantHit per impact (native ApplyDamage ->
// TakeDamage, the impact effects on the gun's attachment), the weapon's AI stimuli, the pawn's NoiseRadius, the stats'
// OnWeaponFire, a direct AmmoCount write and its report through PlaySoundAt. For the damage calls only, Pawn.Weapon is the
// pistol (kills and experience count for it: [OffHand] PistolCredit). The gun in hand, its state, PendingFire, FlashCount
// and ammo are never written.
//
// The host owns the interaction (src/host/offpistol.cpp: a click at a pistol holster draws it, a click at a holster puts it
// back, the off trigger fires) and sends ordered events (shared block v21: DRAW, SHOT with the off line at the pull,
// HOLSTER); the game executes them at the pistol's own rate in game time, draws the pistol in the off hand (a clone of its
// pickup mesh, the hand closed on it), refills it in the holster after its reload time, keeps the second dot's distance,
// and publishes what a DRAW gets and whether it can happen now.
//
// Tests (Debug.GameCommands): "mohavr pistol" (a dump), "mohavr pistol fire <eye|hand|enemy> [head] [main]", "mohavr
// pistol kill [main]", "mohavr pistol upgrade", "mohavr pistol enemy", "mohavr pistol loop <n>", "mohavr pistol sound",
// "mohavr pistol refill", "mohavr pistol empty [n]", "mohavr pistol carrier on|off".
#pragma once
#include <cstdint>

#include "../common/shared_frame.hpp"

namespace mohavr {
struct Config;
}

namespace mohavr::offpistol {

// `bake`: the arm bake is installed (armsik::Install) -- it draws the pistol in the hand; without it none is drawn.
void Configure(const Config& cfg, bool bake);
// A test-channel line (game thread): true when it was an off-hand pistol command (handled here).
bool TestCommand(const wchar_t* line);
// Per Draw (game thread, after the off-hand grenade): the host's events, the held trigger, the refill, the second dot's
// distance, the status block.
void OnDraw(shared::Header* hdr);
// Whether the off hand holds the pistol now.
bool Holding();
// After a NextWeapon: true when it made the held pistol the pending weapon and PistolKeep is on (run it once more).
bool SkipHeldPistol();

// The pistol drawn in the off hand: its clone (0 when none), where its mesh is drawn (rows X, Y, Z, origin; the mirror world
// in left-hand mode, as viewmodel::HandFrames; before the bake's catch-up W), the off hand on it (the hand's frame in the off
// controller's frame and its 15 fingers, reload_grips.inc's order), and a bone of the clone moved off its reference pose
// (the C96's parts: `collapse` = not drawn, else its position in the mesh).
std::uintptr_t CarrierComponent();
bool CarrierFrame(float (&gw)[16]);
bool HandOnGun(float (&rel)[16], const float*& fingers, const char* const*& names);
bool CarrierBone(int index, bool& collapse, float (&pos)[3]);

}  // namespace mohavr::offpistol
