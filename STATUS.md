# Status

_Last updated: 2026-09-29 (round 26 deployed; the "Next" list is current, the history below it is kept)_

## Where things stand
**Update, end of 2026-09-25:** M0, M1 and M2 are done. The game's image reaches the headset through
the 64-bit host (D10), with the headset round passed. The older paragraphs below are kept for
history.

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

- **D10 built: the game's frames reach OpenXR through `MOHAVR-host.exe`** (x64). Shared D3D12
  textures and fences, 1:1 with the XR loop, and the gameplay frame shows on the quad in the
  simulator. The game keeps 505 MB free (largest block 312 MB). **M2 [S] done.**

- **M2 done, including the headset:** the player saw the game on the floating screen in a Quest 3
  through Virtual Desktop, at 90 Hz, stable and smooth, with correct colours (HEADSET-TESTS round 1).

- **M3 [S] done:** `ULocalPlayer::CalcSceneView` was reverse-engineered (ENGINE-NOTES §5g). With
  safetyhook MidHooks, the head pose from the host drives the camera (game yaw + head
  orientation, head translation), and the projection is the headset's asymmetric FOV. The host
  submits a projection layer with the frame's own render pose. Verified in the simulator
  (yaw/pitch/roll correct, each eye filled at 60 FPS).

- **M3 done, including the headset** (round 2): world-locked, level horizon, leaning feels good.
  Headset testing also found and fixed an alt-tab hang (device Reset), haze from motion blur, and a
  wrongly captured head origin (a placeholder pose). Scale is back at 50 per the player, to revisit
  with stereo.

- **M4 [S] done: true stereo through the engine's own split-screen path** (ENGINE-NOTES §5j). The
  same local player is drawn twice per Draw, left and right half, each with its own eye pose and
  asymmetric FOV, in one render pass. About 10 MB extra, and 495 MB still free. Verified in the
  simulator.

- **M4 in the headset:** stereo works and the right-eye flicker is fixed (per-eye FSceneViewState). The
  in-headset menu (host, ImGui, Touch controls) sets world scale live; the player chose **100**, now the
  default. Open: the doubled gun (M7/M8), the per-eye HUD (M5), no controller play yet (M6).

- **Overnight 2026-09-25 (unattended, UNATTENDED-REPORT.md §6 A–L, all [S] parts done):**
  - Menu: Recentre, Height, Turning (smooth/snap), smaller panel (shared block v5/v6).
  - Desktop mirror (`Bridge.Mirror`).
  - **M6 controllers:** an XInputGetState IAT hook gives the game a virtual Xbox pad from the Touch
    controllers (`Input.Controllers`, `[Controls]`); move, turn, fire, reload and pause were verified by
    injection.
  - **Snap turn** through `PlayerController.Rotation` (+0xF4).
  - **M5 cinema screen:** menus (cursor count) and cinematic cameras (yaw test) go flat on the host's screen.
  - **M5 HUD panel:** the per-eye canvas is placed and scaled so both eyes see one panel.
  - `Weapon.HideViewModel` through the game's own exec (ULocalPlayer::Exec).
  - The shot ray mapped for M7.
  - Frame-time logging, and `Render.ResX/ResY` (2880×1620 measured fine).
  - Host-crash fallback, a 30-min soak, death/reload.
  - The player package `dist/MOHAVR-0.7.0.zip`, with install/uninstall tested end to end.
  - Every new feature is behind a switch, off by default; round 5 turns them on for the headset verdict.

- **Headset round 5 passed (2026-09-26):** controllers, tap/hold menu button, smooth/snap turning,
  Height/Recentre, menus on the flat screen, HUD panel, 2880×1620 (steady 90 Hz in the headset), mirror.
  All are now shipped defaults (cutscenes stay 3D, per the player). Follow-ups done and simulator-proven:
  - the player's control layout: multi-input bindings, right-stick flick = crouch, sprint toggle;
  - Aim.HeadPitch (the controller's pitch from the head: gun and shots follow the head vertically);
  - the title-screen height from a tracked pose.

## Next
1. **M7 done** (headset round 11 passed; `Aim.Mode=3`, right hand, is the default): aiming with the right controller plus the
   red-dot reticle (ENGINE-NOTES §5s): `Aim.Mode` 1 head / 2–3 controller, through the player's
   `GetBaseAimRotation` and a per-frame engine trace; the host draws the reticle. Rounds 6–10 passed (window lock,
   decals, A/B in menus).
2. **HEADSET-TESTS round 26** (deployed): left-hand mode fixed (the shoulders not flipped in the mirror world; the grip
   applied after mirroring; the aim line's sideways offset mirrored by the host), the jump and landing animations replaced
   by idle (`Weapon.JumpArms`), the jump camera lift left out (`Camera.JumpLift=0`), the brass from the drawn gun and the
   muzzle flash hidden (`Weapon.Brass`, `Weapon.MuzzleFlash`; execActivateSystem hooked), frame pacing on by default (D16).
   Round 25: jitter passed, left grip passed, pacing measured (sd 0.5 vs 1.3-3.2 ms), left-hand mode failed, idle controllers
   untested. Round 25 as deployed: left-hand mode drawn mirrored (`Weapon.LeftHandMirror`; the proxy's
   determinant sign for culling), the left grip unmapped (`[Controls] LB=none`), controllers that stop tracking held
   relative to the head (`Hands.HoldLost`), and movement: the arms play idle while walking (`Weapon.WalkArms`), the move
   carried with the body (`Weapon.CatchUp`), frame pacing as a live menu switch (off by default; the player's A/B).
   Round 24: sprint passed; slight jitter/rubber banding in all movement, the left grip toggled attachments, the gun
   went double after ~10 s idle (the Quest dropped the controllers), left-hand mode had the right arm reach across;
   dual wielding researched and parked by the player (ENGINE-NOTES 5af). Round 24 as deployed: the first-person arms
   don't play the sprint animation (`Weapon.SprintArms=idle`, a MidHook in the arms' activity node tick); the
   speed-detected sprint lock is removed. Round 23: hit cross and tracers
   passed; the lock failed (never engaged on the ground). Round 23 as deployed: the hit cross hidden (`HUD.HitMarker`), tracers off (`Weapon.Tracers`; they
   start at the third-person gun), the gun hand held on its controller while sprinting (`Weapon.SprintLock`). Round 22:
   shots passed (225 logged, from the gun, on the dot), crosshair gone. Round 22 as deployed: the red dot's trace uses the bullets' own collision flags (0x268BF, per-poly;
   found in the native CalcWeaponFireNative), triggers passed like the bullets do, shots start at the gun along the
   dot's ray (`Aim.ShotFromGun`), a per-shot log (`Aim.ShotLog`). Round 21: red dot switch and pistol arm passed; shots
   failed (two overlapping triggers ping-ponged, aim at the gun). Round 21 as deployed: menu Red dot switch, the game's crosshair hidden (`HUD.Crosshair`), the aim
   passes through triggers like the game's bullets, the free arm with a pistol/grenade from the rifle's arm pose
   (`Weapon.FreeArmPose`), steadier elbow bend and forearm twist. Round 20: shots passed, shoulders better, the elbow
   hinge (`Weapon.ElbowHinge=2`). Round 20 as deployed: the aim ray steps out of whatever it starts inside (round 19's log: runs of
   0.2 m hits while walking with the BAR; the lock had 0 torn reads), `ShoulderWidth` 30, `FreeHand` default tilt 180
   (the player's fix). Round 19: shots kept on the red dot (the shared view lock held only for the copies,
   readers retry and keep the last frame; the aim trace ignores volumes and re-traces past a start inside geometry),
   the free hand's grip taken from a long gun held still, and a Free hand menu page (tilt / turn / roll / forward,
   saved as the player's `[Hands] FreeHand`). Round 18: arm jitter gone, foregrip/reload rules passed; the grenade's
   free hand and stray shots failed (fixed here). Arm IK and the
   earlier hand features are on by default. The player will tune per-gun fits in play and say when.
   **Backlog (the player's):** pick up grenades lying on the ground (the game pools them) and throw them back; dual
   wielding (parked: one weapon at a time in the game, options ranked in ENGINE-NOTES 5af); per-gun fit defaults once
   the player reports theirs; a full manual reload.
3. **Address space:** a control soak without D3D9On12 on the same route; texture-pool limits if needed.

## Risks
- **2 GB address space:** with the D10 host, gameplay leaves **505 MB free (largest block
  312 MB)** at the landing, but the 30-min soak went down to **205 MB free (largest block 102 MB)** after the
  player walked into the town (streaming; it plateaued). Eye render targets in M3/M4 still come out of this. Re-measure after every addition
  with `tools/measure-variant.ps1`.
- **The desktop window under 9On12 is white.** Solved by the host mirror (`Bridge.Mirror=1`/`2`).
- **Engine-side stereo** in this 2007 UE3 branch is unknown, so the effort for M4 is unknown
  until researched.
- **Input:** DirectInput 8 may not see SendInput, so harness input could need the mod's own
  injection earlier than planned.

## Blocked
Nothing.
