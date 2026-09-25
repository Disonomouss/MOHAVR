# Engine notes: Medal of Honor: Airborne

Measured facts only. Each entry says how it was verified. Anything not yet measured goes under
§9 Open questions, not in the tables above it.

## 1. Build pinned by these notes

| | |
|---|---|
| Game | Medal of Honor: Airborne, Steam app **24840**, Steam build **3648** |
| Executable | `UnrealEngine3\Binaries\MOHA.exe`, 15,249,408 bytes |
| SHA-256 | `998BAB97F416CF8D988D8FE2DC71243C19AAEAD1D9DA78D5DA77F23BF246FF04` |
| Linker | MSVC 8.0 (VS2005), linker timestamp 2008-02-23 09:35:52 UTC |

Every address in these notes, and in the mod's address header, is for this hash. The mod
should check it at start-up and stand down on a mismatch.

## 2. PE layout

| Fact | Value | Evidence |
|---|---|---|
| Architecture | x86, 32-bit (PE machine `0x14C`) | PE header |
| Image base | `0x10900000` | PE header; Ghidra metadata |
| ASLR | **No**: `DllCharacteristics = 0x0000` (no DYNAMIC_BASE, no NX) | PE header. Always loads at the image base, so fixed VAs are stable. |
| Large-address-aware | **No** | PE header. The process has 2 GB of user address space, a budget for D3D9On12, the OpenXR runtime and extra eye render targets. |
| Sections | `.text` 9.57 MB, `.textidx`, `CONST`, `.rdata` 3.26 MB, `.data`, `.rsrc`, `.reloc`, `.bind` | PE header |

## 3. SteamStub DRM wrapper

| Fact | Value | Evidence |
|---|---|---|
| Wrapper | SteamStub **Variant 2.1 (x86)** | Steamless 3.1.0.5 identification |
| Stub entry | VA `0x1182D2ED` (RVA `0xF2D2ED`), in `.bind` | PE header |
| Code encryption | **None**: flags `0x26` include NoEncryption | Decoded payload (`tools/unwrap_steamstub21.py`). `.text` entropy 6.54 bits/byte, typical of x86 code. |
| Original entry (OEP) | VA **`0x1112B7EA`**: `call __security_init_cookie; jmp __tmainCRTStartup` (bytes `e8 d5 08 00 00 e9 35 fd ff ff`) | Decoded payload, using Steamless's own offset logic |
| Steamless | Crashes in step 5: the stub's "code section VA" is garbage (`0x73E98FEA`) | Reproduced 3×; source read |
| Unwrapped copy | `work/MOHA.exe.unpacked.exe`, entry point = OEP, `.bind` kept (never runs) | `tools/unwrap_steamstub21.py` |

Implication: static analysis of the original exe is valid, because nothing is encrypted. The
stub runs at start-up and then jumps to the OEP. A DLL that the Windows loader brings in (a
proxy DLL) is loaded before the stub or `WinMain` runs.

## 4. Start-up path (named in the Ghidra project)

| VA | Name | Evidence |
|---|---|---|
| `0x1112B7EA` | `entry_OEP` | §3 |
| `0x1112B529` | `__tmainCRTStartup` | Decompiled: `GetStartupInfoA`, `_acmdln` parsing, then calls WinMain |
| `0x10918200` | `WinMain` | Called as `WinMain(0x10900000, 0, cmdline, nShowCmd)` from `__tmainCRTStartup` |

Ghidra's first auto-analysis started at the stub entry and missed this path. These three
functions were created by hand and saved (2026-09-25).

Other names saved in the Ghidra project (2026-09-25; evidence in §5a/§5b): `InitD3D9Device`
`0x10902DD0`, `CheckD3D9Caps` `0x10902D10`, `FindClosestDisplayMode` `0x109036E0`,
`WindowsClientInit` `0x10921B10`, `CreateDIKeyboard` `0x109229F0`, `EnumJoysticksCallback`
`0x10922F40`.

## 5. Imports and APIs of interest

| API / DLL | Present | Evidence | Relevance |
|---|---|---|---|
| `d3d9.dll`, `Direct3DCreate9` | yes | string scan of the exe | Hook point for the D3D9On12 bridge (lessons §2) |
| `d3dx9_*` | yes | string scan | |
| D3D10 (`d3d10`, `D3D10CreateDevice`) | **no** | string scan | There is no D3D10 path to force off |
| `dinput8`, `DirectInput8Create` | yes, IAT `0x112C6040` | Ghidra import table | See §5a |
| `XInputGetState` / `XInputSetState` | yes, **by ordinal** (2 and 3), IAT `0x112C6804` | Ghidra import table | Gamepads are read through XInput. Callers `0x10924232` (in `FUN_10923d70`) and `0x10922FC1` (in `FUN_10922f40`, the DirectInput joystick enumeration callback). |
| `PhysXLoader` | yes | string scan; DLLs in `Binaries` | |
| `-log`, `Launch.log` | strings present | string scan | UE3 log available for a log-driven harness. Location not yet observed. |

### 5a. DirectInput devices (Ghidra, GUIDs and data formats read from the exe)

| What | Where | Detail |
|---|---|---|
| `IDirectInput8A` | global `0x116AE36C` | `DirectInput8Create(hInst, 0x800, IID_IDirectInput8A @0x1141B66C, ...)` in `FUN_10921b10` (window and input init) |
| Mouse device | global `0x116AE3CC` | `CreateDevice(GUID_SysMouse @0x1141B5CC)`. `SetDataFormat(@0x1141B39C)` = c_dfDIMouse (16 bytes, 7 objects). `SetProperty` 1 (BUFFERSIZE) and 2. Buffered, so reads probably go through `GetDeviceData`. |
| Joysticks | — | `EnumDevices(DI8DEVCLASS_GAMECTRL, callback FUN_10922f40)`; the callback also calls XInput |
| Keyboard device | `FUN_109229f0` | Its own `DirectInput8Create`, then `CreateDevice(GUID_SysKeyboard @0x1141B5BC)`. `SetDataFormat(@0x1141B194)` = c_dfDIKeyboard (256). `SetProperty` 1 (BUFFERSIZE), then Acquire. How it's read, and whether gameplay uses it or window messages, is unverified. |

Implication for M6: hook `IDirectInputDevice8::GetDeviceData` and `GetDeviceState` at the vtable
(covers both devices, however they're read), plus XInput ordinal 2. `SendInput` probably won't
reach mouse-look; keyboard is unknown (PLAN harness item).

### 5b. Direct3D 9 device creation (Ghidra)

| What | Where | Detail |
|---|---|---|
| `Direct3DCreate9` | IAT `0x112C6818`, thunk `0x10F29C30` | 3 callers: `FUN_10902dd0` (device init), `FUN_10902d10` (caps check: `GetDeviceCaps`, shader version < 3 sets a flag), `FUN_109036e0` (`EnumAdapterModes` for X8R8G8B8, picks the closest resolution) |
| `IDirect3D9` | global `0x116DE438` | `Direct3DCreate9(D3D_SDK_VERSION 0x20)` |
| `IDirect3DDevice9` | global `0x116DE43C` | |
| **CreateDevice call** | **`0x1090339A`** (`CALL EAX`, `EAX = vtbl[0x40]`), in `FUN_10902dd0` | `(adapter 0, D3DDEVTYPE_HAL, hFocusWnd, 0x142, &pp, &0x116DE43C)`. Flags 0x142 = FPU_PRESERVE, HARDWARE_VERTEXPROCESSING, DISABLE_DRIVER_MANAGEMENT. Retried with `Sleep(500)` on DEVICELOST/NOTAVAILABLE. |
| Present parameters | stack struct, `memset 0x38` just before | BackBuffer W×H from the requested resolution; Format `0x15` (A8R8G8B8); BackBufferCount 1; `Windowed = !fullscreen`; SwapEffect COPY (3) if windowed, DISCARD (1) if fullscreen; EnableAutoDepthStencil 0; Flags 1 (LOCKABLE_BACKBUFFER) |
| **PresentationInterval** | written at `0x10902FE2` | **IMMEDIATE (0x80000000) when windowed** or when the vsync flag at `0x116CB6E8` is 0; ONE (1) only when fullscreen **and** vsync is on. Windowed mode therefore has no vsync, which is what lessons §2 needs under 9On12. |
| Device reset | `vtbl[0x40]` on the device (`Reset`), loop in the same function | |

## 5c. Command-line switches recognized (UTF-16 strings in the exe)

`WINDOWED`, `FULLSCREEN`, `ResX=`, `ResY=`, `VSYNC`, `ONETHREAD`, `NOSOUND`, `NOSPLASH`,
`ABSLOG`, `SEEKFREELOADING`, and `log`/`Launch.log`. `ONETHREAD` existing implies this build
has a separate render thread, which lessons §2 flags for stereo.

**Measured, first `-log` run (2026-09-25, `steam.exe -applaunch 24840 -windowed ResX=1920 ResY=1080 -log`):**
- Steam passes the arguments through unchanged: MOHA's command line is
  `MOHA.exe -windowed "ResX=1920" "ResY=1080" -log`. MOHA.exe starts about 2 s after the call.
- `-windowed ResX=1920 ResY=1080` works: window class `LaunchUnrealUWindowsClient`, title
  "Medal of Honor Airborne", **client area 1920×1080**, windowed. **Nothing** in the user
  folder changed, so the switches don't persist.
- **`-log` writes no file.** It opens a console window (`ConsoleWindowClass` and a `conhost.exe`
  child, 160×4000 buffer), but the shipping build prints **nothing** to it (0 lines after
  about 60 s at the main menu). No `Launch.log` anywhere (game folder, user folder, temp). The
  engine offers no log lines for the harness to wait on; progress must come from screenshots or
  the window until the mod writes its own log. `tools/read_game_console.py` reads that console
  in case some build or switch does print.
- The main menu is reached by about 60 s (not timed precisely). The menu is Campaign,
  Multiplayer, Options, Quit, right-aligned at x ≈ 1415 and y ≈ 290/363/433/505 (1920×1080).
- **SendInput keyboard reaches the menu:** `sendkey.ps1 down` (scan code 0x50) moved the
  highlight from Campaign to Multiplayer. Gameplay input (DirectInput, §5a) is untested.
- `capture-window.ps1` (PrintWindow) captures the D3D9 window correctly.
- `WM_CLOSE` to the main window closes the game cleanly in 0.5 s.
- **Memory at the main menu:** working set 375 MB, private 579 MB, **virtual 999 MB**, which is
  half of the 2 GB address space (the exe is not large-address-aware, §2).

**Measured with the harness (2026-09-25, 5 runs, `tools/harness.ps1`):**
- Front end: window after about 3 s → main menu after 11–12 s → Enter (Campaign) → Campaign
  Menu after 2 s (New, **Continue**, Select Campaign, Stats & Medals, Extras, Back; left-aligned,
  x ≈ 500, y ≈ 221/293/366/439/514/586). New is highlighted by default, and Down moves to
  Continue. Enter on Continue → black loading screen → **gameplay 11 s later**.
- **The player's save resumes mid-parachute descent over the flak tower** (mission "Secure
  Flak Tower"), looking down; it lands about 15 s later ("Botched Landing"). That gives a
  deterministic camera path every run, useful for M2–M4 comparisons.
- **Virtual memory in gameplay: 1,324–1,346 MB** (working set about 720 MB, private about
  915 MB), so **about 700 MB of address space is left** for everything the mod adds (D3D9On12,
  the OpenXR runtime and loader, eye render targets).
- **SendInput reaches gameplay:** relative mouse moves (`look.ps1`) turn the camera (frame
  difference 40–59, against under 5 for a static scene), so injected input reaches the buffered
  DirectInput mouse. Esc opens the pause menu (tabs Objectives, Options, Save & Load), and Esc
  again resumes.
- Loading screens are pure black (mean luminance < 8).
- Screen checks (`tools/harness-ref/checks.json`): `mainmenu` and `campaignmenu` (title crops on
  the metal frame) and `pausemenu` (tab bar) separate cleanly (0.0 against 25–43 cross-scores).
  Gameplay is proven actively: Esc must open the pause menu.

## 5d. Load order with the MOHAVR proxy (measured in M1, 2026-09-25)

From `MOHAVR.log`, 2 runs:
- `dinput8.dll` (the proxy) `DllMain` runs on the **main thread** 1–2 ms into the log; init
  (log, ini, build check, IAT hook) takes about 4 ms. This is before the SteamStub entry.
- The IAT slot `0x112C6818` holds exactly `d3d9!Direct3DCreate9` at that point: the loader has
  already bound the exe's imports, and SteamStub does not rebind them afterwards (the hook fires).
- **`Direct3DCreate9` is called once**, about 730 ms after init, on the main thread, with SDK
  0x20. The other two callers (§5b) reuse the global `IDirect3D9` at `0x116DE438`.
- `DirectInput8Create` is called twice, at about 1.64 s and 1.69 s (mouse init, then
  keyboard), version 0x0800.
- The proxy costs about 1 MB of virtual memory (1,336–1,337 MB in gameplay, against 1,335 MB
  without it).

## 5e. Presentation and D3D9On12 (measured in M2, 2026-09-25)

**The present path** (`MOHAVR.log`, both variants):
- The game presents with **`IDirect3DDevice9::Present`** (vtable slot 17) on the device's
  implicit swap chain. It never calls `CreateAdditionalSwapChain`.
- **Present runs on a separate render thread**, not the main thread: UE3's render thread (the
  `ONETHREAD` switch, §5c). `CreateDevice` itself runs on the main thread.
- **The menus are uncapped:** about 1,300 presents/s in plain D3D9 and about 1,000/s under
  9On12, using about 2 CPU cores.
- **The Steam overlay** (`gameoverlayrenderer.dll`) has already hooked `IDirect3D9::CreateDevice`
  in plain D3D9 (vtable slot 16 points into it). Under 9On12 it doesn't hook. MOHAVR's hooks chain
  on top of whatever is there.
- `CreateDevice` succeeds the first time in both variants, with the parameters from §5b:
  1920×1080, fmt 21, 1 backbuffer, no MSAA, COPY, windowed, no auto depth, lockable,
  IMMEDIATE, flags 0x142.

**D3D9On12** (`Bridge.D3D9On12=1`: `Direct3DCreate9On12` with no D3D12 device supplied):
- **Works:** `CreateDevice` returns `S_OK`, the game renders correctly into the backbuffer
  (verified by the mod's backbuffer capture in gameplay), and the harness cycle completes.
- **The game window stays white:** every Present returns `S_OK`, but nothing reaches the window.
  Neither PrintWindow nor an on-screen copy shows the frame. The game is not hung. Cause not yet
  investigated (candidates: COPY swap effect or lockable backbuffer under 9On12, window/GDI
  interaction). It doesn't block VR (the frame goes to the headset), but a desktop mirror will
  have to come from the mod.
- **Address-space cost in gameplay** (`tools/vmmap.py`, same scene, 20 s after landing):

| | used | free | largest free blocks |
|---|---|---|---|
| plain D3D9 (mod loaded) | 1,356 MB | 692 MB | 496, 134, 8.6 MB |
| D3D9On12 | 1,493 MB | 555 MB | **331**, 134, 32 MB |
| **change** | **+137 MB** | −137 MB | largest −165 MB |

  The NVIDIA D3D12 user-mode driver `nvwgf2um.dll` (58 MB) replaces `nvd3dum.dll` (44 MB), plus
  `D3D12Core.dll` and more mapped and private memory. `nvgpucomp32.dll` (85 MB) is loaded in both.

**The OpenXR session in-process** (M2 step 1: static loader, D3D11 device on the runtime's
adapter, one 1920×1080 quad swapchain; runtime = the 32-bit OpenXR Simulator):
- Works: the session goes READY → SYNCHRONIZED → VISIBLE → FOCUSED about 100 ms after the
  thread starts, and submits at 60 Hz. The quad shows in both eyes of the simulator preview.
- **Address space in gameplay:**

| configuration | used | free | largest free blocks |
|---|---|---|---|
| plain D3D9 | 1,356 MB | 692 MB | 496 / 134 MB |
| D3D9On12 | 1,493 MB | 555 MB | 331 / 134 MB |
| plain D3D9 + XR session | 1,622 MB | 426 MB | 231 / 132 MB |
| **D3D9On12 + XR session** | **1,751 MB** | **297 MB** | **132 / 108 MB** |

  The OpenXR side costs about 258 MB on top of 9On12, before any eye render targets or shared
  textures. **Too tight to build the renderer on** (see D10). The simulator runtime's own
  footprint (its preview renderer, D3DCompiler_47) is included and differs from a real runtime's.
- The simulator's preview window belongs to the game process, so `Process.MainWindowHandle` can
  point at it. Find the game window by class `LaunchUnrealUWindowsClient` (the harness and
  `focus-game.ps1` now do).

## 6. Content and UnrealScript

| Fact | Value | Evidence |
|---|---|---|
| Package format | Cooked UE3 packages `*.xxx` in `MOHAGame\CookedPC` (1,055 files) | directory listing; umodel |
| Package version | **421 / licensee 11**, engine version 2859, cooker version 38 | UELib and umodel headers |
| Compression | LZO (CompressionFlags 2) | UELib header dump |
| Script packages | `Core`, `Engine`, `GameFramework`, `MOHAGame` (26 MB, 85 MB decompressed) | Decompressed into `work/decompressed` |
| Engine.xxx | 20,103 exports; 20,235 objects, 3,270 functions once loaded | umodel `-list`; UELib |

**Camera path (VR-relevant):** `PlayerController.GetPlayerViewPoint(out Location, out Rotation)`
creates `PlayerCamera` from `CameraClass` if needed, then calls
`PlayerCamera.GetCameraViewPoint(out_Location, out_Rotation)`. Without a camera it falls back
to the view target's `Location`/`Rotation`. Evidence: UELib decompile of the decompressed
`Engine.xxx`. Native operators appear as `__NFUN_nnn__` (for example 114 and 119 are the
object `==` and `!=`).

All 2,483 classes are decompiled to `work/script/<Package>/<Class>.uc` by
`tools/dump-script.ps1` (Core 25, Engine 1,070, GameFramework 24, MOHAGame 1,364; 0 failures).

**Single-player view chain (all UnrealScript):**

| Step | Class.function | Notes |
|---|---|---|
| 1 | `MOHAPlayerController` | `CameraClass = MOHAGame.MOHAPlayerCamera` (multiplayer uses `MOHAMultiplayerCamera`) |
| 2 | `Camera.UpdateCamera(DeltaTime)` | `CheckViewTarget` (native), `UpdateViewTarget`, blends to `PendingViewTarget`, then `ApplyCameraModifiers` |
| 3 | `MOHAPlayerCamera.UpdateViewTarget` | super, then `Controller(Target).Pawn.CalcCamera(DeltaTime, POV.Location, POV.Rotation, POV.FOV)` |
| 4 | `MOHAPlayerPawn.CalcCamera` | 7 versions (per state) in `MOHAPlayerPawn.uc`; the pawn class is `MOHASingleplayerPawn` |
| 5 | `Camera.CameraCache.POV` → `GetCameraViewPoint` → `PlayerController.GetPlayerViewPoint` | Read natively when building the view (native side not yet located) |

Camera modifier: `MOHACamMod_ScreenShake` (screen shake; a comfort option for VR). Defaults:
`MOHAPlayerCamera.DefaultFOV = 80`. `DefaultPlayer.ini`: `fDefaultViewmodelFOV = 80`,
`fSprintFOV = 95`. `DefaultWeapon.ini`: per-weapon `PlayerFOV`, `IronsightsTarget{World,Player}FOV`
and scope FOVs. There are separate world and viewmodel FOVs, and VR must pin both.

**Split-screen: stock UE3, and not overridden by MOHA.** `GameViewportClient` has
`ESplitScreenType` with `eSST_2P_VERTICAL` (`SplitscreenInfo[2]` = two halves, 0.5 × 1.0, at
x = 0 and 0.5), plus `CreatePlayer`, `exec DebugCreatePlayer` and `exec SetSplit`.
`MOHAGameViewportClient` overrides none of that. **Lead for M4 (unverified):** a second
`LocalPlayer` in 2P-vertical split-screen could give side-by-side stereo in one scene render.
Whether the PC renderer still draws multiple player views, and what a second player spawns in
single-player, is untested.

## 7. Configuration

- `MOHAGame\Config\Default*.ini` exists: Engine, Game, Input, Player, Weapon, AI and others.
- Display: `[WinDrv.WindowsClient]` has `StartupResolutionX/Y` and `StartupFullscreen`
  (`BaseEngine.ini`: 1280×720, fullscreen; `DefaultLauncherSettings.ini` "Global" profile:
  1280×1024, fullscreen). There is no vsync key in any shipped ini; the flag is at `0x116CB6E8`
  (§5b).
- Running windowed for the harness should use the command line (`-windowed ResX=… ResY=…`, §5c),
  so the player's ini is never edited.
- **User folder** (created by the game on first run; Documents is redirected to OneDrive here):
  `C:\Users\j_tom\OneDrive\Documents\EA Games\Medal of Honor Airborne(tm)\`
  - `Config\MOHA{AI,Editor,EditorUserSettings,Engine,Game,Input,Juice,Player,Settings,Weapon}.ini`.
    These are the **player's files**: back them up and restore them around every test.
  - `Saved\MOHASAVEDGAME` (36 KB) and `Saved\Profile`: the player's progress.
  - No `Logs\`. Even `-log` writes no file in this shipping build (§5c).
  - Resolve the path through `SHGetKnownFolderPath(FOLDERID_Documents)`, not `%USERPROFILE%\Documents`.
- The player's current settings (read-only): `MOHAEngine.ini` / `MOHASettings.ini`
  `[WinDrv.WindowsClient]` fullscreen at 2560×1440. `MOHASettings.ini [MOHAGame.MOHAScalabilityOptions]`
  has `ForceVSync = False` and `ScreenPercentage = 100.0`, with motion blur, depth of field and
  bloom on. `ForceVSync` is probably what drives the flag at `0x116CB6E8` (unverified).
  The hardware the game detected: RTX 4070 Ti, 24 logical cores, 64 GB RAM.
- **Control table** (`MOHAInput.ini [Engine.PlayerInput]`, 66 bindings; `DefaultInput.ini` has
  38). Actions map to commands, and keys map to actions:
  - Movement axes `aBaseY`/`aStrafe` (W/S/A/D), look `aMouseX`/`aMouseY`; `Duck`/`Crouch`
    (`bCrouch`), `Walking` (`bRun`).
  - Weapons: `FireWeapon` (`bFire | StartFire | MouseFire`), `Ironsights` (RMB, `bAim`),
    `Reload` (R), `Melee` (F; `AirDropMeleePress`), `SwitchGrenade` (G), `SwitchPistol`,
    `SwitchPrimary` and `SwitchSecondary` (1/2/3), `ZoomIn`/`ZoomOut` (X/Z, scroll wheel),
    `AltWeaponFireToggle` (MMB).
  - MOHA-specific: `FlareChuteJumpMultiCmd` (Space: flare the parachute, jump, trigger),
    `SprintIronsightsMove` (`bSprint`), `UseAction` (E; also `SkipBriefing`).
  - **Gamepad bindings are not in any ini.** They're handled in `MOHAPlayerInput.uc` (98
    `XboxTypeS_*` references).
- Native functions worth a look for M7 (from the UELib export comments in
  `MOHAPlayerController.uc`): `execReticuleActorTrace`, `execGetReticuleState`.

## 8. Rendering

Nothing measured yet. See §9.

## 9. Open questions (unverified; do not rely on these)

- Where UE3 builds the view and projection matrices natively (`FSceneView`, or the equivalent in
  this 2007 branch). Look for where `GetPlayerViewPoint` results enter native code.
- Whether the renderer draws two `LocalPlayer` views in 2P-vertical split-screen in single-player
  (§6 lead), and at what cost.
- The render thread is compiled in (`ONETHREAD` exists, §5c). Is it on by default on PC?
- How the keyboard is read in gameplay: DirectInput buffered, or window messages (§5a).
- Near planes: the culler's versus the projection's (lessons §2 warns there may be two).
- Whether about 700 MB of address-space headroom (§5c) is enough for D3D9On12, the OpenXR runtime
  and the eye targets. If not, options include making the process large-address-aware in memory
  (not possible after load), a smaller footprint, or an out-of-process compositor bridge.
