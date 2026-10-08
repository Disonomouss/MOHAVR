// D77: taking a weapon (or a crate's ammo or grenades) by closing a free grip on it -- the game's own hold-to-swap.
#pragma once
#include "../common/shared_frame.hpp"

namespace mohavr::pickup {

void Configure(bool grabPickup);
// Game thread, per Draw: which hand is within reach of a weapon or crate the player can take (hdr->pickupNear), and the
// host's grab (hdr->pickupReqSeq) done through that pickup's MOHACSA.UsedBy.
void OnDraw(shared::Header* hdr);
// Tests (Debug.GameCommands): "mohavr pickup list | take" (pickup.cpp).
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::pickup
