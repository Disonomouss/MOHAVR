// The off-hand grenade (the player, 2026-10-02: "being able to grab a grenade with the off hand and throw it without
// unequipping your gun would be very immersive") -- game side. work/research/dualwield/DESIGN.md.
//
// The gun stays the pawn's Weapon. A grenade weapon in the inventory (frag, Gammon, stick: not the one in the hand, so it
// doesn't tick) launches its own projectile through its script function EALAWeapon.SpawnProjectile, called through
// AActor::ProcessEvent like the reload's sounds; the mod then does what EALAGrenade.ProjectileFire does after it (draw
// scale, light, the cooked damage type, the fuse, the velocity) and takes the grenade from the reserve. The game's own
// throw path can't be used with a gun in the hand: ProjectileFire's IncrementFlashCount plays the GUN's fire effects,
// and FireAmmunition / OnProjectileToss reach the active weapon's attachment and the shared PendingFire.
//
// Spike S1 (tests only, Debug.GameCommands): "mohavr nade" (a dump), "mohavr nade throw <frag|gammon|stick|any>
// <hand|eye> <vx> <vy> <vz> [fuse s]" (velocity in the headset's LOCAL frame, m/s, times Hands.ThrowScale).
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::offhand {

void Configure(const Config& cfg);
// A test-channel line (game thread): true when it was an off-hand command (handled here).
bool TestCommand(const wchar_t* line);
// Per Draw (game thread, after the reload): the thrown grenades followed until they go off.
void OnDraw();

// Spike S2 ("mohavr nade carrier <frag|gammon|stick|off>"): the grenade drawn in the off hand -- its component (attached
// to the arms; 0 when none), and where its mesh goes in the world (rows forward, right, up, origin; the mirror world in
// left-hand mode, as viewmodel::HandFrames); false when there is no off-hand frame (then it is collapsed).
std::uintptr_t CarrierComponent();
bool CarrierFrame(float (&gw)[16]);

}  // namespace mohavr::offhand
