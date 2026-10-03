# The wrist HUD (D55) -- what was built

The player (after headset round 50): "Move the hud to the off hand wrist. Health and minimap should appear on left of wrist
when palm is flat face down, weapon and grenade info on the right. Hud should be adjustable, position and size. Option
between on wrist and on screen."

Research, its adversarial check and the W0 spike: `work/research/wristhud/design.md` (and its helper scripts). Shipped:
`[HUD] Place=wrist` (proven in the simulator; the menu's HUD tab switches it live, the player's choice saved).

**"The minimap" is the radar compass** (`hud_compass` with its squad and objective beads). MOHA's `MOHAHUDMiniMap` is
declared but never created in single player, so there is no other map to show. The menu and the ini say so.

## 1. How it works

### 1.1 The game (src/mohavr/hudtex.cpp, vr_view.cpp, bridge.cpp)
With the HUD on the wrist, the game's HUD pass is drawn **once, into a render target of the mod's own** (approach F of the
design), not into the eye images:
- At the HUD loop's start (`kHudLoopStart`) the stereo Draw's player count is cut to 1: only eye 0 runs its HUD.
- Eye 0's canvas becomes the texture: `[HUD] WristCanvas` x 9/16 (1280x720) at (0,0) (`OnHudView`); its matrix
  (`OnHudMatrix`) is scaled by backbuffer / texture so canvas pixel p lands on texture pixel p (the batches carry the
  backbuffer's size). A matrix that isn't Draw's identity-plus-translation falls back to the screen panel for the session.
- Between the matrix push and the closing flush, every `FCanvas::Flush` with a pending batch records it in an SPSC ring
  (game thread); `FlushCommand::Execute` on the render thread finds its batch there and draws it with the HUD texture bound
  (no depth surface, scissor off), the old target / depth / viewport / scissor restored after.
- **Alpha:** the game's canvas blend writes no alpha (separate alpha ZERO / ONE). A `SetRenderState` filter (vtable slot 57,
  active only inside redirected batches) forces SRCBLENDALPHA ONE / DESTBLENDALPHA INVSRCALPHA: the texture is
  premultiplied RGBA (colour never above alpha, measured). A device Reset restores the slot; it is re-checked every Present.
  Without the filter (slot 57 outside any module, or the swap failing) the HUD is not redirected: it stays on the screen
  panel (a texture with no coverage would composite as an additive glow).
- After the closing flush the HUD elements' rectangles are read (reflection, every pass): `hud_health`, `hud_compass`
  (padded by a rim bead's radius, `NPCCompassBeadSize`/2 x rs), `hud_stanceIcon`, `hud_ammoCountBg`, the two counts
  (left-justified boxes at their positions), the weapon and grenade icons (bounded by the largest tiles, 96 x 256 and
  64 x 120 x rs, right / bottom at the exp bars' corners and 27 px further left: the exp bar's Pos / Size are the current
  icon's tile, measured), the level badges (`ExperienceLevelBg`, with their `bRender`) and the kill medals (the message
  queue's icon locations, 60 x rs). The live `resolutionScale` and the player's death (no pawn, Health <= 0) go with them.
- The bridge copies the texture with each frame into a second shared ring (shared block v28, `hudTexHandles`) in the same
  command list as the eye image, and writes `slotHud[slot]` (drawn, the rectangles, rs, and the hand poses the frame's
  arms were drawn with). After each Present the texture is cleared (`ColorFill`): a frame without a HUD pass publishes an
  empty one (flags 0) and the host hides its quads -- no stale HUD in menus, the pause menu, cinematics or death.
- `[HUD] Place=screen`, flat (cinema) frames and the pause menu run exactly as before (the head-locked per-eye panel); its
  distance, width and height are now live from the host (`hudScreen`, the menu's Screen HUD page).

### 1.2 The host (src/host/wristhud.cpp, menu.cpp, main.cpp)
- `WristHud::TakeFrame` copies the slot's HUD texture with the eye image (under the same fence), with the slot's
  `slotHud` copied beside `slotMeta` / `slotScope` before the host acks the frame (a paced game may reuse the slot after).
- **Two wrist panels** on the off hand (the opposite of the hand holding the gun -- the live gun hand, so a cross-draw from
  a holster moves them to the other wrist; the menu's Gun hand until the hands run; left-hand mode: the right wrist), quad layers
  in LOCAL, placed every XR frame from the pose the frame's arm was drawn with (`[HUD] WristFollow=drawn`; the panels stay
  on the drawn sleeve) -- else the controller's:
  - the wrist: the aim pose + `WristCentre` (13 cm back along the forearm, 3 cm out from the back of the hand);
  - **forearm** layout (default; the forearm across the chest, palm down): the panels along the forearm, the **left
    panel (health, compass, stance) on the player's left** (right-handed, the left wrist: toward the elbow; left-handed,
    the right wrist: toward the hand), the right one (weapon, grenades, ammo, the exp bars, the badges and medals) on the
    right; **across** layout (the arm pointing forward): either side of the wrist;
  - the menu's offsets are in the wrist's own frame whatever the layout: along the arm (+ toward the hand), across it (+
    the controller's -Y), out from the back of the hand;
  - facing up from the back of the hand, tilted 15 deg toward the eyes; `WristGap` 1 cm between them;
  - each panel's crop is the union of its elements' live rectangles (the right one grows with a level badge, its core
    staying put); its size is the crop at `WristScale` 0.30 mm per HUD pixel x the panel's size %.
- **The gate** (`WristShow=look`): shown when the panels face the eyes (within `WristAngle` 55 deg) and the head looks at
  them (within `WristLook` 40 deg), 10 deg of hysteresis, fading in over 0.12 s and out over 0.25 s. `always`: whenever
  the panels face the head (within `WristAngle` + 30 deg, never past edge-on: palm up or the arm hanging they would be
  seen from behind, mirrored), except that with the off hand on the foregrip they show only when looked at (no pop-up in
  the face while aiming). Hidden
  at once with no wrist pass in the frame, no view (menus, cutscenes), a game menu, the MOHAVR menu (except its Wrist
  panels page, where they always show), the off hand untracked, the player dead. Not hidden while the off hand holds the
  knife, the pistol or a grenade or works a reload (the player may want to glance).
- **The rest of the HUD** (hit indicators, the grenade warning, objectives and notifications, prompts, the stopwatch, the
  letterbox / black fade, the scope and plane meters): the same texture with the wrist elements' rectangles cleared, on
  one **head-locked** quad (VIEW space) where the screen panel is (the Screen HUD page's distance / width / height). It
  stays through a recentre (only the LOCAL panels are dropped for those few frames).
- One swapchain: an atlas (the rest's 1280x720 cell, two 512 px cells below it for the panels) composed by a small shader
  (a 1:1 crop, the fade, the backing plate `WristBacking` none / dim 0.35 / dark 0.6, the rest's masks).
- **Layers:** the rest and the two panels are counted before the markers (`15 - layerCount`): projection 1, reticles 0-2,
  the scope's lens 0-1, the wrist HUD 0-3, then the markers, the menu always last. Worst case 7 before the markers, which
  leaves them 8 (the Holsters page needs 8).

### 1.3 The menu (the fourth tab, HUD)
- **HUD** wrist / screen (live; "not available" if the host has no HUD ring), **Wrist shows** when looked at / always,
  **Wrist layout** forearm across chest / arm forward, **Wrist backing** none / dim / dark.
- **Wrist panels** page: Panel (left / right / both), Along the arm (+ toward the hand), Across the arm, Out from the arm (cm, from where it
  sits), Size (%), Tilt toward you (deg), Reset, Back. The panels always show while it is open.
- **Screen HUD** page: Distance, Size (width), Height -- the screen panel live in screen mode, the rest quad on the wrist.
- Saved in the player's ini (`[HUD] Place, WristShow, WristLayout, WristBacking, WristLeftPanel, WristRightPanel, Distance,
  Width, Down` -- only the key changed), the shipped ones the defaults. The tab is after Hands: the regression scripts'
  "right right" still reach Hands.

## 2. Measured (simulator, 1920x1080, 2026-10-03)
- W0 (the spike; rerun on the final build after the review fixes, `wristhud0.ps1 -Mode 2 -Reset -Pause`, tag `wh0-fix`,
  run log `logs/modlogs/20261003-183240-run.log`): 900 passes / 10 s, every batch redirected (0 skipped, dropped or
  overflowed); the eyes HUD-free (the spike: the HUD boxes 9.6-11.2 luma off a screen capture, 0.24-0.56 between two wrist
  runs; the rerun's captures looked at, also after a Reset: `183236-wh0-fix-r1.png`); alpha premultiplied (44,870-45,710
  px covered, 0 with colour above alpha, 0 with colour and no alpha); two device Resets survived ("SetRenderState not hooked
  any more ... hooking again", "render target ... recreated after a device reset", the texture's alpha intact after); the
  pause menu: the texture empty (0 px covered: no stale HUD).
- `resolutionScale` 1.000 at the 1280x720 canvas (bucket 5, `SCREEN_RESOLUTION_1280x720`).
- The rectangles at 1280x720 (StG44 and frag grenade): health 98..354 x 604..664, compass (padded) 88..308 x 384..604,
  stance 284..364 x 524..604; ammo bar 872..1152 x 632..664; the weapon icon box 1008..1139 x 365..629; the badges
  1092..1220 / 941..1069 x 326..646. The left crop 88..364 x 384..664 (276 x 280), the right core 872..1170 x 365..675
  (298 x 310). `check_atlas.py`: the HUD texture's covered pixels in each corner group (left 98..352 x 394..661, right
  878..1152 x 404..663) all inside their crops; nothing of them left in the rest quad. With the Springfield (a 256-tall
  icon: covered from y 370) and the level badges shown (`mohavr hud badges`, `wristhud7.ps1`): the right crop grew to
  872..1220 x 326..675 (10.4 x 10.5 cm, its core in place), every covered pixel (878..1190 x 334..663) inside it.
- The gate (final build): `wristhud1.ps1` (`wh1-fix`, host log `20261003-183406-host.log`): palm down and looked at ->
  shown (facing 24 deg, looked at 6 deg off), palm up -> hidden (facing 160), looking ahead -> hidden (51 deg off); the
  panels' order asserted: health 0.096 m toward the elbow (PASS). `-LeftHand` (`wh1L-fix`, `20261003-183520-host.log`):
  the right wrist (from the hands' live gun hand), health on the player's left and 0.096 m toward the hand (PASS).
  `-Across` (`wh1A-fix`): shown (facing 1 deg, 26 off), palm up hidden (153), looking ahead hidden (76 off).
  `wristhud5.ps1` (`wh5b-fix`): `always` -> shown looking ahead ("always, facing you", facing 76 deg), palm up -> hidden
  ("always, but not facing you", facing 160), the foregrip held -> hidden ("the foregrip held, not looked at"), shown when
  let go. `wristhud6.ps1` (`wh6-fix`): `Suicide` -> hidden (no HUD pass; "the player is dead or has no pawn"), shown
  again after the checkpoint.
- The menu (`wristhud3.ps1`, `wh3-fix`, host log `20261003-184016-host.log`): forearm: the left panel +2 cm along ->
  +0.020 m toward the hand ("panels moved"), 150 % (12.4 x 12.6 cm), tilt 0; the Screen HUD page live (2.50 m away, 2.00 m
  wide, 0.00 m down); the 'across' layout with 'Along the arm' 0 -> +2 cm: moved 0.0200 m, all of it toward the hand
  (ACROSS-ALONG PASS: the offsets are in the wrist's frame in either layout); the resets; the player's ini restored
  identical. `-Place screen` (`wh3s-fix`): "panel 2.00 m at 2.50 m, 0.00 m down".
- Wrist <-> screen x10 (`wristhud2.ps1`, `wh2-fix`, `20261003-183813-*`): 10 menu toggles, the eyes carry the HUD only
  on screen; 1,589 -> 1,555 MB virtual; 0 dropped, skipped or overflowed. A hit indicator and an objective on the rest quad
  (`wh2-fix-hit-host_wrist-ongreen.png`); ToggleShowHUD and the pause menu hide the panels and back. `wristhud7.ps1`
  (`wh7-fix`): the Springfield's 256-tall icon and the level badges inside the right crop (872..1220 x 326..675).
- Regressions with the wrist shipped (final build): scope3 (`wh-scope3-fix`: the Springfield looked through, the eyes and
  the scope column HUD-free, `184846-wh-scope3-fix-through.png`), melee6 (`wh-melee6-fix`: B 200 -> 163, D -> 126, R / P
  nothing, T and S 200), melee9 (`wh-melee9-fix`: S nothing, G a hit, T 200, O / J nothing), knife1 (`wh-knife1-fix`), a
  harness cycle with the shipped defaults (OK in 33 s, `wh-cycle3-*.log`).
- Frame time (`wristhud4.ps1`, final build, paced at 90 Hz, the panels shown): each Draw waited 7.75 / 7.90 / 7.85 ms for
  the headset on the wrist (`wh4w-fix`; 0 of 2,702 XR frames late), 7.60 / 7.88 / 8.05 / 8.03 ms on the screen (`wh4s-fix`;
  one 56-64 ms hiccup); XR frames 11.11 ms; the render thread's redirect 5 us a pass, the HUD's own draws 17 us a pass
  (once, not once per eye); virtual memory 1,573 MB (screen 1,574).
- (The first round of these tests ran on an earlier host build, 17:26:55, before the last edits; everything above was rerun
  on the build with the review fixes, 2026-10-03 18:30.)

## 3. Readability (Quest 3 class, ~25 px/deg)
At `WristScale` 0.30 mm per HUD pixel (size 100 %): the ammo and grenade digits (16 HUD px tall) are 4.8 mm; the health
pills 15 x 32 px are 4.5 x 9.6 mm; the panels 8.3 x 8.4 cm (left) and 8.9 x 9.3 cm (right; 10.4 x 10.5 cm with a badge).
At 40 cm (a glance at a watch) a digit subtends 0.69 deg = **17 display px**; at 50 cm 0.55 deg = 14 px; at 30 cm 0.92 deg =
23 px. The texture then has 23 px per degree at 40 cm: about 1:1 with the display (neither magnified soft nor
minified to aliasing). Bigger: the Wrist panels page's Size (150 % = 7.2 mm digits, 26 px at 40 cm, slightly magnified).

## 4. Unproven / open
- [H] Everything a headset judges: the layouts' left / right (the player's "left of wrist"), the size, the gate (during
  reloads, foregrip holds, throws), the backing, the panels drawn over the gun or the arm (quads have no depth), the
  drawn-pose following under fast arm motion (HEADSET-TESTS round 52).
- The rest quad's elements are drawn at 1280/1560 of the screen panel's virtual size in the headset: about 22 % larger than
  in screen mode at the same Width ([H]).
- The airdrop compass (render mode 2) on the wrist, the plane / scope meters on the rest quad, the subtitles (drawn after
  the HUD loop over the whole backbuffer: unchanged), a level other than the harness's Var_Flk_P.
- The desktop mirror shows no HUD in wrist mode (it is the eye image).
- A hit indicator from the side (yaw 16384) was not caught in a capture (one from ahead was); objectives were.
- The crops' bounds for the stick grenade (120 tall) and the StG44 (96 wide) are from the tiles (the design's check), not
  captured; the BAR, Thompson, M12, M18 and Panzerschreck icons likewise.

## 5. Tests (work/research/tests)
- `wristhud0.ps1` the W0 measurements; `wristhud1.ps1` [-LeftHand] [-Across] palm down + looked at / palm up / looking away
  (forearm: the panels' order along the arm asserted, ORDER);
  `wristhud2.ps1` a hit indicator and an objective on the rest quad, ToggleShowHUD, the pause menu, wrist <-> screen x10;
  `wristhud3.ps1` [-Place screen] the menu's pages (the across layout's 'Along the arm' asserted along the arm,
  ACROSS-ALONG); `wristhud4.ps1 -Place screen|wrist` frame time; `wristhud5.ps1` always (+ palm up) + the foregrip; `wristhud6.ps1` death; `wristhud7.ps1` a 256-tall rifle icon and the level badges.
- Tools (work/research/wristhud): `host_capture.py` (the host's frame, HUD texture and atlas), `check_atlas.py` (the crops
  against the texture's coverage, the rest's leftovers, premultiplication), `zoom.py`.
- Test commands (`Debug.GameCommands`): `mohavr hud hit <yaw>`, `mohavr hud objective`, `mohavr hud badges`,
  `mohavr hud status`.
