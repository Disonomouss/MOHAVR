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
- **The rig is inverted** (a hand-driven FPS rig): Root(0) → Anchor(1) → HipsOffset(2) → Hips(3) → Spine(4) →
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
