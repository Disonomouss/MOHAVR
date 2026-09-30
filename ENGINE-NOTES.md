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

## 5f. The out-of-process bridge (D10, measured 2026-09-25)

`Bridge.D3D9On12=1` + `Bridge.Host=1`, host runtime = the x64 OpenXR Simulator.
- **Game side** (render thread, first Present): `IDirect3DDevice9On12::GetD3D12Device`, then its
  own DIRECT queue, 3 allocators, a list, 2 shared fences, a 3-slot ring of shared
  `B8G8R8A8_UNORM` textures (`ALLOW_RENDER_TARGET | ALLOW_SIMULTANEOUS_ACCESS`, `HEAP_FLAG_SHARED`)
  and a D3D9 `A8R8G8B8` render target. Ready about 2.4 s into the process.
- Per published frame: `StretchRect` backbuffer → our RT, `UnwrapUnderlyingResource` onto our
  queue, `CopyResource` into the slot (**no explicit barriers needed**: the copy works from the
  unwrapped state), signal the game fence, `ReturnUnderlyingResource` with that fence.
- **Host side** (x64): handles arrive through `DuplicateHandle` from the game (the values are in
  the shared block), opened with `ID3D11Device1::OpenSharedResource1` and
  `ID3D11Device5::OpenSharedFence`. Same GPU LUID (`0000AB90`) for the game and the runtime.
- **Rate:** frames go 1:1 with the host's XR loop (900 published = 900 received in 15 s, 60 Hz in
  the simulator). The game skips publishing while the host hasn't acknowledged, never blocks,
  and still runs uncapped.
- **Verified content:** the host's readback of received frames gives mean luma 72.6 (main menu)
  and 56.7. The simulator preview shows the gameplay frame on the quad in both eyes.
- **Game address space in gameplay:** used 1,543 MB, **free 505 MB, largest block 312 MB**,
  against 1,493/555/331 MB for 9On12 alone. The bridge costs the game about 50 MB, and OpenXR
  costs it nothing. (In-process OpenXR had left 297/132 MB.)
- **Lifecycle:** the host exits by itself when the game exits ("the game exited -- shutting
  down", exit 0). If the host is missing or fails, the game logs it and runs without VR.
- The Steam overlay injects into `MOHAVR-host.exe` too (its toast appears over the simulator
  preview), since the host is a child of a Steam-launched game. Harmless so far.
- **Real headset (HEADSET-TESTS round 1, 2026-09-25):** runtime "VirtualDesktopXR" 1.0.10 (x64,
  the system runtime), system "Meta Quest 3", **90 Hz**, and 98.8% of XR frames carried a new
  game frame. The player reported the image stable, smooth and correctly coloured. The game ran in
  the player's own **fullscreen 2560×1440** mode (`windowed 0`, swap DISCARD), so the bridge works
  fullscreen as well as windowed; the host sizes its swapchain from the shared header.

## 5g. Native view construction: ULocalPlayer::CalcSceneView (RE 2026-09-25, Ghidra)

Found by following the name string `GetPlayerViewPoint` (`0x1152AEA0`) through the engine's
name-registration table (`FUN_10D7B750`: build FName with `CALL 0x109E2CF0`, store in a global),
to the FName global `0x116F9280`, and then to its only reader.

| VA | What | Evidence |
|---|---|---|
| `0x10B33A80` | `APlayerController::eventGetPlayerViewPoint`. **Register args:** EDI = `FVector* out_Location`, ESI = `FRotator* out_Rotation`, pushed = the controller. `ProcessEvent` is at vtable `+0xF0`. | decompile; FName `0x116F9280`; 9 callers |
| **`0x10C19910`** | **`ULocalPlayer::CalcSceneView`**. Stack args: `[EBP+8]` = this (ULocalPlayer), `+0xC` = ViewFamily, **`+0x10` = `FVector* ViewLocation`, `+0x14` = `FRotator* ViewRotation`**, `+0x18` = Viewport. Prologue `55 8B EC 83 E4 F0`. | decompile + disasm |
| `this+0x40` | the PlayerController (`Actor`) | `[EDI+0x40]` then GetPlayerViewPoint |
| `this+0x68/0x6C/0x70/0x74` | split-screen Origin X/Y and Size X/Y (floats, fractions of the viewport) | multiplied with the viewport's size X/Y |
| `PC+0x2E4` | `PlayerCamera`; the FOV is `Camera+0x1EC`; flags at `Camera+0x1F0` (bit 2 = constrained aspect, bit 4, bit 0x10); `ConstrainedAspectRatio` = `Camera+0x1F8` | decompile |
| `PC+0x320` | the FOV used when there is no PlayerCamera | decompile |
| `0x116DCB78` | "locked view" flag: when set, the location and rotation come from globals `0x116F7AD4` / `0x116F7B78` instead of GetPlayerViewPoint | decompile |
| **`0x10C19B3C`** | **merge point after the view point is known** (both branches jump here): `8B 77 40  E8 ...` (`MOV ESI,[EDI+0x40]; CALL 0x10BEDFF0`). Hook here to rewrite ViewLocation and ViewRotation through `[EBP+0x10]` and `[EBP+0x14]`. | disasm |
| `0x10919120` / `0x10918FC0` | `FInverseRotationMatrix` construction / `FMatrix` multiply; the view matrix = T(−Loc) · InvRot(Rot) · axis swap `[0 0 1 0; 1 0 0 0; 0 1 0 0; 0 0 0 1]` | decompile |
| **`0x10BED9F0`** | **`FPerspectiveMatrix(HalfFOV, Width, Height, MinZ)`**: 4 floats on the stack, **output matrix pointer in ESI**, returned in EAX. Layout (row-major, row vectors): `M00 = 1/tan(h)`, `M11 = (W/H)/tan(h)`, `M22 = 0.999`, `M23 = 1`, `M32 = −MinZ·0.999`, the rest 0. **HalfFOV is horizontal.** Infinite far plane, Z not reversed. | decompile |
| `0x10C19EA6` → ret `0x10C19EAB` | the normal projection call: `FPerspectiveMatrix(FOV·π/360, W·aspect…, H, 5.0)`. At `0x10C19EAB` (`B9 10 00 00 00`, `MOV ECX,0x10`), EAX points at the fresh matrix, just before it's copied out. | disasm |
| `0x10C19DAA` → ret `0x10C19DAF` | the constrained-aspect projection call (`Camera+0x1F8` aspect). At `0x10C19DAF` (`8B F0`, `MOV ESI,EAX`) EAX points at the matrix. | disasm |
| — | **Near clip = 5.0 units**, hard-coded (`0x40A00000`) at both call sites. | disasm |
| `0x10A96A20` | the `FSceneView` constructor (allocated 0x1E0 bytes) | decompile |

## 5h. Head tracking and headset projection (M3, measured 2026-09-25, x64 simulator)

- The safetyhook MidHooks at `0x10C19B3C` (view merge), `0x10C19EAB` and `0x10C19DAF` (after
  FPerspectiveMatrix) install cleanly from the CreateDevice hook, and the build check covers all
  10 signatures.
- Axis and rotation mapping verified: OpenXR `(x, y, z)` → Unreal `(−z, x, y)`, FRotator from the
  head basis the way UE3's `FMatrix::Rotator` does it. Simulator roll +15° gives quaternion
  z = 0.131, which gives view roll −2,731 units = −15.0°. Yaw +30° (OpenXR: toward −X = left)
  turns the camera left; pitch +20° looks up. Captures are in `logs/shots/m3-game-*.png`.
- **The weapon viewmodel follows the body's aim, not the head.** With the head turned it slides out
  of view. That's expected with a decoupled head, and controllers take over the weapon later (M7).
- **The HUD is screen-space** and doesn't rotate with the head (roll), so it's due its own layer
  (M5).
- Simulator Quest 3 FOV per eye: outer 54.0°, inner 40.0°, up 44.0°, down 54.3°, IPD 64 mm. The
  union widened to 16:9 gives tangents L/R ±2.093 (about ±64.5°), U 0.965, D −1.390. The image is
  very wide on the monitor but correct in the eyes.
- The host's projection layer (both eyes = the same image with the frame's render pose and FOV) is
  accepted: the simulator shows 60 FPS with each eye filled by the game at a natural perspective.
- The simulator's LOCAL puts the head at y = 1.7 m, so the game recentres translation on the
  first tracked pose (translation only).
- The frame-to-pose pairing uses the render-thread-lag rule (the Present thread differs from the
  CalcSceneView thread → the previous view). Not yet measured for swim; that's for the headset.

## 5i. Settings, scale, device loss (2026-09-25)

**FSystemSettings** (vtable at `0x116F56B8`, filled by `FUN_10A7BF10` defaults, then
`FUN_10ECC330(MOHAScalabilityOptions*)`): MOHAScalabilityOptions is a `native` class, and its bool
bitfield at `+0x3C` (declaration order) is copied to ints:

| bit | option | global |
|---|---|---|
| 0 | bAllowBloom | `0x116F56D8` |
| 1 | **bAllowDepthOfField** | **`0x116F56D4`** |
| 2 | bAllowDynamicLights | `0x116F56C4` |
| 3 | bAllowDynamicShadows | `0x116F56C8` |
| 4 | bAllowLightEnvironmentShadows | `0x116F56CC` |
| 5 | **bAllowMotionBlur** | **`0x116F56D0`** |
| 6 | bMinspecCull | `0x116F56FC` |
| 11 | bTickLODAnims | `0x116F5700` |
| float `+0x5C` | ScreenPercentage (if > 25) | `0x116F56F8` (read by CalcSceneView) |

MOHA's script also toggles `WorldInfo.GameModified_bTurnOffMotionBlur` (sprint, some states).
With `Camera.DisableMotionBlur/DepthOfField` the mod zeroes the two ints each view; this was
logged working in gameplay.

**Scale:** `MOHAPlayerPawn` CylinderComponent CollisionHeight = 96 (half-height, so 192 units
standing; radius 40). `DefaultPlayer.ini` StanceCollisionHeights = 96 / 49 / 27 (stand, crouch,
prone). The camera comes from mesh socket `Cam`. So **about 100 units per metre (1 unit ≈ 1 cm)**, not
UE3's usual 2 cm.

**Device loss:** alt-tab from exclusive fullscreen → `Present` returns `0x88760868`
(`D3DERR_DEVICELOST`) → the game calls `IDirect3DDevice9::Reset` (vtable 16) **on the main thread**
(Present is on the render thread). D3D9 refuses Reset while any D3DPOOL_DEFAULT resource exists,
so the mod releases its own before the game's Reset. A new window in the game process (the
simulator preview, when the host runs the simulator) can steal focus from a fullscreen game and
lose the device too.

## 5j. Stereo through the engine's split-screen path (M4, measured 2026-09-25)

**RE:** `GEngine` = global `0x116DD964`; `GEngine+0x2A4` = `TArray<ULocalPlayer*> GamePlayers`
(Data, Num `+0x2A8`, Max `+0x2AC`). There are two CalcSceneView callers, both looping over
GamePlayers:
- `0x10C14230` **UGameViewportClient::Draw(FViewport*, FCanvas*)**: `__thiscall`, single exit
  `RET 8`, 64 KB. Loop 1 (`0x10C146A8`) calls CalcSceneView per player and collects the views into
  one view family (rendered in one pass). Loop 2 (`0x10C15275`) does per-player drawing (HUD).
- `0x10B224E0`: a second loop outside Draw (one mono view per frame). Its purpose isn't known yet;
  it must not overwrite the stereo pose record.

**Method (no allocation, engine array untouched):** a safetyhook InlineHook on Draw points
`GamePlayers.Data` at a static 2-entry array holding the same ULocalPlayer twice (Num = 2), calls
the original, then restores Data, Num = 1 and the player's Origin/Size = (0,0)/(1,1). A MidHook
at CalcSceneView entry (`0x10C19910`, `this` = `[ESP+4]`) makes call 0 the left eye (Origin 0,
Size 0.5×1) and call 1 the right eye (Origin 0.5). The view hook uses that eye's own pose from
xrLocateViews; the projection uses that eye's FOV widened to the half-viewport aspect.

**Results (x64 simulator, gameplay):**
- Side-by-side eyes in one backbuffer, with correct asymmetric frusta: left eye tan L −1.376,
  R 0.839 (54°/40°); widening to 0.889 aspect only grows U/D 0.965/−1.390 → 1.034/−1.459.
- **The HUD is drawn in both halves** (the per-player loop draws it per eye).
- Eye separation 64.0 mm (the simulator IPD). 890 of 900 XR frames carry a new game frame (60 Hz).
- **Memory: used 1,553 MB, free 495 MB, largest block 294 MB** (mono: 1,543/505/312). Two views
  cost about 10 MB, because they render in one family.
- Menus: the 3D backdrop renders in stereo, while the UIScene menu is drawn once over the full
  screen (M5: cinema screen for menus).
- MOHA's menus follow **mouse hover**: a cursor left over an item steals keyboard navigation (the
  harness now parks the cursor on the bottom frame).

**Per-eye view state (headset round 3: "right eye has heavy flickering"):** `ULocalPlayer` members
by LocalPlayer.uc order: Origin `+0x68`, Size `+0x70`, PlayerPostProcess `+0x78` (a UObject),
**ViewState `+0x7C`** (FSceneViewState*, vtable `0x114D3730`, ctor `FUN_10A92BA0`),
ActorVisibilityHistory `+0x80`. The ULocalPlayer ctor `FUN_10C18AF0` does
`ViewState = AllocateViewState()`: **`AllocateViewState` = `0x10A99220`** (cdecl, no args:
`GMalloc->Malloc(0x180, 8)` + ctor). With both eyes on one ULocalPlayer, the right eye used the left
eye's occlusion history. Fix: eye 1 swaps in a second state from AllocateViewState, and Draw restores
the original. **Measured** (`tools/flicker_metric.py`, 12 captures, still player): shared → right/left
frame-change ratio **2.79**, pixels changing >40: 1.05% left vs **8.54%** right. Own state → ratio **1.04**,
1.07% vs 1.11%.

**Also from round 3 (not yet fixed):** the first-person weapon is doubled. It sits a few cm from the
eyes, so a real IPD gives it a huge disparity; the proper fix is M7 (the controller-held weapon). The
HUD is drawn at screen positions inside each half, so it lands at different places per eye
("cross-eyed / edge of view / one eye"): M5, the HUD on its own layer.

## 5k. Gamepad input: XInput every frame, pad or no pad (M6, measured 2026-09-25)

- **`WindowsClientInit`** (Ghidra; it holds the only reference to `EnumJoysticksCallback` `0x10922F40`)
  creates the DirectInput keyboard/mouse and then, **unconditionally**, four joystick slots
  (`FUN_1092acd0`, array `0x116E2818`, count `0x116E281C`, stride `0x1EC`) with type 4 (XInput), index
  0–3 and no DirectInput device. Only then does it call `EnumDevices(DI8DEVCLASS_GAMECTRL, callback)`.
  The callback fills slots only for DirectInput pads that aren't XInput pads.
- **Per frame**, `FUN_10923D70` (the window's input poll) walks those slots only while
  `GetFocus() == the game window`. For a slot with no DirectInput device and index < 4 it calls
  `XInputGetState(index, &state)` through IAT `0x112C6804` (XINPUT1_3.dll ordinal 2, call site
  `0x10924232`). Types 3/4 map `wButtons` through the button table at `+0x88 → +0xB8..0xC7` and the
  thumbs/triggers to the `XboxTypeS_*` keys; a trigger counts as a button above 30.
- So **answering `XInputGetState(0)` is enough** to give the game a pad: no device has to exist and no
  enumeration has to be faked. Verified: with `Input.Controllers=1` the first poll arrives about 13 s
  after launch (at the main menu), and injected states move, turn, fire, reload and pause the game.
- The pad layout is script: `MOHAPlayerInput.uc` `Bindings_Default` (A reload/use/flare, B switch weapon,
  X crouch, Y jump, LB alt-fire, RB grenade, LT aim, RT fire, LS sprint, RS melee, Start pause/menu,
  Back scores; LeftX/Y DeadZone 0.3, RightX/Y 0.2). `Bindings_GOW` and `Bindings_Halo` are alternative
  layouts. LB+RB together feed `PressEnterCheat*` (a cheat-code entry mode; the cheat sequences are all
  face buttons).

## 5l. The player's rotation (snap turn, measured 2026-09-25)

- `ULocalPlayer+0x40` = `Actor`, the APlayerController (§5g, the merge point `MOV ESI,[EDI+0x40]`).
- **`AActor::Rotation` = `+0xF4`** (FRotator: Pitch `+0xF4`, Yaw `+0xF8`, Roll `+0xFC`, ints, 65536 = 360°).
  Source: the native table entry `{"intAActorexecSetRotation", 0x10D2E9D0}` at `0x116152F0` → `FUN_10CE8310` →
  `ULevel::MoveActor` `FUN_10B62090(Actor, Delta, NewRotation, ...)`, which compares `NewRotation` with
  `actor[0x3D..0x3F]` and stores it there.
- In first person, the view's yaw from `GetPlayerViewPoint` equals the controller's `Rotation.Yaw`, give or
  take view shake. Adding to it turns the body, movement and aim at once. UE yaw grows clockwise seen from
  above (a right turn is positive). Verified by snap turn: ±5461 per 30° step, read back through the view.

## 5m. Menus and cinematic cameras (M5 cinema screen, measured 2026-09-25)

- **UI menus show the Windows cursor.** On the game thread, `ShowCursor(FALSE) + 1` (then `ShowCursor(TRUE)`
  to restore) gives the display count: main menu 0, pause menu 0, gameplay −1 (Debug.ViewState probe).
  Decrement-then-restore never shows the cursor. `GetCursorInfo` is useless here (it's global and depends on
  where the cursor is).
- **The player's own view:** in first person, the yaw `GetPlayerViewPoint` returns equals
  `PlayerController.Rotation.Yaw` (+0xF8) within 2 units. The main-menu scene (view yaw 55640 vs controller 0)
  and the parachute landing roll (a camera animation) differ by more than 2048.
- **`PlayerController.Location` (+0xE8) stays at the spawn point** (−1534, −14857, −17720 while the view was at
  7612, −8967, ...): the controller doesn't follow its pawn, so a location test needs the Pawn (offset unknown).

## 5n. The HUD pass in UGameViewportClient::Draw (M5 HUD panel, 2026-09-25)

After the 3D view family is submitted, Draw's second player loop (per player with an Actor) looks up
that player's FSceneView (`ESI`, player index in `EDX` at `0x10C1530C`) and:
- sets the Canvas `ClipX/ClipY` (+0x6C/+0x70 ints, later +0x50/+0x54 floats) from `view+0x24/+0x28`
  (SizeX/SizeY);
- builds identity plus translation `(view+0x1C, view+0x20)` (X, Y) at `[ESP+0x130]` (after the flush call
  `FUN_10B17930(canvas)`), and pushes it with `FUN_10918FC0(&m)` on `canvas+0xC` (the transform stack);
- sets `HUD.Canvas` (`PlayerController+0x344` = myHUD, `HUD+0x3FC` = Canvas), calls `HUD.PostRender`
  (FName `0x116F8E68`) through ProcessEvent (vtable +0xF0), and clears it;
- for player 0 only, a second HUD event (`0x116F9824`) and more canvas work.

MOHA's HUD lays out relative to ClipX/ClipY but draws elements at fixed pixel sizes. Scaling the
pushed matrix (M00, M11) shrinks everything uniformly, text included.

## 5o. Console commands from the mod, and the shot ray (M7 groundwork, 2026-09-25)

**Exec:** `ULocalPlayer` has an FExec subobject at `+0x3C` (the ctor FUN_10C18AF0 stores vtable
`0x114F8AB0`). Its slot 0 = `ULocalPlayer::Exec(const TCHAR* Cmd, FOutputDevice& Ar)` `0x10C1A220`,
thiscall on the subobject. That's the path the game's console takes: the PlayerController, Pawn, Weapon and
HUD exec functions are all reachable. `FOutputDevice::Logf` (FUN_109D8D60) formats and calls vtable slot 1
`Serialize(const TCHAR*, EName)`, so any MSVC object with a virtual destructor then `Serialize` works as `Ar`
(src/mohavr/game_exec.cpp). Useful pawn execs (MOHAPlayerPawn.uc):
- `HideWeapon(bool bWeaponVisible)`: `Weapon.Mesh.SetHidden(!v)` plus `bHidingWeapons`, which survives weapon
  switches. **Verified.**
- `RenderBody(bool bShow)`: swaps FPArms material 1 for `ViewModel_Mesh.NoRenderMatInst`.
- `UpdateGunView(float ViewmodelFOV, float OffsetX, float OffsetY, float OffsetZ)`: the view model's FOV and
  offset. A candidate for pushing the gun further from the eyes instead of hiding it (M7/M8).

**Shot ray** (Engine/Weapon.uc, MOHAGame/EALAWeapon.uc, Engine/Pawn.uc):
- `EALAWeapon.InstantFire` → `PerformWeaponTrace(Instigator.GetWeaponStartTraceLocation(self))`;
  `EndTrace = Start + Vector(GetAdjustedAim(Start)) * GetTraceRange()` → `CalcWeaponFire`.
- `Pawn.GetWeaponStartTraceLocation` → `Controller.GetPlayerViewPoint(POVLoc, POVRot)`, returning POVLoc: the
  game's eye. Our CalcSceneView hook changes only the render view, so shots start at the un-tracked eye.
- `EALAWeapon.GetAdjustedAim` → `Pawn.GetAdjustedAimFor` → `PlayerController.GetAdjustedAimFor`: base =
  `Pawn.GetBaseAimRotation()` (native table `{"intAPawnexecGetBaseAimRotation", 0x10D39090}` at `0x116158E0`;
  not yet a Ghidra function), then a forward trace and aim assist (`AimingHelp`), then `AddSpread`, then
  `MOHAPawn.GetPostAdjustedAimFor`.
- **Consequence today:** the aim is the controller rotation (body yaw + game pitch). With the head driving
  pitch visually and `RightStickY=0`, the game pitch stays where it is: shots go level, not where the head
  looks.
- **M7 options:** (a) head aim: write the head's pitch (and yaw relative to the body) into
  `PlayerController.Rotation` (+0xF4) each frame, which is cheap given snap turn's plumbing; (b) hand aim:
  replace the result of the `execGetBaseAimRotation` native with the right controller's ray (world transform
  as in the view hook), and the start with the hand position (GetWeaponStartTraceLocation is script: needs a
  hook on GetPlayerViewPoint or on the script call).

## 5p. Performance and render resolution (2026-09-25, simulator, gameplay, stereo)

| Resolution (per eye) | Game frame (avg / worst) | Frames > 20 ms | Address space free (largest block) |
|---|---|---|---|
| 1920×1080 (960×1080) | 5.82 ms / 12.2 ms | 0 of 1720 | 423.9 MB (231.4 MB) |
| 2880×1620 (1440×1620) | 5.9–6.1 ms / 9.3–22.8 ms | 0–2 per 10 s | 498.8 MB (286.4 MB) |

- The resolution comes from `ResX=/ResY=` on the command line (§5c), read with a first-match Parse on
  `GetCommandLineW()`. A windowed game can be bigger than the desktop (a 2880×1620 window on a 1080p desktop
  worked), and the D3D9On12 device and the bridge follow the backbuffer size.
- `Render.ResX/ResY` swaps the EXE's `GetCommandLineW` import (import table walk, by name) to return
  `"<exe>" -windowed ResX=W ResY=H <original args>`.
- The simulator paces xrWaitFrame at its preview window's 60 Hz after the first ~15 s, so XR timing from the
  simulator isn't representative.

## 5q. The game window and the viewport size (headset rounds 5–6, 2026-09-26)

- UE3's viewport follows the window's client size, but in windowed mode it does **not** Reset the device when
  the window shrinks. It draws into the top-left client-sized part of the unchanged backbuffer. The stereo pair
  then no longer fills the backbuffer, and the host's fixed half-split mixes both eyes (seen as double vision).
- A window bigger than the desktop (2880×1620 on 2560×1440) is shrunk by Windows when the user drags it
  (reproduced with `WM_ENTERSIZEMOVE` + `SetWindowPos` + `WM_EXITSIZEMOVE`; a plain move doesn't resize).
- The device's creation window isn't the final one to lock: at CreateDevice the game's window was still
  2580×1460. The game's top-level window has class `LaunchUnrealUWindowsClient`; its client equals the
  backbuffer only once the game has finished sizing it (checked from Present).
- The pause screen (Esc/Start) is a tabbed Objectives / Options / Save & Load panel; B or Start closes it.
- **Maximizing can't be held at a bigger-than-desktop size:** with the lock's `WM_GETMINMAXINFO` raising the limits,
  the maximize still asked for 2896×1659 but became 2576×1408 (Windows clamps maximized windows to the monitor).
  The viewport then became 2560×1369 (headset round 9, "glass bowl"). Restoring from minimized into maximized
  arrives while `WS_MINIMIZE` is still set. So `Render.LockWindow` removes `WS_MAXIMIZEBOX`, swallows
  `SC_MAXIMIZE`, restores on `SIZE_MAXIMIZED`, and lets through only the move to the iconic size.
- **Never change the window's style from the render thread:** the lock is found from Present (render thread), and
  `SetWindowLong(GWL_STYLE)` / `SetWindowPos` send messages to the main thread, which can be waiting on the render
  thread (deadlock seen). It posts a registered message and does it in its own window procedure instead.

## 5r. Decals are culled in any view that doesn't start at x = 0 (headset round 7, 2026-09-26)

- **Symptom:** bullet holes only in the left eye. Holes show only in the half at x = 0: swapping the render
  order of the eyes changed nothing, and swapping the halves (left eye drawn on the right) moved the holes to
  the other eye. Not the per-eye FSceneViewState (the shared state behaves the same), not the scissor, and not
  the viewport (both traced at the D3D9 level: `Debug.TraceScissor`).
- **The decal pass:** `FUN_10C36D70` walks the decal list (`this+0xA4/0xA8`). For each decal it calls
  `FUN_10A2ABB0` (the screen box), skips the decal if that returns 0, and otherwise turns the box into the
  scissor rect (`SetScissorRect` = device vtable `+0x12C`) and enables `D3DRS_SCISSORTESTENABLE` (`0xAE`).
  Found with a stack scan from a `SetScissorRect` hook (MOHA has no frame pointers): the first MOHA address
  above the call was always `0x10C36EAF`.
- **`FUN_10A2ABB0`** (stdcall, `RET 0x10`; EAX is an input (`MOV ESI,EAX`); stack: ?, `FSceneView*`, `float* min`,
  `float* max`): projects the decal's 8 corners with `x = SizeX/2 + view.X + ...` (absolute pixels) but clamps
  min/max to `[0, SizeX]` / `[0, SizeY]`. With `view.X = 1440` and `SizeX = 1440`, both ends clamp to 1440 → empty
  → culled. Five callers (decals on each receiver kind: `0x10B964DE`, `0x10B968E8`, `0x10C36A06`, `0x10C36E46`,
  `0x10D23E02`).
- **Fix** (`Render.DecalFix`, src/mohavr/vr_view.cpp): a MidHook at the function's entry. When view X/Y != 0 it
  zeroes them for this call and replaces the return address with a stub that restores them and adds X/Y to
  the box. Only the render thread's own view is touched, for the length of one call.

## 5s. Aiming with the head or a controller (M7, 2026-09-27, simulator)

- **`UPawn::execGetBaseAimRotation`** `0x10D39090` (thiscall, `RET 8`): finishes the params (`[stack+0x1C]`,
  `0x41` = EX_EndFunctionParms), calls the C++ virtual at vtable `+0x354` and copies its FRotator to `*Result`.
  The player's shots take their base aim from it (`PlayerController.GetAdjustedAimFor` → `Pawn.GetBaseAimRotation()`),
  then aim assist and spread. Measured: while firing it's called once per shot for the player's pawn.
- **`Pawn.Controller` = `+0x1EC`** (`execIsHumanControlled` `0x10D38FF0` tests it). Declared after three floats,
  so `sizeof(AActor)` = `0x1E0`, and **`Controller.Pawn` = `+0x1E0`** (Controller's first variable). Verified at
  run time: the local controller's `+0x1E0` is a pawn whose `+0x1EC` is that controller. `Actor.Location` = `+0xE8`.
- **`UWorld::SingleLineCheck`** `0x10B640A0`, LTCG convention (read at execTrace's call, `0x10CE8994`): stack
  `(this = GWorld 0x116DCE78, FCheckResult* Hit, AActor* Source, FVector* End, FVector* Start, FVector* Extent)`,
  `EAX` = trace flags (it ORs in `0x400`), `ECX` = light component (0); callee pops `0x18`; EBX/ESI/EDI/EBP kept.
  Returns nonzero when nothing was hit. `Trace(..., bTraceActors=true)` uses flags `0x60BF`. FCheckResult: Actor
  `+4`, Location `+8`, Normal `+0x14`, Time `+0x20` (init 1.0), Item `+0x24` (init −1), `0x44` bytes.
- **The shot start stays the game's eye** (`GetWeaponStartTraceLocation` → `GetPlayerViewPoint`: the untracked
  camera). So `Aim.Mode` doesn't move the start: each view frame the aiming pose's ray (head, or a controller's
  `/input/aim/pose`, mapped into the world exactly like the eyes) is traced, and the player's
  `GetBaseAimRotation` returns the direction from the game's eye to that hit. The shot then lands on the ray's
  hit point whatever the offset between hand and eye (only something between the eye and the point, which the
  hand can see past, differs).
- **Verified [S]:** head mode, head straight ahead: aim unchanged (P0 Y16388 both ways); head 30° left and 10°
  down: aim Y −5461 (−30.0°) and P −1820 (−10.0°) from the body. Right-hand mode with a test pose 25° left, 5°
  up: the burst's bullet holes land within ~1.5° (spread and recoil) of the aim point projected into the left eye,
  and the host's reticle shows in both eyes at that point.
- The simulator's own aim poses are the identity at the LOCAL origin (the floor); tests use `pad_cmd.py --aim`.

## 5t. The first-person arms and gun (M8, 2026-09-27, simulator)

- **The camera comes from the arms:** `MOHAPlayerPawn.GetCameraPos()` = `GetBoneLocation('Camera')` of `FPArms`
  (a `MOHASkeletalMeshComponent`; the gun mesh hangs off its socket). So moving the arms in the game would move
  the camera. M8 moves only their *drawing*.
- **`UMOHASkeletalMeshComponent.FOV`** at component `+0x3D0` (65 for the player's arms and gun, 0 otherwise).
- **The proxy's per-view transform** `0x10EEA470` (vtable slot at `0x11595E00`): thiscall (proxy; `FSceneView*`,
  `FMatrix* OutLocalToWorld`, `FMatrix* OutWorldToLocal`), `RET 0xC`. Proxy `+0xF0` = the component, `+0x20`
  LocalToWorld, `+0x60` WorldToLocal. With FOV != 0 it returns `LocalToWorld · View(+0x40) · Persp(FOV, SizeX +0x24,
  SizeY +0x28, near +0x1BC) · inverse(ViewProjection)` and its inverse: a flat-screen projection baked in *per
  view*. In stereo each eye gets a symmetric frustum with its own centre, so **the gun is seen double**.
- **Where the parts sit** (ViewModel log, camera frame, Unreal units): the gun's origin 34.2 forward, 11.2 right,
  16.7 down; the arms' root 64.9 straight below the camera.
- **Fix** (`Weapon.ViewModel`, src/mohavr/viewmodel.cpp): an inline hook on that function. For parts with FOV != 0
  in the player's head-tracked view: `LocalToWorld · D` and `D⁻¹ · WorldToLocal`. ViewModel=1: D = identity (true
  3D, where the game put them). ViewModel=2: D = inverse(game camera) · hand frame, the hand frame being the aiming
  controller's pose moved back by `Weapon.GripX/Y/Z` (the camera-frame point that lands on the controller). The game
  camera = the view's untracked location with the game's pitch/yaw (roll 0). Per eye, render thread; D is computed on
  the game thread each player view.
- **Verified [S]:** with the test hand at 15° right/10° down and 20° left/10° up, the arms and gun are drawn at the
  hand, pointing its way, in both eyes.

## 5u. Object names, and which gun is in hand (2026-09-27, simulator)

- **Names:** `FName::ToString` `0x109E2E80` (LTCG: ECX = `&FName {Index, Number}`, EAX = out FString): the names
  array's data pointer is at `[0x116F4A54]`, and each entry's wide string sits at entry `+0x10`. A Number > 0
  means `Name_(Number-1)`.
- **UObject:** Index `+0x04` (−1 = uninitialised, `GetName` `0x1090BBD0`), Outer `+0x28` (`GetPathName`
  `0x109EBAD0`), Name `+0x2C`, **Class `+0x34`**. Class is verified by the names it gives (below).
- **The gun in hand:** the first-person part (FOV != 0) whose Outer isn't the pawn. The arms' Outer is the pawn;
  the gun's is its weapon attachment actor, e.g. `MOHASkeletalMeshComponent_104` of `Attachment_Stg44_14`. The
  key is that actor's class: seen `Attachment_Stg44`, `Attachment_Colt45`, `Attachment_Bar`, and during the jump
  `MOHAParachuteActor`. Y cycles the three weapons.
- **Gun fit** (menu, shared block v8): the host keeps a fit per key (`[GunFit]` in the player's ini: grip x y z,
  angle, aim line up, right) and publishes it for the key the game reports. The game uses it when the keys match,
  else the ini's `Weapon.GripX/Y/Z` and `Aim.RayUp`. The gun frame is the controller's, pitched by the angle; the
  aim line starts at the controller, offset along the gun's up and right. The host's reticle builds the same line.
  Verified [S]: the menu's aim line +2 cm → the game's ray +2 units; angle +10° → ray direction z 0.17; the saved fit
  reloads when the gun comes back into hand.

## 5v. Reflection (script properties by name), and throwing grenades (2026-09-27, simulator)

- **Layout** (`Debug.Reflect` probe on the player's pawn): ObjectArchetype `+0x38` ends UObject (`0x3C`). **UField:
  SuperField `+0x3C`, Next `+0x40`** (early UE3: SuperField lives in UField). **UStruct: Children `+0x4C`**,
  PropertiesSize `+0x50` (`0xB58` for MOHASingleplayerPawn). **UProperty: ArrayDim `+0x44`, ElementSize `+0x48`,
  PropertyFlags `+0x4C`, Offset `+0x64`**. Checked against the known ones: Actor.Location `0xE8`, Rotation `0xF4`.
  Found: Actor.Velocity `0x100`, Pawn.InvManager `0x3A4`, Pawn.Weapon `0x3A8`,
  `MOHA_MKIIFragGrenade.SpawnedExplosive` `0x4D4`. `names::PropertyOffset(object, "Name")` walks the class chain
  and caches per class, so the mod can use any script property without hard-coding its offset.
- **The throw** (EALAGrenade.uc): the fire trigger's analog value (`GetFireAxis`, lagged) is `ThrowStrength`. On
  release `ProjectileFire` spawns the projectile (`SpawnedExplosive`, pooled: the same actor can come back), yaw =
  the pawn's body yaw, pitch from the aim plus `DirectionOffset`, speed = `ExplosiveSpeed + ThrowStrength·(Max −
  ExplosiveSpeed)`, plus the pawn's velocity × `ExplosivePawnVelocityScale`. It starts at the arms' `Camera` bone.
- **Hand throw** (`Hands.Throw`, src/mohavr/throwing.cpp): the host sends the gun hand's velocity over the last
  ~0.1 s when its trigger lets go of a grenade (shared block v10). When the grenade weapon's SpawnedExplosive is a
  new projectile (another actor, or the pooled one with a velocity we didn't write) within 300 units of the eye, its
  Velocity becomes the hand's velocity mapped into the world × `Hands.ThrowScale`, plus the pawn's velocity.
  Releases under 1 m/s keep the game's own throw. Only a grenade within 60 units of the eye and already moving counts (spawned at the
  arms' Camera bone, measured 4-14 units away): headset round 15 caught a pooled grenade lying still 250 units away.
  `ThrowScale` 2.2 (round 15: at 1.5 the player's 7-9 m/s throws gave ~1200 units/s, the game's own ~1900).
- **Verified [S]:** a test release of (0, 3, −8) m/s → the grenade's velocity (−1, 1979, 279) became (0, 1200, 450)
  units/s, 12 units from the eye, and it lay ~6.4 m ahead a second later.

## 5w. Hands: gun hand, foregrip, holsters, reload gesture (2026-09-27, simulator)

- The host (src/host/hands.cpp) works out the gun from both controllers each XR frame and publishes it in the view
  seqlock (shared block v9): gunPose (the gun hand's aim pose, pitched by the fit's angle, turned by the foregrip),
  aimRay (offset by the fit's aim line), gunFlags (valid, two-handed, left). The game maps them like the eyes
  (viewmodel.cpp) and aims along aimRay.
- **Gun hand:** the hand that draws from a holster; the menu's Gun hand is the start. The pad's triggers and grips
  follow it (left gun hand: the left trigger fires).
- **Holsters** (`[Holsters]`, off until round 14): spots from the head in its heading frame (shoulders 20 cm out,
  22 cm down, 8 cm back; hips 22 cm out, 65 cm down), radius 16 cm; a grip squeeze there runs the game's own
  `SwitchPrimary` / `SwitchSecondary` / `SwitchPistol` / `SwitchGrenade` (MOHAPlayerController execs) through the
  command channel (cmdSeq → gexec on the game thread).
- **Foregrip:** the other hand's grip within 12 cm of the fit's foregrip point; while held, the gun turns so that
  point lies on that hand (long guns: the point ≥ 15 cm ahead). **Reload:** the other hand's grip at the magazine
  (8 cm below, 10 cm ahead of the gun hand) → `Reload`. A grip used by a gesture is kept from the pad until
  released; a 30 ms pulse marks entering a spot or the foregrip.

## 5x. The first-person arms skeleton, and where its pose reaches the renderer (arm IK research, 2026-09-28)

- **VM_Arms** (the FPArms mesh): 70 bones. USkeletalMesh RefSkeleton at mesh `+0x7C` (TArray), FMeshBone stride
  `68`: Name `+0`, ParentIndex `+56`, NumChildren `+60`. MatchRefBone `0x10D05140` uses a name map at `+0x8C`.
  Component: SkeletalMesh `+0x1F4`, MeshObject `+0x21C`, SpaceBases `+0x224` (TArray<FMatrix>, component space),
  LocalAtoms `+0x230` (reflection), ActiveMorphs `+0x290`, PredictedLODLevel `+0x2B8`; PrimitiveComponent
  LocalToWorld is script-declared (reflection).
- **The rig is inverted** (a hand-driven FPS rig): Root(0) → HipsOffset(2) (Anchor(1) is Root's other child -- corrected
  2026-09-29 from the cooked parent indices, 5ah) → Hips(3) → Spine(4) →
  Spine1(5) → Spine2(6) → Neck(7) / Tripod(8) → Camera(9); legs 10-17 under Hips. **RightHand(18)** is a child of
  HipsOffset; its fingers 19-33; **RightForeArm(34)** is the hand's child (at the elbow), **RightArm(35)** the
  forearm's (at the shoulder joint), RightArmRoll(36) and **RightShoulder(37)** (clavicle) the arm's; forearm roll
  bones 38-42 hang off the hand. The left side mirrors it: LeftHand 43, fingers 44-58, LeftForeArm 59, LeftArm 60,
  LeftArmRoll 61, LeftShoulder 62, roll bones 63-67. RightProp(68) / LeftProp(69) (weapon attach) under HipsOffset.
  So moving the hands drags the whole arm with them: the "detached arms" with the gun in the hand.
- **The render copy:** `USkeletalMeshComponent::UpdateTransform` `0x10CFAC10` ends with
  `MeshObject->Update(PredictedLODLevel, this, ActiveMorphs)` (vtable `+0x10`), which copies SpaceBases for the
  renderer. MOHA's override `UMOHASkeletalMeshComponent::UpdateTransform` `0x10EEB550` (vtable slot of the arms'
  vtable `0x11587D38`) applies bLockTranslation, then calls it. MOHA's Tick is `0x10EEA7C0`. Detach `0x10CFB180`
  deletes the MeshObject.
- **IK plan:** hook `0x10EEB550`; for the local arms, before the original: with rendered = SpaceBases · L2W · D,
  give each bone group K = (L2W·D)·T·(L2W·D)⁻¹ -- hands/fingers/props untouched (with the gun), body bones and the
  clavicles T = D⁻¹ (at the body), the upper arm / forearm (+ roll bones) the rigid moves that take each old segment
  onto a two-bone solve (shoulder joint at the body, wrist at the gun, the elbow bending toward the game's own);
  restore SpaceBases after the original so game code (the Camera bone) sees the game's pose.
- **Built** (`Weapon.ArmIK`, src/mohavr/arms_ik.cpp; bones found by name from the RefSkeleton). Measured: each arm is
  28.5 (upper) + 27.8 (fore) units; the game's rig puts the shoulder joints at eye height ~25 units behind the eye and
  off-centre (right on the centre line, left 38 units left) -- fine flat, out of reach in VR (the first try stretched
  the sleeves into "sails"). So the shoulders are anchored to the tracked head (`Weapon.ShoulderWidth` 36 /
  `ShoulderDrop` 22 / `ShoulderBack` 6 cm, turned with the body), the torso and each clavicle move with them, and a
  hand out of reach pulls its shoulder along (the arm never stretches). The elbow bends like the game's pose, a little
  down. [S]: the right shoulder drawn 18 units right / 22 below the eye; the left arm reaches from the left shoulder to
  the foregrip as one sleeve (m16-ik2-*.png). Known: the shoulders use the gun's move from the previous frame (a
  frame of lag on fast moves).
- **Round 16/17 refinements:** the upper arm and the forearm bone are turned from the body's pose (moved with the
  shoulder), so they carry no wrist roll (round 16: a 90-degree wrist turn spun the bicep and shoulder); only the
  forearm roll bones take the hand's twist. Just out of reach the arm stretches up to 30% along its segments
  (x' = (x - p)(I + (k-1)a^T a) + p) before the shoulder follows (a far reach had torn the sleeve). **Free off hand**
  (`Weapon.FreeOffHand`): off the foregrip the rig's left (support) hand and fingers move by
  Th = inverse(gun frame at that hand) * the other controller's frame, so a hand on the foregrip at the gun's angle
  gets exactly its on-gun pose; the arm IK then reaches to it.
- **Round 18: baked, not drawn, move.** The arms were solved with the previous frame's D but drawn with the current D
  (the free arm "jittered" with the gun). Now a MidHook just before `MeshObject->Update` (`0x10CFAFAD`, inside
  USkeletalMeshComponent::UpdateTransform; EBX = the component; LocalToWorld and the attachments are final there)
  bakes D into every first-person part's SpaceBases (bone' = bone * L2W * D * L2W^-1) -- the arms (plus the IK) and the
  gun, with the same D in the same tick -- and the proxy hook draws baked parts as they are. Only
  MOHASkeletalMeshComponents are touched (class checked: the MidHook sits in the base class's function). The inline
  hook on the MOHA override restores the game's pose after the copy. **The free hand** is now the mirror of the gun
  hand's grip: rel = gunHand * inverse(gun controller frame), target = S rel S * other controller (S = diag(1,-1,1):
  left-right in the controller frame) -- the rig's left hand bones are mirrored like that (verified: a mirrored grip,
  14 units behind its controller like the gun hand). **Forearm roll bones:** the twist-free forearm turned about its
  axis by 60% of the wrist's twist (the Y axes of the hand under the twist-free forearm and as drawn, projected).

## 5y. The player's shots: spread, and what the weapon is (2026-09-28)

- **Aim assist is not involved:** MOHAPlayerPawn.GetAdjustedAimFor returns GetBaseAimRotation() (our hook) directly.
  EALAWeapon.InstantFire traces from GetWeaponStartTraceLocation (the eye) along GetAdjustedAim = AddSpread(base aim).
- **Spread:** EALASmallArms.AddSpread -> GetAccuracy() (native) -> WeaponAccuracyComponent.AddSpread (native, pimpl;
  penalties for stance, running, turning (yaw/pitch velocity), hip fire vs ironsights). **`execAddSpread` 0x10E4E310**
  (native table entry at 0x11612DE8; thiscall, RET 8; virtual +0x170). Hooked (`Aim.Spread`, default 0): for the
  player's weapon (component.mWeapon.Instigator == the player's pawn, by reflection) the result is pulled back to our
  base aim by the factor. Measured: the game's spread had moved a shot ~1 degree (P64921 Y16792 vs P65093 Y16881).
- **Weapon kind** (shared block v11 `weaponKind`): the pawn's Weapon's class chain -- `EALAGrenade` = a grenade,
  `MOHAPistol` = a pistol, else a long gun (MOHAColt45 -> pistol, MOHAStg44 -> long gun). The host allows the
  foregrip for long guns only and the reload gesture for all but grenades.

## 5z. Stray shots: the shared view lock, and the aim trace (2026-09-28, headset round 18 -> 19)

- **Cause of shots going "somewhere else", near or far, at random:** the host incremented `viewSeq` (odd) before its
  OpenXR hand calls and the whole `Hands::Update`, so the lock stayed odd for a noticeable part of each 11 ms frame.
  The game's `ReadHands` gave up on an odd or changed sequence, and `aim::OnPlayerView` had already cleared
  `g_frame.valid`: that frame's shot used the game's own aim (the body's heading). Also `ReadGun` set `flags` before
  checking the sequence, so a torn read looked like "no gun": the viewmodel dropped the gun ray and the aim fell back
  to the raw controller pose.
- **Fix:** the host works everything out first and holds the lock only for the plain copies; the readers
  (`ReadViews`, `ReadHands`, `ReadGun`) retry up to 64 times with `_mm_pause`; a read that still fails keeps the last
  frame (aim and gun). The 5-second `aim: ray` log line now says `barrel`/`controller` and how many frames were torn.
  Simulator: 0 torn in ~850 frames per 5 s.
- **The aim trace** uses `0x60BF & ~0x08` (TRACE_Actors without TRACE_Volumes: blocking/physics volumes had stopped the
  ray), and a hit within 20 cm of the start (the ray began inside geometry: round 18, 6 of 37 logged rays at 0.0 m) is
  traced again from 20 cm along the ray.
- **The free hand's grip** (5x) is taken only from a real long gun (`weaponKey` `Attachment_*`, kind 0) whose hand has
  held still (< 0.2 cm a frame) for 30 frames -- during a weapon switch the long gun's put-away animation had been
  kept, and the grenade's free hand came out bent back. The player's `[Hands] FreeHand = tilt turn roll forward`
  (shared v12 `freeHand[4]`, the menu's Free hand page) turns it about the wrist in the controller's frame (Z turn,
  Y tilt, X roll) and moves it forward.

- **Round 19 log (headset):** 0 torn reads -- the lock was not it. With the BAR, every 5-s line for ~50 s of
  walking was a hit at exactly 0.2 m: the first trace hit within 20 cm and the re-trace from 20 cm hit at its own
  start. **Round 20:** the ray steps on 20 cm at a time (up to 5 steps, 1 m) while each trace hits within 20 cm of
  its start, and logs the hit actor (FCheckResult.Actor +4; first 12 distinct). Simulator: during the landing the
  ray starts inside `Var_Flk_Roof_Pr_StaticMeshActor_62` (StaticMeshActor) and steps out. What the headset's
  walking case starts inside is still to be read from the next log.
- **The elbow (round 20):** the upper arm and the forearm had each been turned by its own shortest rotation from the
  body pose, so their rolls about their own axes disagreed and the elbow looked twisted. Tried in the simulator
  (the free arm, pistol, head pitched down 45 degrees): a shared hinge frame (dir, shoulder->elbow x elbow->wrist) for
  both -- clean elbow, but the upper arm's roll pinched the shoulder cap into a spike; half the roll on the ArmRoll
  bone -- still pinched. **Kept (`Weapon.ElbowHinge=2`):** the upper arm by its shortest turn (as before, no shoulder
  pinch), the forearm carried by that same turn (as its child) and then turned by the shortest rotation from there
  onto its new direction -- smooth at the shoulder and the elbow. VM_Arms: upper arm + 1 ArmRoll, forearm + 5
  ForeArmRoll per side.

## 5aa. Triggers, the HUD crosshair, bool properties (2026-09-28, round 20 -> 21)

- **What the aim ray started inside (round 20 log):** `Trigger` actors. The game's own shots pass through them:
  `Weapon.CalcWeaponFire` asks `PassThroughDamage(HitActor)` (= `IsA('Trigger') || IsA('TriggerVolume')`), clears the
  trigger's `bProjTarget`, traces again from the hit, and sets it back. The aim does the same (Trigger.bProjTarget:
  +0x78 mask 0x40000), then steps past anything else it starts inside.
- **UBoolProperty.BitMask at +0x80** (found at run time from two bools sharing a word: CursorRenderingEnabled 0x2 /
  bSprintedLastTick 0x1; bRender 0x2 / bFadeOut 0x1). `names::BoolProperty` returns a bool's word offset and bit.
- **The crosshair:** `MOHAHUD.hud_cursor` (MOHAHUDCursor; `PlayerController.myHUD` by reflection). Its
  `CursorRenderingEnabled` (+0x108 mask 0x2) is read only by the native Render and the game sets it again every frame;
  `MOHAHUDObj.bRender` (+0x64 mask 0x2, what EnableElement sets; single player never calls it for the cursor). Both are
  cleared each view (`HUD.Crosshair=0`). Verified in the game's backbuffer (HUD.Mode=0): the bars are gone.
- **The free arm with a pistol or grenade** starts from the arms' pose captured with the free hand's long-gun grip
  (`Weapon.FreeArmPose=1`): the pistol's own left arm is far from the hand, and its nearly straight pose gave the
  elbow's bend direction from noise. The bend now counts the game's bend only as far as its arm is bent (full at 25%
  of the upper arm's length off the shoulder-wrist line), otherwise down and a little out; the forearm roll bones'
  twist is unwrapped frame to frame and held within 150 degrees.

## 5ab. The player's bullets, natively (2026-09-29, round 21 -> 22)

- **EALAWeapon.CalcWeaponFire** = `CalcWeaponFireNative(GetTraceOwner(), StartTrace, EndTrace, GetTraceExtentsHelper(),
  ImpactList)`: native `execCalcWeaponFireNative` 0x10E4CF90 (table entry 0x11612348) -> **0x10F0CF10**
  (ImpactInfo* out, TraceOwner, Start by value, End by value, Extent, ImpactList). It traces with **0x10F0CDE0**,
  then: a hit whose class chain has Trigger or TriggerVolume -> `bProjTarget` (+0x78 bit 0x40000) cleared, recurse from
  the hit towards the same End, set back after; a PortalTeleporter -> the portal transform. Also used by
  WeaponAttachment (third-person effects, owner = the attachment) and vehicles.
- **0x10F0CDE0**: Source = TraceOwner (or `TraceOwner->vfunc(0x2BC)`'s +0x1E0 when set); `SingleLineCheck(GWorld,
  &Hit, Source, End*, Start*, Extent*)` with **EAX = 0x268BF** (`mov eax,0x268BF` at 0x10F0CE5C) = 0x20BF
  (ProjTargets) | 0x4000 | 0x800 (material) | **0x20000 (per-poly collision)**. End*/Start* point at 0x10F0CF10's own
  by-value Start/End, which it goes on to use (RayDir, the pass-through recursion) -> changing them before the call
  moves the whole shot.
- **The aim trace now uses 0x268BF** (was 0x60BF & ~0x08: simple collision, no volumes -- the red dot's point was on
  collision hulls the bullets don't use). Its trigger pass-through keeps every trigger it passed cleared until the end
  (round 21's one-at-a-time version ping-ponged between two overlapping triggers: 11,296 passes in 1,066 frames and the
  aim point at the gun).
- **Aim.ShotFromGun (default 1):** MidHook at the bullet's SingleLineCheck call **0x10F0CE93** (`E8 08 72 C5 FF`;
  [esp] GWorld, +4 &Hit, +8 Source = EBX, +0xC End*, +0x10 Start*, +0x14 Extent*): for the player's shot (armed by our
  GetBaseAimRotation, Source = the pawn, zero extent, along the given aim) Start/End become the red dot's ray (its start
  past anything it began inside; the game's range), unless the eye -> gun segment is blocked. MidHook at **0x10F0CE98**
  (`8D 44 24 14 ...`; Hit at esp+0x14) logs the hit (`Aim.ShotLog`, "shot N:" lines, the first 400).
- **Verified [S]:** 18 shots at three gun angles, all from the gun, 0 cm from the red dot's point at the moment of the
  shot; the game's shot line 0.00-0.03 deg from the aim given. With ShotFromGun=0 and the new flags: 0-2 cm.

## 5ac. The hit marker, tracers and the sprint animation (2026-09-29, round 22 -> 23)

- **Hit marker:** `MOHAHUD.hud_weaponHitNotify` (MOHAHUDImg, +0x5B4 in MOHAHUD_SP): `OnNotifyWeaponHit()` (called by
  EALASmallArms and MOHAPlayerController on a hit) does `EnableElement(true)` + a fade. `HUD.HitMarker=0` clears its
  `bRender` each view, as for the crosshair.
- **Tracers:** `SmallArmsAttachment.UpdateTracerData` (from EALASmallArms after each shot) -> every `TracerFrequency`th
  shot `TurnOnTracer`, which starts the beam at the **attachment's** mesh socket `CurrentWeaponSockets.BarrelTip` --
  for the local player that mesh *is* the first-person gun, in the game's own pose in front of the face (corrected in
  round 26, 5aj: there is no separate third-person gun). `MOHAPawn.CurrentWeaponAttachment` (+0x4A8); `CreateTracers[2]` (byte,
  +0x2F0 on Attachment_Stg44/Colt45) = 0 makes UpdateTracerData return. `Weapon.Tracers=0` zeroes it each view. (From
  the barrel instead: the socket query `GetSocketWorldLocationAndRotation` for that mesh would have to return the
  first-person muzzle through D -- not done.)
- **Sprint:** `Stand_Sprint` when speed > GroundSpeed x weapon multiplier x 1.01 (measured 582 vs GroundSpeed 490).
  The sprint animation moves the arms' gun hand ~20 units and turns it 60-90 deg (component space: idle (-12.8, -46,
  23.7), sprinting (0..6, -28..-33, 33..40)). **The first-person gun is attached to that hand**: its root bone is
  identity and its LocalToWorld follows the hand. **Per frame the arms component updates 3 times** (the first with
  the previous frame's LocalToWorld), then the gun once, at the arms' final hand. While moving, the arms' component also
  moves against the game camera (so a component-space lock left the hand 8-20 units off).
- **Weapon.SprintLock (default 1):** the gun hand's pose relative to the game camera (hand x L2W x camInv, camInv from
  the same view as D) is followed (smoothed) while not sprinting; while sprinting (and 400 ms after, then easing out
  over 250 ms; in over 100 ms) a world move `Xw = inverse(hand x L2W) x target x cam` puts it back, applied before D to
  every first-person part (`A = L2W x Xw x D`) -- the gun, attached to the hand, follows. [S]: the drawn gun hand stays
  within 1.5-2.0 cm of its pre-sprint place in its controller's frame (3.6-37.5 cm without).

## 5ad. The first-person sprint animation, the pawn's state, and part updates per frame (2026-09-29, round 24)

Research workflow wf_ae7690d5 (Ghidra, the scripts, VM_Tree parsed from MOHAGame.xxx; offsets cross-checked by a second
agent), plus probes.
- **Object state:** `UObject.StateFrame` +0x18 (FStateFrame*), `FStateFrame.StateNode` +0x2C (UState*); the state's name
  is the node's FName at +0x2C/+0x30; StateNode == the object's Class means "no state" (execGetStateName 0x109CD9F0 =
  GNatives[284]; execIsInState 0x109CD7B0, execGotoState 0x109CC570, UObject::GotoState 0x109BE3D0 writes it). Other
  FStateFrame fields: +0x14 Node, +0x1C Code, +0x30 ProbeMask, +0x38 LatentAction, +0x3C StateStack.
- **Sprint state machine:** Stand.CheckForIdleStateChange -> GetLocomotionState: SPRINT when VSize(Velocity) >
  GroundSpeed (490) x the weapon's MoveSpeedMultipler x 1.01 && IsSprintEnabled (sprint held, not aiming, standing, an
  Idle-derived state). Multipliers (DefaultWeapon.ini): Colt45/Mauser/grenades 1.0, MP40 0.95, Thompson/shotgun 0.90,
  Garand/Stg44 0.88, K98 0.87, BAR/G43 0.85, Springfield 0.80, Panzerschreck 0.75, M18 0.70. Sprint speed =
  x fSprintSpeedMult 1.35 on the forward share, x stick magnitude: the game sprints whenever stick x (1 + 0.35 f) > 1.01.
  No hysteresis. `Stand_Sprint.BeginState` -> `SetActivity(2)`: `CurrentActivity` (byte, pawn +0x884) = 2 exactly
  while in Stand_Sprint; `fActivityBlendTime` +0x894 = 0.25 s.
- **The animation:** only the arms' AnimTree (`FPArms`, VM_Arms, ViewModel_AnimTree.VM_Tree): its root activity node
  (MOHAAnimNodePlayerActivity, `pawn.ActivityNode` +0x96C; Children TArray +0xC0/+0xC4, 0x40-byte entries, Anim +0x08,
  Weight +0x0C) has one child per EPlayerActivity; child 2 = a MOHAAnimNodeWeaponType with one `<weapon>_sprint` loop per
  weapon (18 frames, 0.6 s), faded in and out over 0.25 s (0 s into Fire/Throw). No weapon code or weapon animation is
  involved: the gun is attached to the arms' `RightGun` socket, and the game camera comes from FPArms' `Cam` socket
  (GetPawnViewLocationNative 0x10E81DE0).
- **The activity node's tick** (UMOHAAnimNodePlayerActivity::TickAnim 0x10E66400): ESI = node, EBX = [ESI+0x3C]
  SkelComponent, EDI = [EBX+0x4C] its Owner (class-checked a MOHAPlayerPawn), `movzx eax,[edi+0x884]` at 0x10E6647B,
  `cmp eax,[esi+0xE0]` (ActiveChildIndex) at 0x10E66482, and EAX pushed unchanged at 0x10E664A8 as the child to blend to
  (vtable +0x190 with PlaybackLength +0x890, blend time +0x894). **Weapon.SprintArms** (default idle): a MidHook there
  turns 2 into 0 (idle) or 1 (walk) for the local pawn's own FPArms -- the arms never blend into the sprint loop; speed,
  zoom, blur and sound (state-driven) are unchanged. Installed whatever Weapon.ViewModel is. The children it switches to
  are blend nodes, not sequences, so the re-trigger path (ECX) restarts nothing (review wf_19864723). [S]: the drawn gun
  hand turned <= 1 deg in its controller's frame while sprinting (84 deg with the game's animation), 3-9 cm (the walk bob
  vs the idle pose).
- **Part updates per frame while moving:** the arms and the gun (gun first) update up to 6 times a frame; two of them sit
  37 cm higher and the first lags one frame of motion; only the last pair is drawn (steady to 0.1 cm frame to frame).
  The per-sprint log ("armik: sprint of N ms -- ...") uses each frame's last arms update.
- **Why the speed-detected lock failed (round 23):** its threshold (~500) was above the game's for most weapons (sprints
  at 421-495 went unlocked), it restarted its ramp on every flicker around the threshold, and it fired during the
  parachute glide.

## 5ae. Left-hand mode drawn mirrored, and why culling needed the proxy's determinant (2026-09-29, round 25)

- **The problem:** the arm IK (and every game animation) is right-handed: with the gun in the left hand the right arm
  reached across the chest to it.
- **Weapon.LeftHandMirror (default 1):** with the gun in the left hand (gunFlags bit 4), viewmodel.cpp works in a mirror
  world -- reflected across the body's centre plane (through the tracked head, normal = the body's right axis from the
  game camera's yaw): the gun frame and both controller frames become `MirrorFrame(f, R)` (own Y flipped, then the world
  reflected, so they stay right-handed); D = camInv x the mirrored gun frame. The bake and the IK run as for a right hand
  (arms_ik only flips the shoulders' right axis); the proxy hook draws the parts through `R` (L2W x R, R x W2L; R is its
  own inverse). The game's animations (reload, bolt, pin pull) come out left-handed, the gun mirrored.
- **Culling** (research agent): the cull mode comes only from `FPrimitiveSceneProxy::LocalToWorldDeterminant`
  (**proxy +0xA0**, float): the element's ReverseCulling bit = det < 0, read right after the 0x10EEA470 call by
  FSkeletalMeshSceneProxy::DrawDynamicElements (0x10D06571) and its two decal paths (0x10D06AD5, 0x10D06F52), all
  `comiss xmm0,[ebx+0xA0]`. Writers: FScene::AddPrimitive (0x10A974C9) and the per-tick UpdateTransformCommand
  (0x10A9771B), both before the frame's draws; source comp+0x60 (USkeletalMeshComponent::SetTransformedToWorld
  0x10CFBED0). Not the per-view matrix, not the bones. The hook sets the sign (never flips it: it runs for both eyes and
  every pass) for first-person parts only; the three readers' bytes are checked at install. Lighting is unaffected
  (the vertex shader gets L2W/W2L and full T/B/N; the reflection is consistent). First-person parts cast no shadows.
  Frustum culling uses the component's bounds sphere (fCustomBoundsSize: 1000 arms, 100 gun) at the component origin.
- **[S]:** a left-hand draw of the BAR (the left hand at the right-shoulder holster): mirror off -- the right arm
  crosses to the gun; mirror on -- the left arm holds the gun from the left, the right hand free on its controller, all
  solid (before the culling fix the mirrored gun drew as a dark inside-out silhouette).
- **Round 25 in the headset: the arms crossed** (the right shoulder reached to the left hand and vice versa) **and the gun
  sat wrong in the hand.** Two mistakes of mixing the worlds, both fixed in round 26: (1) arms_ik flipped the shoulders'
  right axis in the mirror world -- but there the gun is in the rig's right hand on the right, so its shoulders are where
  they always are; (2) the fit's grip point was put on in the real world and then mirrored, so its sideways part (grip Y,
  ~11 units) landed on the wrong side (2 x 11 cm off). Now the controller frame is mirrored first and the grip applied in
  the mirror world (gunFrame = T(-grip) x MirrorFrame(ctrl)); the host mirrors the aim line's sideways offset (fit
  rayRight) for a left gun hand (hands.cpp, reading [Weapon] LeftHandMirror). The simulator test that "passed" had both
  hands near the middle, where crossed arms look plausible. [S] round 26: the same rifle drawn right-handed and
  left-handed at mirrored poses (hands 28 cm out, the gun turned 20 deg out, head 30 deg down) -- the left-hand image
  flipped matches the right-hand one: the arms from their own shoulders, the gun in the hand alike
  (`logs/shots/r26-lh-fix-compare.png`). Mirroring swaps the sleeves' details (the rig's right arm is drawn on the left).

## 5af. One weapon at a time: what an off-hand grenade or pistol would take (2026-09-29, research; parked)

Research agent (read-only, the scripts incl. the map packages' weapon classes, Ghidra). **The player parked this.**
- No quick grenade: RB = `SwitchGrenade` (MOHAPlayerInput.uc:500); throwing always makes the grenade the active weapon;
  after a throw the game raises another grenade (or `WeaponEmpty` -> `SwitchGrenade` cycles types or returns to
  `LastSmallArmsWeapon`); no automatic return to the gun. `SwitchPreviousWeapon` (MOHAPlayerController.uc:892) returns
  from any grenade.
- The grenade leaves on an AnimNotify (`OnProjectileToss` in `grenademkiia_fire` at 0.09 s / `_alt_fire` at 0.038 s),
  not a timer: suppressing activities 17/18 would stop throws. Release-to-spawn measured 117-124 ms.
- Switching: old weapon PutDownTime + new EquipTime (Stg44/BAR/K98/Garand 0.30 s, Colt 0.33 s, grenades 0.20 s);
  `Weapon.EquipTime`/`PutDownTime` are plain floats (writable; the Colt upgrade resets EquipTime on equip). The old
  weapon's attachment (its first-person mesh) is destroyed at WeaponIsDown; there's never a second weapon mesh.
- `UObject::ProcessEvent` = vtable +0xF0 = 0x109CE980 (thiscall Function, Parms, Result); it rejects functions with a
  native index (Spawn, Destroy, SetTimer...).
- Options ranked: A1 swap-and-return (low), A2 fast swap (+ short equip times, lower/raise activities to idle), A3 the
  mod throws an inactive grenade weapon through ProcessEvent so the gun stays (high), B4 two firing guns (very high).

## 5ag. Controllers that stop tracking (2026-09-29, round 24 -> 25)

- Round 24's log: both aim poses went "none" for 5.3 s (19:43:55); while no hand is tracked the gun fell back to the
  game's own flat-screen placement in front of the eyes (seen double) -- the player: "after 10 seconds idle ... until
  you move or input" (the Quest drops idle controllers / switches to hand tracking).
- **Hands.HoldLost (default 1):** host `Pad::HoldLost` after `LocateHands`: a tracked hand's pose is kept relative to the
  head's position and heading (yaw only); a lost hand is put back there and still reported valid. `lost=l|r|both|none`
  in pad_cmd.txt simulates it. [S]: tracked vs lost frames look the same with the hold; without it the gun jumps to the
  game's placement.

## 5ah. Movement jitter: the camera is an animated bone, the move lagged a tick, the game ran uncapped (2026-09-29, round 25)

Research workflow wf_6ebf7b6e (the camera chain, the mod's view integration, frame pacing; the chain re-checked by a
fourth agent), plus simulator measurements (`logs/modlogs/r25-walk-*`).
- **The game camera is the arms' `Cam` socket.** GetPawnViewLocationNative 0x10E81DE0 (vt+0x494 of both pawn vtables,
  0x11571C60 and 0x115877E0) = FPArms (+0x874) socket CameraSocketName (+0x9F8, 'Cam' = bone Camera, no offset) via
  0x10D00FA0 (SpaceBases[i] x LocalToWorld), + R.Z x fJumpCameraOffset (+0x948) + R x the weapon's GetViewOffset + R x the
  debug offsets (ints +0x6EC..+0x6F4); fallback Location + (0, 0, AnimatedStanceHeights[eCurrentStance] (+0x640) -
  CollisionHeight). The view rotation is the same socket's (GetViewRotationNative 0x10E81D70), else Controller.Rotation.
  CalcCamera then adds the weapon's ProcessViewPush and the camera modifiers (MOHACamMod_ScreenShake). UWorld::Tick
  0x10B3BB60 runs UpdateCamera after every tick (ProcessEvent at 0x10B3C48D; skipped while paused): the camera reads the
  frame's final pose.
- **Bone chain:** Root(0) → HipsOffset(2) → Hips(3) → Spine(4) → Spine1(5) → Spine2(6) → Tripod(8) → Camera(9) (5x
  corrected). UpdateSkelControls 0x10E7E7A0 drives HipControl on HipsOffset (RotateArms_Pitch about the camera, so pitch
  leaves it in place; -CollisionHeight; the stance spring vCurrentStanceHeightMod +0x898 at fStanceHeightRestoreForce 3/s;
  ironsights lean/peek; vCurrentAnimOffset_AS +0x960; ShakeOffset +0xACC) and LagControl on Hips (<= 4 units). MOHA's
  ForceUpdateComponents override 0x10E83580 runs on every MoveActor and SetQuickRotation, which is why the parts update
  several times a frame (5ad; the 37-unit ones are stepUp's probe, MaxStepHeight 35 + 2.0 at 0x115A2070).
- **The walk bob:** STAND_WALK (1) = MOHAAnimNodeLinearBlendBySpeed (the weapon's idle -> <weapon>_run by speed),
  CROUCH_WALK (3) the *_walk loops; they move the Camera bone 0.6-2.9 units side to side (~1.25 Hz), 0.2-1.6 up (~2.5 Hz)
  and 0.1-2.1 fore/aft, <= 0.45 deg yaw (decoded from VM_AnimSet_NoBazooka). The VR view is built on that camera
  (vr_view OnViewPoint: g_world.base = the game's loc), so the world swayed against the eyes. **Weapon.WalkArms=idle**
  (default): the SprintArms MidHook (0x10E66482) also turns 1 and 3 into 0 for the local FPArms (crouch-idle is 0 too;
  footsteps are timer-driven -- SetFootstepTimer/NextFootstep; prone isn't used: no crawl sequences in the anim set, and
  Prone_Crawl_In/Out end on OnAnimEnd, so 5/6 are left alone). [S] (running ~380 u/s, `view: moving` log): the camera
  against the body 0.0 / 0.0 / 0.5 cm peak to peak fwd/right/up (the game's walk: up to 4.7 / 20.4 / 1.5, the 20 cm while
  a strafe began), and a 70-deg turn of the gun hand during a run (round 24's log had it once) gone.
- **The move lagged a tick:** viewmodel::OnPlayerView builds D = camInv x gunFrame at Draw N; the bake applies it in tick
  N+1, after the body moved by W = P_N^-1 P_now (P: the pawn's Location +0xE8 and yaw +0xF8), so the drawn gun sat
  (R_gun R_cam^-1 - I) x the move off (0.68 x with the gun 40 deg off the view's axis), the free hand and the shoulders
  the whole move, and a snap turn jumped them for a frame. **Weapon.CatchUp=1** (default): D' = W^-1 D W and the IK's
  targets (the head, both controller frames) x W (viewmodel::BodyMoveSinceView; none over 1 m -- a teleport). It holds in
  the mirror world (the mirror plane moves with the body: W^-1 R W). [S]: the gun held 40 deg to the side, running,
  strafing, sprinting: the gun hand within 0.4 cm of its controller (0.5 in the left hand; 2-8 cm without). The per-move
  log (`armik: a walk/a sprint of N ms`) now measures against the controller moved with the body, either way.
- **Frame pacing:** uncapped (122-330 fps), the host took the first frame published after each ack, so the world time of
  each shown frame varied by up to a game frame. **The menu's Frame pacing** (hdr->pace, shared block v13; default the
  shipped `[Bridge] Pace=0`, the player's own once toggled): Hook_Draw first waits for the previous Draw's Present
  (bridge::PresentsSeen, counted after the publish; 25 ms timeout, 2 s backoff after 3 misses), then for the host's frame
  event `Local\MOHAVR_Frame_<pid>` (auto-reset; the host sets it once per XR frame after writing the poses and taking the
  last frame; 25 ms timeout). A paced frame is always the one presented next, so MetaForPresentedFrame pairs it by that
  (g_current), not by the render-thread-lag rule. Paced, the game publishes up to kRing ahead of the ack (the host takes
  the newest; slot = F % kRing, not publishedSlot). [S]: one game frame per shown frame (uncapped 2-18), ~80 Draws/s
  instead of 160-220, no timeouts, toggled live both ways. The host logs `perf: the world shown was X ms behind each XR
  frame (sd Y)` -- the sd is the jitter: in the simulator 17 ms (sd 8) paced vs 21 ms (sd 6) uncapped, but its loop stalls
  20% of its frames (worst 33 ms) and catches up in bursts (80 of its 90 events a second get a Draw); the headset's loop
  is steady (round 24: worst 12-13 ms, 0 late). **[H] round 25:** the player noticed no difference, but the log shows it:
  uncapped the world was 14-21 ms behind each XR frame, sd 1.3-3.2 ms; paced 10.9 ms, sd 0.4-1.0 ms (3-5x steadier,
  3-10 ms fresher). On by default since round 26 (D16).
- **Not done (options):** a bob-free VR base from the pawn's own state (would also still the jump dip, recoil push,
  screen shake and weapon view offset, but splits the view from the game camera: the gun's D and the shot start must stay
  on the game's); step-ups snap the camera up to 35 units (MOHA has no eye smoothing: OldZ is unused); the paced frame's
  poses could be predicted one frame further (it is shown one frame after its Draw).

## 5ai. Jumps and falls: the landing animations and the jump camera lift (2026-09-29, round 26)

The player: "A similar animation to sprint happens when walking over rough terrain or falling a small distance."
- **Falls play animated jump states:** JumpStart (26) / JumpIdle (27) / JumpEnd (28 soft, 29 hard, by Velocity.Z against
  fHardJumpLandingSpeed) -- MOHAPlayerPawn.uc ~4236-4330; a drop off any ledge over 75 units enters them
  (CheckForAnimatedJumpTransition). They run on timers (JumpStartTimerDone after fJumpStartAnimTime, JumpEndTimerDone ->
  ActivityDoneEvent) and Landed, **not on the animations ending** -- so the arms can play something else. The landing
  moved the game camera 11-18 cm and turned the gun hand ~68-72 deg in its controller's frame (the round-25 headset log's
  per-walk lines right after landings: 64-72 deg). **Weapon.JumpArms=idle** (default): the activity MidHook turns 26-29
  into 0 as well. [S] (a fall with a soft landing): the camera moved 0.5 cm against the body, the gun hand <= 2 deg
  (with the game's: 14/18/11 cm, 68 deg). New log: `view: in the air N ms -- the game camera moved ... (jump activities:
  ...)` per jump or fall (Physics == PHYS_Falling, by reflection), to 0.8 s after landing.
- **A deliberate jump (Xbox Y / the controller's A) plays no jump animation** (none of 26-29 in the log): it is the
  procedural jump -- fJumpCameraOffset (pawn, reflection; rises toward fMaxJumpCameraOffset, falls back at 80 units/s
  after landing), added along the view's up axis in GetPawnViewLocationNative (ApplyJumpCameraOffset 0x10E81FD0), *after*
  the arms' Cam socket. It lifted the VR view up to 8 cm on every jump, and since the gun is placed against the game
  camera (D = C^-1 G) while the arms stay on the socket, the gun dropped as far below the hand (the per-walk log: 8.0-8.2
  cm after each jump). **Camera.JumpLift=0** (default): vr_view OnViewPoint takes it back out of the camera (along
  FRotationMatrix's Z at the view's pitch/yaw) before the eyes and g_world are built; aim keeps the game's camera
  (g_gameCam), where its shots start. [S]: jumps 0.4-0.5 cm (was 7.8-8.4), the gun hand 0.3 cm (was 8.0-8.2).
- Not touched: the recoil push (ApplyPush, 0.4-3 units per shot) and the screen shake's location and rotation are added
  after the socket too, so they still move the view and the gun against the hand (not reported).

## 5aj. The muzzle flash and the brass: at the game's gun pose; both moved to the drawn gun (2026-09-29/30, rounds 26-28)

The player (round 25): "Muzzle flash is visible in front of player instead of on gun barrel. Move to gun barrel if
possible, hide otherwise." Research agent (read-only), then probes.
- **One gun mesh:** WeaponAttachment's `Mesh` = `ThirdPersonMesh` = `WeaponMeshComponent`; for the local player that is
  the first-person gun (FOV 65) on FPArms' `RightGun` socket (WeaponAttachment.uc AttachTo 154-220 attaches it to a body
  only for other pawns; EALAWeapon.AttachmentChanged 930-940; MOHAPlayerPawn.AttachNewWeaponMesh 1171-1198). So the
  flash, the brass, the tracers, the muzzle light and the fire sounds all sit at that gun's sockets **in the game's own
  pose** (the mod moves the gun only in its render copy): ~0.5-0.9 m in front of the camera.
- **Placement:** SmallArmsAttachment.StartMuzzleFlash / StartShellEjectParticles: `GetSocketWorldLocationAndRotation(
  BarrelTip / ShellEject_Player)`, `PSC.SetTranslation`, `SetRotation`, `ActivateSystem` (the components are
  SetAbsolute(true,true,true), DPG 2 for the local player). execSetTranslation/Rotation only store the fields and set
  `bNeedsUpdateTransform` (the attachment isn't static); **ActivateSystem** (C++ 0x10BDBC40, this in EAX) applies it
  (UpdateComponent 0x10AE7110 -> UpdateTransform) before it re-initialises the emitters, and particles spawn on the
  next tick. Its exec, **execActivateSystem 0x10D640A0** (native table entry 0x116165F0; thiscall, ECX = the component,
  [esp+4] FFrame&, [esp+8] Result, RET 8; the 48 bytes through the call are pinned), is the hook point:
  `muzzle.cpp` compares the component with the local pawn's attachment's MuzzleFlashPSComponent / ShellEjectPSComponent
  (reflection), and either moves the pending `Translation`/`Rotation` (reflection) by the gun's drawn move -- the D' of
  the gun's last bake (arms_ik BakedMove, after CatchUp) and the left hand's mirror (a reflected frame gets its own Y
  flipped back: a rotator can't reflect) -- then sets `bNeedsUpdateTransform`; or, after the call, sets
  `bSuppressSpawning` (ActivateSystem clears it; the emitters then skip spawning, bursts too, until the next shot).
- **[S] brass (Weapon.Brass=gun, default):** moved 20-44 cm onto the drawn gun per shot; its LocalToWorld after
  ActivateSystem is exactly the moved one; the casings fly out of the drawn gun's ejection port (slow motion:
  `EnableCheats` + `SloMo 0.05` through Debug.GameCommands; `logs/shots/r26-mz-slobarrel-muzzle.png`). The ShellEject
  systems are world-space emitters. In left-hand mode the port is at the mirrored side, the casings fly right.
- **[S] the flash at the drawn barrel (Weapon.MuzzleFlash=barrel, the default from round 27)** renders there, in both
  hands. Round 26 concluded it didn't show -- wrong: **its flame shows for about one frame**, the game's own as well, and
  the captures missed it. Paused at the first Draw after the shot (Debug.MuzzleFreeze, below; BAR; simulator), the flame
  sits at the drawn muzzle: right hand 45.9 cm from the game's spot, projected to (800, 515) px of the left eye, where the
  flame leaves the muzzle (`logs/shots/004827-frz3-def.png`, shipped defaults); left hand (mirrored, the gun turned 20 deg
  left: the flash's yaw 70 deg, its roll mirrored) 70.7 cm, projected to (393, 515) px, at the mirrored gun's muzzle
  (`logs/shots/004448-frz3-l2.png`). Traced each Draw in slow motion (SloMo 0.05), the moved flash and the game's live
  alike: particles from the 3rd Draw through the 15th / 16th (about 0.02 s of game time; the bounds at their muzzle,
  growing forward; LastRenderTime advancing every Draw, so drawn), yet frozen at the 8th Draw neither shows a flame any
  more, only faint smoke (`logs/shots/005953-frz4-mid8.png` moved, `010310-frz4-vmgame.png` the game's). The view-model
  proxy hook (0x10EEA470) is never called for the flash's proxy (counted: 0 calls), and local- vs world-space emitters
  (round 26's guess) make no difference. The muzzle light (MuzzleFlashDLight, lighting only), the tracers and the fire
  sounds still come from the game's gun pose.
- **Debug.MuzzleFreeze=N** (the [S] tool): N Draws after the first flash the mod runs `FreezeFrame 0` (needs
  `EnableCheats` first, e.g. through Debug.GameCommands, and `Camera.CinemaScreen=0`, as the pause shows the cursor);
  until then it traces the flash each Draw (L2W, bounds, LastRenderTime against WorldInfo.TimeSeconds), then logs its
  transform with the left eye's final view (location, rotation, FOV tangents): project the origin with
  u = (tan_h - L)/(R - L), v = (U - tan_v)/(U - D) into the left half of a capture (`harness.ps1 shot`). Paused, nothing
  is re-baked: arms_ik's IsBaked window (250 ms) ran out and the proxy hook moved the gun a second time (the first frozen
  frames showed the gun ~20 deg off its flash), so under Debug.MuzzleFreeze the last bake stays valid.
- **Weapon.MuzzleFlash=hide:** the flash suppressed; [S] slow-motion captures with the left hand -- the game's flash in
  front of the face (a frame scoring 269 flame-coloured pixels) gone (max 12, noise).
- **[S] the brass through the left hand's mirror (Weapon.BrassMirror=1, the default from round 28).** Round 27, in the
  headset: "with left hand the brass comes out of the wrong ride of the gun and flys off in the wrong direction". The
  brass templates (`ShellEjectParticleTemplate` per gun in DefaultWeapon.ini, e.g. `HUS_VFX_BAR_muzzle.ShellEject`; the
  data is cooked into the level, read from the decompressed `Var_Flk_P`) are one world-space mesh emitter each (casing
  meshes, material `GCm_Wpn_emptyShells`; MeshRotation with bInheritParent), whose StartVelocity and StartLocation are in
  the socket's frame and go through the component's full LocalToWorld at spawn. StartVelocity min..max: BAR
  (-50..50, 150, 100), Thompson (-45..45, 125..150, 20..150), G43 (-30..30, 100..125, 150..175), Springfield
  (-50..50, 175..225, 25..50), K98 (-50..50, 175..215, 35..50), Colt (-10..10, 55..70, 135..145), but Garand (200,
  175..200, 125..150), STG44 (50..150, 250..300, 100), MP40 (0..75, 100..150, 150..175), C96 (-30..10, 20..40,
  140..170): thrown along the socket's +Y (sideways) and +Z (up), several also forward along X, so no proper rotation
  mirrors them all. The moved frame's own Y flipped back (a rotator can't reflect) threw them the right-hand way, across
  the gun. A negative **Scale3D.Y** on the component, with that rotation, makes LocalToWorld the exact mirrored frame, and
  ActivateSystem applies it (the applied Y axis logged 0.94 -0.34 -0.05 left-handed against -0.94 -0.34 -0.05
  right-handed). The mesh emitter sizes each casing by the component's Scale x Scale3D (FParticleMeshEmitterInstance's
  UpdateBoundingBox 0x10CAEBC0 reads component +0x1C8 Scale, +0x1CC Scale3D; its StaticType 0x11622A30, vtable
  0x1150DDD0), so the casings are mirrored as well: drawn 10x (a research run) the mirrored casing is a solid, lit mirror
  image of the right-hand one, not inside-out (`logs/shots/095035-frz5-l-bigs.png`, `095136-frz5-r-bigs.png`; the mesh
  particles' draw path, and where its cull mode comes from, not located within the time box). [S] left hand, 15 Draws
  after a shot: the casing out to the left of the mirrored BAR (`logs/shots/095427-frz5-l-def.png`, shipped defaults);
  without it, across to the right (`094007-frz5-l-old.png`); the right hand unchanged (`094122-frz5-r-ref.png`).

## 5ak. Weapon upgrades, and the pause-menu crash of 2026-09-30 (gun-fit session)

- **Upgrades don't change the model** (MOHAWeaponUpgrade.uc): each gun is one skeletal mesh with its upgrade parts built
  in; an upgrade shows or hides them through SkelControlSingleBone controls (UpgradeMeshesToAdd / ToReplace /
  ToHideOnly, on the weapon's mesh and the attachment's), and changes ammo, kick and accuracy. The attachment class (the
  `[GunFit]` key) and the mesh origin stay the same, so one fit per gun covers every upgrade level. The player's save has
  every upgrade (extended magazines shown -- relevant to a manual reload). `NumExpLevels` in DefaultWeapon.ini only sets
  the thresholds; editing it would be a game-file change (standing rule 1) -- a test that needs no upgrades would do it in
  memory.
- **The game crashed once** (10:43:16, as the pause menu opened: `cinema: ON` is the last game-thread line), the only
  MOHA.exe crash on record: an access violation in ucrtbase memcpy reading 0x1A580000, called by d3d9on12.dll from
  d3d9.dll from the game's DrawIndexedPrimitive (FUN_10902550 +0x148 on the device, from the mesh-element draw
  FUN_10A9A0B0): a triangle list of 3156 vertices (2146 triangles); the copy was 3156 x 48 bytes, so D3D9On12 copied the
  element's whole vertex range from a buffer that was smaller or already freed. No mod code on the stack. The minidump
  holds no heap, so the mesh isn't known (`logs/dumps/MOHA.exe.31496.dmp`; `%LOCALAPPDATA%\CrashDumps` keeps the
  latest). Not reproduced: 40 pause/resume cycles in gameplay in the simulator (`pausestress`, 20 into the flat menu view).
  **A second one at 11:16:35**, the same stack (the game's own message box: "Rendering thread exception", memcpy under
  d3d9on12 under 0x10902638 / 0x10A9A18C / 0x10AB6A64) and the same moment: the first frames after the pause menu
  opened (`cinema: ON`), both times after 4-5 pauses and many weapon switches in a gun-fit session (the Thompson, then
  the M12 shotgun in hand; the right hand, the mirrored brass not in use). The casing meshes (`GCm_Wpn_emptyShells`,
  4-6 KB) are too small to be the 3156-vertex mesh; 48-byte vertices suggest a skinned first-person part.
  **Debug.CrashDump=1** (crash_dump.cpp, default on): a vectored handler, for an access violation inside
  d3d9/d3d9on12/d3d12/ucrtbase, logs a stack scan and writes once `%TEMP%\MOHAVR\crash-<pid>.dmp` with the memory the
  stack points at (the mesh element, its buffers), then leaves the crash to the game. [S] `Debug.CrashDumpTest=1`: a
  caught read at 0x10 in ucrtbase's memcpy at the first Draw -- logged, a 19 MB dump written, the game carried on.
- **The host crashes on shutdown** after its clean "exit 0" (0xC0000409 fail-fast, module unknown, the same offset every
  time): at the end of every Virtual Desktop session since 2026-09-25, never with the simulator -- in the VD runtime's
  teardown, after the host's work is done. Harmless so far; a clean fix would skip the runtime's teardown
  (TerminateProcess after flushing the log).

## 5al. Head-directed movement (2026-09-30, round 29)

The game moves along the body's heading (the controller yaw the right stick turns); the view adds the head's yaw on
top. `[Controls] MoveDirection=head` (default; the menu's "Move direction" toggles it, the player's choice kept in their
ini): the host turns the move stick by the head's yaw in LOCAL (the twist about +Y, 2 atan2(qy, qw), left positive)
before the game sees it, so forward is where you look; not in the menu layout. [S] the simulator's head 45 deg left
(rendered yaw 16388 -> 8196), stick forward 2.5 s: `body` walked along the heading (90 deg Unreal), `head` 50 deg left of
it (40 deg; a slope climbed 163 units on the way) -- `logs/modlogs/r29-move-*`.

## 5am. Manual reload, M0: the probe's measurements (2026-09-30; RELOAD-DESIGN.md)

`Debug.ReloadProbe=1` (reload.cpp; logs `logs/modlogs/reload-m0-MOHAVR.log`, `reload-m0b-MOHAVR.log`), no behaviour
change. Settled:
- **The harness save's loadout** is now G43 + BAR + Colt (plus grenades, Comp B), in Var_Flk, every gun at upgrade
  level 2. `GiveWeapon MOHAGameNonNative.MOHAThompson` (`MOHA_MP40`, `MOHAG43`, `MOHAMauser`) adds the gun to the inventory
  but MOHA's `SwitchWeapon` only walks the loadout slots; the engine's **`NextWeapon`** reaches them, and they come fully
  upgraded from the profile's weapon experience (Thompson 50-round drum, MP40 64). `UpgradeWeapon` is not needed.
- **Offsets:** reflection equals the static ones -- `AmmoCount[3]` +0x2D4, `MaxAmmoCount[3]` +0x2E0 (the second element is
  the alt mode's: G43 1, BAR 20, Thompson 30, MP40 32), `bAlternateFireMode` 0x400 / `bInfiniteAmmo` 0x1 of +0x2EC,
  `AmmoClass[3]` +0x2FC; `MOHAInventoryManager.AmmoStorage` +0x228 (stride 12), `NumAmmoClasses` +0x2A0 (10 classes). The
  exact and the subclass (native) reserve matches pick the same entry for the Rifle, AutoRifle, Pistol and SMG classes.
  **The Colt has `bInfiniteAmmo` set** (the pistol's reserve never runs out).
- **Bone space and scale:** the game's pose (SpaceBases) is in mesh space (the StG44-style values of the design hold: G43
  `Bolt` (0, -9.16, 19.70), BAR `Bolt` (2.00, -6.60, 12.30) and `magazine` (-0.02, 0.73, 20.84) in taped state A, Colt
  `gunSlide` (0, -7.00, 2.50), Thompson `Bolt` (0, -11, -3.5), MP40 `Bolt` Z 17.31 and `chamber_slide` 25.73 at idle);
  the gun component's L2W and L2W x D have unit rows (scale 1).
- **RefSkeleton:** `FMeshBone` position at +28 (matches the pose for static bones; G43 `bullet` parked at Z -37.5, MP40
  `Bolt` Z 18.575 in the bind pose), ParentIndex at +56.
- **Hidden upgrade parts:** the 3x3 is zeroed and the translation kept (|det| 0: Thompson `upgrade_03_hide_magazine`,
  MP40 `magazine` / `upgrade_01_tapedMagazine`, G43 `altFire_grenade`); the visible variants are as the design lists
  (Thompson drum, MP40 64-round, BAR and G43 both magazine bones).
- **Bakes per Draw:** 2 standing, 6.7 on average (up to 10) while moving.
- **The folded native:** `execHasReserveAmmo` is called for the `MOHAGameInfo` at level start (result 0, left alone);
  for the pawn's weapon it's called on every equip.

**M1, the game rules [S] (2026-09-30, `logs/modlogs/reload-m1-*`):** `Weapon.ManualReload=1`, `ManualReload.Hook=1`;
events through the host's test channel (`pad_cmd.txt` `reload=eject|take|insert|rack|drop` -> shared block v14's ring ->
`reload::OnDraw`). The G43 fired dry went 20 -> 0 through `WeaponSingleFire`/`Active` only, never `WeaponReload` (the hook
blocked the game's own reload each time it asked). G43 (closed): eject at 0 -> 0; insert -> pending; rack -> 20, reserve
-20; eject at 15 -> 1 kept, 14 back; insert -> 20. At the cap (`GiveAmmo rifle 200` -> 120): eject at 16 -> 1 kept, 15
owed; insert -> 20 from the owed first (reserve 116): no round lost. BAR (open): fired empty -> the bolt forward; eject,
insert (pending), rack -> 20; eject at 15 -> 0 kept, 15 back; insert (cocked) -> 20. Colt (infinite): eject at 4 -> 1;
insert -> 7. The host: "engaged" once a converted gun is in hand with both hands tracked.

**M2, the visuals in the bake [S] (2026-09-30, `logs/modlogs/reload-m2-*`, `logs/shots/*-m2-side-*`, `*-m2-bar-*`):**
`reload::OnGunBake` (after the arms_ik bake, gun only) overrides the drawn matrices of the listed bones: the magazine
group collapsed (3x3 zeroed, as the game hides upgrade parts) while the magazine is out; the action bone's mesh-space Z
held at its empty position (closed: clip 0; open: not cocked); the top round collapsed unless the magazine is in with
rounds. The game's pose is put back after the render copy, so sockets, the flash and the brass never see it. The per-gun
RefSkeleton check (bone count, names, root parents, positions within 0.05 u) passed for the G43, BAR and Colt. Seen: the
G43 bolt locked back (19.70 -> 8.34), the BAR's charging handle at the back of its slot (12.30 -> -2.80), the Colt's
slide back with the barrel showing (2.50 -> -1.85); the G43's magazine gone when dropped and back on insert; after a
switch away and back an empty G43 stays at 0 (the equip refill blocked) and still locked back; the rack puts the action
home.

**M3, the host's magazine [S] (2026-09-30, `logs/modlogs/reload-m3-*`, `reload-m3side-*`, `logs/shots/*-m3-*`,
`*-m3side-*`, `m3side-pouchring.png`):** the game samples the geometry in the gun bake (G = the gun controller frame x
the body's carry; the grab point = MagGrab x A, the way out = MagOut x A's 3x3, the action's grab point at its held Z) and
publishes it in the host's gun frame only after a fresh bake of the gun in hand (reloadGeoSeq). G43 in the gun frame
(cm, right up back): magazine grab 1.9 -10.4 -15.3, out 0.01 -1.00 -0.04; action grab 0.4 3.2 -10.1, back 0 0 1,
travel 12.6 (19.70 -> 7.08 u at scale 100). The host's state machine (RELOAD-DESIGN 3.2) drives the drawing through the
view block (reloadFlags bits 1-2, magPull, magPose, read with the hand frames in one pass): B on the gun hand -> EJECT
(clip 20 -> 1 kept, reserve +19), and the pad log shows B kept from the game; the grip in the belt pouch (`@pouch`) ->
TAKE, the magazine drawn in the left hand with its grab point 0.0 cm from the controller (`Hold 0 0 0`); at the well
(`@mag`) -> INSERT (0.0-0.2 cm, 0 deg; clip 20, reserve -19); the grip at the magazine -> grabbed, the group slides
2.5 cm with a 2.5 cm pull (seen from the side); pulled past 4 cm it comes away as held (EJECT; its grab point 4.0 cm off
the controller after a one-step 8 cm move, as grabbed, no snap); let go -> DROP (no ammo change). The MOHAVR menu opened
while holding one -> DROP, state out; closed -> driving again. The Holsters page shows the pouch ring between the hip
rings. Seen at Hold 0 0 0: the magazine sits just above the drawn fingers (the aim point is ahead of the palm) -- [H].

**M4, the rack [S] (2026-09-30, `logs/modlogs/reload-m4-*`, `logs/shots/*-m4-*`, `m4-bolt-zoom.png`):** the off hand's
grip within 5 cm of the action's grip point takes it (`@bolt`); the host publishes reloadFlags bit3 and `rack` (0..1 of
the travel), and the bake draws the action at z_h + rack x (zBack - z_h). G43 fired dry (bolt locked back at 8.34): a
1.5 cm tug and let go with nothing fed -> RACK, no change; eject, pouch, insert -> pending ("rack needed"); a tug ->
clip 0 -> 20, reserve 40 -> 20, the bolt home. A loaded G43 pulled 3 / 6 / 9 / 13 cm: the bolt drawn at 16.66 / 13.62 /
10.56 / 7.08 (full back), armed at 13.2 cm (85 % of 12.6), let go -> RACK as a press check (no change). The BAR fired
dry (handle held back at -2.80): a tug -> cocked; insert -> 20 at once. The held magazine stays drawn 45 cm to the left
of the view's centre with the gun pointed away (the culling question, R10: no culling seen). "Held back" (the tug) is
the empty hold only: the G43's fire animation briefly moves the bolt back on every shot.

**M5, the other guns [S] (2026-09-30, `logs/modlogs/reload-m5-*`, `reload-drum-*`, `logs/shots/*-m5-*`, `*-drum-*`):**
given with `GiveWeapon` and reached with 7 `NextWeapon`s from the G43 (then one more each), at the profile's upgrades.
Every RefSkeleton check passed. Geometry in the gun frame (cm, right up back) at the empty hold:
- **Thompson** (upgrade 3, the 50-round drum): magazine grab 0.0 -3.2 -6.8 r 9, out -1.00 0.07 0 (to the gun's left);
  action grab 0.8 8.3 -4.0, travel 11.7 (the bolt forward when empty). Fired dry -> bolt forward; eject, pouch, insert
  -> pending; a full pull (armed at 11.8 cm) and let go -> 50, reserve 250 -> 200. MOHA's art hangs the drum on the
  gun's left with its face across the barrel (face-on from behind, edge-on from the side); the held drum is turned 0 deg
  from the seated one (trace), so a held drum seen from the side looks rounder only by parallax.
- **MP40** (64): magazine grab 0.4 -13.9 -25.5 r 7, out -0.01 -1.00 0.08; action grab -2.6 6.0 -18.9, travel 17.2 (bolt
  and chamber_slide). Fired dry -> bolt forward; eject, pouch, insert, a full pull (armed at 17.5) -> 64, reserve
  200 -> 136.
- **C96** (`Attachment_Mauser`, 20 rounds, infinite reserve like the Colt): magazine grab 1.4 -9.8 -3.3 r 7, out -0.14
  -0.99 0.03; locked back when empty; eject, pouch, insert (pending), a tug -> 20.

**M6, the left hand and the fallbacks [S] (2026-09-30, `logs/modlogs/reload-m6a-*`, `reload-m6b-*`, `logs/shots/*-m6a-*`):**
the G43 drawn with the left hand (mirrored): Y drops the magazine (Y kept from the game), the right hand takes one from
the pouch and inserts it at the mirrored gun's well (0.0 cm), fired dry, eject, insert (pending), a tug -> 20. The menu's
Manual reload off: the G43 fired dry reloads by itself (WeaponReload, clip 0 -> 20, the mod re-syncs) and the reload
gesture sends the game's Reload. With Manual reload on: `[ManualReload] Hook=0` -> the game reloads by itself and the
host never engages; `Camera.Stereo=0` and `Weapon.HideViewModel=1` -> "the Draw hook or the arm bake is missing": no
blocking hook. The hidden gun (HideViewModel) is still baked (0.7-1.1 gun bakes per Draw).

**M7, the reload sounds [S] (2026-09-30, `logs/modlogs/reload-m7b-*`):** the game's reload cues are the arms' reload
animations' `AnimNotify_Sound`s; with the reload animations gone they are silent, so the mod plays them at its events.
The cues are found by reflection, no new addresses: `pawn.FPArms.AnimSets[].Sequences[].Notifies[]` (AnimNotifyEvent,
16 bytes: Time, Notify, Comment) -> `AnimNotify_Sound.SoundCue` -- 120 cues from 473 sound notifies in 553 sequences of
the 2 arm animsets, named `group.name` (`G43.G43_WpnReloadClipOut_PC_STG44`). Played through **AActor::ProcessEvent**
(vtable +0xF0 = 0x10DB1FA0, thiscall Function, Parms, Result; checked against the weapon's vtable before each call; a
new signature row) on the weapon's script function **`PlaySoundAt(ASound, SourceLocation)`** (Actor.uc: WorldInfo.
CreateAudioComponent at the spot, auto-destroyed, Play), at the drawn gun's aim-line start. Not `WeaponPlaySound`:
MOHA never calls it and its `PlaySound(.., bNoRepToOwner=true)` goes through the owner-replication path. Parameter
offsets come from the UFunction's own properties. Proof: after each EJECT / TAKE / INSERT / RACK the trace finds an
AudioComponent of WorldInfo with that SoundCue and a wave instance, 0.01-0.03 s into playback; DROP plays nothing. A
loopback recording of the PC's audio could not separate the cues from the level's battle ambience (a silent DROP
scored as high), so audibility and level are [H].

**Twin (taped) magazines [S] (2026-09-30, `logs/modlogs/reload-taped-*`, `reload-twin-*`, `logs/shots/*-twin-*`,
`twin-zoom.png`):** in the game the pair is visual only: `TapedMagMode` (a byte enum on MOHABar / MOHAStg44 / MOHA_MP40,
reflection; MOHAStg44 +0x6E8) flips A <-> B in `WeaponReload.EndState` at upgrade >= 2 (BAR) / 1 (StG44), and in B the
idle and fire animations pose the pair so its other half is in the well. Measured from the game's own reloads (Hook=0):
**BAR** A = identity at (-0.02, 0.73, 20.84), B = turned 180 deg about Z at (2.58, 8.92, 21.34); **StG44** A (0, 2.50,
17.68), B (-3.64, 2.50, 17.68); both magazine bones of the pair move together. The mod: taped when the pair's second bone
shows (`Taped=`, `TapedA/B`, `TapedRot` per gun); the half in the gun and each half's rounds per weapon; the off hand's
trigger flips a held pair (the host's flags bit5, the trigger kept from the pad); inserted flipped, the host sends
INSERT_OTHER (event 6) and the game puts that half's own rounds in and writes `TapedMagMode`; the bake draws the pair in
the half's pose (the mesh-space move between the two rests) until the game's own animations show it. BAR fired to 15:
pulled out (A keeps 15), flipped, the other half in -> 20; pulled out, flipped back, the first half in -> 15. StG44 26:
out (1 kept, A 25), flipped -> 30; out, flipped back -> 26. The reserve ends where it started (no round lost). The MP40's
pair (upgrade 0 only; this profile's MP40 is always 1+) has no B data: it reloads as one magazine.

**Round 31 follow-ups [S] (2026-09-30, `logs/modlogs/reload-r31b-*`, `logs/shots/*-r31b-*`, `r31b-flip-zoom.png`):**
- **The hand point** (the white dot): every hand interaction (holsters, foregrip, reload spots, the held magazine, the
  dot) uses the aim point moved by `[Hands] HandPoint` (cm: forward, up, in toward the palm; mirrored for the left
  hand); `ForegripRadius` and `ReloadRingScale` size the rings. All three on the Holsters page, the player's own.
- **The flip is seen:** a held pair eases to the new half over 0.35 s. Part-way poses turn about the A->B move's own
  fixed axis (for the BAR's 180 deg about Z: the line through (a + b) / 2 = (1.28, 4.83) in XY, i.e. about the pair's
  own centre line) while Z moves evenly; the StG44's pair slides.
- **A dropped magazine falls:** from the well (B) or the hand (let go), with the drawn frame's speed (plus 0.4 m/s out
  of the well), tumbling ~140 deg/s about its right axis, to the feet's height (pawn Location z less
  CylinderComponent.CollisionHeight), resting there until 2 s after the drop (`[ManualReload] DropFall`). Seen on the
  G43 (x10 slow motion, `Debug.ReloadSlowMo`): out of the well, tilting as it drops.
- **The harness save now starts with the M1 Garand and the MP40** (the player's round-30 session): tests reach the G43
  and BAR with `GiveWeapon` and NextWeapon until the trace names them.

**The reload grips [S] (round 31, 2026-09-30; `tools/reload_grips.py` -> `src/mohavr/reload_grips.inc`,
`logs/modlogs/reload-sockets-*`, `reload-grips*-*`, `logs/shots/*-grips*-*`, `grips-zoom.png`):**
- **Measured:** every gun mesh's frame is exactly the arms' `RightProp` frame (identity, all 13 first-person parts but
  Comp B), and `LeftHand` and `RightProp` are both children of `HipsOffset`; the fingers are Hand -> X1 -> X2 -> X3.
  So the left hand in gun-mesh space at any frame of an arms animation is LeftHand's local key x inv(RightProp's).
- **The bake (offline):** the arms' reload sequences (VM_AnimSet_NoBazooka) and the guns' (their AnimSets), read from the
  umodel exports (PSA keys to the engine: t (x, -y, z), q (x, -y, z, w), the root's w negated). Per gun, the frame where
  the left hand is closest to the part gives "mag" (the magazine still seated), "hold" (the magazine out) and "bolt"
  (near the rack sound): the hand in the part's bone frame, and the 15 fingers in the hand's. 17 of 21 grips (hand bone
  1.7-12.9 units from the part); left out: the Colt's magazine and slide and the C96's seated magazine (the animation's
  left hand never touches them).
- **The runtime:** the gun bake puts the grip on the drawn part (grabbed magazine, held magazine, racked action) and
  the arms' IK takes it as the free hand's target, with the grip's fingers. A held magazine's place in the hand
  (the hold grip applied to the free hand's own frame, `armsik::FreeHandRel`) goes to the host (shared block v15
  `magHeld`, caps bit6), which puts a pouch magazine there at once and eases a pulled one there (~0.1 s), so the insert
  test uses what is drawn. G43 held magazine: 1.6 cm left, 5.8 down, 5.9 back of the aim point.
- **Seen:** the hand wrapped round the G43's and MP40's seated magazines, the magazines in the fist (G43, MP40, BAR,
  StG44, Thompson, C96), the hand on the G43's receiver at the bolt. With the test hand turned like the gun hand, the
  held magazines meet the well turned 16-68 deg (inserted at InsertAngle 75); the Colt's 101 deg (its grip pushes the
  magazine up on an open palm: palm up in the headset). `pad_cmd` `@magin` = the aim point that seats a held magazine.

**Round 32 [S] (2026-09-30, `logs/modlogs/reload-r32-*`, `reload-r32b-*`, `logs/shots/*-r32-*`, `r32-menu.png`):**
- **The flip in the hand:** the hold grip and `magHeld` take the pair's pose for the half that came out of the gun
  (`preBase`), not the flipped one drawn; round 31 held the flipped pose, so the hand turned the pair back and the
  insert direction flipped. Now the pair turns in the fist and meets the well as before the flip (BAR: 68 deg both, with
  the test hand unturned).
- **The Thompson's drum:** a disc 18.6 wide, 18.5 tall, 5.4 deep centred at mesh (0, 1.6, 11.5) (`geo.txt`) -- the old
  grab point was its centre, up in the receiver. Now (0, 5.0, 11.5) with MagR 11: grabbed at its lower rim, not the
  foregrip.
- **GrabTrigger** (MP40; caps bit7): the magazine is a press candidate only with the off hand's trigger held; the trigger
  is kept from the pad near the magazine and while it is held. Grip alone: nothing; trigger + grip: grabbed.
- **TriggerRack** (Colt; caps bit8): a locked-back action (bit3) that needs a rack (bit4) is released by the gun hand's
  trigger (RACK, that press kept from the pad): clip 0 -> 7.
- **Grip adjustments** (shared block v16 `gripKey`, `gripAdj[3][6]`, seqlock `gripSeq`; the menu's Reload grip page,
  `[ReloadGrip] <weapon>.<mag|hold|bolt>` in the player's ini): the hand in the part's frame turned at the wrist about
  the gun's mesh axes (tilt X, turn Y, roll Z), then moved along them (forward +Z, up -Y, right -X).
- **The menu in tabs** (General / Weapons / Hands; the tab row is item -1). `[Controls] LT=none` (the player: the left
  trigger no longer zooms). `[Hands] HandPoint=-6 -4 3` (the player's) is the shipped default.

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
