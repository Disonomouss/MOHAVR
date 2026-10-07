# Goal: physical crouch, comfort, compatibility, the known issues, mounted guns and a mission sweep

Set 2026-10-08 by the player ("Write a goal prompt to have you work through items 1-4", after the feature review the same
day). For the Claude Code session that runs `/goal` on it: read `STATUS.md`, then this file. Keep the checklist below
current (DONE / BLOCKED / WONTFIX, with the evidence in one line) and commit this file with each item.

## Rules for this goal
1. `CLAUDE.md`'s standing rules and `UNATTENDED-REPORT.md` §5 apply:
   - never modify game files;
   - touch the player's data (MOHA `Config\` and `Saved\`, `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and
     `MOHAVR.freehand.bin`) only through the harness's backup and restore;
   - verify every hook's prologue bytes;
   - addresses go only in `addresses.hpp`.
2. **Don't ask the player anything and don't wait for answers.** Whatever only the headset can judge becomes a
   question in HEADSET-TESTS **round 54**. If the player replies mid-goal, record the reply in HEADSET-TESTS, add the
   fixes to this checklist and carry on.
3. **Every behaviour gets an ini switch** (rule 7, §5.2), and a menu item where a player would want to change it:
   - **comfort and feel** (the vignette, seated mode, physical crouch): ship **off**;
   - **a fix for a fault** (the image-format fallback, the known issues): ships on once the simulator proves it;
   - **diagnostics** (the capabilities log): on.
   Record each default in DECISIONS (the next free number is **D61**).
4. **Research time box: about 2 h per item.** Then write up what's known, mark the item **BLOCKED (reason)** and go to
   the next. Don't repeat an approach that has failed 3 times. **WONTFIX (reason)** is allowed where the research shows
   the cost is out of proportion (say what it would take).
5. **This PC's game is the EA app's copy** (D58, `tools/gamedir.txt`). It holds **the player's own install**, from
   the setup program. The player authorises test deploys over it for this goal, so use `deploy.ps1 -PlayerAgreed`;
   F3 leaves a working install behind.
6. **Test runs:**
   - one game launch at a time;
   - copy the game and host logs to `logs/modlogs` before any relaunch;
   - `tools/deploy.ps1 undeploy` after each run (it reports the setup's `MOHAVR-README.md` as extra: expected);
   - never leave the game running;
   - after a crash: keep the logs, run `tools/harness.ps1 restore`, note it, and carry on with another item.
7. **Commit per item** on `main`: the code together with STATUS, ENGINE-NOTES, DECISIONS and this file. The message
   ends with the Co-Authored-By trailer. **Don't push, don't bump the version, don't publish a release:** the player
   decides that.
8. **Edits:** make them with the file tools, or with Python scripts in the scratchpad. Bash heredocs mangle backslashes
   here. Run `deploy.ps1` and `harness.ps1` with `&` in PowerShell, from the repo root (`Set-Location E:\Clause\MOHAVR`
   first: a relative path from another folder silently runs nothing).

## Proving it in the simulator
1. **Build:** `tools/build.ps1`. It falls back to Visual Studio's bundled vcpkg; the first build fetches the ports.
2. **Deploy the test configuration:**
   `& .\tools\deploy.ps1 deploy -PlayerAgreed -Set 'Render.ResX=1920','Render.ResY=1080',"OpenXR.RuntimeJson=$PWD\tools\OpenXR-Simulator\bin\openxr_simulator.json",'Debug.GameCommands=1'`.
   Add the item's own switches. The EA app drops command-line arguments, so the resolution comes from `Render.ResX/ResY`.
3. **Launch:** `tools/harness.ps1 launch` (it clicks through `moha_setup.exe`), then `to-gameplay`. The save resumes
   mid-parachute over the Flakturm tower. Game commands go in `%TEMP%\MOHAVR\game_cmd.txt`.
4. **Drive it:**
   - the head: `python tools/sim_pose.py`;
   - the hands and raw presses: `python tools/pad_cmd.py --seq ...`;
   - the menu: `python tools/menu_cmd.py`.
   CLAUDE.md lists the test poses (the wrist HUD's: `hand=l,0.13,-0.40,0.30,90,0,90` with `--pitch -45`).
5. **Record:**
   - screenshots: `harness.ps1 shot`; for the host's quads, `tools/sim_window_shot.ps1`;
   - read both logs.
6. **Finish the run:** `harness.ps1 quit`, then `deploy.ps1 undeploy`.

## Track A: quick wins (comfort and stance)
- [x] **A1 Physical crouch.** Crouching for real today only lowers the camera, and the game's stance stays standing
  until the stick click (Xbox X, a toggle). Wanted: the game's crouch follows the player's real head height.
  - **The threshold:** the head below a share of the standing eye height (calibrated from the recentre or the menu,
    default about 75 %).
  - **Hysteresis:** no flicker at the threshold.
  - **The game's stance is the truth:** read it from the game (find or add it in the shared block) and drive the
    toggle until it matches. Never toggle blind.
  - **The stick click still works:** a manual stance change takes over until the real height crosses the threshold again.
  - **Excluded:** menus, ladders, mounted guns, cinematics and the parachute.
  - **Proven [S] when** `sim_pose.py --y` crouches and stands the pawn (the log, and the eye height from §5an's
    `Debug.EyeFloor`), the click still works, and no toggle fires in a menu.
  - `[Controls] PhysicalCrouch=0` and its threshold; the menu (General). Round 54: does it trigger by accident?

  **DONE** (D61, ENGINE-NOTES 5bt; `logs/modlogs/20261008-0609*`): head 0.50 m down -> `asking the host to crouch`, `the game
  crouched` 22 ms later, `settled crouched (the head's) -- the eye 110.7 cm above the feet (the game camera 95.9,
  compensation 64.8)`; up -> stood, 160.9; a quick re-crouch 110.7; the stick crouch 95.9 (`the stick's`), adopted by
  the head (110.7), a stick stand while low held (110.9) until the head crossed; the pause menu: no request, backing out
  crouched at once; the menu's off: no request. The line is a drop in metres (0.40, LOCAL has no floor), not a share.
- [x] **A2 Vignette.** A comfort tunnel that darkens the edge of the view while the player moves or turns by stick (not
  for head motion). Strength none / light / strong; it fades in and out over about 0.2 s.
  - Drawn by the host: a quad or a mask over the projection layer, both eyes, never over the menu.
  - `[Comfort] Vignette=0`; the menu (General).
  - **Proven [S] when** `sim_window_shot` shows it while the stick moves, gone when still; no frame-time cost in the
    perf lines.

  **DONE** (D62; `logs/shots/vig-*.png`): strong, a 3 s stick walk -> `vignette: in (moving by stick)` / `out`; the edge
  brightness 30.7 still, 3.3 moving, 30.5 after, the centre lit in both eyes; the XR frame 11.11 ms (0 of 900 late).
- [x] **A3 Seated mode.** A height offset so a seated player gets the standing eye height.
  - A "Calibrate seated" item in the menu: the current head height becomes the game's standing eye. It's saved in
    the player's ini.
  - It works together with A1 (crouch is relative to the calibrated height) and with `MinEyeHeight`.
  - `[Comfort] Seated=0`. **Proven [S]** with `sim_pose.py --y 1.2`: the eye at the standing height, and crouch,
    jumps and the landing unchanged.

  **DONE** (D63): the origin already makes a seated head the standing eye after a Recentre (the "Calibrate" is the existing
  Recentre); new: Seated (menu, ini) with SeatedCrouchDepth 0.25 m. [S]: sat (0.5 m down) -> crouched on the seated line;
  Recentre seated -> `origin set at (0.000 1.200 0.000)`, stood, the eye 160.9 cm; a 0.30 m lean -> crouched (130.8, the
  real head), up -> stood; a jump 711 ms, no crouch action; the landing path unchanged (no compensation standing).

## Track B: compatibility (other headsets and runtimes)
- [x] **B1 Capabilities log.** At start the host logs:
  - the runtime's instance extensions (one line);
  - the swapchain formats offered;
  - the system name and its tracking properties;
  - the view configuration's recommended size and refresh rate.
  This is what a remote tester's log must answer. **[S]:** the lines appear with the simulator.

  **DONE** (D64): with the simulator -- `runtime extensions (9): ... XR_KHR_composition_layer_depth ...`, `system "OpenXR
  Simulator" (vendor 0): orientation tracking yes, position tracking yes; swapchains up to 4096x4096, 16 layers`, `the runtime
  recommends 1280x1400 per eye`, `swapchain formats 29 28 91 87 10 2 24 20 40 45 55; reference spaces VIEW LOCAL STAGE;
  refresh rate not exposed`.
- [x] **B2 Image-format fallback.** The host stops with "runtime offers no B8G8R8A8 swapchain format". When only
  R8G8B8A8 (sRGB or not) is offered, use it, with a swizzling copy (a shader blit, not CopyResource) for the game's
  BGRA frames, the menu, the wrist HUD, the reticle and the scope view.
  - `[Debug] ForceRgbaSwapchain=1` takes that path with the simulator.
  - **Proven [S] when** that path renders the same image (colours compared on a capture, red stays red) at the
    same frame time.

  **DONE** (D65; `logs/shots/b2-*`): forced -> `swapchain format 29 (R8G8B8A8, sRGB) -- the game's frames are blitted
  into it`; against the BGRA run the captures match (gameplay 31.4/30.0/27.1 vs 31.4/30.1/27.1; the menu's highlight
  31.5/55.5/83.9 vs 31.2/55.2/84.3, title 179/152/87 vs 182/154/88; no swapped red/blue), the XR frame 11.11 ms on both.
- [x] **B3 More controller profiles bound directly**, as D59 did for the G2. Each is enabled only if the runtime
  offers its extension, with its paths checked against the OpenXR spec. Each is logged with "bound as", and the
  README's controller line is updated.
  - **The HTC Vive Cosmos** (`XR_HTC_vive_cosmos_controller_interaction`): Touch's layout.
  - **The Pico 4** (`XR_BD_controller_interaction`, `/interaction_profiles/bytedance/pico4_controller`): Touch's
    layout.
  - **First-generation WMR** (`/interaction_profiles/microsoft/motion_controller`, core): sticks, a trackpad, a menu
    button each, no face buttons. Design a mapping (for example, the trackpad's four quadrants as A/B/X/Y) and write
    it into the README.
  - **The HTC Vive wands** (`/interaction_profiles/htc/vive_controller`): no sticks. Only if B3's others are done and
    a trackpad scheme is clear (movement on the left pad, turning on the right); else WONTFIX with the reason.
  - **[S]:** the simulator still binds Touch; the suggestions for the profiles it offers are accepted. **[H]:** a
    tester per device (round 54 asks who has one).

  **DONE** (D66): the Cosmos and Pico 4 (Touch's layout; their extensions when offered), first-generation WMR and the Vive
  wands (the trackpad scheme: upper / lower half = upper / lower face button, centre = stick click; the wands' pads are
  the sticks) -- the wands done, not WONTFIX. [S]: `actions attached (... Touch, Index, WMR, Vive wands ...)`, no
  `bindings not accepted`, `bound as .../oculus/touch_controller`, `cycle OK in 54s`; SteamVR here names the Cosmos, WMR
  and Vive profiles.

## Track C: the known issues (README "Known issues")
- [x] **C1 The wrist HUD over the gun or arm.** The panels are composition quads, always on top of the projection
  layer. Options, cheapest first:
  - fade the panels while the gun or the gun hand's arm is between the eyes and a panel (geometry from the poses
    the host already has);
  - `XR_KHR_composition_layer_depth`, if the runtime offers it;
  - draw the panels into the game's frame with depth.
  **Proven [S]** by a capture with the gun hand swept across the wrist.

  **DONE** (D67; `logs/shots/c1-*.png`): the geometric fade (the gun's line, forearm to barrel, within 8 cm of the eye's
  line to a panel -> dimmed to 15%); depth composition rejected (no game depth on the bridge). [S]: the gun hand across the
  wrist -> `the left/right panel behind the gun -- dimmed`, away -> `clear of the gun again`; the captures agree.
- [x] **C2 The desktop mirror without the HUD in wrist mode.** In wrist mode the mirror window (`src/host/mirror.cpp`)
  shows no HUD. Draw the HUD texture on it: the full-screen HUD, as the flat game shows it. Behind `[Bridge] MirrorHud=1`.
  **[S]:** a capture of the mirror window.

  **DONE** (D68; `logs/shots/c2-mirror.png`): `mirror: the HUD drawn over the mirror (wrist mode: the frame has none)`;
  the capture shows the compass, health, grenades, the weapon and 50/90 where the flat game draws them.
- [x] **C3 Dropped magazines and ejected rounds through walls.** Find why: they're drawn without depth, or they don't
  collide, or they're drawn after the scene. Fix it, or limit it (for example, no ejection inside a wall's distance).
  **[S]:** a scripted ejection next to a wall, from the side.

  **DONE** (D69): the falls traced at the drop (falltrace.cpp) -- they land on what is under them or stop at a wall; the
  depth group can't change for the magazine (a bone of the gun), so staying in front of surfaces is the fix. [S]: the test
  path -- open street `landed on TOP` (13 u above the feet' plane), facing the low wall `a WALL ... after 0.68 s`; a
  Thompson magazine ejected beside the wall `meets something under it after 0.13 s -- it rests at 3391 (the feet at 3264)`.
- [x] **C4 The StG44's dust cover.** The bolt body and the dust cover are part of the body mesh (round 52). Time box
  1 h: is a separable part or a material trick possible? Otherwise WONTFIX with the reason.

  **WONTFIX** (D70): one material (6,166 faces), and the port's faces are skinned to `RootOffset` (151, the body) or `Bolt`
  (242, the handle and rod); no bone or art for the cover. It would take in-memory re-skinning of the GPU vertex buffers
  and bone maps for every LOD plus a hinge pose (days, crash risk), or new art (not shippable).

## Track D: mounted guns (research first)
- [x] **D1 Research** (time box 2 h; D33 and GOAL 2026-09-30 A6 have the background):
  - how the player mounts a gun: the controller's `PlayerMountedMG` / `PlayerUsingMG` states;
  - how the game drives the mounted gun's aim and the camera;
  - what the mod does there today: the hands, the aim ray, the view;
  - how to reach a nest in the simulator: another map with `open <map>` through `game_cmd.txt`, or a nest
    spawned or moved near the save's tower.
  Write `MOUNTED-DESIGN.md`: what's known, the options (the view locked to the gun or free, both hands on the handles
  aiming it, or the head aiming as the game does now), a recommendation, and the first phase.

  **DONE** (MOUNTED-DESIGN.md): the nest is a `MOHAPawn` in the pawn list, manned through `MyCSA.UsedBy`; the states
  `PlayerMountingMG` / `PlayerMountedMG` and `rMGRot` read; reached in the simulator (`mohavr mg list` found 10 nests,
  `goto` and `use` manned one). Before the fix the view model drew the mounted MG42 in the hand and the hand aimed it.
  Recommendation: Phase 1 the game's way (the head aims), Phase 2 hands on the handles after the headset round.
- [x] **D2 Phase 1, if D1 found a path:** the view and the hands at a nest are sane (no doubled gun, no aim ray off into
  the sky, the HUD readable). The gun aims with the hands on its handles if the research makes that feasible, behind
  `[Weapon] MountedHands=0`. **[S]:** at a nest, firing at a known spot hits it. Otherwise BLOCKED with the reason.

  **DONE** (D71; `logs/shots/0656*-d2-*.png`): Phase 1 -- `[Weapon] MountedGame=1`: the gun on its mount drawn by the
  game, no hand ray, no reticle; a soldier placed 6 m ahead killed by the burst (`Health 0 ... MOHAMGDamageType on
  Spine1`, ammo 78 -> 60); the head pitches the gun with the view, the stick turns the mount. The hands on the handles
  (MountedHands) is designed (MOUNTED-DESIGN 3.2), not built: how it should feel goes to the headset first (round 54).

## Track E: mission sweep (unattended QA)
- [x] **E1 Every mission loads with the mod and the guns' extras work.** List the campaign maps (`CookedPC`, the
  game's map list). Load each one with `open <map>` through `game_cmd.txt`. In each:
  - reach gameplay;
  - give the knife and a gun with ejected rounds;
  - fire and eject;
  - throw a grenade;
  - check the logs for the model look-ups (the knife and the rounds borrow other guns' models; the README says they
    were checked in the first levels only), errors and stand-downs;
  - take one screenshot.
  Write a table per map (loaded, gameplay, knife, rounds, grenade, errors) into `work/research/goal/sweep.md` and a
  summary in STATUS. Fix what breaks, or record it as a known issue. A script in `work/research/tests/` that reruns the
  sweep.

  **DONE** (D72; `work/research/goal/sweep.md`, `work/research/tests/sweep.ps1`): 7 of 7 maps load with the mod (no
  stand-down, error or crash); `open` loads only the persistent level, so the live table is a floor, and the packages
  show every borrowed class in every mission; live Husky lacked the G43 bullet and M12 shell -> `RackRoundAlt` (the
  StG44 / G43 fall back to the K98's bullet2; tested: `a live round thrown (RackRoundAlt)`); the M12 shell and the knife
  recorded as known issues where their source isn't loaded.

## Finish
- [x] **F1** After the last change:
  - `tools/harness.ps1 cycle` OK;
  - the wrist menu button (D60) A-D and the manual reload's quick pass (one gun of each kind) rerun.

  **DONE** (the final build): `cycle OK in 54s`; D60 A `MOHAVR menu opened`, B `MOHAVR menu closed`, C `the game's Start` (a
  game menu), D X looking away reached the game (the grenade); the Thompson `EJECT 50 -> 0`, `INSERT 0 -> 50`; the K98 fired
  and worked by hand (`BOLT UP`, `BOLT BACK ... the case ejected`, `BOLT FORWARD ... a round chambered`, `BOLT DOWN`); the
  Garand emptied, `INSERT 0 -> 8 ... the action closed on its own`. (The M12's class isn't loaded in the save's Husky.)
- [x] **F2** HEADSET-TESTS round 54: what changed, how to try it, and the [H] questions for each item. Ask in it whether
  testers have a Cosmos, Pico, WMR first-gen or Vive wands.

  **DONE:** HEADSET-TESTS round 54 -- A1-A3, B (testers' devices and runtimes, their logs), C1-C3, D, E, with what changed,
  how to try it and the questions.
- [ ] **F3** Leave everything in order:
  - STATUS updated, everything committed, the game not running;
  - `tools/deploy.ps1 undeploy`, then `& .\tools\deploy.ps1 deploy -PlayerAgreed` with no `-Set`: the shipped
    defaults, so the player's install works with the new build;
  - `tools/package.ps1` builds the packages at the current version. Don't bump it and don't publish.
  - Say all of this in STATUS: the player reruns the setup or publishes after their headset round.
