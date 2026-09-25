// Direct3DCreate9 via the game's IAT (ENGINE-NOTES 5b).
// M1: observe and pass through. M2: optionally create the object through Direct3DCreate9On12
// (Bridge.D3D9On12), so every D3D9 resource is backed by D3D12 (D5a).
#pragma once

namespace mohavr::hooks {

// Installs the IAT hook if the slot holds exactly d3d9!Direct3DCreate9. Logs and returns
// false (game untouched) otherwise.
bool InstallDirect3DCreate9(bool useD3D9On12);

}  // namespace mohavr::hooks
