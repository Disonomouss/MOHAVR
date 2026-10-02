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
// The host owns the interaction (src/host/offhand.cpp: take at a grenade holster, pin, cook, throw, put back) and sends
// ordered events (shared block v20); the game executes them, keeps the fuse clock in game time, and publishes what a take
// can get and whether it can happen now. The grenade is counted out only when it flies.
//
// Tests (Debug.GameCommands): "mohavr nade" (a dump), "mohavr nade throw <frag|gammon|stick|any> <hand|eye> <vx> <vy> <vz>
// [fuse s]" (a launch without the host; velocity in the headset's LOCAL frame, m/s, times OffHand.ThrowScale), "mohavr
// nade carrier <frag|gammon|stick|off>".
#pragma once
#include <cstdint>

#include "../common/shared_frame.hpp"

namespace mohavr {
struct Config;
}

namespace mohavr::offhand {

// `bake`: the arm bake is installed (armsik::Install) -- it places the grenade drawn in the hand; without it none is drawn.
void Configure(const Config& cfg, bool bake);
// A test-channel line (game thread): true when it was an off-hand command (handled here).
bool TestCommand(const wchar_t* line);
// Per Draw (game thread, after the reload): the host's events, the fuse, the status block, the thrown grenades.
void OnDraw(shared::Header* hdr);

// Spike S2 ("mohavr nade carrier <frag|gammon|stick|off>"): the grenade drawn in the off hand -- its component (attached
// to the arms; 0 when none), and where its mesh goes in the world (rows forward, right, up, origin; the mirror world in
// left-hand mode, as viewmodel::HandFrames); false when there is no off-hand frame (then it is collapsed).
std::uintptr_t CarrierComponent();
bool CarrierFrame(float (&gw)[16]);
// The off hand on the held grenade (as the gun hand holds that type, mirrored): its frame in the off controller's frame and
// its 15 fingers (reload_grips.inc's order); false when it isn't drawn.
bool HandOnGrenade(float (&rel)[16], const float*& fingers, const char* const*& names);

// Whether the off hand may take something now, as far as the game goes (shared with the off-hand pistol): a live player
// pawn with a weapon drawn in hand (not the HellBox, not the parachute or the landing), no cinematic, weapons not held or
// disabled by the game, no mounted gun (the script part at 4 Hz). `why` says what stands in the way ("" when nothing).
bool BaseAvailable(std::uintptr_t pawn, std::uintptr_t inv, std::uintptr_t gun, const char*& why);
// Whether the off hand holds a grenade now (the pistol is refused meanwhile).
bool Holding();

}  // namespace mohavr::offhand
