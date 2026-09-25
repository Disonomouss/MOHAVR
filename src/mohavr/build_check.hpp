// Is the MOHA.exe we're loaded into exactly the pinned build (D4)? Checks the in-memory PE
// header fields and every byte signature in addresses.hpp. No file I/O, safe in DllMain.
#pragma once

namespace mohavr {

// Logs each check. Returns true only if everything matches. `forceFail` simulates a
// mismatch (Debug.TestWrongBuild) to prove the stand-down path.
bool CheckBuild(bool forceFail);

}  // namespace mohavr
