// Direct3DCreate9 via the game's IAT (ENGINE-NOTES 5b). M1: observe and pass through.
// M2 replaces the body with the D3D9On12 bridge (D5a).
#pragma once

namespace mohavr::hooks {

// Installs the IAT hook if the slot holds exactly d3d9!Direct3DCreate9. Logs and returns
// false (game untouched) otherwise.
bool InstallDirect3DCreate9();

}  // namespace mohavr::hooks
