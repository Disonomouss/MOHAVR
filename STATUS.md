# Status

_Last updated: 2026-09-25_

## Where things stand
**M0 and M1 are done.** The mod exists: an x86 `dinput8.dll` proxy that verifies the exe build,
logs, and hooks `Direct3DCreate9` through the IAT, standing down cleanly on any mismatch. The
harness deploys it, drives the game to gameplay, and waits on the mod's log. The mod is currently
**not deployed**; the game folder matches its baseline. Next: **M2** (PLAN.md), starting with the
address-space budget for D3D9On12.

## Done
- RE/VR toolchain mirrored from RDR2VR and verified (tools/SETUP.md):
  - MCP bridges: `ghidra`, `cheatengine`, `x64dbg` (x32dbg for MOHA), `openxr-simulator`.
  - Ghidra project `ghidra-projects/MOHAVR.gpr`: MOHA.exe analyzed. It is served headless by
    `tools/start-ghidra-headless.ps1`.
  - 32-bit OpenXR Simulator (`tools/OpenXR-Simulator-x86`), with a fix for the decorated export
    name. The loader-style smoke test passes for x86 and x64.
- Game-specific tools: Steamless, UE Explorer, umodel with the decompressor, and apitrace (x86).
- SteamStub 2.1 analysed: code is not encrypted, and the OEP and WinMain are found and named
  (ENGINE-NOTES §3–4).
- UnrealScript decompiles. The camera entry point is known: `PlayerController.GetPlayerViewPoint`
  → `PlayerCamera.GetCameraViewPoint` (ENGINE-NOTES §6).
- Project documents created: CLAUDE.md, ENGINE-NOTES, DECISIONS, ROADMAP, PLAN, HEADSET-TESTS.
  Git repository initialised (local only).
- Static research (ENGINE-NOTES §5a–§6):
  - D3D9 `CreateDevice` call site `0x1090339A`, with the presentation parameters decoded.
    Windowed mode means no vsync.
  - Input: DirectInput mouse and keyboard with buffer sizes set; XInput imported by ordinal.
  - All UnrealScript decompiled (`work/script`). The single-player view chain is mapped down to
    `MOHAPlayerPawn.CalcCamera`.
  - Lead for stereo: stock UE3 2P-vertical split-screen is intact.

- The user config and save folder is located (under OneDrive Documents), and the control table
  is recorded (ENGINE-NOTES §7).

- First instrumented launch (2026-09-25): windowed 1920×1080 works. There's no engine log (D9).
  SendInput drives the menus, WM_CLOSE quits cleanly, and 999 MB of virtual memory is already
  used at the main menu. `tools/userdata.ps1` backs up and restores the player's folder
  (verified byte-identical).

- Harness built and accepted (M0). Measured: gameplay uses about 1,335 MB of virtual memory,
  leaving **about 700 MB** of the 2 GB. SendInput reaches gameplay. The save resumes
  mid-parachute over the flak tower, giving a deterministic test scene.

- M1 accepted: init about 1 ms into the process, and `Direct3DCreate9` reaches the hook about
  730 ms later. The stand-down test and the offline smoke test pass, and undeploy restores the
  folder baseline.

- M2 started. **D3D9On12 works** (the game renders correctly through it) and costs +137 MB of
  address space, leaving 555 MB free with a largest block of 331 MB, enough for the OpenXR side.
  Device and Present hooks are in; the game presents from its render thread at over 1,000 fps in
  menus. The mod captures the backbuffer for the harness. Open issue: under 9On12 the game's
  window stays white (not blocking).

- **OpenXR runs inside MOHA** (static x86 loader, D3D11 session, a test quad visible in the
  simulator), but with 9On12 it leaves only 297 MB free (largest block 132 MB), too tight to
  build the renderer on. D10 proposes moving OpenXR into a 64-bit host process.

## Next
1. Decide D10 (out-of-process OpenXR host) with the user. Then build the shared-texture
   handoff: game (9On12, per-eye copy into shared D3D12 textures plus a fence) → host (OpenXR).

## Risks
- **2 GB address space (the dominant risk):** 9On12 alone leaves 555 MB free (largest block
  331 MB); 9On12 plus an in-process OpenXR session leaves **297 MB (largest 132 MB)**. D10's
  host process is the proposed answer. Re-measure after every addition with
  `tools/measure-variant.ps1`.
- **The desktop window under 9On12 is white.** A mirror window must come from the mod.
- **Engine-side stereo** in this 2007 UE3 branch is unknown, so the effort for M4 is unknown
  until researched.
- **Input:** DirectInput 8 may not see SendInput, so harness input could need the mod's own
  injection earlier than planned.

## Blocked
Nothing.
