#include "config.hpp"

#include <windows.h>

#include "log.hpp"

namespace mohavr {

Config LoadConfig(const std::wstring& dir) {
    const std::wstring ini = dir + L"\\MOHAVR.ini";
    Config c;
    const bool present = GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    auto get = [&](const wchar_t* sec, const wchar_t* key, bool def) {
        return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini.c_str()) != 0;
    };
    c.enabled        = get(L"General", L"Enabled", c.enabled);
    c.hookD3D9       = get(L"Hooks", L"Direct3DCreate9", c.hookD3D9);
    c.d3d9On12       = get(L"Bridge", L"D3D9On12", c.d3d9On12);
    c.bridgeHost     = get(L"Bridge", L"Host", c.bridgeHost);
    c.testWrongBuild = get(L"Debug", L"TestWrongBuild", c.testWrongBuild);

    c.xrEnabled      = get(L"OpenXR", L"Enabled", c.xrEnabled);
    wchar_t buf[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"OpenXR", L"RuntimeJson", L"", buf, MAX_PATH, ini.c_str());
    c.xrRuntimeJson = buf;

    MLOG("config: %s -- Enabled=%d Hooks.Direct3DCreate9=%d Bridge.D3D9On12=%d Bridge.Host=%d OpenXR.Enabled=%d "
         "OpenXR.RuntimeJson=%s Debug.TestWrongBuild=%d",
         present ? "MOHAVR.ini" : "no MOHAVR.ini, defaults", c.enabled, c.hookD3D9, c.d3d9On12, c.bridgeHost, c.xrEnabled,
         c.xrRuntimeJson.empty() ? "(system runtime)" : "(set)", c.testWrongBuild);
    if (c.bridgeHost && !c.d3d9On12) MLOG("config: Bridge.Host=1 needs Bridge.D3D9On12=1 -- the host will not be started");
    if (c.bridgeHost && c.xrEnabled) MLOG("config: Bridge.Host=1 and OpenXR.Enabled=1 -- using the host; in-process OpenXR is off");
    return c;
}

}  // namespace mohavr
