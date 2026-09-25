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
