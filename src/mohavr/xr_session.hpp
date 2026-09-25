// The OpenXR side (M2). Runs entirely on its own thread with its own D3D11 device on the
// adapter the runtime asks for; the game's D3D9 device is never touched from here.
//
// Step 1 (this file): instance, system, session, event loop, and a world-locked quad showing a
// test pattern -- proves OpenXR works inside MOHA.exe. Step 2 feeds the game's frame into the
// quad via 9On12 shared textures (D5a).
#pragma once
#include <string>

namespace mohavr::xr {

// Starts the XR thread once. `runtimeJson` (may be empty) is applied as XR_RUNTIME_JSON for this
// process before the loader looks for a runtime (D3). Never call from DllMain.
void Start(const std::wstring& runtimeJson);

}  // namespace mohavr::xr
