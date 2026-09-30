# Goal: a manual reload for every accessible gun, and the parachute landing

Set 2026-09-30 by the player. For the Claude Code session that runs `/goal` on it: read `STATUS.md`, then this file.
Keep the checklist below current (DONE / BLOCKED, with the evidence in one line) and commit this file with each item.

## Rules for this goal
1. `CLAUDE.md`'s standing rules and `UNATTENDED-REPORT.md` §5 apply. In particular, never modify game files. Touch the
   player's data (MOHA `Config\` and `Saved\`, and `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and `MOHAVR.freehand.bin`) only
   through the harness's backup and restore. Verify every hook's prologue bytes. Addresses go only in `addresses.hpp`.
2. **Don't ask the player anything and don't wait for answers.** What only the headset can judge goes into
   HEADSET-TESTS round 36 as a question. If the player does reply mid-goal (round answers, new reports), record the
   reply in HEADSET-TESTS, add the fixes to this checklist and carry on.
3. **Every behaviour gets an ini switch.** Rule 7 and §5.2, resolved:
   - **A gun's manual reload:** its `[ManualReload]` line ships on once the simulator has proven its full cycle (D22's
     precedent). Each gun can be turned off on its own.
   - **The landing fix:** ships on once proven, because it removes a fault. Record that in DECISIONS.
   - **Anything else that changes comfort** (fades, a flat view, moving the view): ships off.
4. **Research time box: about 2 h per item.** Then write up what's known, mark the item **BLOCKED (reason)** and go to
   the next. Don't repeat an approach that has failed 3 times.
5. **Test runs:**
   - one game launch at a time;
   - copy the game and host logs to `logs/modlogs` before any relaunch;
   - `tools/deploy.ps1 undeploy` after each run;
   - never leave the game running;
   - after a crash: keep the logs, run `tools/harness.ps1 restore`, note it, and carry on with another item.
6. **Commit per item:** the code together with STATUS, ENGINE-NOTES, DECISIONS and this file. The message ends with the
   Co-Authored-By trailer.
7. **Edits:** make them with the file tools, or with Python scripts in the scratchpad. Bash heredocs mangle backslashes
   here. Run `deploy.ps1` with `&` in PowerShell so every `-Set` applies.

## Proving it in the simulator
1. **Deploy the test configuration:** `& .\tools\deploy.ps1 deploy -Set 'Render.ResX=0','Render.ResY=0',"OpenXR.RuntimeJson=$PWD\tools\OpenXR-Simulator\bin\openxr_simulator.json",'Debug.GameCommands=1','Weapon.ManualReload=1','Debug.ReloadTrace=1'`.
2. **Launch:** `tools/harness.ps1 launch`, then `to-gameplay`. The save resumes mid-parachute.
3. **Get the gun:** write lines to `%TEMP%\MOHAVR\game_cmd.txt`: `EnableCheats`, `GiveWeapon MOHAGameNonNative.<Class>`, then
   `NextWeapon` until the log line `trace -- Attachment_X is in state` names the gun. The classes are MOHAM1Garand,
   MOHAK98, MOHASpringfield, MOHAM12CombatShotgun, MOHAMauser, MOHAPanzerschreck and MOHAM18RecoillessRifle.
4. **Drive the hands:** `python tools/pad_cmd.py --seq ...`, with `hand=l,@mag|@pouch|@bolt,...`, raw presses and
   `reload=...`. Add new spots and events there as needed.
5. **Record:** `harness.ps1 shot`, and read the logs.
6. **Finish the run:** `harness.ps1 quit`, then `deploy.ps1 undeploy`.

The round 33–35 test scripts are templates: `work/research/tests/`.

The research behind Step 1:
- `RELOAD-DESIGN.md`;
- `work/research/reload/`, which is gitignored and local:
  - `reload_logic.md`: §7 item 10, "Later steps", covers these guns;
  - `skeletons.md` and `anims.md`: bones and motion;
  - `nonnative/*.uc`: the weapon classes;
  - `psa/` and `psk/`: the exports that `tools/reload_grips.py` bakes grips from;
- the decompressed weapon packages: `work/research/dec/`.

## Track A: manual reload
Done, headset rounds 30–35 passed: Thompson, MP40, StG44, BAR, G43, Colt 45, and the C96 from upgrade 1. Every new
gun gets the same things:
- a switch;
- the game's own reload blocked while the manual one is engaged;
- empty-gun visuals;
- sounds from the game's cues;
- grips from the game's reload animation;
- a pouch for new ammo.

Items, in D21's order:
- [ ] **A0 Inventory.** From the scripts and `upgrades.txt`, confirm every gun the player can reach: loadouts,
  pick-ups, mission-only launchers, and upgrade levels that change the reload. Add any that are missing here.
- [ ] **A1 M1 Garand, en-bloc clip.**
  - The game's last shot pings and throws the empty clip. Keep that.
  - The op-rod locks back: `Bolt` Z 13.87 → 1.00, with `bolt_sheath` following.
  - A clip from the pouch is pushed down (+Y) into the top of the receiver. The `clip` bone's idle position is
    (0,-5.68,9.00).
  - Seating the clip closes the bolt on its own (no rack) and loads min(8, reserve).
  - The game allows a reload only at 0 rounds. Default: B (the clip latch) ejects a partial clip and its rounds go back
    to the reserve, behind a switch.
- [ ] **A2 K98 and Springfield, bolt actions.**
  - After each shot the player works the bolt by hand: up, back, forward, down.
  - The trigger is blocked until that's done.
  - The game's own rechamber is turned off (`WeaponRechamberAnim='None'` per instance), and the attachment's
    `Rechamber()` ejects the case.
  - Loading with the bolt open: a stripper clip pushed down, and/or single rounds, whichever the game's animations show.
  - Also check the scope and upgrade variants.
- [ ] **A3 M12 shotgun.**
  - The pump is worked by hand after each shot, with the trigger blocked until then.
  - Shells go in one at a time from the pouch into the loading port, up to the tube's maximum.
- [ ] **A4 C96 below upgrade 1.** A fixed magazine charged from the top with a stripper clip (check its reload
  animation).
- [ ] **A5 Panzerschreck and M18 recoilless.** A rocket or shell from the pouch into the tube's rear or the breech,
  loaded only when empty.
- [ ] **A6 Anything A0 found.** The rifle grenade (alt fire) keeps the game's own reload unless everything else is done.

**Done for a gun** means a scripted simulator run shows all of these:
- the full cycle: empty → open/out → new ammo in → action worked → it fires;
- the clip and reserve counts right in the log, with no rounds lost or made;
- with the switch off, the game's reload works again;
- the visuals and grips right in screenshots;
- its sound cues in the log.

After any change to shared code, re-check the Step 1 guns with a quick scripted reload of each.

## Track B: the parachute landing
The player's report: "currently you clip into the ground when landing".

What's known:
- The save lands about 15 s after Continue, a "Botched Landing" (ENGINE-NOTES §1 notes).
- The landing roll is a camera animation, which the player chose to keep in 3D.
- Jumps and falls are covered in §5ai: `Camera.JumpLift=0`, `Weapon.JumpArms=idle`.
- The eye is the game's view location plus the tracked head, scaled by the world scale.

Items:
- [ ] **B1 Reproduce and measure.** Log every frame, from 2 s before to 5 s after touchdown:
  - the eye's height above the ground, from a downward trace from the view;
  - the camera animation's offset;
  - the pawn's eye height and state.

  Take screenshots at the lowest point. Vary the landing: flared (Space), not flared, and other head heights through the
  openxr-simulator MCP. Find which term puts the view below the surface.
- [ ] **B2 Fix it behind a switch.** Candidates: keep the view a margin above the floor, drop the animation's downward
  translation, or stop the tracked height from stacking on it. Proven when:
  - the eye stays at least 10 cm above the ground through every measured landing;
  - walking, crouching, jumping and the regression cycle are unchanged.

## Finish
- [ ] **F1** After the last change: `tools/harness.ps1 cycle` OK, and a final scripted reload of every converted gun.
- [ ] **F2** HEADSET-TESTS round 36: what changed, how to try it, and [H] questions per gun and for the landing.
- [ ] **F3** STATUS updated, everything committed, and the game not running. Then run `tools/deploy.ps1 undeploy`,
  then `& .\tools\deploy.ps1 deploy` with no `-Set` (the shipped defaults, for the player), and say so in STATUS.
