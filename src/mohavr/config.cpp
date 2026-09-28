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
    c.bridgeMirror   = static_cast<int>(GetPrivateProfileIntW(L"Bridge", L"Mirror", c.bridgeMirror, ini.c_str()));
    if (c.bridgeMirror < 0 || c.bridgeMirror > 2) c.bridgeMirror = 0;
    c.testWrongBuild = get(L"Debug", L"TestWrongBuild", c.testWrongBuild);
    c.controllers    = get(L"Input", L"Controllers", c.controllers);
    c.debugViewState = get(L"Debug", L"ViewState", c.debugViewState);
    c.debugGameCommands = get(L"Debug", L"GameCommands", c.debugGameCommands);
    c.debugSwapEyes  = get(L"Debug", L"SwapEyeOrder", c.debugSwapEyes);
    c.debugSwapHalves = get(L"Debug", L"SwapHalves", c.debugSwapHalves);
    c.debugTraceScissor = get(L"Debug", L"TraceScissor", c.debugTraceScissor);
    c.debugReflect   = get(L"Debug", L"Reflect", c.debugReflect);
    c.hudMode        = static_cast<int>(GetPrivateProfileIntW(L"HUD", L"Mode", c.hudMode, ini.c_str())) == 1 ? 1 : 0;
    auto getf = [&](const wchar_t* sec, const wchar_t* key, float def, float lo, float hi) {
        wchar_t b[32] = L"";
        GetPrivateProfileStringW(sec, key, L"", b, 32, ini.c_str());
        const float v = static_cast<float>(_wtof(b));
        return b[0] && v >= lo && v <= hi ? v : def;
    };
    c.hudDistance    = getf(L"HUD", L"Distance", c.hudDistance, 0.3f, 20.0f);
    c.hudWidth       = getf(L"HUD", L"Width", c.hudWidth, 0.1f, 10.0f);
    c.hudDown        = getf(L"HUD", L"Down", c.hudDown, -2.0f, 2.0f);
    c.hudScale       = getf(L"HUD", L"Scale", c.hudScale, 0.1f, 2.0f);
    c.renderResX     = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResX", c.renderResX, ini.c_str()));
    c.renderResY     = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResY", c.renderResY, ini.c_str()));
    c.lockWindow     = get(L"Render", L"LockWindow", c.lockWindow);
    c.decalFix       = get(L"Render", L"DecalFix", c.decalFix);
    if (c.renderResX < 640 || c.renderResX > 7680 || c.renderResY < 480 || c.renderResY > 4320) c.renderResX = c.renderResY = 0;
    c.aimHeadPitch   = get(L"Aim", L"HeadPitch", c.aimHeadPitch);
    c.aimMode        = static_cast<int>(GetPrivateProfileIntW(L"Aim", L"Mode", c.aimMode, ini.c_str()));
    if (c.aimMode < 0 || c.aimMode > 3) c.aimMode = 0;
    c.aimRayUp       = getf(L"Aim", L"RayUp", c.aimRayUp, -30.0f, 30.0f);
    c.hideViewModel  = get(L"Weapon", L"HideViewModel", c.hideViewModel);
    c.hideBody       = get(L"Weapon", L"HideBody", c.hideBody);
    c.throwByHand    = get(L"Hands", L"Throw", c.throwByHand);
    c.armIK          = get(L"Weapon", L"ArmIK", c.armIK);
    c.freeOffHand    = get(L"Weapon", L"FreeOffHand", c.freeOffHand);
    c.shoulderWidth  = getf(L"Weapon", L"ShoulderWidth", c.shoulderWidth, 10.0f, 80.0f);
    c.shoulderDrop   = getf(L"Weapon", L"ShoulderDrop", c.shoulderDrop, 0.0f, 60.0f);
    c.shoulderBack   = getf(L"Weapon", L"ShoulderBack", c.shoulderBack, -30.0f, 30.0f);
    c.throwScale     = getf(L"Hands", L"ThrowScale", c.throwScale, 0.2f, 5.0f);
    c.viewModel      = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"ViewModel", c.viewModel, ini.c_str()));
    if (c.viewModel < 0 || c.viewModel > 2) c.viewModel = 0;
    c.gripX          = getf(L"Weapon", L"GripX", c.gripX, -200.0f, 200.0f);
    c.gripY          = getf(L"Weapon", L"GripY", c.gripY, -200.0f, 200.0f);
    c.gripZ          = getf(L"Weapon", L"GripZ", c.gripZ, -200.0f, 200.0f);
    c.cinemaScreen   = static_cast<int>(GetPrivateProfileIntW(L"Camera", L"CinemaScreen", c.cinemaScreen, ini.c_str()));
    if (c.cinemaScreen < 0 || c.cinemaScreen > 2) c.cinemaScreen = 0;

    c.headTracking      = get(L"Camera", L"HeadTracking", c.headTracking);
    c.headPosition      = get(L"Camera", L"HeadPosition", c.headPosition);
    c.headsetProjection = get(L"Camera", L"HeadsetProjection", c.headsetProjection);
    c.stereo            = get(L"Camera", L"Stereo", c.stereo);
    c.stereoViewState   = get(L"Camera", L"StereoViewState", c.stereoViewState);
    {
        wchar_t u[32] = L"";
        GetPrivateProfileStringW(L"Camera", L"UnitsPerMeter", L"100", u, 32, ini.c_str());
        const float v = static_cast<float>(_wtof(u));
        if (v > 1.0f && v < 1000.0f) c.unitsPerMeter = v;
    }
    c.noMotionBlur   = get(L"Camera", L"DisableMotionBlur", c.noMotionBlur);
    c.noDepthOfField = get(L"Camera", L"DisableDepthOfField", c.noDepthOfField);
    c.xrEnabled      = get(L"OpenXR", L"Enabled", c.xrEnabled);
    wchar_t buf[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"OpenXR", L"RuntimeJson", L"", buf, MAX_PATH, ini.c_str());
    c.xrRuntimeJson = buf;

    MLOG("config: %s -- Enabled=%d Hooks.Direct3DCreate9=%d Bridge.D3D9On12=%d Bridge.Host=%d Bridge.Mirror=%d "
         "Input.Controllers=%d OpenXR.Enabled=%d OpenXR.RuntimeJson=%s Debug.TestWrongBuild=%d",
         present ? "MOHAVR.ini" : "no MOHAVR.ini, defaults", c.enabled, c.hookD3D9, c.d3d9On12, c.bridgeHost, c.bridgeMirror,
         c.controllers, c.xrEnabled,
         c.xrRuntimeJson.empty() ? "(system runtime)" : "(set)", c.testWrongBuild);
    MLOG("config: Camera.HeadTracking=%d HeadPosition=%d HeadsetProjection=%d Stereo=%d UnitsPerMeter=%.1f "
         "DisableMotionBlur=%d DisableDepthOfField=%d", c.headTracking, c.headPosition, c.headsetProjection, c.stereo,
         c.unitsPerMeter, c.noMotionBlur, c.noDepthOfField);
    if (c.bridgeHost && !c.d3d9On12) MLOG("config: Bridge.Host=1 needs Bridge.D3D9On12=1 -- the host will not be started");
    if (c.headTracking && !c.bridgeHost) MLOG("config: Camera.HeadTracking=1 needs Bridge.Host=1 (the host supplies the head pose)");
    if (c.bridgeHost && c.xrEnabled) MLOG("config: Bridge.Host=1 and OpenXR.Enabled=1 -- using the host; in-process OpenXR is off");
    return c;
}

}  // namespace mohavr
