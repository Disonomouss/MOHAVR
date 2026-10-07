// The off-hand knife (the player, 2026-10-03: "The mp40 has an upgrade that is a knife for its melee attack, can we make
// the knife an off hand equip with melee functionality? Add a holster to lower back for it."; OFFKNIFE-DESIGN.md) -- game
// side. The MP40's "Dagger" mesh (its attachment's KnifeMesh, the class default's subobject) cloned onto the arms and drawn
// in the off hand by the arm bake (a carrier, as the off-hand pistol and grenade), the hand closed on it with the game's own
// grip relation (the arms' LeftHand KnifeSocket).
#pragma once
#include <cstdint>
#include <string>
#include <cstdint>

#include "../common/shared_frame.hpp"
#include "config.hpp"

namespace mohavr::knife {

// `bake`: the arm bake is installed -- it draws the knife in the hand; without it none is drawn.
void Configure(const Config& cfg, bool bake);
// Debug.GameCommands: "mohavr knife template|carrier on [forward|icepick]|carrier off|status".
bool TestCommand(const wchar_t* line);
// GOAL E (the mission sweep): the knife's template in this level -- "found (<mesh>)" or "NOT FOUND".
std::string TemplateStatus(std::uintptr_t pawn);
// Per Draw (game thread).
void OnDraw(shared::Header* hdr);
// The knife held in the off hand.
bool Holding();
// A draw is wanted and not made yet: the arm bake frees the off hand (its relation to its controller is what the knife is
// placed by, FreeHandRel), whatever [Weapon] FreeOffHand says.
bool Pending();

// The knife drawn in the off hand: its clone (0 when none), where its mesh is drawn (rows X, Y, Z, origin; the mirror world in
// left-hand mode, as viewmodel::HandFrames; before the bake's catch-up W), the off hand on it (the hand's frame in the off
// controller's frame, its fingers).
std::uintptr_t CarrierComponent();
bool CarrierFrame(float (&gw)[16]);
bool HandOnKnife(float (&rel)[16], const float*& fingers, const char* const*& names);
// The knife's mesh in the off controller's frame (rows X, Y, Z, origin; units) -- for its strike points.
bool KnifeInController(float (&m)[16]);

}  // namespace mohavr::knife
