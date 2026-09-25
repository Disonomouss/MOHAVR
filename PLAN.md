# Plan

The checklist for the current milestone. An unattended session can work through it top to
bottom. Tick items as they're done, adding a one-line result. A blocked item stays unticked
and gets marked **BLOCKED**, with the reason (time-box research at about two hours). Stop and
ask the user before any step that touches the game folder, launches the game for the first
time, or needs the headset.

## Current: M0 (headless test rig), plus the research that M1 and M2 need

### Needs the user's go-ahead first
- [ ] First launch of MOHA through Steam, so it creates its user ini and log. Record both
      locations in ENGINE-NOTES §7.
- [x] Decide how the game runs windowed. The command line (`-windowed ResX= ResY=`) never
      touches the player's ini (D7). Still to confirm on first launch.

### Static research (can run unattended; no game launch)
- [x] Script dump and camera search. All 2,483 classes are in `work/script/`
      (`tools/dump-script.ps1`). The view chain is mapped, and a stock split-screen lead was
      found (ENGINE-NOTES §6).
- [ ] Read `DefaultInput.ini` and record the control table (action → key) in ENGINE-NOTES.
      *Display keys are done (§7); the control table is still open, and is needed for M6.*
- [x] `Direct3DCreate9`, `CreateDevice` and the presentation parameters: CreateDevice at
      `0x1090339A`, and windowed mode means no vsync (ENGINE-NOTES §5b).
- [x] DirectInput and XInput: mouse and keyboard are DirectInput with buffer sizes set;
      XInput is imported by ordinal (ENGINE-NOTES §5a).
- [ ] ~~In Ghidra: find where the log file is opened~~. Replaced: observe the log location on
      the first launch; that's cheaper and definitive.

### Harness (after the first launch)
- [ ] `tools/harness.ps1` for MOHA, adapted from RDR2VR's: launch via Steam (app 24840),
      wait for log lines, quit, kill with a Steam cooldown, and back up and restore the
      player's ini.
- [ ] Check whether `tools/sendkey.ps1` (SendInput) reaches the menus. If not, record it and
      fall back to the mod's own input injection (M6).
- [ ] A front-end walk to live gameplay, driven by log lines.
- [ ] Screenshots: `screenshot.ps1` for the game window and `capture-window.ps1` for the
      simulator preview.
