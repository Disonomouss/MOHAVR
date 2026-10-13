// D103: pointing at the game's menus with a controller (the player: "control the menus by pointing the controller at
// the buttons and pressing right trigger to select. Like virtual desktop or steamvr do"). The game's UI follows the
// Windows cursor (GetCursorPos, then ScreenToClient) -- but its window is bigger than the desktop, so the real cursor
// can't reach all of it. The game's GetCursorPos import is answered with the pointed spot instead, while the host
// points; the trigger reaches the window as mouse button messages.
#pragma once

#include "../common/shared_frame.hpp"

namespace mohavr {
struct Config;
}

namespace mohavr::pointer {

bool Install(const Config& cfg);
// Game thread, per Draw: the host's pointer (where the ray meets the flat screen) -> the cursor and the clicks.
void OnDraw(shared::Header* hdr);
// Tests (Debug.GameCommands): "mohavr cursor <x> <y>" (client pixels; the cursor held there 15 s), "mohavr click".
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::pointer
