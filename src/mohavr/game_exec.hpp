// Console commands inside the game (ENGINE-NOTES 5o): ULocalPlayer's FExec interface (+0x3C, vtable
// slot 0 = ULocalPlayer::Exec 0x10C1A220) runs a command the way the game's console would -- the
// player controller, its pawn, weapon and HUD all get to handle exec functions. Game thread only.
#pragma once
#include <cstdint>

namespace mohavr::gexec {

// Runs `cmd` for `localPlayer` (a ULocalPlayer*). False if the FExec vtable isn't the one this build
// is known to have (then nothing is called) or the game didn't recognise the command.
bool Run(std::uintptr_t localPlayer, const wchar_t* cmd);

}  // namespace mohavr::gexec
