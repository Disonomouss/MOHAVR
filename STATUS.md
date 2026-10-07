# Status

_Last updated: 2026-10-01 (GOAL.md done: every reachable gun reloads by hand, the landing fixed; HEADSET-TESTS round 39
prepared; **the shipped defaults are deployed** for the player -- `tools/deploy.ps1 deploy`, no `-Set`)_

## Where things stand
**Update, end of 2026-09-25:** M0, M1 and M2 are done. The game's image reaches the headset through
the 64-bit host (D10), with the headset round passed. The older paragraphs below are kept for
history.

**M0 and M1 are done.** The mod exists: an x86 `dinput8.dll` proxy that verifies the exe build,
logs, and hooks `Direct3DCreate9` through the IAT, standing down cleanly on any mismatch. The
harness deploys it, drives the game to gameplay, and waits on the mod's log. The mod is currently
**not deployed**; the game folder matches its baseline. Next: **M2** (PLAN.md), starting with the
address-space budget for D3D9On12.

**The EA app's copy is supported** (2026-10-07, D58, ENGINE-NOTES 1b): the same build as Steam's under EA's OOA wrapper.
The build check knows both; on the EA copy the IAT hooks wait for the game's entry point. Harness cycles pass on it
(`tools/gamedir.txt` now points at it: Steam's copy is uninstalled from this PC). The setup program finds, installs
into and uninstalls from the EA copy. Open: a headset session on the EA copy [H].
**GOAL.md (2026-10-08, running):** crouch, comfort, compatibility, the known issues, mounted guns, a mission sweep.
Done: A1 physical crouch (D61, shipped off), A2 the vignette (D62, shipped off), A3 seated (D63, shipped off), B1 the capabilities log (D64), B2 the RGBA swapchain fallback (D65). Questions for the headset collect in HEADSET-TESTS round 54.
**0.8.2** (published 2026-10-07): the EA copy (D58) and the Reverb G2's controllers bound directly (D59). The tester's G2
worked. **0.8.3** (published 2026-10-07): the wrist as a menu button (D60: wrist HUD up + hold X = the MOHAVR
menu, tap = pause), for runtimes that keep the menu button (SteamVR on the G2); HEADSET-TESTS round 53 for the tester.
The player's own install in the EA copy has no mod files (test deploys): they reinstall with the 0.8.3 setup.
**Release 0.8.1** (2026-10-05, D57): 0.8.0 crashed in the first mission (out of 2 GB of address space in VR; not new in
0.8.0). The installers now offer the 4 GB flag in MOHA.exe (on by default, reversible), proven in the simulator.
**Release 0.8.0** (2026-10-05, D56): `MOHAVR-0.8.0-Setup.exe` (a setup program, Inno Setup) and the zip, on the private
GitHub repo (github.com/Disonomouss/MOHAVR, releases). Packages: `tools\package.ps1`.

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
2. **HEADSET-TESTS round 29** (deployed): movement where you look (`Controls.MoveDirection=head`, D20, ENGINE-NOTES
   5al) and the crash recorder (`Debug.CrashDump`) for the pause-menu crash (twice in the gun-fit sessions, 5ak).
   **Gun fits done** for the 14 loadout weapons and shipped as per-weapon defaults (a shipped [GunFit]; the M18 and
   Panzerschreck keep the global default; mission-only weapons later). **Manual reload** (D21, RELOAD-DESIGN.md): Step 0 research
   done and verified; **M0 done** (the probe, ENGINE-NOTES 5am: offsets, mesh space, scale 1, the hidden parts, the folded
   native; the other guns reachable with GiveWeapon + NextWeapon); **M1 done** (the hook, the ammo rules, shared block v14's
   events: 5am); **M2 done** (the empty-gun visuals: action holds, the magazine hidden when out, the top round); **M3 done**
   (the host's magazine: B/Y drops it, the off hand grabs and pulls it out, the belt pouch gives a new one, it goes in at
   the well; the menu's Manual reload toggle; the pouch on the Holsters page: 5am); **M4 done** (the rack: a pull past 85 %
   and let go, or a tug on an action held back); **M5 done** (Thompson drum, MP40, C96: 5am); **M6 done** (the left hand, the
   fallbacks; `[Weapon] ManualReload=1` now ships on, D22); **M7 done** (the reload sounds: each gun's own cues through
   ProcessEvent -> PlaySoundAt, `[ManualReload] Sounds=1`; 5am); **twin magazines done** (BAR, StG44: pull the pair out,
   flip it with the off hand's trigger, insert the other half; each half keeps its rounds; 5am). **HEADSET-TESTS round 30
   answered** (all good; asked for: the hand point, ring sizes, a visible flip, a falling magazine -- done, 5am -- and the
   reload animations' hand poses on the magazine, slide and bolt -- done: the reload grips, D23, 5am). Round 31 answered; **round 32 deployed** (grips adjustable per gun, the menu in tabs,
   the BAR flip in the hand, the Thompson drum grab, the MP40 trigger grab, the Colt trigger release, LT unmapped: D24).
   Round 32 answered (movement where you look: good); round 33 answered (grips adjusted, the drum fixed); round 34
   answered (the twist after a flip fixed; "grab like held" was the wrong way round); **round 35 passed** (still
   deployed: "hold like grab" per gun; the free hand's hold from the rifles for every weapon, kept between sessions: the
   BAR's wrist, D25). Step 1 (box magazines) is complete; the player tunes the grips per gun once all gun functionality
   is done. **GOAL.md** (the player's /goal, 2026-09-30) drives the work now: the manual reload for every remaining gun
   (the Garand's clip, the bolt actions, the shotgun, the C96 below upgrade 1, the launchers) and the parachute
   landing. **Landing done** (B1-B2): the botched landing's roll took the eye to 17.8 cm above the feet, into the
   ground in VR; `[Camera] MinEyeHeight=60` holds it at 60 cm (D26, ENGINE-NOTES 5an). **A0 inventory done** (13 guns +
   the mounted MG42); **A1 the Garand done** (the en-bloc clip: the game's flight, the mod's ping, a seat that closes,
   the latch; D27, ENGINE-NOTES 5ao). **A5 the Panzerschreck done** (a rocket slides in at the rear: D28, 5ap); the M18
   next with the bolt actions' turning parts. **A2 the bolt actions done** (the K98 and Springfield: the bolt worked by
   the off hand after every shot, the case, HoldOpen, stripper clips and single rounds: D29, 5aq; shared block v18).
   **A3 the M12 done** (the foregrip is the pump; shells one at a time through the port: D30, 5ar; v19). **A4 the C96
   below upgrade 1 done** (a line per upgrade range; the stripper clip into the fixed magazine: D31, 5as). **A5 the M18
   done** (the breech by its knob: unlock, swing open -- the case falls -- the round slid in, shut, lock: D32, 5at). **The
   mounted MG42 keeps the game's belt reload** (D33: blocked -- no VR handling of mounted guns yet, no simulator path).
   **A6 the Step 1 guns' lower levels checked** (the Thompson's stick, the MP40's taped pair, the single StG44 and BAR,
   the G43's 10-round magazine: 5au). **"Give all weapons"** in the menu's Weapons tab (the player's request,
   2026-10-01; D34): the game's own cheats give every gun in any mission ([S] `logs/modlogs/giveall1-*`). **Round 37**
   (round 36's mid-round report): after the give, Y steps through every gun (the game's own switch only cycles the
   slots); a button held as the menu closes no longer reaches the game; the Colt's trigger release no longer fires; the
   Colt's slide takes the C96's bolt grip. The grenade pickup is parked (the player). **Round 38** (the player: "all of
   the reloads for weapons are functional"; D35, ENGINE-NOTES 5aw): the M12's foregrip pumps only with the off hand's
   trigger, the MP40's foregrip hand takes the magazine with it; the pistols' hand closes on the magazine; the bolt
   actions' knob grip; the gun fit's aim line to +-200 cm and a foregrip right / left line. **Round 39** (round 38's
   report: the M12, MP40, C96, Springfield and M18 good): the Colt holds its magazine the C96's way, the Springfield its knob
   the K98's way (D35 addendum); a launcher's rocket starts on the aim line (`Aim.LauncherFromGun`, D36, ENGINE-NOTES
   5ax) -- the player's Panzerschreck fit needs a reset. **Round 40** (the parachute): the landing's camera animation left
   out (`Camera.SteadyLanding`: the standing eye and the controller's heading through activities 41-42), and the
   parachute's harness no longer carried by the gun hand (only a WeaponAttachment goes in the hand; D37, ENGINE-NOTES
   5ay). The Panzerschreck is parked (the player: "we will come back to this"). **Round 41** (round 40's report):
   no-gun first-person parts drawn in true 3D (the parachute's harness was double; the landing's body re-based onto the
   held view); the Panzerschreck's tube, which the game turns 12.9 deg in, drawn along the controller (`[BarrelDir]`,
   D38, ENGINE-NOTES 5az) and the player's aim line set onto it. **Round 42** (round 41 passed): once landed, the
   parachuting body hidden (`Weapon.LandingBody=0`, the pawn's RenderBody, its material kept; D39, ENGINE-NOTES 5ba).
   **The off-hand grenade ("dual wield", the player's request 2026-10-02): researched and proven feasible** -- a research
   workflow wrote `OFFHAND-DESIGN.md`, and its two spikes pass in the simulator: the holstered grenade launches from the off
   hand through its own `SpawnProjectile` while the gun stays in hand (owner, instigator, fuse, cooked damage, reserve and
   HUD count right; the gun untouched), and a clone of the grenade's first-person mesh is drawn in the off hand (ENGINE-NOTES
   5bb). Test commands only (`mohavr nade ...`); nothing changes in play. Next, if the player wants it: OFFHAND-DESIGN
   section 10, Phase 1 (the host's grab / pin / throw and the shared block v20) behind `[OffHand] Grenade=0`.
   **Off-hand grenade Phase 1 done** (D40, ENGINE-NOTES 5bc): with `[OffHand] Grenade=1` (the menu: Weapons -> Off-hand
   grenade) the off hand takes a grenade at the grenade holster, its trigger pulls the pin, a second squeeze cooks it, and
   letting go throws it with the hand's speed while the gun stays in the other hand (shared block v20). Proven in the
   simulator (nade3-nade10) and through two adversarial reviews. **HEADSET-TESTS round 43 deployed** with it switched on;
   next: Phase 2 (the fingers' grip). Fixed alongside (ENGINE-NOTES 5bd): with Windows up 24.9-49.7 days the
   weapon-in-hand check and the desktop mirror's window finder never ran (a tick deadline starting at 0).
   **The off-hand pistol (the player, 2026-10-02: "Is it possible to build similar system for using the pistol with the
   off hand?"): researched and proven feasible** -- a research workflow wrote `OFFPISTOL-DESIGN.md`, and its spike S1
   passes in the simulator: the holstered Colt fires from the off hand while the BAR stays in hand (the game's own trace,
   damage and death, the kill recorded for the Colt, its report playing; the BAR untouched), through the layer below the
   game's fire states (ENGINE-NOTES 5be). Test commands only (`mohavr pistol ...`); nothing changes in play.
   **Spike S2 passes** (ENGINE-NOTES 5bf): the Colt or the C96 is drawn in the off hand with the hand closed on its grip
   (standing, walking, beside a Colt in the gun hand, and in left-hand mode), through a carrier now shared with the
   grenade.
   The player's choices:
   - a click draws the pistol and another puts it back;
   - it refills in its holster;
   - a second dot;
   - a chest holster, with a menu page to choose what each holster holds;
   - two pistols.

   ("It is unbalanced but it is fun so I don't mind.")
   **Off-hand pistol Phase 1 done** (D41, ENGINE-NOTES 5bg):
   - with `[OffHand] Pistol=1` (the menu: Weapons -> Off-hand pistol), a click at the pistol holster draws it into the
     off
     hand and a click at any holster puts it back;
   - its trigger fires it through the layer below the game's fire states, 0 cm from its own second red dot;
   - it refills in the holster, and the switch weapon never takes it (shared block v21).

   Proven in the simulator (pistol8, pistol9) and through an adversarial review. **HEADSET-TESTS rounds 42, 43 and 44
   passed** (2026-10-02). Round 43 brought requests: a click-to-take option for the grenade, the trigger pin and cook
   for
   the gun hand's grenades with a grip throw, and the grenade held as the main hand holds each type.
   **The chest holster and the per-holster menu done** (D42): `[Holsters] Chest=SwitchPistol`, and the Holsters page's
   Holds chooses what each holster draws (saved for the player).
   **The off-hand grenade held as the gun hand holds each type, and a click hold** (D43; round 43's requests).
   **The gun hand's grenades by pin, cook and grip** (D44, shipped on).
   **Two pistols** (D45, shipped on): with a pistol in the gun hand, the off hand draws its twin.
   **HEADSET-TESTS round 45 passed.** **The pouch reload** (D46, shipped on).
   **The off-hand pistol's slide and muzzle flash** (D47, shipped on). **HEADSET-TESTS round 46 passed.**
   **The off-hand pistol's brass and kick** (D48, shipped on, proven in the simulator).
   **Physical melee** (D49, shipped on, proven in the simulator; MELEE-DESIGN.md): the drawn gun's butt (a pistol's grip,
   the M12's bayonet at level 2) swung into an enemy does the game's own melee. Two code reviews' findings fixed (11 in all:
   the bolt guns, a fired M12 or an emptied Garand counted as reloading; a glance or a tracking step could arm; the levers
   from the raise; props narrowed).
   **HEADSET-TESTS rounds 47 and 48 passed** ("Melee is excellent").
   **Scopes you raise to your eye** (D50, shipped on, proven in the simulator; SCOPE-DESIGN.md): with both hands on a scoped
   gun and an eye at the eyepiece, that eye looks through it -- a third view the game renders, shown in the drawn eyepiece
   (realistic or the game's zoom, in the menu). **HEADSET-TESTS round 49 passed** ("It feels really good").
   Crouch on the right stick click (round 50; the game's button melee on the down flick).
   **The off-hand knife** (D51, shipped on, proven in the simulator; OFFKNIFE-DESIGN.md): once the MP40's Dagger is earned,
   the off hand draws it from a lower-back holster and stabs or slashes with it (150; physical melee's second channel).
   **HEADSET-TESTS round 50 passed** (crouch on the click good; the knife worked: 5 hits on soldiers). Its follow-ups (D52):
   the knife always available and its hold adjustable (Knife grip page, shared block v26), the button melee unmapped, each
   holster's ring on its own. **The M18's scope** (D53, shipped on, proven in the simulator): its Telescope M86C measured in
   the body mesh, 2.8x or the game's zoom, the game's ring sight. **The rack eject** (D54, shipped on, proven in the
   simulator; ENGINE-NOTES 5bq): a full stroke of a loaded closed-bolt action (the Colt, C96, StG44, G43, Garand), a bolt
   drawn back on a live round (K98, Springfield) or the M12 pumped on a live shell throws the round out of the drawn port,
   seen as a live round (a gun mesh's round bone on a carrier, falling to the feet), and spends it (the game's
   ConsumeAmmo); the menu's "Rack ejects a round" and "Ejected round lost / kept" (shared block v27, no layout change).
   **HEADSET-TESTS round 51** (the rack eject) is prepared.
   **The wrist HUD** (D55, shipped on: `[HUD] Place=wrist`, proven in the simulator; WRISTHUD-DESIGN.md, ENGINE-NOTES 5br):
   the game's HUD pass drawn once into a texture of the mod's own (the eyes HUD-free), shared with each frame (shared block
   v28); health + the compass (the game's "minimap") + stance on the off wrist's left panel, weapon and grenade info on the
   right, shown when the palm-down wrist is looked at (or always), the rest (hits, objectives, prompts) head-locked in
   front; the menu's HUD tab (wrist / screen live, shows, layout, backing, the Wrist panels and Screen HUD pages).
   A code review's fixes (all rerun on the final build, WRISTHUD-DESIGN 2): the host copies `slotHud` before the ack;
   no redirect without the alpha filter; the panels follow the live gun hand (a cross-draw); `always` only while the
   panels face the head; the menu's offsets in the wrist's frame in both layouts (+ along = toward the hand); the
   head-locked rest kept through a recentre; the Screen HUD page writes only the key changed; "health toward the elbow"
   corrected for left-hand mode (health is on the player's left: toward the hand there).
   **HEADSET-TESTS round 52** (the wrist HUD) is prepared.
   Next: the StG44's scope attached by hand.
   **HEADSET-TESTS round 28 passed** (still deployed): the left hand's brass thrown mirrored (`Weapon.BrassMirror=1`: a negative
   Scale3D.Y on the brass component, D19, ENGINE-NOTES 5aj). Round 27: the flash at the barrel, left-hand mode, jumps and
   falls, idle controllers and the right hand's brass passed; the left hand's brass failed (thrown the right-hand way,
   across the gun). Round 27 as deployed: the muzzle flash at the drawn barrel, both hands (`Weapon.MuzzleFlash=barrel`,
   D18; round 26's "doesn't render" was wrong -- the flame shows for one frame, the game's too; `Debug.MuzzleFreeze`
   pauses the world after a shot to see it, ENGINE-NOTES 5aj). Round 26: the player asked to move the flash, its other
   questions carry over; the game log was lost (a simulator deploy left in place ran the player's next two launches;
   `deploy.ps1` now keeps the logs it finds). Round 26 as deployed: left-hand mode fixed (the shoulders not flipped in
   the mirror world; the grip
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
- **The pause-menu crash (twice on 2026-09-30):** inside D3D9On12's copy for a DrawIndexedPrimitive (a vertex range
  larger than its buffer), no mod code on the stack, not reproduced in the simulator (ENGINE-NOTES 5ak). Debug.CrashDump records the next.
- **The host crashes on shutdown** after every Virtual Desktop session (after its clean exit; harmless so far).
- **A rare startup hang:** twice in many simulator launches (2026-09-25; 2026-09-30 00:36) the game sat on a black
  screen: it ran (120 fps; a pawn and HUD within 3 s) and CalcSceneView ran (mono views), but the viewport Draw never
  did, so no stereo and every frame black; a relaunch was fine. Cause unknown (a guess: a startup movie that never ends,
  during which UE3 skips the viewport Draw). Logs `logs/modlogs/r26-frz3-l2-hang-*`. If the player meets it: relaunch.
- **The desktop window under 9On12 is white.** Solved by the host mirror (`Bridge.Mirror=1`/`2`).
- **Engine-side stereo** in this 2007 UE3 branch is unknown, so the effort for M4 is unknown
  until researched.
- **Input:** DirectInput 8 may not see SendInput, so harness input could need the mod's own
  injection earlier than planned.

## Blocked
Nothing.
