// MOHAVR.ini, next to the DLL. Every behaviour has a switch (standing rule 7).
#pragma once
#include <string>

namespace mohavr {

struct Config {
    bool enabled        = true;   // [General] Enabled   -- 0: pure dinput8 proxy, nothing else
    bool hookD3D9       = true;   // [Hooks]   Direct3DCreate9
    bool d3d9On12       = false;  // [Bridge]  D3D9On12 -- create the game's IDirect3D9 via Direct3DCreate9On12 (M2)
    bool testWrongBuild = false;  // [Debug]   TestWrongBuild -- pretend the build check failed (M1 acceptance)
    bool xrEnabled      = false;  // [OpenXR]  Enabled -- start an OpenXR session after device creation (M2)
    std::wstring xrRuntimeJson;   // [OpenXR]  RuntimeJson -- if set, XR_RUNTIME_JSON for this process only (D3)
};

Config LoadConfig(const std::wstring& dir);

}  // namespace mohavr
