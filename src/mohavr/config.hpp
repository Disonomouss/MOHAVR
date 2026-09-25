// MOHAVR.ini, next to the DLL. Every behaviour has a switch (standing rule 7).
#pragma once
#include <string>

namespace mohavr {

struct Config {
    bool enabled        = true;   // [General] Enabled   -- 0: pure dinput8 proxy, nothing else
    bool hookD3D9       = true;   // [Hooks]   Direct3DCreate9
    bool testWrongBuild = false;  // [Debug]   TestWrongBuild -- pretend the build check failed (M1 acceptance)
};

Config LoadConfig(const std::wstring& dir);

}  // namespace mohavr
