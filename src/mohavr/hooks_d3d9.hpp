// Direct3DCreate9 via the game's IAT (ENGINE-NOTES 5b).
// M1: observe and pass through. M2: optionally create the object through Direct3DCreate9On12
// (Bridge.D3D9On12), so every D3D9 resource is backed by D3D12 (D5a).
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::hooks {

// Installs the IAT hook if the slot holds exactly d3d9!Direct3DCreate9. Logs and returns
// false (game untouched) otherwise. Keeps a copy of `cfg` (Bridge.*, OpenXR.* switches).
bool InstallDirect3DCreate9(const Config& cfg);

}  // namespace mohavr::hooks
