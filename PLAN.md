# Plan

The checklist for the current milestone. An unattended session can work through it top to
bottom. Tick items as they're done, adding a one-line result. A blocked item stays unticked
and gets marked **BLOCKED**, with the reason (time-box research at about two hours). Stop and
ask the user before any step that touches the game folder, launches the game for the first
time, or needs the headset.

## M0 (headless test rig), plus the research that M1 and M2 need — DONE

### Needs the user's go-ahead first
- [x] First launch (done by the user). The user config and saves are under OneDrive
      Documents (ENGINE-NOTES §7).
- [x] First `-windowed ResX= ResY= -log` launch. Windowed 1920×1080 works. **No engine log**
      (a console opens but stays empty), so the harness uses screenshots (D9). Main menu reached,
      SendInput works in menus, WM_CLOSE works, and the user folder is unchanged (ENGINE-NOTES §5c).
- [x] Decide how the game runs windowed. The command line (`-windowed ResX= ResY=`) never
      touches the player's ini (D7). Still to confirm on first launch.

### Static research (can run unattended; no game launch)
- [x] Script dump and camera search. All 2,483 classes are in `work/script/`
      (`tools/dump-script.ps1`). The view chain is mapped, and a stock split-screen lead was
      found (ENGINE-NOTES §6).
- [x] Control table recorded in ENGINE-NOTES §7. Gamepad bindings live in script
      (`MOHAPlayerInput.uc`), not ini.
- [x] `Direct3DCreate9`, `CreateDevice` and the presentation parameters: CreateDevice at
      `0x1090339A`, and windowed mode means no vsync (ENGINE-NOTES §5b).
- [x] DirectInput and XInput: mouse and keyboard are DirectInput with buffer sizes set;
      XInput is imported by ordinal (ENGINE-NOTES §5a).
- [ ] ~~In Ghidra: find where the log file is opened~~. Replaced: observe the log location on
      the first launch; that's cheaper and definitive.

### Harness (after the first launch)
- [x] `tools/harness.ps1`: launch, wait, key, shot, mem, ingame, to-gameplay, quit (with a kill
      fallback and Steam cooldown), and restore. The player's Config and Saved are backed up and
      restored every run (D8).
- [x] SendInput reaches the menus **and gameplay** (mouse-look turns the camera).
- [x] Front-end walk to live gameplay from the save, driven by screenshot checks (D9).
- [x] Screenshots: `capture-window.ps1` (PrintWindow) works for the D3D9 window.
- [x] **M0 acceptance: 4 of 4 consecutive `cycle` runs OK, about 30 s each.**

## M1 (the mod loads and stands down safely) — DONE 2026-09-25
- [x] CMake x86 `dinput8.dll` proxy forwarding all six exports (`tools/build.ps1`, static CRT,
      /W4 clean).
- [x] `MOHAVR.log` and `MOHAVR.ini` beside the DLL (module-handle path), with the previous log
      kept as `.prev`.
- [x] Build check from the in-memory PE header plus 4 signatures (D4); `Debug.TestWrongBuild`
      proves stand-down.
- [ ] ~~Hook library~~. Moved to M2: M1 needed only a verified IAT swap (`patch.cpp`).
- [x] `Direct3DCreate9` IAT hook, verified before the swap. Reached about 730 ms after init.
- [x] `tools/deploy.ps1` deploy, undeploy and status (with the user's OK; baseline-verified).
- [x] Harness `wait-log`, and every run's mod log is kept in `logs/modlogs/`.

## Next: M2 (D3D9 → OpenXR bridge, mono)
- [ ] **Address-space budget first:** in the hook, create the D3D9 object through
      `Direct3DCreate9On12` instead (behind an ini switch) and measure gameplay virtual memory
      against the ~1,335 MB baseline. It must leave room for the OpenXR runtime plus swapchains.
- [ ] Hook library for x86 (vcpkg `safetyhook` or `minhook`, x86-windows) with prologue checks.
- [ ] Hook `IDirect3D9::CreateDevice` (vtable) to force a windowed, headset-sized backbuffer
      (vsync is already off in windowed mode, ENGINE-NOTES §5b).
- [ ] OpenXR in the process: the x86 loader (vcpkg `openxr-loader:x86-windows`), plus a
      session on a D3D11 device run through `run-with-openxr-sim.ps1` (x86 simulator).
- [ ] Per-frame copy of the backbuffer into a shared texture, then the XR thread's swapchain,
      shown on a world-locked quad first.
