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
- [ ] Decide how the game runs windowed: an ini key or the `-windowed` command line. Record
      the choice in DECISIONS.

### Static research (can run unattended; no game launch)
- [ ] Grep the decompiled UnrealScript (`work/decompressed`) for camera, FOV and view code:
      `Camera`, `GetCameraViewPoint`, `FOVAngle`, `CalcCamera`, split-screen. Dump the key
      classes to text under `work/script/`.
- [ ] Read `DefaultEngine.ini`, `DefaultInput.ini` and `DefaultGame.ini`. Record the
      resolution/windowed/vsync keys and the control table (action → key) in ENGINE-NOTES.
- [ ] In Ghidra: find the callers of `Direct3DCreate9` and `CreateDevice`, and the
      presentation-parameters setup. Record the VAs with evidence.
- [ ] In Ghidra: find the `DirectInput8Create` usage, and whether XInput is imported (by ordinal?).
- [ ] In Ghidra: find where the log file is opened and the `-log` switch is parsed.

### Harness (after the first launch)
- [ ] `tools/harness.ps1` for MOHA, adapted from RDR2VR's: launch via Steam (app 24840),
      wait for log lines, quit, kill with a Steam cooldown, and back up and restore the
      player's ini.
- [ ] Check whether `tools/sendkey.ps1` (SendInput) reaches the menus. If not, record it and
      fall back to the mod's own input injection (M6).
- [ ] A front-end walk to live gameplay, driven by log lines.
- [ ] Screenshots: `screenshot.ps1` for the game window and `capture-window.ps1` for the
      simulator preview.
