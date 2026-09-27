// MOHAVR.ini, next to the DLL. Every behaviour has a switch (standing rule 7).
#pragma once
#include <string>

namespace mohavr {

struct Config {
    bool enabled        = true;   // [General] Enabled   -- 0: pure dinput8 proxy, nothing else
    bool hookD3D9       = true;   // [Hooks]   Direct3DCreate9
    bool d3d9On12       = true;   // [Bridge]  D3D9On12 -- create the game's IDirect3D9 via Direct3DCreate9On12 (M2)
    bool controllers    = true;   // [Input]   Controllers -- headset controllers drive a virtual Xbox pad 0 (M6; needs Bridge.Host)
    int   hudMode       = 1;      // [HUD] Mode -- 0 = as the game draws it (per eye half), 1 = one head-locked panel (M5)
    float hudDistance   = 2.0f;   // [HUD] Distance (m), Width (m), Down (m below eye level)
    float hudWidth      = 2.4f;
    float hudDown       = 0.1f;
    float hudScale      = 0.5f;   // [HUD] Scale -- the HUD's own pixel size inside the panel (1 = as designed)
    int  renderResX     = 2880;      // [Render] ResX/ResY -- the game's resolution (windowed), 0 = the game's own (M9)
    int  renderResY     = 1620;
    bool lockWindow     = true;   // [Render] LockWindow -- keep the game window at its render size (stereo split)
    bool decalFix       = true;   // [Render] DecalFix -- decals (bullet holes) in the right eye too (ENGINE-NOTES 5r)
    bool aimHeadPitch   = true;   // [Aim] HeadPitch -- the player's pitch (gun, shots) follows the head
    int  aimMode        = 3;      // [Aim] Mode -- 0 the game's (body yaw), 1 head, 2 left hand, 3 right hand (M7)
    bool hideViewModel  = false;  // [Weapon] HideViewModel -- hide the first-person gun (the pawn's HideWeapon exec)
    bool hideBody       = false;  // [Weapon] HideBody -- hide the first-person body/sleeves (RenderBody exec)
    int  cinemaScreen   = 1;      // [Camera]  CinemaScreen -- flat on the host's screen: 1 UI menus, 2 + cinematic cameras (M5)
    bool debugViewState = false;  // [Debug]   ViewState -- write the game camera to %TEMP%\MOHAVR\view_state.txt (tests)
    bool debugSwapEyes  = false;  // [Debug] SwapEyeOrder -- draw the right eye first (experiments only)
    bool debugSwapHalves = false; // [Debug] SwapHalves -- left eye in the right half (experiments only)
    bool debugTraceScissor = false;  // [Debug] TraceScissor -- log scissor rects set while the viewport is offset
    bool debugGameCommands = false;  // [Debug] GameCommands -- run console commands from %TEMP%\MOHAVR\game_cmd.txt (tests)
    int  bridgeMirror   = 1;      // [Bridge]  Mirror -- the host's desktop mirror: 0 off, 1 over the game window, 2 own window
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
