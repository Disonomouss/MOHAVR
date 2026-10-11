// Is the MOHA.exe we're loaded into exactly the pinned build (D4)? Checks the in-memory PE
// header fields and every byte signature in addresses.hpp. No file I/O, safe in DllMain.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mohavr {

// Logs each check. Returns true only if everything matches. `forceFail` simulates a
// mismatch (Debug.TestWrongBuild) to prove the stand-down path.
bool CheckBuild(int testMode);  // 1: a timestamp mismatch simulated; 2: a CheckSum-only one (D80)

// D58: is this the EA app's copy (the OOA wrapper) rather than Steam's? Valid once CheckBuild has passed.
bool IsEaBuild();

// D92: is this the disc's no-DVD exe, still packed (its header's entry point is the unpacker's)? Header only: safe in
// DllMain, before the game's code exists. CheckBuild runs once it is unpacked.
bool IsDiscWrapper();

// The pinned build's bytes at `va`? As patch::BytesMatch, but on the disc's copy a SecuROM splice CheckBuild verified counts
// as the instruction it stands for (D95). Use it to re-check a signature next to the hook.
bool CodeMatches(std::uintptr_t va, const std::uint8_t* expected, std::size_t size);

}  // namespace mohavr
