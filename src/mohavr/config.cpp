#include "config.hpp"

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

#include "log.hpp"
#include "../common/render_presets.hpp"

namespace mohavr {

Config LoadConfig(const std::wstring& dir) {
    const std::wstring ini = dir + L"\\MOHAVR.ini";
    Config c;
    c.iniPath = ini;
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
    c.testWrongBuild = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"TestWrongBuild", c.testWrongBuild, ini.c_str()));
    c.controllers    = get(L"Input", L"Controllers", c.controllers);
    c.debugViewState = get(L"Debug", L"ViewState", c.debugViewState);
    c.debugEyeFloor  = get(L"Debug", L"EyeFloor", c.debugEyeFloor);
    c.debugGameCommands = get(L"Debug", L"GameCommands", c.debugGameCommands);
    c.debugCrashDump = get(L"Debug", L"CrashDump", c.debugCrashDump);
    c.debugCrashDumpTest = get(L"Debug", L"CrashDumpTest", c.debugCrashDumpTest);
    c.debugReserveLow = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"ReserveLow", 0, ini.c_str()));
    c.debugReloadProbe = get(L"Debug", L"ReloadProbe", c.debugReloadProbe);
    c.debugReloadTrace = get(L"Debug", L"ReloadTrace", c.debugReloadTrace);
    c.debugOffHandTrace = get(L"Debug", L"OffHandTrace", c.debugOffHandTrace);
    c.debugMeleeTrace = get(L"Debug", L"MeleeTrace", c.debugMeleeTrace);
    c.debugMuzzleFreeze = static_cast<int>(GetPrivateProfileIntW(L"Debug", L"MuzzleFreeze", 0, ini.c_str()));
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
    c.hudCrosshair   = get(L"HUD", L"Crosshair", c.hudCrosshair);
    c.hudHitMarker   = get(L"HUD", L"HitMarker", c.hudHitMarker);
    c.hudRedirect    = get(L"HUD", L"Redirect", c.hudRedirect);
    {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"HUD", L"Place", L"screen", v, 16, ini.c_str());
        c.hudPlace = !_wcsicmp(v, L"wrist") ? 1 : 0;
    }
    c.hudCanvas = static_cast<int>(GetPrivateProfileIntW(L"HUD", L"WristCanvas", c.hudCanvas, ini.c_str()));
    if (c.hudCanvas < 640 || c.hudCanvas > 2560) c.hudCanvas = 1280;
    c.weaponTracers  = get(L"Weapon", L"Tracers", c.weaponTracers);
    c.leftHandMirror = get(L"Weapon", L"LeftHandMirror", c.leftHandMirror);
    {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Weapon", L"SprintArms", L"idle", v, 16, ini.c_str());
        c.sprintArms = (!_wcsicmp(v, L"game") || !wcscmp(v, L"0")) ? 0 : (!_wcsicmp(v, L"walk") || !wcscmp(v, L"1")) ? 1 : 2;
        GetPrivateProfileStringW(L"Weapon", L"WalkArms", c.walkArms ? L"idle" : L"game", v, 16, ini.c_str());
        c.walkArms = !(!_wcsicmp(v, L"game") || !wcscmp(v, L"0"));
        GetPrivateProfileStringW(L"Weapon", L"JumpArms", c.jumpArms ? L"idle" : L"game", v, 16, ini.c_str());
        c.jumpArms = !(!_wcsicmp(v, L"game") || !wcscmp(v, L"0"));
        auto mode = [&](const wchar_t* key, int def, const wchar_t* moved) {
            GetPrivateProfileStringW(L"Weapon", key, L"", v, 16, ini.c_str());
            if (!_wcsicmp(v, L"hide") || !wcscmp(v, L"0")) return 0;
            if (!_wcsicmp(v, L"game") || !wcscmp(v, L"1")) return 1;
            if (!_wcsicmp(v, moved) || !wcscmp(v, L"2")) return 2;
            return def;
        };
        c.muzzleFlash = mode(L"MuzzleFlash", c.muzzleFlash, L"barrel");
        c.brass = mode(L"Brass", c.brass, L"gun");
    }
    c.catchUp        = get(L"Weapon", L"CatchUp", c.catchUp);
    c.brassMirror    = get(L"Weapon", L"BrassMirror", c.brassMirror);
    c.manualReload   = get(L"Weapon", L"ManualReload", c.manualReload);
    c.reloadHook     = get(L"ManualReload", L"Hook", c.reloadHook);
    c.keepChambered  = get(L"ManualReload", L"KeepChambered", c.keepChambered);
    c.reloadSounds  = get(L"ManualReload", L"Sounds", c.reloadSounds);
    c.dropFall      = get(L"ManualReload", L"DropFall", c.dropFall);
    c.fallTrace     = get(L"ManualReload", L"FallTrace", c.fallTrace);
    c.mountedGame   = get(L"Weapon", L"MountedGame", c.mountedGame);
    c.mountedHands  = get(L"Weapon", L"MountedHands", c.mountedHands);
    c.kick          = getf(L"Weapon", L"Kick", c.kick, 0.0f, 2.0f);
    c.kickAim       = get(L"Weapon", L"KickAim", c.kickAim);
    c.grabPickup    = get(L"Controls", L"GrabPickup", c.grabPickup);
    c.loadoutList   = get(L"Controls", L"LoadoutList", c.loadoutList);
    c.holdOpen       = get(L"ManualReload", L"HoldOpen", c.holdOpen);
    c.reloadGrips   = get(L"ManualReload", L"Grips", c.reloadGrips);
    c.renderResX     = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResX", c.renderResX, ini.c_str()));
    c.renderResY     = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResY", c.renderResY, ini.c_str()));
    c.lockWindow     = get(L"Render", L"LockWindow", c.lockWindow);
    c.decalFix       = get(L"Render", L"DecalFix", c.decalFix);
    // D73: a resolution preset -- the player's menu choice (%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini [Render] Preset), else the
    // shipped [Render] Preset; "custom" keeps ResX/ResY. [Render] UserPreset=0 (the harness's runs) ignores the player's.
    {
        wchar_t key[32] = L"", local[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"Render", L"Preset", L"custom", key, 32, ini.c_str());
        const bool userPreset = GetPrivateProfileIntW(L"Render", L"UserPreset", 1, ini.c_str()) != 0;
        const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        const std::wstring udir = ln > 0 && ln < MAX_PATH ? std::wstring(local) + L"\\MOHAVR\\" : std::wstring();
        if (userPreset && !udir.empty()) GetPrivateProfileStringW(L"Render", L"Preset", key, key, 32, (udir + L"MOHAVR.user.ini").c_str());
        char k8[32] = "";
        for (int i = 0; i < 31 && key[i]; ++i) k8[i] = static_cast<char>(key[i] < 128 ? key[i] : '?');
        const int idx = presets::Find(k8);
        const presets::Preset& p = presets::kPresets[idx];
        int w = p.eyeW, h = p.eyeH;
        std::string how = p.label;
        if (!std::strcmp(p.key, "auto")) {
            const std::wstring hs = udir + L"MOHAVR.headset.ini";
            w = static_cast<int>(GetPrivateProfileIntW(L"Headset", L"EyeWidth", 0, hs.c_str()));
            h = static_cast<int>(GetPrivateProfileIntW(L"Headset", L"EyeHeight", 0, hs.c_str()));
            wchar_t sys[64] = L"";
            GetPrivateProfileStringW(L"Headset", L"System", L"", sys, 64, hs.c_str());
            char s8[64] = "";
            for (int i = 0; i < 63 && sys[i]; ++i) s8[i] = static_cast<char>(sys[i] < 128 ? sys[i] : '?');
            how = w > 0 ? std::string("Auto: what \"") + s8 + "\" asked for last time" : "Auto, but no headset seen yet -- Custom this time";
        }
        if (w > 0 && h > 0) {
            if (w > presets::kMaxEye || h > presets::kMaxEye) {
                const float k = static_cast<float>(presets::kMaxEye) / static_cast<float>(w > h ? w : h);
                w = static_cast<int>(w * k) & ~1;
                h = static_cast<int>(h * k) & ~1;
                how += " (capped at 2560 per eye)";
            }
            c.renderResX = 2 * w;
            c.renderResY = h;
        }
        c.renderPreset = how;
    }
    if (c.renderResX < 640 || c.renderResX > 7680 || c.renderResY < 480 || c.renderResY > 4320) c.renderResX = c.renderResY = 0;
    c.aimHeadPitch   = get(L"Aim", L"HeadPitch", c.aimHeadPitch);
    c.aimMode        = static_cast<int>(GetPrivateProfileIntW(L"Aim", L"Mode", c.aimMode, ini.c_str()));
    if (c.aimMode < 0 || c.aimMode > 3) c.aimMode = 0;
    c.aimRayUp       = getf(L"Aim", L"RayUp", c.aimRayUp, -30.0f, 30.0f);
    c.debugReloadSlowMo = getf(L"Debug", L"ReloadSlowMo", c.debugReloadSlowMo, 1.0f, 100.0f);
    c.rackRoundSpeed = getf(L"ManualReload", L"RackRoundSpeed", c.rackRoundSpeed, 0.0f, 10.0f);
    c.rackRoundUp    = getf(L"ManualReload", L"RackRoundUp", c.rackRoundUp, -1.0f, 5.0f);
    c.rackRoundSpin  = getf(L"ManualReload", L"RackRoundSpin", c.rackRoundSpin, 0.0f, 100.0f);
    c.pouchMag       = get(L"ManualReload", L"PouchMag", c.pouchMag);
    c.rackRoundRest  = getf(L"ManualReload", L"RackRoundRest", c.rackRoundRest, 0.0f, 30.0f);
    c.aimSpread      = getf(L"Aim", L"Spread", c.aimSpread, 0.0f, 1.0f);
    c.aimShotFromGun = get(L"Aim", L"ShotFromGun", c.aimShotFromGun);
    c.aimShotLog     = get(L"Aim", L"ShotLog", c.aimShotLog);
    c.aimLauncherFromGun = get(L"Aim", L"LauncherFromGun", c.aimLauncherFromGun);
    c.hideViewModel  = get(L"Weapon", L"HideViewModel", c.hideViewModel);
    c.hideBody       = get(L"Weapon", L"HideBody", c.hideBody);
    c.landingBody    = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"LandingBody", c.landingBody, ini.c_str()));
    c.throwByHand    = get(L"Hands", L"Throw", c.throwByHand);
    c.armIK          = get(L"Weapon", L"ArmIK", c.armIK);
    c.freeOffHand    = get(L"Weapon", L"FreeOffHand", c.freeOffHand);
    c.freeArmPose    = get(L"Weapon", L"FreeArmPose", c.freeArmPose);
    {
        wchar_t v[512] = L"";
        GetPrivateProfileStringW(L"Weapon", L"FreeHandFrom", L"", v, 512, ini.c_str());
        c.freeHandFrom.clear();
        for (const wchar_t* q = v; *q; ++q) c.freeHandFrom += static_cast<char>(*q < 128 ? *q : '?');
    }
    {
        wchar_t v[1024] = L"";
        GetPrivateProfileStringW(L"Weapon", L"GiveAllList", L"", v, 1024, ini.c_str());
        c.giveAllList.clear();
        for (const wchar_t* q = v; *q; ++q) c.giveAllList += static_cast<char>(*q < 128 ? *q : '?');
    }
    c.freeHandSave   = get(L"Weapon", L"FreeHandSave", c.freeHandSave);
    c.elbowHinge     = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"ElbowHinge", c.elbowHinge, ini.c_str()));
    c.shoulderWidth  = getf(L"Weapon", L"ShoulderWidth", c.shoulderWidth, 10.0f, 80.0f);
    c.shoulderDrop   = getf(L"Weapon", L"ShoulderDrop", c.shoulderDrop, 0.0f, 60.0f);
    c.shoulderBack   = getf(L"Weapon", L"ShoulderBack", c.shoulderBack, -30.0f, 30.0f);
    c.bodyTurn       = get(L"Weapon", L"BodyTurn", c.bodyTurn);
    c.bodyTurnLag    = getf(L"Weapon", L"BodyTurnLag", c.bodyTurnLag, 0.0f, 3.0f);
    c.bodyTurnMax    = getf(L"Weapon", L"BodyTurnMax", c.bodyTurnMax, 0.0f, 180.0f);
    c.throwScale     = getf(L"Hands", L"ThrowScale", c.throwScale, 0.2f, 5.0f);
    c.offHandThrowScale = getf(L"OffHand", L"ThrowScale", c.offHandThrowScale, 0.2f, 5.0f);
    c.offHandPassThrower = get(L"OffHand", L"PassThrower", c.offHandPassThrower);
    c.offHandHudType = get(L"OffHand", L"HudType", c.offHandHudType);
    c.offHandCarrier = get(L"OffHand", L"Carrier", c.offHandCarrier);
    {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"OffHand", L"PistolCredit", L"pistol", v, 16, ini.c_str());
        c.offPistolCredit = _wcsicmp(v, L"main") != 0;
        GetPrivateProfileStringW(L"OffHand", L"PistolRefill", L"game", v, 16, ini.c_str());
        c.offPistolRefill = !_wcsicmp(v, L"instant") ? 2 : !_wcsicmp(v, L"off") ? 0 : 1;
    }
    c.offPistolUpgrades = get(L"OffHand", L"PistolUpgrades", c.offPistolUpgrades);
    c.offPistolKeep = get(L"OffHand", L"PistolKeep", c.offPistolKeep);
    c.offPistolPair = get(L"OffHand", L"PistolPair", c.offPistolPair);
    c.offPistolSlide = get(L"OffHand", L"PistolSlide", c.offPistolSlide);
    c.offPistolFlash = get(L"OffHand", L"PistolFlash", c.offPistolFlash);
    c.offPistolBrass = get(L"OffHand", L"PistolBrass", c.offPistolBrass);
    c.offPistolKick = getf(L"OffHand", L"PistolKick", c.offPistolKick, 0.0f, 1.0f);
    c.meleePhysical = get(L"Melee", L"Physical", c.meleePhysical);
    c.meleeHandSpeed = getf(L"Melee", L"HandSpeed", c.meleeHandSpeed, 0.0f, 10.0f);
    c.meleeButtSpeed = getf(L"Melee", L"ButtSpeed", c.meleeButtSpeed, 0.5f, 20.0f);
    c.meleeBladeHandSpeed = getf(L"Melee", L"BladeHandSpeed", c.meleeBladeHandSpeed, 0.0f, 10.0f);
    c.meleeSlashSpeed = getf(L"Melee", L"SlashSpeed", c.meleeSlashSpeed, 0.5f, 30.0f);
    c.meleeSlashCos = getf(L"Melee", L"SlashCos", c.meleeSlashCos, 0.0f, 1.0f);
    c.meleeThrustSpeed = getf(L"Melee", L"ThrustSpeed", c.meleeThrustSpeed, 0.5f, 20.0f);
    c.meleeThrustCos = getf(L"Melee", L"ThrustCos", c.meleeThrustCos, 0.0f, 1.0f);
    c.meleeThrustTravel = getf(L"Melee", L"ThrustTravel", c.meleeThrustTravel, 0.0f, 1.0f);
    c.meleeMaxTurn = getf(L"Melee", L"MaxTurn", c.meleeMaxTurn, 0.1f, 50.0f);
    c.meleeTravel = getf(L"Melee", L"Travel", c.meleeTravel, 0.0f, 1.0f);
    c.meleeHold = getf(L"Melee", L"Hold", c.meleeHold, 0.0f, 0.5f);
    c.meleeTargetCooldown = getf(L"Melee", L"TargetCooldown", c.meleeTargetCooldown, 0.0f, 5.0f);
    c.meleeMuzzle = get(L"Melee", L"Muzzle", c.meleeMuzzle);
    c.meleeWorld = get(L"Melee", L"World", c.meleeWorld);
    c.meleeProps = get(L"Melee", L"Props", c.meleeProps);
    c.meleeLOS = get(L"Melee", L"LineOfSight", c.meleeLOS);
    c.meleeChargeKill = get(L"Melee", L"ChargeKill", c.meleeChargeKill);
    c.scopeEnable = get(L"Scope", L"Enable", c.scopeEnable);
    c.scopeColumn = static_cast<int>(GetPrivateProfileIntW(L"Scope", L"Column", c.scopeColumn, ini.c_str()));
    if (c.scopeColumn < 0 || c.scopeColumn > 2048) c.scopeColumn = 512;
    c.debugScopeView = get(L"Debug", L"ScopeView", c.debugScopeView);
    c.debugScopeFov = getf(L"Debug", L"ScopeViewFov", c.debugScopeFov, 1.0f, 90.0f);
    c.debugKnifeTrace = get(L"Debug", L"KnifeTrace", c.debugKnifeTrace);
    {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"OffHand", L"KnifeGrip", L"forward", v, 16, ini.c_str());
        c.offKnifeForward = _wcsicmp(v, L"icepick") != 0;
        GetPrivateProfileStringW(L"Knife", L"Require", L"any", v, 16, ini.c_str());
        c.knifeRequire = !_wcsicmp(v, L"carried") ? 2 : !_wcsicmp(v, L"earned") ? 1 : 0;
    }
    c.offKnifeTilt = getf(L"OffHand", L"KnifeTilt", c.offKnifeTilt, -90.0f, 90.0f);
    c.knifeDamage = getf(L"Knife", L"Damage", c.knifeDamage, 1.0f, 10000.0f);
    c.knifeThrustSpeed = getf(L"Knife", L"ThrustSpeed", c.knifeThrustSpeed, 0.5f, 20.0f);
    c.knifeThrustCos = getf(L"Knife", L"ThrustCos", c.knifeThrustCos, 0.0f, 1.0f);
    c.knifeThrustTravel = getf(L"Knife", L"ThrustTravel", c.knifeThrustTravel, 0.0f, 1.0f);
    c.knifeSlashSpeed = getf(L"Knife", L"SlashSpeed", c.knifeSlashSpeed, 0.5f, 30.0f);
    c.knifeSlashCos = getf(L"Knife", L"SlashCos", c.knifeSlashCos, 0.0f, 1.0f);
    c.knifeHandSpeed = getf(L"Knife", L"HandSpeed", c.knifeHandSpeed, 0.0f, 10.0f);
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
    c.damageTint     = get(L"Camera", L"DamageTint", c.damageTint);
    c.fireShake      = get(L"Camera", L"FireShake", c.fireShake);
    c.roomScale      = get(L"Comfort", L"RoomScale", c.roomScale);
    c.jumpLift       = get(L"Camera", L"JumpLift", c.jumpLift);
    c.steadyHeading  = get(L"Camera", L"SteadyHeading", c.steadyHeading);
    c.minEyeHeight   = getf(L"Camera", L"MinEyeHeight", c.minEyeHeight, 0.0f, 150.0f);
    c.steadyLanding  = get(L"Camera", L"SteadyLanding", c.steadyLanding);
    c.physicalCrouch = get(L"Controls", L"PhysicalCrouch", c.physicalCrouch);
    c.crouchDepth    = getf(L"Controls", L"CrouchDepth", c.crouchDepth, 0.15f, 0.80f);
    c.seated         = get(L"Comfort", L"Seated", c.seated);
    c.seatedCrouchDepth = getf(L"Controls", L"SeatedCrouchDepth", c.seatedCrouchDepth, 0.10f, 0.60f);
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
    MLOG("config: Weapon.WalkArms=%s JumpArms=%s SprintArms=%d CatchUp=%d MuzzleFlash=%d Brass=%d Camera.JumpLift=%d (frame "
         "pacing: the host's [Bridge] Pace and its menu)", c.walkArms ? "idle" : "game", c.jumpArms ? "idle" : "game",
         c.sprintArms, c.catchUp, c.muzzleFlash, c.brass, c.jumpLift);
    if (c.bridgeHost && !c.d3d9On12) MLOG("config: Bridge.Host=1 needs Bridge.D3D9On12=1 -- the host will not be started");
    if (c.headTracking && !c.bridgeHost) MLOG("config: Camera.HeadTracking=1 needs Bridge.Host=1 (the host supplies the head pose)");
    if (c.bridgeHost && c.xrEnabled) MLOG("config: Bridge.Host=1 and OpenXR.Enabled=1 -- using the host; in-process OpenXR is off");
    return c;
}

}  // namespace mohavr
