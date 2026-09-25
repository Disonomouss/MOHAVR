// MOHAVR.ini, next to the DLL. Every behaviour has a switch (standing rule 7).
#pragma once
#include <string>

namespace mohavr {

struct Config {
    bool enabled        = true;   // [General] Enabled   -- 0: pure dinput8 proxy, nothing else
    bool hookD3D9       = true;   // [Hooks]   Direct3DCreate9
    bool d3d9On12       = true;   // [Bridge]  D3D9On12 -- create the game's IDirect3D9 via Direct3DCreate9On12 (M2)
    bool controllers    = false;  // [Input]   Controllers -- headset controllers drive a virtual Xbox pad 0 (M6; needs Bridge.Host)
    int   hudMode       = 0;      // [HUD] Mode -- 0 = as the game draws it (per eye half), 1 = one head-locked panel (M5)
    float hudDistance   = 2.0f;   // [HUD] Distance (m), Width (m), Down (m below eye level)
    float hudWidth      = 2.4f;
    float hudDown       = 0.1f;
    float hudScale      = 0.5f;   // [HUD] Scale -- the HUD's own pixel size inside the panel (1 = as designed)
    int  cinemaScreen   = 0;      // [Camera]  CinemaScreen -- flat on the host's screen: 1 UI menus, 2 + cinematic cameras (M5)
    bool debugViewState = false;  // [Debug]   ViewState -- write the game camera to %TEMP%\MOHAVR\view_state.txt (tests)
    int  bridgeMirror   = 0;      // [Bridge]  Mirror -- the host's desktop mirror: 0 off, 1 over the game window, 2 own window
    bool bridgeHost     = true;   // [Bridge]  Host -- start MOHAVR-host.exe and hand it the frames (D10; needs D3D9On12)
    bool testWrongBuild = false;  // [Debug]   TestWrongBuild -- pretend the build check failed (M1 acceptance)
    bool  headTracking      = true;   // [Camera] HeadTracking -- head pose drives the view (M3; needs Bridge.Host)
    bool  headPosition      = true;   // [Camera] HeadPosition -- also apply head translation (6DoF)
    bool  headsetProjection = true;   // [Camera] HeadsetProjection -- render with the headset's FOV
    bool  stereo            = true;   // [Camera] Stereo -- two eye views side by side via the engine's split-screen path (M4)
    bool  stereoViewState   = true;   // [Camera] StereoViewState -- give the right eye its own FSceneViewState (fixes flicker)
    float unitsPerMeter     = 100.0f; // [Camera] UnitsPerMeter -- 100 per the player in stereo (round 4; ENGINE-NOTES 5i)
    bool  noMotionBlur      = true;   // [Camera] DisableMotionBlur -- while head tracking (head motion = camera motion)
    bool  noDepthOfField    = true;   // [Camera] DisableDepthOfField -- while head tracking
    bool xrEnabled      = false;  // [OpenXR]  Enabled -- start an OpenXR session after device creation (M2)
    std::wstring xrRuntimeJson;   // [OpenXR]  RuntimeJson -- if set, XR_RUNTIME_JSON for this process only (D3)
};

Config LoadConfig(const std::wstring& dir);

}  // namespace mohavr
