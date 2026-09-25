# Status

_Last updated: 2026-09-25_

## Where things stand
The toolchain is complete. No mod code exists yet. The current milestone is **M0 (headless
test rig)**; see PLAN.md.

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

## Next
1. First `-windowed ResX= ResY= -log` launch, with the user's go-ahead: confirm the switches,
   and record the log location and milestone log lines.
2. The harness (backup and restore of Config and Saved per D8).

## Risks
- **2 GB address space:** the exe is not large-address-aware. D3D9On12, the OpenXR runtime and
  extra render targets all share that space. Measure headroom in M2.
- **Engine-side stereo** in this 2007 UE3 branch is unknown, so the effort for M4 is unknown
  until researched.
- **Input:** DirectInput 8 may not see SendInput, so harness input could need the mod's own
  injection earlier than planned.

## Blocked
Nothing.
