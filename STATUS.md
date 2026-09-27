# Status

_Last updated: 2026-09-25 (overnight, end of the unattended checklist)_

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
2. **HEADSET-TESTS round 13** (deployed): the aim ray raised 8 cm to the gun's barrel (`Aim.RayUp`; round 12: the shots
   were a bit low). **Round 12 passed:** the arms and gun drawn at the aiming controller in true 3D (`Weapon.ViewModel=2`,
   now the default; ENGINE-NOTES §5t): not doubled, the fit feels OK. Next: per-weapon barrel line (the muzzle
   socket instead of one RayUp), the HUD crosshair in controller modes, the arms detached from the body (IK).
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
