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

## 5. Imports and APIs of interest

| API / DLL | Present | Evidence | Relevance |
|---|---|---|---|
| `d3d9.dll`, `Direct3DCreate9` | yes | string scan of the exe | Hook point for the D3D9On12 bridge (lessons §2) |
| `d3dx9_*` | yes | string scan | |
| D3D10 (`d3d10`, `D3D10CreateDevice`) | **no** | string scan | There is no D3D10 path to force off |
| `dinput8`, `DirectInput8Create` | yes | string scan | Keyboard and mouse likely go through DirectInput, so hook `GetDeviceState` (lessons §3); `SendInput` may not reach it |
| `XINPUT*` | DLL name present; `XInputGetState` not found as a string | string scan | May be imported by ordinal. Unverified. |
| `PhysXLoader` | yes | string scan; DLLs in `Binaries` | |
| `-log`, `Launch.log` | strings present | string scan | UE3 log available for a log-driven harness. Location not yet observed. |

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

## 7. Configuration

- `MOHAGame\Config\Default*.ini` exists: Engine, Game, Input, Player, Weapon, AI and others.
- No user ini or log exists yet; the game has not been run on this machine. The first launch
  should generate the user-level ini. Record its location here.

## 8. Rendering

Nothing measured yet. See §9.

## 9. Open questions (unverified; do not rely on these)

- Where UE3 builds the view and projection matrices natively (`FSceneView`, or the equivalent in
  this 2007 branch). Look for where `GetPlayerViewPoint` results enter native code.
- Whether this branch supports split-screen or multiple views per frame that stereo could reuse
  (lessons §2: check before re-running passes).
- The render thread: is it present and enabled on PC in this build?
- Near planes: the culler's versus the projection's (lessons §2 warns there may be two).
- The windowed-mode and resolution ini keys for this build (`ResX`/`ResY`/`Fullscreen` were not
  found in `DefaultEngine.ini`).
- The log location and the log lines that mark "in gameplay" for the harness.
- Whether `SendInput` reaches the game (DirectInput 8).
