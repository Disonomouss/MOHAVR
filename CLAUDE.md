# MOHAVR — VR mod for Medal of Honor: Airborne

Native 6DoF VR mod (OpenXR) for Medal of Honor: Airborne (Steam app 24840): a 2007 Unreal
Engine 3 game, 32-bit, Direct3D 9.

## Read first
- `LESSONS-FOR-NEXT-VR-MOD.md`: lessons from GunVR, the previous project (2005 D3D9 game). Its
  methods and D3D9 → OpenXR plumbing carry over; its engine facts do not. Verify each claim
  against MOHA before relying on it.
- `STATUS.md`: where things stand. Start every session here.
- `PLAN.md`: the checklist an unattended session works through.
- `ENGINE-NOTES.md`: measured MOHA facts, with addresses and evidence.
- `tools/SETUP.md`: the RE/VR toolchain and how to use each tool.

## Standing rules
1. **Never modify game files.** Patch in memory only. The mod ships only its own DLL(s) and ini.
   `work/` holds derived copies (unwrapped exe, decompressed packages) for analysis; nothing in
   it is shipped or copied into the game folder.
2. **The mod must run against the original, SteamStub-wrapped `MOHA.exe`.**
   `work/MOHA.exe.unpacked.exe` is for analysis and debugging only.
3. **All engine addresses live in one header**, each with a comment saying what it is and how
   it was verified. Addresses are valid for the build pinned in `ENGINE-NOTES.md` §1 only.
4. **Verify the prologue bytes of every hook** before installing it. If they differ, stand down
   and log it; never patch blindly.
5. **Match the game's bitness:** everything that loads into MOHA is x86. The OpenXR runtime for
   tests is `tools/OpenXR-Simulator-x86`.
6. **The player's settings are theirs.** User overrides live in a separate ini that sits on top
   of the shipped defaults. Back it up before every test and restore it afterwards. Never delete
   or edit it for a test.
7. **Every new behaviour gets an ini switch.** It is on by default only once the simulator has
   proven it.
8. **Mark each item [S] (the simulator can prove it) or [H] (only the headset can judge it).**
   The player's verdict in the headset is the acceptance test.
9. **Time-box research items** (about two hours). When the box runs out, write up what's known
   and mark the item blocked, with the reason, rather than looping.
10. **Update STATUS, ENGINE-NOTES and DECISIONS after every milestone**, in the same commit as
    the code.
11. **Keep copies of every log before relaunching the game.** The game overwrites its log.

## Build and deploy
- `tools/build.ps1` builds `build/x86/dinput8.dll` (the mod) and verifies that it's x86 and
  exports under plain names.
- `tools/deploy.ps1 deploy [-Set 'Section.Key=Value']`, `undeploy` and `status`. The mod lives in
  the game's `Binaries` as `dinput8.dll` plus `MOHAVR.ini`. Undeploy keeps its logs and verifies
  the folder is back to its baseline. **Undeploy when a test session ends.**
- Engine addresses live only in `src/mohavr/addresses.hpp`; every patch goes through
  `src/mohavr/patch.cpp` (verify, then write).

## Test harness
- `tools/harness.ps1 cycle`: a cold start to proven gameplay and back, about 30 s, unattended.
  Other actions: `launch`, `to-gameplay`, `ingame`, `wait <check>`, `state`, `key <keys>`,
  `shot`, `mem`, `quit`, `restore`.
- It **always** backs up the player's `Config\` and `Saved\` before launching and restores them
  after quitting (D8). If a run dies, run `tools/harness.ps1 restore` before anything else; the
  next `launch` refuses to start until then.
- The engine writes no log, so screen state comes from `tools/screen_match.py` checks in
  `tools/harness-ref/` (D9). Add a new screen with
  `python tools/screen_match.py --add NAME shot.png X0 Y0 X1 Y1`, using a static UI area.

## Environment notes
- MCP bridges: `ghidra`, `cheatengine`, `x64dbg` (x32dbg for MOHA), `openxr-simulator`. All
  four need their app running.
- Ghidra without the GUI: `tools/start-ghidra-headless.ps1` serves `ghidra-projects/MOHAVR.gpr`
  (`/MOHA.exe`) on :8089 for the `ghidra` MCP server. Call `save_program` to persist edits.
- Windows PowerShell 5.1 drops empty-string arguments to native commands (pass `'""'`), and
  `Set-Content` defaults to ANSI. Write files with the file tools or `[IO.File]`.
- `Select-String` is case-insensitive by default. Use `-CaseSensitive` when grepping logs.
- Keep game launches short and separate. An interrupted launch can hang the session.
