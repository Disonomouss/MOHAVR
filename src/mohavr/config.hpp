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
    bool  hudCrosshair  = false;  // [HUD] Crosshair -- the game's own crosshair (0: hidden; the red dot aims)
    bool  hudHitMarker  = false;  // [HUD] HitMarker -- the red cross on a hit (0: hidden)
    bool  weaponTracers = false;  // [Weapon] Tracers -- the player's tracers (0: none; they start at the gun's game pose)
    int   muzzleFlash   = 0;      // [Weapon] MuzzleFlash -- the player's muzzle flash: 0 hide, 1 game, 2 barrel (experimental)
    int   brass         = 2;      // [Weapon] Brass -- the player's ejected brass: 0 hide, 1 game, 2 from the drawn gun
    bool  leftHandMirror = true;   // [Weapon] LeftHandMirror -- with the gun in the left hand, the arms and gun drawn mirrored
    int   sprintArms    = 2;      // [Weapon] SprintArms -- what the first-person arms play while sprinting: 0 game, 1 walk, 2 idle
    bool  walkArms      = true;   // [Weapon] WalkArms -- the first-person arms play idle while walking too (idle | game)
    bool  jumpArms      = true;   // [Weapon] JumpArms -- ... and while jumping, falling and landing (idle | game)
    bool  catchUp       = true;   // [Weapon] CatchUp -- the gun, arms and free hand follow the body's move since their view
    int  renderResX     = 2880;      // [Render] ResX/ResY -- the game's resolution (windowed), 0 = the game's own (M9)
    int  renderResY     = 1620;
    bool lockWindow     = true;   // [Render] LockWindow -- keep the game window at its render size (stereo split)
    bool decalFix       = true;   // [Render] DecalFix -- decals (bullet holes) in the right eye too (ENGINE-NOTES 5r)
    bool aimHeadPitch   = true;   // [Aim] HeadPitch -- the player's pitch (gun, shots) follows the head
    int  aimMode        = 3;      // [Aim] Mode -- 0 the game's (body yaw), 1 head, 2 left hand, 3 right hand (M7)
    float aimSpread     = 0.0f;   // [Aim] Spread -- the game's shot spread, scaled (0 none, 1 the game's; Mode 1-3)
    bool  aimShotFromGun = true;  // [Aim] ShotFromGun -- the shot starts at the gun, along the red dot's ray (modes 2/3)
    bool  aimShotLog    = true;   // [Aim] ShotLog -- log where each shot went, against the red dot (first 400)
    float aimRayUp      = 8.0f;   // [Aim] RayUp -- cm the hand's aim ray is raised to the gun's barrel (ViewModel=2 only)
    bool hideViewModel  = false;  // [Weapon] HideViewModel -- hide the first-person gun (the pawn's HideWeapon exec)
    bool throwByHand    = true;   // [Hands] Throw -- grenades fly with the gun hand's velocity at the trigger release (M8)
    float throwScale    = 2.2f;   // [Hands] ThrowScale -- times the hand's speed
    bool armIK          = true;   // [Weapon] ArmIK -- the arms reach from the body to the gun in the hand (M8)
    bool freeOffHand    = true;   // [Weapon] FreeOffHand -- off the foregrip the support hand follows the other controller
    bool  freeArmPose = true;  // [Weapon] FreeArmPose -- the free arm starts from the long gun's arm pose (pistol, grenade)
    int   elbowHinge = 2;  // [Weapon] ElbowHinge -- 0 each arm segment on its own, 1 on the elbow's hinge, 2 the forearm carried by the upper arm
    float shoulderWidth = 30.0f, shoulderDrop = 22.0f, shoulderBack = 6.0f;  // [Weapon] Shoulder* -- cm, from the head
    int  viewModel      = 2;      // [Weapon] ViewModel -- 0 the game's (flat FOV trick), 1 true 3D, 2 in the aiming hand (M8)
    float gripX = 34.0f, gripY = 11.0f, gripZ = -17.0f;  // [Weapon] GripX/Y/Z -- the camera-frame point (Unreal units)
                                                     // put at the controller (fwd/right/up)
    bool hideBody       = false;  // [Weapon] HideBody -- hide the first-person body/sleeves (RenderBody exec)
    int  cinemaScreen   = 1;      // [Camera]  CinemaScreen -- flat on the host's screen: 1 UI menus, 2 + cinematic cameras (M5)
    bool debugViewState = false;  // [Debug]   ViewState -- write the game camera to %TEMP%\MOHAVR\view_state.txt (tests)
    bool debugSwapEyes  = false;  // [Debug] SwapEyeOrder -- draw the right eye first (experiments only)
    bool debugSwapHalves = false; // [Debug] SwapHalves -- left eye in the right half (experiments only)
    bool debugTraceScissor = false;  // [Debug] TraceScissor -- log scissor rects set while the viewport is offset
    bool debugReflect   = false;  // [Debug] Reflect -- log the class/property layout of the player's pawn once (research)
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
    bool  jumpLift          = false;  // [Camera] JumpLift -- the game's camera lift on a jump (fJumpCameraOffset) in the view
    bool xrEnabled      = false;  // [OpenXR]  Enabled -- start an OpenXR session after device creation (M2)
    std::wstring xrRuntimeJson;   // [OpenXR]  RuntimeJson -- if set, XR_RUNTIME_JSON for this process only (D3)
};

Config LoadConfig(const std::wstring& dir);

}  // namespace mohavr
