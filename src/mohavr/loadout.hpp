// D79: the mission loadout's weapon list, workable with a pad (the game's list picks whatever its first input moves to and
// closes -- the flat game's mouse hides it; a pad reached only the next gun down).
#pragma once
#include <cstdint>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::loadout {

void Configure(bool fixList);
// Game thread, per Draw: finds the focused list of the open UI scene.
void OnDraw(shared::Header* hdr);
// The XInput hook (any thread): the pad state the game will see, with the list's up / down / A taken while the fix drives
// the list.
void FilterPad(shared::PadState& pad);
// D96: the name of the first active UI scene whose name starts with one of `prefixes` (commas), else empty. Game thread.
std::string CinemaScene(const std::wstring& prefixes);
// Tests (Debug.GameCommands): "mohavr ui" logs the active scenes and the focused control.
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::loadout
