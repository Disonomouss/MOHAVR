# How much of MOHAVR can be built without the player testing

_Written 2026-09-25, end of day 1. For the player, and for the unattended (overnight) Claude session
that works through the checklist at the bottom._

## 1. Where things stand

Working and headset-verified (Quest 3 via Virtual Desktop):

| | Status |
|---|---|
| M0 test rig | Done. A cold start to proven gameplay and back in 30 s, unattended, with the player's data restored. |
| M1 mod loads safely | Done. An x86 `dinput8.dll` proxy with a build check, verified hooks, and stand-down on any mismatch. |
| M2 image in the headset | Done. D3D9On12 + shared textures → the 64-bit `MOHAVR-host.exe` runs OpenXR (the game keeps ~495 MB free). |
| M3 head tracking | Done. World-locked, level horizon, leaning feels good. |
| M4 stereo | Done in principle. The engine's own split-screen path, per-eye pose/FOV, per-eye view state (flicker fixed). Scale **100** chosen by the player. |
| In-headset menu | Working. World scale live and saved per player. |
| Hardening | Alt-tab / device-reset recovery, placeholder-pose rejection, motion blur and DOF off in VR, deploy/undeploy with baseline check. |

Known open issues from the headset:
- The first-person gun is seen double (it sits centimetres from the eyes, so real IPD gives a huge disparity).
- The HUD is drawn per eye at screen positions, so it's cross-eyed or at the edge of view.
- Menus: the 3D backdrop is stereo, but the menu UI is drawn across both eyes.
- No controller play yet: moving, aiming and firing still use mouse and keyboard.
- The game's own desktop window is white under 9On12 (there's no desktop mirror).

## 2. What the simulator can and cannot prove

The unattended session has the 64-bit OpenXR Simulator, the game (driven by `tools/harness.ps1`), mod-side
frame capture, host-side capture, Ghidra (headless), decompiled UnrealScript, and full logs. It has no
headset and no player.

**Can be proven [S]:**
- Anything visible in a frame: layouts, what's in which eye, where a quad sits, whether an element is doubled or missing.
- Logic and state: menu detection, cutscene detection, input mapping reaching the game, settings saved and restored.
- Numbers: address space, frame rate/time, eye-to-eye consistency (`tools/flicker_metric.py`), stability over long runs.
- Controller input, by **injection**: the simulator has no controller buttons, so the host gets a test channel
  (like `tools/menu_cmd.py`) that feeds synthetic controller state through the same code path.

**Needs the player [H]:**
- Comfort (turning style, vignette, anything that could cause nausea), and how things *feel*: aim, weapon fit, hand positions.
- Readability and comfortable placement of the HUD and menus.
- Real-runtime quirks (Virtual Desktop differs from the simulator: tracking flags, controller profiles, timing).
- Final visual quality: sharpness, shimmer on head motion, stutter.

Rule for the night: build each item to its **[S] acceptance**, ship it **behind an ini switch that's off by
default** where it could affect comfort, and write the **[H] question** into a new `HEADSET-TESTS.md` round
for the morning.

## 3. Estimate, milestone by milestone

| Milestone | What remains | Unattended share | Needs the player for |
|---|---|---|---|
| **M5 Layers** (menus/cutscenes on a cinema screen, HUD layer) | Detect menu, cutscene and "no pawn" states (RE + script); render mono full-screen in those states and show it on a world-locked screen in the host; find where the HUD is drawn (one-frame device-call trace) and redirect it to its own texture/quad | **~80%** | Screen size/distance, HUD placement and readability |
| **M6 Controller input** | The host reads Touch controllers → shared block → the game's **XInput** (`XInputGetState` is imported by ordinal; the game already has full gamepad bindings) as a virtual Xbox pad; a remappable table in the ini; injection test channel | **~85%** | Mapping feel, deadzones, turning comfort |
| **M7 Controller aiming** | RE the shot ray (script `GetAdjustedAim` / native trace); drive aim from the right controller pose; the body follows with a gentle servo | **~55%** | Aim accuracy/feel, the servo |
| **M8 Hands and weapon** | RE the first-person weapon mesh transform; attach it to the controller pose (this also fixes the doubled gun); per-weapon offsets in the menu | **~40%** | Weapon fit, arm/hand look (many rounds expected) |
| **M9 Comfort + menu** | Snap/smooth turn, vignette (host-side), seated/standing height offset, recentre, IPD/scale, all in the menu and saved | **~65%** | Every comfort default |
| **M10 Release** | Install/uninstall script (finds the game, backs up, removes cleanly), README, a zip, shipped defaults = the VR config | **~95%** | A final play-through |
| Engineering | Desktop mirror window (host), render-resolution option with memory checks, 30–60 min soak runs, death/reload/level-change checks | **~90%** | Sharpness vs. performance choice |

**Overall:** roughly **70% of the remaining work** can be done and [S]-verified unattended. The last 30% is
tuning that only the player can judge. It's cheap per item, but it takes several headset rounds (M8 above all).

**What limits one night:** reverse engineering is the pacing item (M5 state detection, the HUD split, M7's aim
ray, M8's weapon mesh), each time-boxed at about 2 h. The realistic overnight target is **M6 fully, M5 mostly,
plus mirror, recentre and packaging**, with M7 started. Items that hit their time box are marked BLOCKED with
what was learned.

## 4. Preconditions for the night (please check before sleeping)

- [ ] **Don't let the PC lock or sleep.** The harness drives menus with SendInput and focuses windows; a locked
      desktop blocks both. (Settings → Power: screen off is fine, sleep = never; screen-saver lock off.)
- [ ] Steam running and logged in (the harness launches via `steam -applaunch 24840`).
- [ ] The headset doesn't need to be connected. The night uses the simulator (per-process runtime; Virtual Desktop
      is untouched).
- [ ] Nothing else full-screen on the main monitor (the game window must be able to come to the front).

## 5. Standing rules for the unattended session

1. All of `CLAUDE.md`'s standing rules apply, especially: never modify game files; the player's data
   (MOHA `Config\`/`Saved\` and `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini`) is backed up and restored around
   every run (the harness does this); verify every hook site's bytes before patching.
2. **Every behaviour change is behind an ini switch**, default **off** if it can affect comfort or
   gameplay, until the player approves it in a headset round.
3. **Commit after each checklist item** with its [S] evidence (log lines, captures, metrics) in the message
   and in ENGINE-NOTES / DECISIONS.
4. **Time-box research at ~2 h.** When the box runs out: write up what's known, mark the item **BLOCKED (reason)**
   here, and move to the next item. Don't loop.
5. Keep the game folder clean between runs (`tools/deploy.ps1 undeploy`). **At the very end, deploy the
   morning headset configuration** and say so in STATUS.md.
6. If the game or host crashes: keep the logs (`logs/modlogs`), `tools/harness.ps1 restore`, record it, and
   continue with a different item. Never leave the game running.
7. Don't delete or rewrite the player's `MOHAVR.user.ini`; read it only to learn their settings (world scale 100).

## 6. Checklist (in order; each item: [S] acceptance → evidence → commit)

### A. Housekeeping (quick)
- [x] Round 4 recorded; the World-scale default is 100 (ini + code) — done 2026-09-25 evening.
- [x] Shipped defaults for the proven VR path: `D3D9On12=1`, `Host=1`, `HeadTracking=1`, `Stereo=1`. Done: a cycle with
      the shipped ini (only the simulator RuntimeJson set) gives stereo at scale 100 (log 20260925-213049). (1ad40f7)
- [x] Pruned `logs/shots` (71 → 20) and `logs/backup` (33 → 20).

### B. Menu additions (host) — [S] via `tools/menu_cmd.py`
- [x] **Recentre** item: the host re-creates LOCAL at the head's heading and x/z, keeps the old space for frames
      already rendered in it, and bumps `recenterSeq` (shared block v5) only after publishing new-space views; the
      game reads the sequence before the views and takes a new origin. [S] passed: head turned 30° and moved 0.3 m →
      Recentre → the new origin is (0, 1.7, 0) and the view faces the same way as at yaw 0 (screenshots
      213956-s0 / 213958-s1 / 214002-s2). The OpenXR Simulator ignored `poseInReferenceSpace`; it now honours it
      (local, uncommitted-to-upstream change in `tools/OpenXR-Simulator/src/runtime.cpp`; its own tests pass).
- [x] **Height** item (± 5 cm steps, ±60 cm, saved as `HeightOffset` in the player's ini). [S] passed: +20 cm
      raises the camera and 0 returns it exactly (shots h0/h20/h0b). The gun doesn't follow the offset (it hangs
      off the pawn's eye) — part of the M7/M8 weapon work.
- [x] Panel shrunk to 1024×480, opens 15 cm below eye level. `tools/menu_cmd.py` batches now apply one command
      per frame (before, "down down" moved one row).

### C. Desktop mirror (host) — [S]
- [x] A host window showing the left eye (or the game frame), so the monitor isn't white. Off by default
      (`Bridge.Mirror`). [S]: `capture-window.ps1 -Title "MOHAVR mirror"` shows gameplay.
      Done (`src/host/mirror.cpp`): `Bridge.Mirror=1` = a click-through overlay kept on the game's client area
      while the game is in front; `=2` = its own movable window. Shows the centre of the left eye, cropped to
      the window's shape, with no shaders (DXGI stretches). [S] passed, checked with `tools/mirror_check.py`:
      mode 1 → the screen over the game shows gameplay (white fraction 0.001, was 1.0), the overlay rect equals
      the game's client rect, WindowFromPoint at the centre hits the game (click-through), hidden while the game
      is minimised and back after; menus, Continue and the pause menu still take input. Mode 2 →
      `capture-window.ps1` shows gameplay (shot mirror2.png). Candidate default for mode 1 after round 5.

### D. M6 controller input → virtual Xbox pad — [S] by injection
- [x] RE (ENGINE-NOTES §5k): `WindowsClientInit` always creates four XInput joystick slots, so the game
      calls `XInputGetState(0..3)` every frame while its window has focus, with or without a pad. Answering
      pad 0 is enough. The pad layout is `MOHAPlayerInput.uc` `Bindings_Default`.
- [x] Host: a gameplay action set (`src/host/pad.cpp`: sticks, triggers, grips, A/B/X/Y, stick clicks; Touch
      and Index bindings, suggested together with the menu's), synced while the MOHAVR menu is closed (it
      publishes a centred, idle pad while the menu is open). Shared block **v6**: a seqlocked `XINPUT_GAMEPAD`.
      With controllers on, the left menu button is shared: tap = the game's Start, hold 0.6 s = the MOHAVR
      menu (`Controls.MenuHoldSeconds`).
- [x] Game: a verified IAT swap of `XInputGetState` (`src/mohavr/xinput_hook.cpp`). The slot must hold the
      loaded XInput DLL's ordinal 2 (it is XINPUT1_3.dll), else the mod stands down. Pad 0 comes from the host
      while it runs; everything else is the real XInput. Behind `Input.Controllers=1`, **default 0**.
- [x] Mapping table `[Controls]` in MOHAVR.ini (read by the host), defaults = MOHA's own pad layout on Touch;
      right stick Y off (the head drives pitch; `RightStickY=1` turns it on).
- [x] Test channel: `tools/pad_cmd.py` (`%TEMP%\MOHAVR\pad_cmd.txt`, a queue of timed states). Also
      `Debug.ViewState=1`: the game camera (x y z yaw pitch) in `%TEMP%\MOHAVR\view_state.txt`, 5 times a second.
- [x] [S] passed (sim, gameplay, from the game's own camera and HUD): left stick forward 2 s → moved about 733
      units along the heading; right stick 0.5 s → yaw 16388 → 26048 (about 53°); strafe → sideways; RT 0.4 s →
      ammo 30 → 27; A → reload (27/103 → 30/100); Start → the pause menu (harness `pausemenu`), Start again →
      back in game. The first poll came through about 13 s after launch.
- [ ] Headset question (round 5): the mapping feels right? Stick deadzone OK? Turning comfortable? Does tap
      vs hold on the menu button work? (Not testable in the simulator: its buttons come only from its window.)

### E. Turning comfort — [S] logic, [H] feel
- [x] Snap turn (`Comfort.SnapTurn=0|30|45`, default 0 = smooth). RE (ENGINE-NOTES §5l): PlayerController =
      `LocalPlayer+0x40`, `AActor::Rotation` at `+0xF4` (from `ULevel::MoveActor`, reached via `execSetRotation`).
      The host detects a flick (over 70%, re-armed under 30%), zeroes smooth turning and adds the step to
      `snapYawTotal` (shared block v6). The game adds each change to the controller's `Rotation.Yaw` once, and
      only while that yaw is the one the view came from (within about 11°), so cutscene or vehicle cameras are
      left alone. [S] passed with `Debug.ViewState`: two flicks right and one left at 30° → yaw 16388 → 21849 →
      27310 → 21849 (exactly ±5461 = 30°); at 45° → +8192.
- [x] Menu item **Turning** (smooth / snap 30° / snap 45°, saved as `[Comfort] SnapTurn` in the player's ini).
      [S]: menu → snap 45 → saved; the next flick = +8192.
- [ ] Headset question (round 5): snap vs smooth, and which step feels right.

### F. M5 menus/cutscenes on a cinema screen — [S]
- [x] Detection (ENGINE-NOTES §5m). **UI menu:** the game thread's `ShowCursor` count is ≥ 0. Measured: main
      menu 0, pause 0, gameplay −1; read without ever showing the cursor. **Cinematic camera:** the view's yaw
      isn't the controller's `Rotation.Yaw` (more than about 11° apart; in play they match within 2 units). "No
      pawn" isn't separate: death and matinee cameras fail the same yaw test. The controller's own `Location`
      (+0xE8) does NOT follow the pawn, so it can't be used.
- [x] In those states (debounced 150 ms) the Draw hook skips the stereo split, the view hook leaves the game's
      camera alone, and the frame is published as `hasView=0`, so the host shows it on its world-locked screen.
      The screen: `ScreenDistance` / `ScreenWidth` (default 2.0 m / 1.6 m, the round-1 "large but comfortable"),
      at head height when it appears (LOCAL is at eye level on VD but on the floor in the simulator).
      `Camera.CinemaScreen`: 0 off (default), 1 = UI menus, 2 = menus + cinematic cameras.
- [x] [S] passed (`tools/sim_shot.py` = the simulator's composited headset view): main menu → one flat image on
      the screen in both eyes (was split); Continue → gameplay in stereo; Esc → pause menu on the screen; Esc →
      stereo again. With mode 2 the parachute landing roll (a game camera animation) also went flat for 2.1 s,
      then back to stereo: feel question for round 5. No in-engine cutscene is reachable from the harness save,
      so real cutscenes are [H].

### G. M5 HUD on its own layer — [S] mechanics, [H] placement
- [x] RE (ENGINE-NOTES §5n; static, no trace needed): Draw's second per-player loop (`0x10C152xx`) gives each
      player's Canvas the clip `view+0x24/0x28` and a translation matrix `view+0x1C/0x20` (on the stack at
      `ESP+0x130`), pushes it, then calls `HUD.PostRender`.
- [x] **Changed approach** (simpler, and no extra texture or address space): no separate render target. Just
      before each eye's HUD pass, `HUD.Mode=1` gives that eye's canvas the rectangle where a head-locked panel
      (`HUD.Width` 2.4 m, `HUD.Distance` 2.0 m ahead, `HUD.Down` 0.1 m below eye level) appears in that eye,
      from its own projection and IPD offset, so both eyes fuse one flat HUD. MOHA's HUD positions elements by
      the clip but draws them at fixed pixel sizes, so the canvas gets a virtual clip of panel/`HUD.Scale` and
      its matrix is scaled by `HUD.Scale` (0.5). Two MidHooks (`0x10C1530C`, `0x10C15440`), both signatures in
      the build check. `HUD.Mode=0` (default) = unchanged.
- [x] [S] passed: gameplay capture shows the whole HUD (compass, health, grenades, ammo, weapon icon) laid out
      normally inside a centred panel in each eye, where each eye sees "2 m straight ahead" (left-eye centre at
      603 px of its half, right at 357 px: the eyes' FOVs mirror each other; shot 222702-hud2). The canvas is
      520 × 292 px per eye (virtual 1040 × 585).
- [ ] [H] round 5: HUD depth and readability (the eye images are 960 px wide, so small text is soft); tune
      Width/Distance/Scale. A HUD "off" mode isn't done (not needed if the panel works).
- Note: one launch in this block sat on a black screen for 150 s before the main menu (game responsive, only
  mono views, no Draw). A relaunch of the same build was fine. Watched in J.

### H. M7 aiming and the doubled gun — start, time-boxed
- [x] `Weapon.HideViewModel=1` (default 0) runs the pawn's own exec `HideWeapon 0` through
      `ULocalPlayer::Exec` (FExec at `LocalPlayer+0x3C`, slot 0 `0x10C1A220`, vtable checked before every
      call; output goes to MOHAVR.log). It's re-issued every 3 s for new pawns; the pawn's `bHidingWeapons`
      keeps it hidden across weapon switches. [S] passed: "handled" from the first pawn on, and the capture shows
      the empty hands with no gun (shot 223122-hideweapon). `Weapon.HideBody=1` = `RenderBody 0` (sleeves).
      Not visually verified.
- [x] RE of the shot ray, written up in ENGINE-NOTES §5o. Start = `Pawn.GetWeaponStartTraceLocation` →
      `Controller.GetPlayerViewPoint` (the game's eye, which our render-only view hook does NOT change).
      Direction = `Pawn.GetBaseAimRotation()` (native `execGetBaseAimRotation` `0x10D39090`) →
      `PlayerController.GetAdjustedAimFor` (aim assist) → spread → `MOHAPawn.GetPostAdjustedAimFor`. So shots go
      along the controller rotation (body yaw + game pitch), not the head. Next steps for M7 are listed there.
- [ ] [H] round 5: with `HideViewModel=1`, is the view comfortable? (The arms still show.)

### I. Performance and resolution — [S]
- [x] Frame timing is logged every 10 s: the game's Present-to-Present (`perf: game frame ...` in MOHAVR.log) and
      the host's XR frame (`perf: XR frame ...` in MOHAVR-host.log). Measured in gameplay, stereo, simulator:
      **1920×1080: 5.8 ms (172 fps), worst 12 ms, 0 frames over 20 ms**; the XR loop ran at 90 Hz and then locked
      to exactly 60 Hz (16.68 ms) about 15 s in. That is the simulator pacing to its preview window (60 Hz
      monitor), not the mod; real pacing is [H].
- [x] **Render resolution** (`Render.ResX/ResY`, default 0 = the game's own), done differently from the plan: UE3
      sizes everything from `ResX=/ResY=` on the command line, so the game's import of `GetCommandLineW` (found by
      walking its import table, verified against kernel32's export) returns the command line with
      `-windowed ResX= ResY=` placed first (first match wins). Verified: the launch said 1920×1080 → the device
      was created 2880×1620 and the bridge carried it. **2880×1620 (1.5×, 1440×1620 per eye): 6.0 ms per frame
      (166 fps), worst 9–23 ms, 498.8 MB of address space free, largest block 286 MB** (at 1080p in the same
      scene: 423.9 MB free, largest 231 MB; the free total varies with time in the level more than with
      resolution). The harness's screen matching doesn't work at other resolutions (MOHA's UI is fixed-pixel),
      so gameplay was reached blind (Enter, Down, Enter).
- [ ] [H] round 5: try `Render.ResX=2880 Render.ResY=1620`: sharper? Still smooth?

### J. Robustness — [S]
- [ ] A 30-minute soak in gameplay (idle + periodic injected movement) → no leaks (vmmap trend), no errors.
- [ ] Death/reload (if injectable) and level change: the stereo view state and bridge survive.
- [ ] A host crash mid-game (kill the host process) → the game keeps running without VR; logged.

### K. M10 packaging — [S]
- [ ] `release\install.ps1` / `uninstall.ps1` for players: find the game via the Steam library folders, copy
      dinput8.dll + MOHAVR-host.exe + MOHAVR.ini, back up/restore, refuse if a foreign dinput8.dll exists,
      clean removal.
- [ ] `release\README.md`: requirements, install, controls, the menu, known issues.
- [ ] `tools/package.ps1` → `dist\MOHAVR-<version>.zip`. [S]: install → cycle OK → uninstall → baseline.

### L. Morning handover
- [ ] HEADSET-TESTS **round 5** written: every [H] question from tonight's items, with the ini switches to try.
- [ ] STATUS.md updated: done / blocked / next.
- [ ] **Deploy the round-5 configuration** (Virtual Desktop runtime: `OpenXR.RuntimeJson=` empty), game not running.
