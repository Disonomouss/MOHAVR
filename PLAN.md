# Plan

The checklist for the current milestone. An unattended session can work through it top to
bottom. Tick items as they're done, adding a one-line result. A blocked item stays unticked
and gets marked **BLOCKED**, with the reason (time-box research at about two hours). Stop and
ask the user before any step that touches the game folder, launches the game for the first
time, or needs the headset.

## Current: M0 (headless test rig), plus the research that M1 and M2 need

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

## Next: M1 (the mod loads and stands down safely)
- [ ] CMake project for an x86 `dinput8.dll` proxy that forwards `DirectInput8Create` to the
      system DLL (D5). Build it with the VS toolchain for x86.
- [ ] Mod log next to the exe (path from the module handle, not the working directory), plus
      an ini.
- [ ] Build check (SHA-256 or size plus timestamp of MOHA.exe, D4); stand down on a mismatch.
- [ ] Hook library (safetyhook or MinHook, x86 via vcpkg), with prologue verification.
- [ ] First hook: `Direct3DCreate9` (IAT or inline). Log that it was reached, and confirm the
      proxy's init runs first.
- [ ] Deployment script: copy the proxy into `Binaries\`, and remove it again (**needs the
      user's OK: it adds a file to the game folder**).
- [ ] Harness: wait on the mod's log lines instead of only screenshots.
