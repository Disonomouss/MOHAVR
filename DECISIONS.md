# Decisions

Each entry records why we went this way and not another. A **Proposed** decision is only
recorded here until evidence confirms or rejects it; then mark it **Decided** or **Rejected**,
with the reason and the date.

---

### D1. The mod targets the original, SteamStub-wrapped exe — Decided 2026-09-25
We never ship or rely on an unwrapped exe. Shipping one would mean distributing and replacing a
game binary, which breaks the rule "never modify game files" and Steam's verify-files step.
Because the stub doesn't encrypt code (ENGINE-NOTES §3), hooks at fixed VAs work in the wrapped
exe as well. The unwrapped copy in `work/` is for analysis and debugging only.

### D2. Everything that loads into the game is x86 — Decided 2026-09-25
MOHA.exe is 32-bit (ENGINE-NOTES §2). That covers the mod DLL, the hooking library and the
OpenXR loader, all built for `x86-windows` (vcpkg is at `C:\dev\vcpkg`). The OpenXR runtime for
tests is the x86 simulator build.

### D3. Test against a per-process OpenXR runtime, not the machine's — Decided 2026-09-25
`tools/run-with-openxr-sim.ps1` sets `XR_RUNTIME_JSON` for one process and picks the x86 or x64
build from the exe's PE header. The machine-wide runtime (Virtual Desktop, which also registers
a 32-bit manifest) stays untouched, and the real headset uses it.

### D4. Addresses are hard-coded, with a build check — Decided 2026-09-25
The exe has no ASLR and loads at `0x10900000` (ENGINE-NOTES §2), so fixed VAs are simpler and
more reliable than pattern scans. They are safe only because the mod checks the exe's SHA-256
(or size plus timestamp) at start-up, stands down on a mismatch, and verifies each hook's
prologue bytes.

### D5. Load through a `dinput8.dll` proxy in the game's `Binaries` folder — Decided 2026-09-25
*Confirmed in M1:* the proxy loads under the wrapped exe. Its `DllMain` runs on the main thread
at about 1 ms, and the game's first `Direct3DCreate9` call arrives about 730 ms later, through
our IAT hook. `tools/deploy.ps1` adds only MOHAVR's own files and restores the folder to its
baseline.

*Original reasoning:*
It adds a file rather than modifying one. MOHA imports `dinput8` (ENGINE-NOTES §5), so the
Windows loader maps the proxy before the SteamStub entry and before `WinMain`. That is early
enough to hook `Direct3DCreate9` and change the device parameters, which lessons §2 requires.
Alternatives: a `d3d9.dll` proxy (conflicts with D5a below), or an ASI loader.
To confirm: the proxy loads under the wrapped exe and its init runs before the first
`Direct3DCreate9` call.

### D5a. D3D9 → OpenXR through D3D9On12, with zero copies — Proposed
This follows the path proven in lessons §2: create the device with `Direct3DCreate9On12`, copy
each eye into a shared D3D12 texture, and signal a shared fence that the XR thread's D3D11 device
consumes. There is no D3D10 path to worry about (ENGINE-NOTES §5). Keep a system-memory readback
fallback. Risk: the 2 GB address-space limit (the exe is not large-address-aware). Measure how
much address space D3D9On12 and OpenXR use early.

### D7. The harness runs the game windowed from the command line — Decided 2026-09-25
It passes `-windowed ResX=… ResY=…` (and `-log`) rather than setting `StartupFullscreen` in an
ini, so the player's ini is never edited (standing rule 6). Windowed mode also gives an
IMMEDIATE presentation interval, so no vsync (ENGINE-NOTES §5b). Confirmed 2026-09-25: a
1920×1080 windowed client area, and nothing persisted to the user folder.

### D9. The harness detects progress from the window and screenshots, then from the mod's log — Decided 2026-09-25
Lessons §1 says to detect progress from log lines. MOHA's shipping build writes no log, even with
`-log` (ENGINE-NOTES §5c). Until M1 gives us our own log, the harness uses the process and window
state plus screenshot checks, for example the main-menu highlight (brightness 255 on the
selected item). Once the mod exists, it logs its own state transitions (menu, loading, in
gameplay), and the harness waits on those, as lessons §1 intends.

### D8. The harness backs up the player's whole user folder — Decided 2026-09-25
Before every test the harness copies `...\EA Games\Medal of Honor Airborne(tm)\Config\` and
`Saved\` (the save game and profile) into the project's `logs\backup\<timestamp>\`, and restores
them afterwards. The backup goes into the project, not beside the originals, because that folder
is under OneDrive and a copy there would sync. The game rewrites its user ini files on exit, so
restoring after the run, not only before it, is what keeps the player's settings intact
(lessons §1).

### D10. Run OpenXR in a separate 64-bit host process — Decided 2026-09-25 (user's choice over patching MOHA.exe as large-address-aware)
*Built and verified the same day:* `MOHAVR-host.exe` shows the game's frames in the simulator. The
game keeps 505 MB free (largest block 312 MB), against 297/132 MB with OpenXR in-process
(ENGINE-NOTES §5f). Protocol: `src/common/shared_frame.hpp`.

*Original proposal:*
**Problem:** MOHA is 32-bit and not large-address-aware, and we may not change the exe
(standing rule 1). With D3D9On12 plus an in-process OpenXR session, gameplay leaves only 297 MB
free, with no free block over 132 MB (ENGINE-NOTES §5e). That's before eye render targets and
before heavier levels.
**Proposal:** the game process keeps only what must be there (the 9On12 hooks, the per-eye
copies into *shared* D3D12 textures, and a shared fence). A 64-bit `MOHAVR-host.exe` (started by
the mod) opens those textures and fence by NT handle and owns everything else: the OpenXR loader
and runtime, the D3D11/D3D12 device, the swapchains, the desktop mirror window (which also covers
the white-window issue), and in time controller input. Poses and settings flow back through a
small shared-memory block.
**Why:** it removes the roughly 258 MB OpenXR cost from the game's address space entirely,
isolates runtime crashes from the game, and gives the runtime a normal 64-bit process.
Cross-process shared textures and fences are standard D3D12 and cost no copies. Lessons §2's
zero-copy path is kept; only the consumer side moves.
**Costs:** a second binary, an IPC protocol, and lifecycle handling (host started, stopped, or
crashed). There is about one frame of extra latency risk if not pipelined with care.
**The in-process session stays** behind `OpenXR.Enabled` as a diagnostic, and is what proved the
plumbing.

### D6. Ghidra runs headless by default — Decided 2026-09-25
`tools/start-ghidra-headless.ps1` serves the analyzed project to the `ghidra` MCP server with no
GUI, so analysis doesn't depend on someone opening Ghidra.

### D11. Aim by bending the shot from the game's eye, not by moving its start — Decided 2026-09-27
`Aim.Mode` (M7) traces the head's or controller's ray in the world each frame and makes the player's
`GetBaseAimRotation` point from the game's (untracked) eye to that hit point (ENGINE-NOTES 5s).
**Why:** the shot start comes from script (`GetWeaponStartTraceLocation` → `GetPlayerViewPoint`), which the camera
uses too, so moving it would mean hooking the script VM or disturbing the camera. Bending from the eye needs one
native hook and lands every hit exactly where the ray points, at any distance.
**Costs:** one engine line trace per frame; a target the hand can see but the eye can't (around a corner) is hit
from the eye's side. Projectiles (grenades) still leave from the gun.

### D12. Shots start at the gun, on the red dot's ray — Decided 2026-09-29 (round 22; revises D11's cost)
The player's bullets go through the native `CalcWeaponFireNative`; a MidHook at its SingleLineCheck call (ENGINE-NOTES
5ab) moves the player's shot onto the red dot's ray from the gun (`Aim.ShotFromGun=1`), unless the eye -> gun segment is
blocked; the aim trace uses the bullets' own flags (per-poly collision).
**Why:** bending from the eye (D11) put shots elsewhere whenever the eye's line met something the gun's didn't, and the
aim trace's simple collision disagreed with the bullets' per-poly one. One ray, one start, the same flags: the dot and the
shot can't disagree (225 headset shots: all from the gun, 210 within 2 cm).
**Costs:** one more MidHook pair in the bullet path; a hand pushed through a wall falls back to the eye.

### D13. Replace the first-person sprint animation, don't compensate it — Decided 2026-09-29 (round 24)
`Weapon.SprintArms=idle` makes the first-person arms' activity node see idle (or walk) while the pawn sprints (a MidHook
in its tick, ENGINE-NOTES 5ad), instead of correcting the drawn gun each frame (round 22's `Weapon.SprintLock`).
**Why:** the correction had to guess when the animation plays (speed thresholds differ per weapon and stick; the lock
missed most sprints and jittered on flicker), fight a 20-unit, 60-90 degree swing, and still left the elbows and shoulders
animating. Removing the animation at the one node that plays it needs no detection, timers or reference pose, and also
stills the camera's sprint shake (the camera comes from the arms' Cam socket).
**Costs:** a MidHook in the anim tick (filtered to the local pawn's FPArms); the sprint looks like standing, not running.

### D14. The walk animation goes too, and the move is carried with the body — Decided 2026-09-29 (round 25)
`Weapon.WalkArms=idle` extends D13 to walking and crouch-walking; `Weapon.CatchUp=1` carries the gun's move (and the arm
IK's targets) along with the body's move since the view it was built from (ENGINE-NOTES 5ah).
**Why:** the VR view is built on the game camera, a bone of the animated arms, so every step swayed the world against
the eyes; removing the animation at its node stills it with nothing to detect (as D13). The move's one-tick lag is exact
to remove: the body's move between the view and the bake is known (the pawn's location and yaw).
**Costs:** the arms look like standing while walking (in VR the hands are the controllers anyway); CatchUp assumes the
camera and the hand frames move with the body between the view and the bake (the hands' own motion in that tick isn't
predicted). **Not chosen:** a bob-free VR base built from the pawn's state -- more general, but it splits the view from
the game camera (the gun's move and the shot start must stay on the game's) and filters deliberate moves too.

### D15. Frame pacing is the player's switch, in the menu, off by default — Decided 2026-09-29 (round 25)
The game can start each Draw on the host's per-XR-frame event: one game frame per headset frame, paired with its poses
by identity and published without waiting for the host's ack. The menu's Frame pacing turns it on and off live
(hdr->pace); the shipped `[Bridge] Pace` (0) is its default.
**Why:** uncapped, the headset showed frames at uneven world times, which pacing removes on a steady XR loop -- but the
simulator's loop isn't steady (it stalls and catches up), so it proves pacing works, not that it is smoother (standing
rule 7). A live switch lets the player compare both in one session.
**Costs:** a Draw waits up to 25 ms when the host doesn't signal (headset asleep: the game slows to ~40 fps); a frame
that takes longer than a headset frame shows a frame late, as uncapped.

### D16. Frame pacing on by default — Decided 2026-09-29 (round 26; revises D15)
The shipped `[Bridge] Pace` is 1; the menu switch stays.
**Why:** the player compared both in the headset and noticed no difference, and asked for the recommendation; the round-25
log measured it: paced, the world was 10.9 ms behind each XR frame with a spread of 0.4-1.0 ms, uncapped 14-21 ms with
1.3-3.2 ms (3-5x steadier, fresher), and the game renders 90 frames a second instead of 120-330 (the GPU is shared with
the headset's encoder).
**Costs:** as D15: with the headset asleep the game slows to ~40 fps (the waits time out).

### D17. The player's muzzle flash hidden, the brass moved — Decided 2026-09-29 (round 26)
`Weapon.MuzzleFlash=hide`, `Weapon.Brass=gun`: one hook on execActivateSystem either moves a particle component's
pending transform onto the drawn gun or suppresses its spawning (ENGINE-NOTES 5aj).
**Why:** the player asked for the flash at the barrel, else hidden. Moved there, the flash's transform and particles are
at the drawn muzzle but it doesn't render (in the simulator; the brass, world-space emitters, does) -- not resolved within
the time box. Hidden is deterministic; the brass from the drawn gun is right.
**Costs:** no muzzle flash (the muzzle light still flashes at the game's barrel); `barrel` stays as an option to retry.
**Revised by D18** (round 27): the moved flash does render at the barrel; its flame shows for one frame and the captures
missed it.

### D18. The player's muzzle flash at the drawn barrel — Decided 2026-09-30 (round 27; revises D17)
`Weapon.MuzzleFlash=barrel` is the shipped default (`hide` and `game` stay).
**Why:** the player asked for the flash at the barrel ("Move to gun barrel if possible, hide otherwise"; after round 26:
"Move the flash."). It renders there: its flame shows for about one frame, like the game's own, which round 26's captures
missed; frozen right after a shot (Debug.MuzzleFreeze) it sits at the drawn muzzle in either hand, and traced frame by
frame it lives and draws exactly like the game's (ENGINE-NOTES 5aj).
**Costs:** the muzzle light still flashes at the game's gun pose (a light, not a sprite); if the gun isn't drawn moved
(no bake in the last 250 ms) the flash stays where the game put it.

### D19. The left hand's brass mirrored by a negative scale — Decided 2026-09-30 (round 28)
`Weapon.BrassMirror=1`: with the gun drawn mirrored, the brass component gets `Scale3D.Y` negative on top of its
rotation, so its LocalToWorld is the exact mirrored frame (ENGINE-NOTES 5aj).
**Why:** the player saw the left hand's casings fly the wrong way. The templates throw casings in the socket's frame,
several of them forward as well as sideways (the Garand +200 along X), so a proper rotation that mirrors the sideways
throw would reverse the forward one; only a reflection mirrors every gun's brass exactly.
**Costs:** each casing mesh is drawn mirrored too; at 10x it renders as a solid mirror image, but where the mesh
particles' cull mode comes from wasn't found (the time box) -- if casings ever look hollow, `BrassMirror=0` restores
round 27's behaviour.

### D20. Movement follows the head by default — Decided 2026-09-30 (round 29)
`[Controls] MoveDirection=head`: the move stick's forward is where the player looks; `body` (the game's own) stays in
the menu.
**Why:** the player asked for it; the aim already comes from the controller and turning from the right stick, so the
body's heading only decided where "forward" walks.
**Costs:** looking around while walking steers the walk (the usual trade of head-directed locomotion).

### D21. Manual reload: the player's design — Decided 2026-09-30 (before Step 0)
A physical reload replaces the game's reload animation, gun by gun: box magazines first (Thompson, MP40, StG44, BAR,
G43, Colt, maybe the C96), then the Garand's clip, then the bolt actions, the shotgun and the launchers (Claude's order,
at the player's "your choice"). The player's choices: new magazines come from a pouch in the **middle of the belt**; the
magazine drops by **a button on the gun hand or by pulling it out** with the off hand; **racking is required** after an
empty magazine; rounds left in a dropped magazine go **back into reserve**; the gun **shows empty** (slide or bolt back,
chamber open) while its magazine is empty.
**Why:** the player's requests (round 29 follow-up). Guns without data keep today's gesture reload (the off hand at the
magazine sends the game's own Reload).
**Costs:** per-gun data (bones, travel, grab points) and per-gun testing; later guns need their own mechanics.
**Refined after Step 0's research (the player, same day):** "empty" follows each gun's own mechanism -- the G43, Colt and
C96 lock back; the open-bolt Thompson and MP40 show the bolt **forward** when empty (their loaded look is bolt back); the
StG44 and BAR, which have no empty pose in the art, show the **charging handle held back** as the empty cue. A closed-bolt
gun (StG44, G43, Colt, C96) **keeps one round chambered** when a magazine with rounds is dropped. The drop button is
**B on the right gun hand, Y on the left**, taken from the pad only while a converted gun is drawn. The pouch gives a
magazine **only once the gun's magazine is out**. The magazine-out, magazine-in and rack **sounds** are played by the mod.
**Twin (taped) magazines** (the StG44's and BAR's upgrade, and the MP40 at its first level): the pair is pulled out with
the off hand, **flipped with the off hand's trigger** while held (the BAR's pair turns over, the StG44's side-by-side pair
slides across, as in the game's own taped states A/B), and the other half inserted -- once, before a new pair comes from
the pouch; each half keeps its own round count, and the half in the gun decides the gun's taped state.

### D22. Manual reload, Step 1 as built: the defaults of the open choices — Decided 2026-09-30 (M6)
The box-magazine guns (Thompson, MP40, StG44, BAR, G43, Colt, C96 from upgrade 1) reload by hand; the switch
(`[Weapon] ManualReload`, the menu's "Manual reload") ships on after M0-M6 passed in the simulator. The defaults of
RELOAD-DESIGN 0.4-0.5, each a switch the player can judge in the headset: the Thompson and MP40 show the bolt forward when
empty and the StG44/BAR the charging handle back (`Empty=none` per gun turns a cue off); a closed bolt keeps its
chambered round on a drop (`KeepChambered=1`); the release button is B / Y on the gun hand and is kept from the pad while
the manual reload drives the gun (`ReleaseButton`); one magazine model per gun, so the pouch gives one only once the
gun's is out; one RACK event whose effect the game decides (a loaded closed bolt loses nothing); the clip caps at its
maximum (no +1). As built: a magazine that comes away from the gun must first leave the well (InsertRadius + 2 cm)
before it can go back in; "held back" (a tug racks it) is the empty hold only.
**Why:** RELOAD-DESIGN's verified choices; the player's D21 answers where they gave one.
**Costs:** the reload is silent until M7 (sounds); twin magazines come after M7.

### D23. The reload grips come from the game's own reload animations — Decided 2026-09-30 (round 31)
The player asked for the game's reload hand poses on the magazine, slide and bolt ("a much more polished feel for the
hand to snap on"). The off hand's drawn pose while it grabs the seated magazine, holds one, or racks is the arms' reload
animation's at the frame where its left hand is on that part (baked by `tools/reload_grips.py`): the drawn hand snaps
onto the part with the animation's fingers, and a held magazine sits in the hand as the animation holds it.
**Why:** the player's request; the animations are the game's own art, so the grips match the guns exactly.
**Costs:** baked data from the animations (regenerate if the guns change); where the animation's left hand never
touches a part (the Colt's slide and magazine grab, the C96's grab) the hand stays with the controller; a held
magazine is turned the way the animation holds it, so the player lines it up by turning the wrist.

### D24. The left trigger is the mod's, not the game's zoom — Decided 2026-09-30 (round 32, the player)
`[Controls] LT=none`: the off hand's trigger flips twin magazines and arms the MP40's magazine grab; the game's aim /
zoom (LT) is not mapped (in VR the player aims down the sights). Per-gun reload quirks the player asked for: the MP40's
magazine is grabbed only with the off hand's trigger held (`GrabTrigger`), the Colt's trigger also releases its
locked-back slide once a magazine is in (`TriggerRack`).
**Why:** the player's requests (round 31 answers).
**Costs:** no zoom on the pad (it can be mapped back in `[Controls]`).

### D25. The free hand's hold comes from the rifles and is kept between sessions — Decided 2026-09-30 (round 35)
The free off hand (and the hand holding a magazine) is drawn with one hold for every weapon: the gun hand's grip of a
rifle (`[Weapon] FreeHandFrom`: G43, Garand, K98, Springfield, M12), mirrored onto the other controller, with that
rifle's arm pose. The mod keeps it between sessions in `%LOCALAPPDATA%\MOHAVR\MOHAVR.freehand.bin` (derived data, not a
setting; the test backup covers it like the player's ini).
**Why:** the player: "off hand wrist twisted when BAR is equipped" -- the hold had followed the last long gun held
still (the BAR's is 17 deg off and open-handed, the SMGs' a fist); the rifles agree within 2 deg. Kept between sessions
so a session begun with the BAR (or a loadout with no rifle) has it too.
**Costs:** until a listed rifle has been drawn once, the last long gun stands in; a second file in the player's
folder.

### D26. The view is held above the feet (the parachute landing) — Decided 2026-09-30 (GOAL B)
`[Camera] MinEyeHeight=60`: the eye is kept at least 60 cm above the pawn's feet. The parachute's botched landing rolls
the game's camera to 18 cm above the floor; in VR the head's orientation replaces the roll's tumble, so the player
looked out level from ankle height, into the ground. Shipped **on** although it changes the view: it removes a fault
(GOAL.md rule 3 -- CLAUDE.md rule 7 and UNATTENDED-REPORT 5.2 resolved that way for fixes), and it touches nothing
above 60 cm (a crouch is 96, standing 161).
**Why:** the player's report (round 35); measured 17.8 cm before, 60.0 cm after (ENGINE-NOTES 5an).
**Costs:** the landing's dip still plays down to 60 cm (a comfort option to keep the view level through the landing is
an [H] question, round 36); a death or knock-down camera near the pawn is held at 60 cm too.

### D27. The Garand's manual reload: the game's clip flight, a closing seat, a latch that empties it — Decided 2026-09-30 (GOAL A1)
The Garand (`[ManualReload] Attachment_M1Garand`, on): the last shot keeps the game's own flying clip (the mod adds the
ping the blocked animation had), a clip seated from the pouch loads the gun and the op-rod closes on its own (a real
Garand's release, not the game's animation where the hand rides the rod), a seated clip can't be pulled out, and the
release button is the clip latch: it pops a part-used clip up out of the receiver with **all** its rounds back to the
reserve (per-gun `KeepChambered=0`: a round kept in the chamber would make the game throw a second empty clip at its
shot). The grenade launcher (level 2) keeps the game's reload.
**Why:** GOAL.md A1 (the player's goal), the research (`work/research/goal/garand.md`), and the real rifle where the game
has no opinion; each is a switch ([H] round 36: the latch, KeepChambered, the seat closing vs the rod, the pop speed).
**Costs:** a seat into a gun with a round chambered (latch off) is not modelled.

### D28. Long rounds slide in (the Panzerschreck) — Decided 2026-09-30 (GOAL A5)
A rocket from the pouch goes nose first into the Panzerschreck's rear mouth and slides home as the off hand pushes it
(`Insert=slide`), instead of Step 1's snap at the well -- the rocket is 66 cm long and sits 19 cm inside the tube, so a
snap would need the hand inside the tube. Letting go part-way loads it (it slides home). Shared block v17 carries the
length and depth.
**Why:** GOAL A5; the research (`work/research/goal/launchers.md`); the game's own animation pushes the rocket in the same way.
**Costs:** [H]: how far the player must push (the whole 85 cm, or less), and the launcher's 12.8 deg yaw in the hand.

### D29. Bolt actions are worked by the off hand after every shot — Decided 2026-10-01 (GOAL A2)
The K98 and Springfield: after each shot the trigger does nothing until the off hand has worked the bolt (up, back --
the case flies -- forward, down); the game's own rechamber is off for the gun while the manual reload drives it. An
emptied bolt stays open until a round is loaded (`HoldOpen=1`). With the bolt open, a stripper clip (from upgrade 1)
strips as it is seated and is pushed out as the bolt closes; below upgrade 1, single rounds, one per pouch trip. No game
grip exists for the off hand on the knob (the game's animations use the right hand), so the drawn hand stays with the
controller there.
**Why:** GOAL A2 (the player's goal); the off hand reloads everywhere else (Step 1); the real rifles and the game's
rechamber animation work the same four motions.
**Costs:** [H] -- reaching across to the knob (the gun-hand option is not built), the path's thresholds, the clip
stripping on seating (not a thumb push), no hand grip on the knob, the round / clip held at the aim point.

### D30. The M12's pump is its foregrip -- Decided 2026-10-01 (GOAL A3)
After every shot the M12's trigger does nothing until it is pumped: the off hand, holding the foregrip two-handed, slides
back along the gun (the case flies at 85 % of the stroke, `PumpArm`) and forward again (a shell into the chamber). The
game's own rechamber is off for the gun while the manual reload drives it. Shells go one at a time from the pouch into
the loading port (the action closed), up to 8; an emptied gun loaded again needs one pump. The drawn support hand rides
the pump. Without a foregrip ([Hands] Foregrip=0) the pump is grabbed at its own ring.
**Why:** GOAL A3; a separate pump grab would take the forend from the foregrip (reload grabs win), so the player could
not aim two-handed by the pump; the real M12 is pumped by the hand on the forend.
**Costs:** [H] -- the stroke length (11.5 cm, armed at 9.8), sideways drift turning the gun while pumping, one shell per
pouch trip, no slam fire, a pump of a loaded gun keeps its shell.

### D31. A line per upgrade range; the C96 below upgrade 1 loads by stripper clip -- Decided 2026-10-01 (GOAL A4)
A gun may have one `[ManualReload]` line per range of upgrade levels (`Key@N` with MinUpgrade / MaxUpgrade). The C96 at
levels -1..0 (a fixed 10-round magazine): fired empty the bolt stays back; a stripper clip from the pouch, seated in the
guides, strips its rounds in; a tug on the bolt and letting go closes it on them. A part-loaded C96 can't be topped up.
**Why:** GOAL A4; it is the game's own reload at that level (`mauser_gun_reload_2`), done by hand with Step 1's pieces.
**Costs:** [H] -- no separate thumb push (seating strips), no top-up, the held clip's grip (the game's thumb-on-top frame).

### D32. The M18's breech is worked by its knob -- Decided 2026-10-01 (GOAL A5)
After the shot the trigger does nothing until the breech has been reloaded: the off hand takes the handle's knob, turns
it up (unlock) and swings the breech open (the spent case falls out), a round from the pouch goes into the chamber nose
first and is pushed home, then the breech is swung shut and the knob turned down (lock). The breech stays where it is
let go; it closes empty too (`HoldOpen=0`).
**Why:** GOAL A5; it is the game's own reload (`m18_gun_reload`) by hand: the same turn-then-swing, the same cues; the
Panzerschreck's slide insert for the round.
**Costs:** [H] -- reaching behind the shoulder to the breech, the knob's arc (up and over, then back and to the right),
the round's length (43 cm to push home), no hand grip on the knob, the case thrown out automatically at the opening.

### D33. The mounted MG42 keeps the game's belt reload -- Decided 2026-10-01 (GOAL A6)
The mounted MG42 (47 nests in 6 missions; a 100-round belt, infinite belts) keeps the game's own reload, which runs by
itself when a belt is empty. A manual version (lid up, a belt, lid down, cock) is blocked: the mod has no VR handling of
a mounted gun yet (the view and the hands at a nest were never tried), and there is no simulator path (the class isn't in
the test save's level; the nests are outside the tower the save lands on).
**Why:** GOAL rule 4 (the time box); nothing is lost meanwhile (infinite belts).
**Costs:** the one gun without a manual reload; revisit once mounted guns are tried in the headset.

### D34. "Give all weapons" in the menu -- Decided 2026-10-01 (the player's request)
The menu's Weapons tab has "Give all weapons": the host sends the game `mohavr giveall`, and the game side runs the game's
own cheats on the player controller -- `EnableCheats`, `GiveWeapon MOHAGameNonNative.<Class>` for each class in `[Weapon]
GiveAllList` (the 13 guns with a reload), `GiveAmmo`. Shown with the shipped `[Weapon] GiveAllMenu=1`.
**Why:** the player wants every gun at hand to try the manual reloads; the test channel that did this (`game_cmd.txt`,
`Debug.GameCommands`) is off for the player.
**Costs:** [H] -- the guns join the inventory at the profile's upgrade levels and are reached with next weapon (the holster
spots know the normal slots only); whether the cheats leave a mark on the save or progress is unknown (the harness always
restores `Saved\`); a launcher's class outside its missions may give nothing.
**Addendum (2026-10-01, round 36's report):** the game's switch weapon (`SwitchWeapon` -> `MOHAInventoryManager.SwitchWeapon`)
cycles only the slot weapons, so the given guns were unreachable. After the give, the host keeps the Xbox B (switch weapon)
from the game and sends the engine's `NextWeapon` instead, until the game reports a new pawn.

### D35. Round 38: the foregrip's trigger, the pistols' and bolt actions' grips, the fit's limits -- Decided 2026-10-01
The player's requests after round 36 ("all of the reloads for weapons are functional"):
- **The M12's foregrip holds the gun without pumping it** (`[Weapon] PumpTrigger=1`): the pump is taken only while the
  off hand's trigger is squeezed (0.6 to take it, kept above 0.4); without it the foregrip is the normal two-handed hold.
  The player chose the off hand's trigger (asked: the gun hand's trigger fires).
- **The MP40 works alike** (`[Weapon] ForeGrabTrigger=1`): on a `GrabTrigger` gun, holding the foregrip and squeezing that
  hand's trigger takes the magazine (within 3 x the grab ring's radius: the MP40's magazine is about 15 cm below its
  foregrip point); the foregrip is let go, the hand holds the magazine.
- **The pistols' magazine grip snaps on**: the Colt's and the C96's grab grip is their own hold (the reload animation's
  insert pose; they had none, so the hand never closed on the magazine in the well), and the hold stays theirs.
- **The bolt actions' knob grip**: the K98's and Springfield's off hand on the knob is the game's right hand on it (the
  rechamber animation, frame 12) mirrored about the knob's plane; drawn only while the off hand holds the bolt
  (`reloadFlags` bit 6, no layout change), so an open bolt let go leaves the hand free.
- **The gun fit's aim line** goes to +-200 cm (was +-30; 2 cm steps past 30): the Panzerschreck's aim needed more.
- **Foregrip right / left** in the gun fit (+-40 cm; the 9th value of `[GunFit]`, mirrored in left-hand mode): the M18's
  foregrip to the left.
**Why:** the player's requests; the switches are on by default because the simulator proved them (`feat1`-`feat4`).
**Costs:** [H] -- the squeeze while holding the foregrip (a two-handed aim with the trigger held is the pump, not a hold);
the MP40's reach from the foregrip to the magazine; the pistols' grip is the animation's, not hand-made; the knob grip is
mirrored, not the game's own left hand (the game has none).
**Tried:** the MP40's hold borrowed for the pistols, moved by the difference of the grab points: a translation only, it put
the held magazine 16 cm from the hand and turned 65 deg (the magazine bones' axes differ); dropped.
**Addendum (round 38's report):** the M12, the MP40, the C96, the Springfield and the M18 passed. The Colt takes the C96's
magazine hold for both the grab and the hold, carried through the gun frame (the hand on the C96's seated magazine,
moved from its grab point to the Colt's and turned by the angle between the two MagOut directions). The knob grips: the
player had the rifles mixed up -- the pose they liked was the K98's (round 38's, unchanged), and the Springfield takes it
(moved by the difference of the Knob points; both bolt bones are the mesh's axes at rest), turned on 33 deg about the
bolt's axis through the knob for its smaller lift (60 vs 93 deg), so the lifted bolt -- the pull back -- is held as the
K98's is.

### D36. A launcher's rocket starts on the aim line -- Decided 2026-10-01 (round 38's report)
`[Aim] LauncherFromGun=1` (with `ShotFromGun`): each frame, the player's projectile weapon (fire mode 0 `EWFT_Projectile`,
not a grenade) gets `EALAWeapon.PhysicalStartFireOverride` = where the aim ray starts, unless something stands between
the eye and it. `ProjectileFire` spawns the rocket there instead of `GetPhysicalFireStartLoc` (the Panzerschreck's and
M18's `GetBarrelPosition`: the game's own first-person gun, beside the head), and its aim, from the game's trace (already
run from the gun along the ray), is the ray itself.
**Why:** the player: "the Panzerschreck's missile comes from beside the player and adjustments don't seem to take or maybe
the aim changes at distance" -- the rocket flew from beside the head towards the dot's point, crossing the aim line only
there, so no aim-line offset could fix it (the player had pushed it to 200 cm left, 52 up). The rocket flies straight
(`MOHAProj_Rocket` is PHYS_Projectile): no drop to allow for.
**Costs:** [H] -- the rocket leaves from the aim line's start (at the hand, on the tube's line once the fit is right), not
the tube's mouth; a fit with a large aim-line offset now moves the rocket's start with it (the player's Panzerschreck fit
needs "Reset this gun").

### D37. The parachute: the landing's camera left out, the harness not in the hand -- Decided 2026-10-01
`[Camera] SteadyLanding=1`: through the parachute landing (CurrentActivity 41 CONTROLLED_LANDING, the roll; 42
REMOVE_GEAR, getting up) the view is held at the pawn's standing eye (Location + BaseEyeHeight) facing the controller's
yaw -- at once on the touchdown frame, eased back to the game's camera over 0.25 s when the landing ends (where the two
meet: 160.0 against 161.0 cm). The head still turns and moves the view. And only a weapon's attachment (a
`WeaponAttachment`: every gun, grenade and the demo charge) is put in the gun hand: while parachuting the newest
first-person part was the `MOHAParachuteActor`'s harness, which the hand carried with the arms; now the game draws them.
**Why:** the player (after round 39): "While the camera no longer goes underground, it still freaks out in the same way.
The chest of the character is held like a gun in the right hand and moves with it." MinEyeHeight (D26) only floored the
roll at 60 cm; the roll still dropped the view 1 m and turned it.
**Costs:** [H] -- none of the landing's motion is seen (a comfort choice; `SteadyLanding=0` gives the game's, floored);
the arms' gear-removal animation plays in front of a still view; while parachuting the arms and harness are where the
game puts them, not following the controllers.
**Addendum (round 40's report: "now that the chest is not held like a weapon it is back to the double vision effect";
"the body visibly contorts around you"):** first-person parts with no gun in the hand were left to the game's own
transform -- the flat-screen view-model FOV applied per eye, seen double. They are now drawn in true 3D with no hands
(`viewmodel::DrawWithoutHands`: the arm IK and the reload stand down): while parachuting where the game puts them; during
the held landing re-based from the game's tumbling camera onto the held view (d = inverse(game camera) * held, level),
so the body stays in front of you as the flat game shows it instead of rolling around the still view.

### D38. A barrel the game points off the view is turned onto the controller ([BarrelDir]) -- Decided 2026-10-01
The game's own pose points every gun's barrel (mesh +Z) exactly along the camera's forward -- except the Panzerschreck,
whose tube on the right shoulder is turned 12.9 deg left and 3.8 deg up (towards the flat screen's crosshair). A gun in
`[BarrelDir]` (its barrel direction in the camera frame) is drawn turned about the fit's grip point so the barrel runs
along the controller, parallel to the aim line. The player's Panzerschreck aim line set to where the turned tube's axis
lies for their grip: up 7.5, right -0.5 (from up 46, right -128; backed up).
**Why:** the player: "the further away I aim the more to the right the red dot goes. Missile fires from left side of the
gun now." -- the aim line ran along the controller and the tube 13 deg off it, so no offset could put the line on the tube
at every distance (and the rocket, from the line's start, left beside the tube).
**Costs:** [H] -- the tube sits 13 deg differently in the hand than before (turned about the grip); a measured constant,
not live (an animation that turns the tube still turns it).

### D39. Once landed, the parachuting body is hidden -- Decided 2026-10-01 (round 41's report)
`[Weapon] LandingBody=0`: while the landing's view is held (D37), the first-person body the game shows for the airdrop
(MOHAPlayerController's AirDropLanding / AirDropLanded states call `RenderBody(true)`: FPArms material 1 -- legs, torso,
gear) is hidden again with the pawn's own `RenderBody 0`, as in play; the arms stay. `RenderBody(false)` keeps the
material it hides in `BodyMatInst`, so the body's own material (read after the first call) is put back after the landing
(and for 10 s after, as the game's own EndState call can come later) -- otherwise the next airdrop or briefing would
show no body.
**Why:** the player: "Once landed, could the player body appear instead of the parachuting body. It moves around and
looks strange."
**Costs:** [H] -- the arms still play the landing's brace and gear removal for ~3 s before the gun comes up;
`LandingBody=1` gives the game's body back.

### D40. The off-hand grenade, Phase 1: take, pin, cook and throw with the gun in hand -- Decided 2026-10-02
`[OffHand] Grenade` (shipped 0; the menu's Weapons tab "Off-hand grenade" is the player's, saved in their ini): with a
gun in the gun hand, the off hand's grip at a grenade holster (`[Holsters]` LeftHip=SwitchGrenade, or a
SwitchFragGrenade / SwitchGammon / SwitchStick spot) takes a grenade into that hand (TAKE, the pin in; drawn there,
`Carrier=1`); its trigger pulls the pin (PIN); a second squeeze lets the spoon go (COOK: the fuse burns -- the game
keeps the clock in game time, so the game's pause stops it -- and the controller ticks; held to the end it goes off in
the hand, the game's own rule); letting go of the grip throws it with the hand's speed (x `ThrowScale` 2.2, at most
`MaxHandSpeed` 12 m/s; under `MinThrowSpeed` 1 m/s it is tossed the game's gentlest way along the view,
`SlowRelease=toss`), or puts it back while the pin is in (PUT BACK: nothing used). `Pin=auto` pulls the pin at the take;
`Cook=pin` starts the fuse at the pin, `off` at the throw. A grenade is counted out only when it flies. The host owns
the interaction (src/host/offhand.cpp: none / held / armed / cooking) and sends ordered events through shared block v20;
the game (src/mohavr/offhand.cpp) executes them through the grenade weapon's own `SpawnProjectile` (ENGINE-NOTES 5bb)
and publishes the counts, whether a take can happen, whether one held may stay, and its own state (the host takes the
game's after two disagreeing polls).
- An off-hand press at a grenade holster is the off-hand grenade's whenever it is switched on, the game's side is alive
  and a gun is in the other hand: a take, or a refusal (a 60 ms pulse) when a take can't happen now -- the landing, a
  weapon switch, a cinematic, weapons disabled, a mounted gun, the HellBox, none left. Never the holster's own draw
  then, which would put the gun in the off hand (nade3: a press during the landing did). Switched off, with the game's
  side quiet or no gun in the other hand, the holster draws as before.
- While one is held: the off trigger is kept from the game (a pin pull isn't the aim), and Xbox RB (the game's own
  grenade switch: the controller's X) too -- each until let go, also after the hold; a trigger squeezed at all (above
  0.15) when the hold starts or a freeze ends must be let go first (a fist closing on the grab, a finger trailing it);
  the gun hand at a grenade holster is refused, at a gun holster it switches guns (the hold stays; a switch to a grenade
  or the HellBox ends it); an off-grip press after a freeze is the grenade's; the foregrip's ring and pulse and the
  reload's grab rings are hidden; the menu's Gun hand waits.
- Frozen while a menu is open or the off hand's grip action sleeps (a sleeping controller, the dashboard); let go
  meanwhile, it goes back with the pin in or the spoon on, and is tossed once cooking. Opening the MOHAVR menu (it
  doesn't pause the game) tosses a cooking grenade -- not over the game's own pause menu, which stops the fuse. Let go
  while the hand isn't tracked (a fast overhand wind-up), it flies with the last tracked velocity from the last tracked
  point (none after 0.25 s: tossed).
- The game making it unavailable while held (a ladder, a cinematic, death) puts it back, or tosses a cooking one -- a
  live grenade is never silently removed (a weapon destroyed with its inventory isn't touched: the hold just ends). The
  toss goes along the view's heading, pitched from the horizon to 45 degrees (looking down at the hand doesn't aim it at
  the feet) plus the weapon's DirectionOffset; its first 2 m are traced (through triggers, as bullets go): blocked, 30
  degrees higher; blocked again, the hand's own velocity (a slow release's; a forced toss has none and drops where the
  hand is).
- The game's side runs no script calls while the switch is off and nothing is held (rule 7). The grenade is drawn in the
  hand only with the arm bake (`[Weapon] ArmIK=1`, `ViewModel=2`), which places it.
**Why:** the player (2026-10-02): "Being able to grab a grenade with the off hand and throw it without unequipping your
gun would be very immersive." `OFFHAND-DESIGN.md` (the research and the spikes S1, S2) says how; this is its Phase 1,
with S2's carrier so the grenade is seen in the hand. An adversarial review (four reviewers, a skeptic each, a
completeness critic: 23 of 25 findings confirmed) and a second pass on its fixes (5 more confirmed) shaped the rules
above.
**Costs:** [H] -- the hand stays open around the grenade (the fingers' grip: Phase 2); no pin or spoon sounds (Phase 4);
`Throw=trigger`, `Estimator=peak`, `Spot=chest` not yet (OFFHAND-DESIGN 8, 10). `Carrier` ships on (the design had it
off until Phase 3): S2 and nade5 show it in the simulator. The game cooks every throw of its own from the pin pull; here
the fuse starts at the spoon (`Cook=pin` for the game's way).

### D41. The off-hand pistol, Phase 1: draw, fire and holster with the gun in hand -- Decided 2026-10-02
`[OffHand] Pistol` (shipped 0; the menu's Weapons tab "Off-hand pistol" is the player's, saved in their ini). With a
weapon in the gun hand, the off hand's grip at a pistol holster (a holster whose command is SwitchPistol: the shipped
RightHip) draws the holstered pistol into that hand (DRAW); `PistolHold=toggle` (shipped; the player chose "click")
keeps it out without the grip, and a squeeze at any holster puts it back (HOLSTER); `grip` holds it while the grip is
held. Its trigger fires one round per pull (SHOT, with the off line of that moment; the game drops a pull sooner than
the pistol's own RefireCheckTime, in game time), the C96 at its 712 level fires while the trigger stays held, an empty
pistol clicks. Back in the holster it refills after the pistol's own ReloadInterval[0] in game time
(`PistolRefill=game`; Colt 1.5 s, C96 1.75 s at level 2); drawn sooner, it keeps its count (`instant`, `off`). A second
red dot sits on its line (`PistolDot=1`, with the menu's Red dot). The pistol is the inventory manager's PistolWeapon,
or the other pistol of a Colt + C96 pair when one of them is in the gun hand. The host owns the interaction
(src/host/offpistol.cpp: none / held) and sends ordered events through shared block v21 (the SHOT carries the off line
at the pull); the game (src/mohavr/offpistol.cpp) executes them through the layer below the game's fire states
(ENGINE-NOTES 5be), draws the pistol in the off hand (5bf), and publishes the pistol a draw gets, whether a draw can
happen, whether one held may stay, its clip and its state (the host takes the game's after two disagreeing polls).
- The shot: the bullets' own native trace from the off line's start (past anything it starts inside; from the eye at the
  line's aim point when something stands between the eye and the gun: D12's rule), ProcessInstantHit per impact with
  Pawn.Weapon the pistol for the damage (its kills and experience: `PistolCredit=pistol`), the stimuli, the stats, a
  direct AmmoCount write, the report at the muzzle. The gun in hand, its state, PendingFire, FlashCount and ammo are
  never written. At the draw the save's upgrade level is applied if the pistol is behind (`PistolUpgrades=1`: a C96
  given at level -1 fires as the save's level 2), and the shared magnum mix is put right for the weapon in hand.
- An off-hand press at a pistol holster is the off-hand pistol's whenever it is switched on, the game's side is alive
  and the gun hand is tracked: a draw, or a refusal (a 60 ms pulse) when a draw can't happen now -- the landing, a
  weapon switch, a cinematic, weapons disabled, a mounted gun, the HellBox, a grenade in the off hand, the only pistol
  in the gun hand. Never the holster's own SwitchPistol then (nade3's rule).
- While it is out: the off trigger is kept from the game (until let go); the off grip's presses are the pistol's (a
  click at a holster puts it back, elsewhere nothing); the foregrip and the reload's spots are out of reach and their
  rings and pulses hidden; the gun hand at a pistol holster is refused; the menu's Gun hand waits; the game's switch
  weapon (Xbox B: the controller's Y), when it would take the held pistol (from the secondary; from the primary with no
  secondary; from a grenade back to it), is turned into SwitchPrimary (`PistolKeep=1`) -- the long gun changes, the
  pistol stays. X (the game's grenade switch) works: a grenade in the gun hand, the pistol kept.
- Frozen (no shots) while a menu is open, the off hand's grip action sleeps or the off hand isn't really tracked (a shot
  from a held pose would aim wrong); a pull already under way when it is drawn or a freeze ends must be let go first.
- The game ends the hold when the pistol is gone (a death), when a switch takes it to the gun hand, or when it won't let
  it stay for two frames (put back, with its refill). A new pawn drops everything. Switched off with nothing held or
  due, the game's side makes no script calls (rule 7).
**Why:** the player (2026-10-02): "Is it possible to build similar system for using the pistol with the off hand?", then
"1. Both 2. Add chest holster, add option in menu to decide what is in each holster. 3. Click 4. Yes and yes 5. It is
unbalanced but it is fun so I don't mind." `OFFPISTOL-DESIGN.md` (the research and the spikes S1, S2) says how; this is
its Phase 1 with the player's choices (click, refill, a second dot). The review's rules (ENGINE-NOTES 5bg) are part of
it. [H] round 44 passed.
**Costs:** [H] -- no slide, hammer, flash, brass or kick yet (Phase 2); the C96's buttstock and box magazine follow its
upgrade level only as far as the bone names go (a capture of the main C96 at each level is still to compare); two
pistols (the same pistol twinned) and the chest holster with the per-holster menu are the next steps. The long gun is
one-handed while it is out (the physical trade every dual-wield game makes).

### D42. A chest holster, and what each holster holds is the player's -- Decided 2026-10-02
A fifth holster spot, `Chest` (shipped `[Holsters] Chest=SwitchPistol`, `ChestSpot=0 -34 10 12`: cm from the head, right,
up, forward, radius), the off-hand pistol's cross-draw. The menu's Holsters page gains **Holds** for the selected holster:
primary, secondary, pistol, grenade, frag grenade, Gammon bomb, stick grenade, reload or nothing, saved in the player's ini
(`[Holsters] <Name>` = the game command, or none) on top of the shipped one; Reset this spot puts back both the place and
the contents. An empty holster's ring shows only on that page, so it can be placed. The off-hand pistol and grenade
follow the contents (a pistol holster draws the off-hand pistol, a grenade holster the off-hand grenade).
- Only a draw (a Switch command) makes the pressing hand the gun hand: a holster may run Reload.
- The off hand at the foregrip and a holster at once (the chest and a long gun at low ready): the closer centre wins.
- A command the player wrote into the ini and the menu doesn't list is shown as it is; left goes to "nothing", right to
  "primary".
**Why:** the player (2026-10-02): "Add chest holster, add option in menu to decide what is in each holster." A review
(one reviewer, a skeptic: 3 of 3 confirmed) set the three rules above.

### D43. The off-hand grenade held as the gun hand holds it, and a click hold -- Decided 2026-10-02 (round 43)
- **Where and how it is held.** The grenade in the off hand is placed and posed as the gun hand holds that type, mirrored.
  Each type's mesh pose at the gun hand's idle (viewmodel's camera-frame log) and the type's fit (the player's
  `[GunFit]`, as the gun hand uses) give its place on the off controller: `carrier::MirroredHold`, the pistol's rule
  generalised. The hand closes on it with the game's own grip from each type's idle (grenademkiia / gammongrenade /
  stickgrenade_idle frame 0), mirrored by tools/reload_grips.py into the rows `offnade`. While one is drawn the arms take
  the free hand, whatever FreeOffHand says.
- **`[OffHand] GrenadeHold`** (shipped `grip`; the menu's Weapons tab "Grenade hold", the player's):
  - `grip` is as before;
  - `click`: a click at a grenade holster takes it and it stays in the hand. The trigger pulls the pin and a second pull
    cooks. Squeeze the grip, swing and let go to throw (a slow release is lobbed). A click at any holster puts it back
    while the pin is in.
**Why:** the player (round 43): "add option for grenade to either be held with grip or triggered with grip and put away
with another grip to the holster", "The grenade appears above left hand, can position and pose match main hand grenades
for each type." The throw by squeeze-and-release was their choice.

### D44. The gun hand's grenades by pin, cook and grip -- Decided 2026-10-02 (round 43)
`[Weapon] GrenadePin=1` (shipped on, proven in the simulator; the menu's Weapons tab "Hand grenades", the player's). A
grenade in the gun hand works as the off hand's does:
- its trigger pulls the pin and a second pull cooks;
- squeeze the gun hand's grip, swing and let go to throw it with the hand's speed (a slow release is lobbed);
- the game's own throw never starts (that trigger is kept from the game while a grenade is in the gun hand).

What happens if it leaves the hand:

| It leaves the hand... | Result |
|---|---|
| with the pin in | nothing |
| armed (pin out, not cooking) | goes back unused |
| cooking | tossed |
| held to the end of the fuse | goes off in the hand |

After the last one the gun hand switches to the last gun, as the game's own throw does.

The host runs a second grenade state machine on the gun hand (OffHandGrenade's main mode: no take, the hold comes with the
weapon in hand); its events carry kNadeMain (shared block v22, no layout change), and the game launches from the weapon in
hand through the same SpawnProjectile path.
**Why:** the player (round 43): "Arming the grenade with trigger is good, add that to main hand grenades", "[cooking] Works add
to main hand grenades"; the grip throw was their choice. The review of D43 also set: a click or gun-hand throw squeeze under
way ends with a freeze (the armed grenade stays; cooking keeps its toss), and the foregrip-versus-holster rule only while the
foregrip can be taken.

### D45. Two pistols: the gun hand's own pistol twinned -- Decided 2026-10-02
`[OffHand] PistolPair=1` (shipped on, proven in the simulator). With a pistol in the gun hand and no other one carried, the
off hand at a pistol holster draws its twin. It is the same object as the gun hand's pistol, so nothing new enters the
inventory, the saves or the weapon cycle.
- The twin's rounds are a count the mod keeps; the object's AmmoCount stays the gun hand's (its HUD, its reload). The twin
  starts full and refills in the holster after the reload time.
- It fires through the same path: no credit swap is needed (it is already the weapon in hand), and the pistol's fire mode
  is put back after each shot.
- It goes back when the gun hand puts its pistol away.
- With a Colt and a C96, the off hand still draws the other pistol (D41).
**Why:** the player (2026-10-02), asked "Rifle plus a pistol in the off hand, or two pistols as well?": "Both".

**The reviews of D44 and D45** (5 and 2 of 3 confirmed). D44 (the gun hand's grenade):
- its hold no longer counts as "a grenade in the off hand": the off-hand pistol stays out;
- the last grenade's switch never goes to the off hand's pistol;
- a switch of grenade type while the pin is out puts it back (cooking: tossed), and a COOK on a grenade no longer in hand
  is refused;
- the game publishes whether the gun hand's grenade may stay (nadeCaps bit5: a ladder, a mounted gun, a cinematic put it
  back or toss it);
- no gun-hand change while it is live;
- known: the trigger mask can lag a fresh grenade by up to 250 ms (the weapon key's check).

D45 (the twin): the fire mode is put back on a failed trace too, and any weapon switch of the gun hand ends the twin at
once. The off-hand grenade and pistol ship on (rounds 43 and 44 passed).

### D46. The pouch reload: a hand holding a gun grips the ammo pouch -- Decided 2026-10-02
`[Hands] PouchReload=1` (shipped on, proven in the simulator; the menu's Weapons tab "Pouch reload", the player's). A grip at
the ammo pouch (`[Holsters] MagPouchSpot`, the middle of the belt) reloads at once, with no animation:
- by the gun hand with a gun (not a grenade), the gun's clip is topped up from its reserve (owed rounds first) by a direct
  write, the low-ammo mix follows, and a converted gun plays its magazine-in cue. The manual reload sees its clip rise
  without it (magazine in, ready: also after a dropped magazine, a bolt or a pump);
- by the off hand holding the pistol (or its twin), the pistol is refilled and stays in the hand.

The pouch ring shows near a hand while a gun is in the gun hand. A press there comes before every other use of that hand's
grip. Round 45's answers: the chest spot is right; the click hold is the player's choice (`GrenadeHold=click` now ships);
hand grenades and the grenade pose pass; the off-hand pistol lacks its flash and slide.
**Why:** the player (2026-10-02): "when a hand with a gun equipped grips the ammo holster, it automatically reloads. No
animation, just an instant reload." -- "Make it a toggle option in menu."

### D47. The off-hand pistol's slide and muzzle flash -- Decided 2026-10-02 (round 45)
`[OffHand] PistolSlide=1` and `PistolFlash=1` (both shipped on, proven in the simulator).
- **The slide:** the Colt's `gunSlide` (the C96's `Bolt`) on the drawn clone moves back on each shot: back in 20 ms, held
  40 ms, forward over 70 ms. The distance is the one the game's own fire animations use. It stays back while the held
  pistol (or its twin) is empty, and goes forward when it is refilled (holster or pouch). It is a bone fix in the carrier's
  bake, like the C96's clip.
- **The flash:** the pistol's own first-person flash template (`1911a_muzzleflash_FP`, `c96_muzzleflash_FP`) fires at the
  drawn muzzle along the off line on each shot. Its component is minted by the game's own
  `SmallArmsAttachment.InitHitspangPSC` on the gun hand's attachment, with an absolute transform, and is remade with the
  next attachment. muzzle.cpp's hook leaves it alone. A grenade in the gun hand gives no flash (no SmallArmsAttachment).
- `Debug.MuzzleFreeze` now covers this flash too (`muzzle::ArmFreeze`).
**Why:** round 45, answer 5: "No, just the lack of flash and slide back on the off hand pistol."

**The review** (2 of 6 confirmed):
- The flash's component is forgotten at the first Draw after the gun hand's attachment changes, which destroys it. Before
  this, a purge could recycle the address and the next off shot could write to another object.
- The flash's distance along the off line is recomputed with every placement: a fit change while the pistol is held, and
  the fit's angle.

Also fixed, though rated harmless: the slide bone is forgotten with the carrier, and a new shot starts the slide from
where it is, so the C96's full auto doesn't snap it forward.

### D48. The off-hand pistol's brass and kick -- Decided 2026-10-02 (round 46)
`[OffHand] PistolBrass=1` and `PistolKick=1` (both shipped on, proven in the simulator).
- **The kick** is the game's own pistol fire animation replayed: the arms' `colt45_fire` / `mauser_fire` move the gun mesh
  (RightProp) against the camera, which is exactly the kick the gun hand's drawn pistol shows (viewmodel moves the whole
  camera frame onto the controller).
  - `tools/pistol_kick.py` bakes it per frame, mirrored into the left hand as the hold is, into `pistol_kick.inc`: the
    Colt flips 21.8 deg, the one-handed C96 26 deg, both at 67 ms, and back about 6 units.
  - It moves the drawn pistol and the hand on it together; the aim line and its dot don't move.
  - It fades out from 0.30 to 0.45 s (the game blends the fire back into its idle). A shot while the gun is still
    rising leaves it; a later one rejoins the rise at the same angle, so the 712's full auto holds the gun up.
  - `PistolKick` scales it (0 none .. 1 the game's).
- **The brass:** the pistol's own `ShellEject` template (DefaultWeapon.ini's `ShellEjectParticleTemplate`) at the mesh's
  eject socket (`ShellEject` on `tag_eject`), on a second component minted like the flash's. The hold is two reflections
  (the mesh's and the controller's), so the drawn pistol is not mirrored and the casings leave its right side, as from
  the gun hand.
**The review** (4 of 5 confirmed):
- The flash is now at the drawn, kicked muzzle along the drawn barrel (it was on the un-kicked off line, off the gun on
  follow-up shots). The off line is still used when the pistol isn't drawn in the hand.
- In left-hand mode the brass, and now the flash, go through the draw mirror as the drawn pistol does (they were at its
  mirror-world spot).
- A shot that comes while the gun is still up blends the whole pose (rotation and move) into the rise. Before, it matched
  the angle alone: a 4-unit, 7-deg snap.

**Why:** round 46, answer 4: "It has no brass or recoil." Round 46's other answers: the pouch reload works with both
hands and the pouch is where they reach; the slide and the flash are good.

### D49. Physical melee: the swing of the gun does the game's melee -- Decided 2026-10-03
`[Melee] Physical=1` (shipped on, proven in the simulator; the menu's Weapons tab "Physical melee", the player's).
- **What strikes:**
  - a long gun's butt plate;
  - a pistol's grip bottom, or the C96's shoulder stock;
  - the M12's bayonet at level 2, thrust or slash.
  The strike does the game's own melee: `TakeDamage` with the weapon's melee damage type, impulse and the hit bone's
  multiplier, then the attachment's melee impact effects. The weapon never enters its melee state: no animation, no
  lockout.
- **Damage:**
  - butt and grip: the weapon's base melee damage (50: three torso hits, a head hit kills);
  - the bayonet: the upgrade's 200;
  - sprinting: the game's charge kill.
  A kill is a melee kill for the gun in hand (the stats, the XP).
- **Speed** is measured on the host's gun pose in the room and against the head's heading, through levers taken while
  the gun is quiet. A stroke counts only when it is one in both frames. Walking, turning, a glance or a duck, the game's
  kick and tracking jumps never count as a swing.
  - Butt or grip: 3 m/s with the hand at 1.5.
  - A bayonet thrust: the hand along the barrel at 2.5 m/s, 15 cm, not turning.
  - A bayonet slash: the tip at 8 m/s across the blade, the hand at 2.5.
- **Contact** is the drawn gun's strike points swept through the world with the bullets' collision. The game's eye must
  see the hand and the hand the contact.
- **Who and what takes it:**
  - allies (the game's own team rule), the dead and the invincible are left alone;
  - props take the game's damage only if meant for melee (the Flakturm's vent covers, physics props);
  - the rest of the world shows the impact effect and a short pulse, and doesn't spend the swing.
- **Rate:** one hit per swing; the weapon's re-melee interval after a hit; 0.5 s per soldier.
- **Feedback:** the host pulses the gun hand (both hands two-handed) per strike, through shared block v23 (meleeOn,
  meleeHits, meleePower, meleeKind).
- **What stays:** the right stick click stays the game's melee (the launchers have only it; the MP40's "Dagger" is a knife
  in the game's left hand, not on the gun, so it stays on the button).
- **The design and the critique:** MELEE-DESIGN.md. A three-lens critique of the first draft found 13 defects, all fixed
  before the tests: the levers, the bayonet gates, props, the world, body motion, spikes, tracking, the clocks, entry from
  inside a body, snap turns, effects after a kill, the menu's room and the pulse merge.
- **Two code reviews** (4 of 6, then 7 of 8 confirmed; all fixed; MELEE-DESIGN 5). The first:
  - the busy bit was the pouch's "magazine out", which is a bolt gun's resting state, so the Kar98k, the Springfield and
    a fired M12 never struck. It is now the manual reload working the gun (`Reload::GunHandBusy`);
  - a glance or a duck with the gun still could arm a strike. Each gate now passes in both frames;
  - the levers were taken from the raise's first frame. They now come only from a quiet gun;
  - physics props now count only with their impulse on, and destructible props don't.

  The second:
  - busy is now only the reload under way: not a clip the game threw out, an empty reserve, a hold left from the last
    gun, or a foregrip just held (`PumpTrigger=0`);
  - a draw by the other hand gets the 0.4 s hold-off;
  - a tracking step arms nothing: the newest sample alone must move at half the gate too. The jump test now covers the
    foregrip hand and the gun's turn.
**Why:** the player (2026-10-03): "In game with most weapons clicking right stick whacks enemies with the butt of the gun.
Can the motion of hitting with the butt of the gun do the melee damage? If this system can be worked out, apply it for
special cases, such as upgraded guns that have bayonets." 

### D50. Scopes you raise to your eye: a third view through the drawn eyepiece -- Decided 2026-10-03
`[Scope] Enable=1` (shipped on, proven in the simulator; the menu's Weapons tab "Scopes", the player's), `Zoom=real`
("Scope zoom": real or the game's), `TwoHands=1`. SCOPE-DESIGN.md.
- **What:** with both hands on a scoped gun (the Springfield, the G43 from level 1, the StG44 from level 2) and an eye just
  behind the eyepiece on its axis, that eye looks through the scope: a magnified view in the eyepiece, for that eye only,
  with the scope's reticle. No button, no overlay; the game's own scope state never runs.
- **How the view is made:** the stereo Draw's third player renders the scope view (its own view state, a square frustum,
  the HUD loop over the eyes only, the first-person parts shrunk) into a 512 px column taken from the eyes' width only
  while a scope is at an eye. Of the options researched (a crop of the eye image, a third view, an engine scene capture,
  a zoom of the eye's whole view) it is the only one at full resolution and in step with the eyes, on machinery the mod
  already proved; a crop is blurry and sees the scope's own tube, a scene capture has never run in this game, a whole-view
  zoom is a known sickness trigger.
- **Where:** each scope's tube from the gun meshes (tools/scope_points.py), carried by the live scope bone into the host's
  gun frame (the StG44's mount). The camera at the objective down the optical axis, zeroed to the bore (the StG44's tube
  is drawn 3.9 deg off its barrel); the reticle at the scope's zero (50 m) on the aim line.
- **The lens:** a one-eye quad on the drawn eyepiece, direction-mapped (parallax-free), with an exit-pupil shadow and the
  field stop's ring.
- **Magnification:** the real scopes' (2.5x, 4x) or the game's zoom on the turning stick.
- **Not covered:** the M18 (its sight is part of the body mesh).
- **The code review** (8 of 17 confirmed, all fixed; SCOPE-DESIGN 5): the lens a frame ahead of the drawn gun, the view's
  x truncated a pixel at some widths, a stale lens picture on raising, the render thread's column x, the geometry after
  a re-mount or a hand change, the lens mid-recentre.
**Why:** the player (2026-10-03): "I don't want scopes to be a button press that brings up an overlay. I'd like scopes to
feel natural, you bring them up to your eye and can see through them"; "Make both the game's value and realistic a toggle
in menu. Two hands needed to use scope."


### D51. The off-hand knife: the MP40's Dagger from a lower-back holster, with its own strikes -- Decided 2026-10-03
`[OffHand] Knife=1` (shipped on, proven in the simulator; the menu's Weapons tab "Off-hand knife", the player's),
`KnifeHold=toggle`, `KnifeGrip=forward`, `KnifeTilt=10`; `[Knife] Require=earned`, `Damage=150`; `[Holsters]
LowerBack=Knife`, `LowerBackSpot=0 -58 -22 16`. OFFKNIFE-DESIGN.md.
- **What:** once the Dagger is earned (the MP40's second upgrade), with a gun in the gun hand the off hand's grip at the
  lower back draws the knife; a press at any holster puts it back (or let go, with KnifeHold=grip). A stab along the blade
  or a slash across it into a soldier does the game's melee at the Dagger's 150; the off hand pulses.
- **The knife:** the class default MP40 attachment's KnifeMeshComponent (found with FindObject), cloned onto the arms as a
  third carrier the arm bake draws in the off hand -- no MP40 needs to be carried.
- **The hold, level-triggered:** the host holds whether it is held (knifeFlags in the view seqlock), the game draws or
  sheathes to match and publishes whether it can (shared block v25). The knife has no state to carry, so no event ring
  (the pistol's design, A.3, was larger than needed).
- **The strikes:** physical melee's per-hand state became a Channel; the knife is a second channel on the off hand's pose
  with the bayonet's thrust / slash estimate at a hand-held blade's speeds (stab 2 m/s, slash 4 m/s). The gun's channel is
  unchanged (its regressions identical). The knife strikes whether or not "Physical melee" is on (its own switch).
- **Holsters:** six now (the pouch moved to the 7th spot); the hand's holster is the one it is deepest in (distance over
  radius), not the list's last match -- the lower back touches the left hip.
- **The code review** (7 findings confirmed, all fixed; OFFKNIFE-DESIGN 3): a draw impossible with FreeOffHand=0, a retry
  after the host's let-go, every squeeze holding strikes off, a shared pulse slot, the test command undone.
**Why:** the player (2026-10-03): "The mp40 has an upgrade that is a knife for its melee attack, can we make the knife an
off hand equip with melee functionality? Add a holster to lower back for it."


### D52. Round 50's follow-ups: the knife always available and adjustable, no button melee, each holster's ring -- Decided 2026-10-03
- **The knife is always available:** `[Knife] Require=any` (the new default; `earned` / `carried` stay as choices).
- **The knife's hold is the player's:** the menu's Weapons tab "Knife grip" sets the grip (forward or icepick) and moves
  (cm along the controller) and turns (degrees about the handle) the knife in the hand, saved in their ini ([OffHand]
  KnifeGrip, KnifeAdj). Shared block v26 (`knifeAdj[6]` at 2976, 3000 bytes).
- **The game's button melee is unmapped** (`[Controls] RS=none`): a swing is physical melee, the knife the off hand's.
- **Each holster's ring shows or hides on its own** (the Holsters page "Ring shown", [Holsters] <Name>Ring); every ring
  still shows while that page is open, so a hidden one can be found and moved.
**Why:** the player (round 50): "Crouch is good. Unbind melee from flick stick down, no longer needed. Knife needs hand
position adjustment. Make the knife always available, not gated behind mp40 upgrade." "Holsters need to be able to be
toggled visible individually."

### D53. The M18's scope: its Telescope M86C measured in the body mesh -- Decided 2026-10-03
`[Scope] M18=1` (shipped on, proven in the simulator; the menu's Scopes and Scope zoom cover it).
- **What:** the M18 looks through like the other scoped guns (two hands, the eye at the eyepiece): its telescope on the
  tube's left, 2.8x real (the Telescope M86C) or the game's zoom (40 deg; Enhanced Scope 30..58), with the game's own ring
  sight (US_M18_Scope_HUD, measured: a fine and heavy cross and four rings) as the lens's third reticle.
- **Where:** the telescope has no bone; tools/scope_points.py measures a part of a bone (a box in mesh space, the largest
  connected piece of its triangles, a wider top ring) and the eyepiece's glass (the lens equals the drawn 2.6 cm glass).
  The tube is parallel to the bore (0.000 deg), 9.0 u off it. The other rows regenerate byte-identically.
**Why:** the player (after round 50): "The M18 needs scope functionality." Research: work/research/m18scope.

### D54. A loaded action racked throws its live round out, spent -- Decided 2026-10-03
`[ManualReload] RackEject=1` (shipped on, proven in the simulator; the menu's Weapons tab "Rack ejects a round"),
`RackEjectKeep=0` (the menu's "Ejected round  lost / kept"). Revises RELOAD-DESIGN 0.5.2 and D22's "a loaded closed bolt
loses nothing", and D30's "a live shell stays (a press check loses nothing)".
- **Which guns:** only the closed-bolt actions that really extract a live round -- the Colt, the C96 (every level), the
  StG44, the G43 and the Garand on a full stroke of the slide / handle / op-rod; the K98 and the Springfield when the bolt
  is drawn back (controlled feed: a second stroke throws the round the first one fed); the M12 on a full pump with a live
  shell. The open-bolt Thompson, MP40 and BAR (the chamber is empty at rest), the M18 (no extractor) and the Panzerschreck
  throw nothing; nor does a gun in its rifle-grenade mode (not converted then).
- **When:** a Step 1 action at its arm point (RackArm 0.85, not a tug of a locked-back one): a new host event RACK BACK
  (11), sent only while the game reports a live round chambered that a stroke would throw (`reloadState` bit16); a bolt at
  BOLT BACK (the chamber is now tracked for bolts too); the pump at PUMP BACK. A partial pull, a tug, an empty gun, a spent
  case: nothing new (a spent case / hull leaves as before, no count change).
- **The count:** the weapon's own `ConsumeAmmo(0)` through ProcessEvent (the game's spent-round path: skipped in its
  upgrade sequence), a direct AmmoCount - 1 plus the low-ammo mix if it can't be called; only while the manual reload
  blocks the game's reload, after the shot detector (never taken for a shot). Kept: the round goes back to the reserve
  (its cap's overflow carried, as every return). The Garand racked out at its last round also throws its empty clip
  (its own `EjectClip`) with the ping: the magazine is out, the op-rod held back.
- **The visible round is a live round for every gun** (the player asked to see one; the game's shell meshes are all empty
  cases): a carrier -- a clone of a class-default attachment's WeaponMeshComponent (`FindObject`, the knife's way, D51),
  its Animations cleared, attached to the arms -- of which the arm bake draws only the round bone, on a ballistic fall from
  the drawn gun's ShellEject port (out to its right, RackRoundUp of that up, a little back, plus the port's own speed;
  tumbling RackRoundSpin; flat at the feet for RackRoundRest, then gone; two in flight, the oldest reused). The sources:
  each gun's own round bone (Colt `bullet`, G43 `bullet`, K98 `bullet2`, Springfield `bullet01`, M12 `shell`); the
  Garand's .30-06 from the Springfield's; the StG44's from the G43's at 0.6 its length; the C96's from the Colt's
  (x1.05 / 0.85: the MP40's 9 mm draws dark). A class not loaded in a level: the gun's own brass instead.
- **Shared block v27** with no layout change: event 11, `reloadFlags` bit7 (on) and bit8 (kept), `reloadState` bit16.
- **The code review's fixes (2026-10-03):** a stroke ends when the off hand lets the action go (`reloadFlags` bit6), so a
  stroke the host drops without a RACK (its abort, a gun change) doesn't eat the next one's round; the magazine taken out
  while the slide is held back after RACK BACK takes every round (the chamber is empty: no KeepChambered round); in
  left-hand mode the round lives in the real world (the port taken through the draw's mirror, the round's bake undoing
  the mirror it is drawn through), so a head moved or turned while it falls or lies doesn't move it; the menu's two
  items moved after "Reload spots" (the regression scripts' step counts to the earlier Weapons items stay valid; the tab
  holds up to 24). HEADSET-TESTS round 51.
**Why:** the player (after round 50): "Sliding back the bolt/slide and pumping the shotgun should eject a round visibly and
it should count as a round spent. Only for guns in which this would be accurate for." Research and its adversarial check:
work/research/rackeject/design.md (the brass-only Phase 1 for the StG44, C96 and Garand dropped for the carrier, at the
check's and the lead's call).
**Costs:** a bolt or pump worked by habit now costs a round; the M12's action slide lock isn't modelled. Every ejected
round is a new component (garbage-collected after it is detached).

### D55. The HUD on the off hand's wrist, drawn once into a texture of the mod's own -- Decided 2026-10-03
`[HUD] Place=wrist` (shipped, proven in the simulator; the menu's new HUD tab switches wrist / screen live), `WristShow=look`,
`WristLayout=forearm`, `WristBacking=dim`, `WristScale=0.30`, `Redirect=1` (0: the hooks never installed). WRISTHUD-DESIGN.md.
- **How:** approach F of the research -- the game's HUD pass drawn once (the HUD loop cut to eye 0) into a 1280x720 render
  target of the mod's own, by binding it around each HUD batch's `FlushCommand::Execute` on the render thread (the canvas
  never binds a target; ENGINE-NOTES 5br), with premultiplied alpha forced by a `SetRenderState` filter (the game's canvas
  writes no alpha). The bridge copies it with each frame into a second shared ring (shared block v28, layout 3000 ->
  3864: `hudTexHandles`, `hudTexW/H`, `hudCaps`, `hudPlace`, `hudScreen`, `slotHud[3]`); the texture is cleared after each
  Present, so a frame without a pass shows nothing. Chosen over plan B (a permanent HUD column from the eyes' width: -13 %
  of each eye every frame, no alpha) because the W0 spike proved it inside its time box: every batch redirected, the
  eyes HUD-free, alpha, no flicker, Resets survived, frame time unchanged.
- **What goes where:** two panels on the off hand's wrist (the hand not holding the gun: the hands' live gun hand, so a
  cross-draw moves them; the menu's Gun hand until the hands run), facing up with the palm flat
  and face down -- health, the compass ("the minimap": the radar compass; the game's MiniMap never exists in single
  player) and the stance icon on the player's left; the weapon and grenade info (ammo, the icons, the exp bars, the level
  badges and kill medals while they show) on the right. Everything else (hit indicators, the grenade warning, objectives,
  notifications, prompts, the stopwatch, the letterbox / fade) stays head-locked: the same texture with the wrist
  elements cleared, on one quad where the screen panel is. The crops come from the HUD's live elements (reflection each
  pass, the live resolutionScale published; the icons bounded by the largest tiles), not from assumed layouts.
- **When:** looked at (the panels face the eyes within 55 deg and the head looks within 40 deg; fading) or always while
  they face the head (never seen from behind, mirrored; and not popping up while the off hand holds the foregrip unless
  looked at); hidden in menus, cutscenes, death, the off hand
  untracked; not hidden while it holds the knife, pistol or a grenade or works a reload. Placed from the pose the frame's
  arm was drawn with (they stay on the drawn sleeve).
- **Adjustable:** the HUD tab (HUD, Shows, Layout -- forearm across the chest / arm forward, Backing) and two pages: Wrist
  panels (which panel, along / across / out, size, tilt, reset; the panels always show while it is open) and Screen HUD
  (the screen panel's distance / size / height, now live from the host: `hudScreen`). All in the player's ini.
- **Safety:** the hooks' prologues (and the FlushCommand vtable's slot) are checked before install; a canvas matrix that
  isn't Draw's falls back to the screen panel for the session; the host publishes the wrist only once its HUD ring opened
  (else the game would draw its HUD into a texture nobody shows); no render target (a Reset) or no alpha filter (the
  `SetRenderState` slot outside a module, or the swap failing: the texture would have no coverage) -> the screen panel for
  that Draw. The host copies `slotHud` with `slotMeta` / `slotScope` before it acks the frame. Mid-recentre only the LOCAL
  panels are dropped (the head-locked rest stays). The menu writes only the key changed (the Screen HUD page included).
- **Proven** on the final build (after a code review's fixes; WRISTHUD-DESIGN 2 names the runs): W0 with two Resets and the
  pause menu, the gate both hands and both layouts (the panels' order along the arm asserted), always, the menu's pages
  (the across layout's offsets asserted along the arm), the rest quad, ten toggles, death, frame time in both places,
  scope3 / melee6 / melee9 / knife1, a harness cycle with the shipped defaults.
**Why:** the player (after round 50): "Move the hud to the off hand wrist. Health and minimap should appear on left of
wrist when palm is flat face down, weapon and grenade info on the right. Hud should be adjustable, position and size. Option
between on wrist and on screen." Research, its adversarial check and the W0 spike: work/research/wristhud/design.md.
**Costs:** one render-target bind per HUD batch (5 us a pass on the render thread) and a 3.7 MB GPU copy per frame (the
HUD's own draws now once instead of twice); ~15 MB of the game's address space (measured +1 MB virtual at gameplay); up to
three more quad layers; the panels draw over the gun or the arm in front of them (quads have no depth); the desktop mirror
shows no HUD on the wrist setting.

### D56. Release 0.8.0 with a setup program -- Decided 2026-10-05
- **What:** `dist\MOHAVR-0.8.0-Setup.exe`, an Inno Setup 6 installer (`installer\MOHAVR.iss`, built by
  `tools\package.ps1` beside the zip, which keeps the script installer). It keeps release\install.ps1's rules:
  - the game found through Steam (its uninstall entry for app 24840, then every library in libraryfolders.vdf), a folder
    without UnrealEngine3\Binaries\MOHA.exe refused;
  - a dinput8.dll that isn't MOHAVR's (no "MOHAVR-host.exe" string) never overwritten, nor removed on uninstall;
  - the game not running (install and uninstall);
  - an update whose shipped MOHAVR.ini differs keeps the old one as %LOCALAPPDATA%\MOHAVR\MOHAVR.ini.previous;
  - the player's settings never touched; the uninstaller kept out of the game's folder (ProgramData\MOHAVR\uninstall);
    the mod's logs removed with it.
  Administrator rights (a UAC prompt), the usual for Program Files; `/DTestBuild` builds a no-UAC copy for the tests.
- **The release:** version 0.8.0 (MOHAVR_VERSION), the README rewritten for today's controls and features, a GitHub
  release on the private repo with the setup and the zip.
- **Proven [S]:** work/research/tests/setup1.ps1 on fake game folders (not the game and another mod's dll refused with
  nothing written; a clean install byte-identical to the build; an update keeps the edited ini; the uninstaller leaves only
  the game's file; the player's settings unchanged), and the real game folder found by itself, installed and uninstalled
  back to tools\deploy.ps1's baseline.
**Why:** the player (2026-10-05): "Release latest version", "Build the installer for 0.8.0".

### D57. The game may use 4 GB: an optional large-address-aware flag in MOHA.exe -- Decided 2026-10-05 (the player's choice)
- **Why it's needed:** 0.8.0 crashed in the first mission on two PCs: `Present` failed with E_OUTOFMEMORY, then D3D9On12
  faulted in a copy. MOHA is 32-bit and not large address aware: 2 GB. Measured in Hus_M1_P at 2880x1620 (ENGINE-NOTES 5bs):
  - the VR path climbs from ~1.46 to ~1.75-1.86 GB within 4 minutes of the opening cutscene;
  - the unmodded game stays at ~1.4 GB;
  - the round-42 build (1 October) climbs the same way, so it was never 0.8.0's features;
  - the extra is the D3D9-on-D3D12 path: 16 private 32 MB reservations (512 MB, 176 MB committed) against the game's 5;
  - at the crash, 122 MB free, the largest block 20 MB.
- **What:** both installers offer "Let the game use up to 4 GB of memory", on by default.
  - It sets IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20 at e_lfanew + 22) in `MOHA.exe`.
  - The original exe is kept first: setup in ProgramData\MOHAVR, the zip's script in %LOCALAPPDATA%\MOHAVR.
  - Only a flag the installer set is recorded. That flag is cleared on uninstall, or when the option is unticked on an update.
  - An exe already large address aware is left alone.
  - The mod logs at start whether it has 4 GB (`memory: MOHA.exe large address aware: yes/no`).
  - Steam's "verify files" restores the original exe; running setup again sets the flag again.
- **This revises standing rule 1** (never modify game files) for this one bit, at the player's request. D10 had chosen the
  64-bit host over patching the exe; the first mission now needs both. Rule 2 holds: it is still Steam's SteamStub-wrapped
  exe that runs.
- **Proven [S]:**
  - The flagged exe starts through Steam (SteamStub accepts it) and has 4095 MB.
  - Hus_M1_P ran 7 minutes past where both PCs crashed, with 2.2 GB free.
  - With `[Debug] ReserveLow=1500` forcing the game and the mod above 2 GB:
    - Hus_M1_P ran 5 minutes at 3.36 GB;
    - a harness cycle passed;
    - the StG44 was drawn and baked and a rack threw a live round, as without it;
    - the knife was drawn.
  - work/research/tests/setup2.ps1 and ziplaa.ps1: ticked, unticked, re-ticked and uninstalled restore the exe
    byte-identically; an already flagged exe is left alone.
**Why:** the player (2026-10-05), after the crash: "Make the optional step."

### D58. The EA app's copy of the game is supported: the same build, recognised by its wrapper -- Decided 2026-10-07
- **Why it can work:** the EA app sells the same compiled MOHA.exe as Steam (ENGINE-NOTES 1b): the same TimeDateStamp,
  section layout and `.rdata`; only the DRM wrapper differs (EA's OOA, which encrypts the code on disk, for SteamStub, which
  doesn't). Once `Activation.dll` has decrypted it, every address in `addresses.hpp` holds, so there is one address set,
  not two.
- **What:**
  - The build check knows both wrappers by the entry point. It accepts the EA header fields (SizeOfImage, CheckSum, entry)
    and still requires the same timestamp and all 39 signatures. The log names the store.
  - On the EA copy the mod loads while its imports are still being resolved (in table order), so the IAT hooks
    (Direct3DCreate9, GetCommandLineW for Render.ResX/ResY, XInputGetState) wait for the game's entry point: the OEP's
    `jmp __tmainCRTStartup` goes through a stub of the mod's (verified before written). Steam's path is unchanged.
  - `render_res` finds GetCommandLineW in the game's own import table (`kGameImportDirRva`) instead of the header's,
    which the EA copy points at the DRM's table. The same table for Steam.
  - Both installers find the game through the EA app too (after Steam). The setup program already runs as administrator.
    The zip's scripts say to run them as administrator when the folder (Program Files) refuses writes.
  - The setup program no longer defaults to a remembered folder that no longer holds the game. Only that folder is
    replaced, never one given with `/DIR` or typed. (A first version replaced any folder without the game: test 1's
    deliberately wrong `/DIR` installed into the real EA copy. It was put back byte-identical: the exe's flag cleared,
    the dll removed. The player's `MOHAVR.ini.previous`, a copy of an older shipped ini, was overwritten by test 4 with no
    backup to restore.)
  - The setup program repeats the folder checks (the game is there, no other mod's `dinput8.dll`) in `PrepareToInstall`,
    which runs also when an update skips the folder page (`DisableDirPage=auto`). Its message boxes are suppressible, so
    `/SUPPRESSMSGBOXES` test runs no longer stop on them.
  - The tools: `gamedir.txt` points at the EA copy on this PC (Steam's is uninstalled). `deploy.ps1` keeps its baseline
    per game folder. `harness.ps1` starts the EA copy directly and clicks through `moha_setup.exe` (Play, OK). The EA
    app drops the harness's arguments, so harness runs deploy with `Render.ResX=1920`, `Render.ResY=1080` (the mod adds
    `-windowed ResX ResY` itself). `laa.py` uses `gamedir.py`.
- **Rule 2 extended:** the mod runs against the original wrapped exe: Steam's SteamStub or the EA app's OOA, never an
  unwrapped one.
- **Proven [S]:** harness cycles on the EA copy (launcher clicked through, main menu, campaign, gameplay, clean quit, the
  player's data restored), with every hook installed and the host's OpenXR session at 90 Hz (899 new game frames of 900).
  With the 4 GB flag set by `laa.py`: a cycle passed and the mod saw 4095 MB; the exe was byte-identical after clearing it.
  The setup program (test build): work/research/tests/setup1.ps1 passes on fake folders (refusals with nothing written, a
  clean install, an update's `.previous`, the uninstall), with the real EA copy checked untouched after each case. Run
  without `/DIR`, it found the EA copy, installed, set the 4 GB flag, and uninstalled back to the deploy baseline with the
  original exe. Not yet proven: a headset session on the EA copy [H].
**Why:** the player (2026-10-07): "This is the directory of the EA store version of the game. Can we make it compatible
with the vr mod?"

### D59. The HP Reverb G2's controllers bound directly -- Decided 2026-10-07
- **Why:** a tester plays on a Reverb G2 through SteamVR with the Oasis driver (a native SteamVR headset since Windows
  dropped WMR). SteamVR's OpenXR runtime offers `XR_EXT_hp_mixed_reality_controller` (found in vrclient_x64.dll), and its
  G2 bindings name the controller `hpmotioncontroller`, so it likely reports the HP profile. The host bound only Touch and
  Index (plus the simple controller, menu only), which left the G2 to SteamVR's remapping of the Touch bindings.
- **What:** the host enables the extension when the runtime offers it. It binds `/interaction_profiles/hp/mixed_reality_controller`
  exactly as Touch, whose layout the G2 copies (X/Y, A/B, grip, trigger, stick with click, menu): the menu's actions and
  the whole gameplay pad. It logs whether the runtime offers the profile, and which profile each hand is bound as on
  every interaction-profile change (the line to read when a controller misbehaves). The README says so, and where to
  rebind under SteamVR.
- **Proven [S]:** the simulator doesn't offer the extension. The host doesn't enable it there and still binds Touch (logged
  "bound as /interaction_profiles/oculus/touch_controller"); harness cycle OK. The HP paths follow the extension's spec.
  **[H]:** the tester's G2 session (the log's "HP Reverb G2 controller profile: yes" and "bound as .../hp/mixed_reality_controller").
- **Also, the tools:** `deploy.ps1` refuses a folder that holds the player's own install (the setup's README or its
  uninstall entry for that folder). Before this, a test deploy took the player's 0.8.2 install for its own (the dll was
  byte-identical to the build) and undeploy removed its three files. `harness.ps1` polls for the host's exit instead of
  `WaitForExit`, which threw "Access is denied" once.
**Why:** the player (2026-10-07): "A tester is using Reverb G2 with Oasis drivers (Native SteamVR headset). Will it be
compatible?" -- "Do it".

### D60. The wrist as a menu button -- Decided 2026-10-07
- **Why:** the tester's Reverb G2 (D59) works, but its left menu button doesn't reach the game: SteamVR keeps it (its
  dashboard). Without it there is neither the MOHAVR menu (hold) nor the game's pause (tap). The player: "Lay palm flat to
  bring up hud, then hold X." Index users had the same gap: the menu's toggle was never bound for Index (no menu button).
- **What:** while the wrist HUD's gate is open (`WristHud::PanelsUp`: the off hand palm down, facing and looked at, or
  "always" facing), the off hand's lower face button (X; A in left-handed mode) works as the menu button does. Held for
  `MenuHoldSeconds` it opens the MOHAVR menu; a tap is the game's Start. While the MOHAVR menu is open, either closes it.
  The gate works whichever HUD place is chosen. A press that starts with the gate open (or the menu open) is kept from the
  game until let go (`Pad::SetWristMasked`: X's grenade). A press that starts with the gate closed is the game's as before.
  `[Controls] WristMenu` (1, shipped). `pad.BeginFrame` (the frame's test state) moved ahead of the menu input, so the
  wrist button and the pad's mapping read the same state in the same frame. Before this, a test press leaked to the game
  for one frame; real controllers were never affected. `PulseStart` now also uses this frame's time.
- **Proven [S]** (simulator, the EA copy, the wrist test pose): A, wrist up, hold X 1 s -> "MOHAVR menu opened", no
  weapon change. B, menu open, tap X -> closed (X held over the menu kept from the game). C, wrist up, tap X -> "the
  game's Start", the game's pause menu. D, looking away with the hand down, tap X -> the game's grenade, the gesture not
  engaged. **[H]:** HEADSET-TESTS round 53 (the tester's G2).
- **Also:** `deploy.ps1 -PlayerAgreed` deploys over the player's own install when they've said so (they reinstall after).
**Why:** the player (2026-10-07): "It worked for them, we need another way to open the mod menu. Lay palm flat to bring up
hud, then hold X."

### D61. Physical crouch: the game's stance follows the real head -- Decided 2026-10-08 (GOAL A1)
- **Why:** crouching for real lowered only the view; the pawn stayed standing (too tall for low cover) until the stick
  click. GOAL.md A1, from the feature review (2026-10-08).
- **What:** `[Controls] PhysicalCrouch` (shipped **0**: it changes how the game plays; GOAL rule 3), `CrouchDepth`
  0.40 m, and the menu's General tab ("Physical crouch", live to the game through `hdr->crouchMode`, shared block v29,
  no layout change).
  - The game side decides once per frame. The head's drop below the origin is checked against the line, with 10 cm of
    hysteresis. LOCAL has no floor, so the line is a drop in metres, not GOAL's "share of the eye height". The
    decision needs the game in gameplay: walking, alive, no menu, no cinematic, no landing, no ladder or mounted-gun
    state.
  - The game's stance (the collision height) is the truth. A toggle is asked only when it differs, one at a time
    (`crouchReqSeq`; the host pulses Xbox X, the game's own crouch), with a retry after 1.2 s. After three failures it
    waits for the head to cross the line again.
  - A stance change nobody asked for is the stick's: the stick has the stance until the head crosses the line. Going
    down with the game already crouched adopts the crouch as the head's.
  - While the crouch is the head's, the game's own crouch drop is added back to the camera (ENGINE-NOTES 5bt), so the
    eye is the real head. Without that, the drop would count twice.
- **Proven [S]** (the EA copy, the simulator's head moved by `sim_pose.py --y`):
  - head 0.50 m down: crouched, the eye **110.7 cm** above the feet; up: stood, 160.9;
  - a quick re-crouch: 110.7 again;
  - the stick: crouch 95.9; a later head crouch adopted (110.7) and stood with the head; a stick stand while low held
    until the head rose;
  - the pause menu: no request; backing out with the head low crouched at once;
  - the menu's off: no request.
  **[H]** HEADSET-TESTS round 54: does it trigger by accident (leaning in, picking things up, looking down)?

### D62. The comfort vignette -- Decided 2026-10-08 (GOAL A2)
- **Why:** comfort for stick locomotion. GOAL.md A2, from the feature review: the roadmap's M9 vignette was never built.
- **What:** `[Comfort] Vignette` (shipped **0**, comfort: GOAL rule 3; 1 light, 2 strong) and `VignetteFade` 0.2 s; the
  menu's General tab ("Vignette", saved in the player's ini). The host draws it (`vignette.cpp`):
  - a head-locked quad 1 m ahead and 4 m square, black, with premultiplied alpha rising from a clear centre to the edge
    (a smoothstep: light from 0.42 to 0.80 half-widths at 75 % at most, strong from 0.28 to 0.62 at full);
  - 16 pre-baked levels, copied into its swapchain only while shown;
  - layered right after the game's image, so the reticle, the scope, the wrist HUD, the rings and the menu stay above it.
  The signal (`Pad::Motion`) is the move stick's deflection or the smooth turn past XInput's usual 24 % dead zone, or a
  snap step for 0.3 s. Head motion doesn't count, and nothing counts in menus (the pad is neutral there).
- **Proven [S]** (the EA copy, `logs/shots/vig-*.png`): with strong, a 3 s walk (`pad_cmd.py --seq "ly=1 dur=3"`) logged
  "vignette: in (moving by stick)" then "out". The captures' edge brightness was 30.7 still, **3.3 moving** and 30.5 after;
  the centre stayed lit, in both eyes. The XR frame stayed 11.11 ms (0 of 900 late). **[H]** round 54: the strength and the
  fade.

### D63. Seated play -- Decided 2026-10-08 (GOAL A3)
- **Found:** most of seated play was already there. The game side takes its height origin from the first tracked head
  pose and again at every Recentre (the host's recentre keeps y; ENGINE-NOTES 5h). Sitting, then Recentre, makes the
  seated head the game's standing eye, and the menu's Height still offsets it. GOAL's "Calibrate seated" item is
  therefore the existing Recentre, not a second calibration. A saved offset wouldn't transfer between sessions anyway:
  LOCAL's origin moves with each session.
- **What's new:** `[Comfort] Seated` (shipped **0**) and the menu's General tab ("Seated", live through `crouchMode`
  bits 2-3, no layout change). Seated, physical crouch (D61) uses `[Controls] SeatedCrouchDepth` (0.25 m): from a
  chair the head can't drop 40 cm. The eye's height is logged a second after each new origin (the crouch's
  "settled" line). The README and the menu's note say to sit, then Recentre.
- **Proven [S]** (the EA copy, Seated=1, PhysicalCrouch=1):
  - sitting down mid-game (the head 0.5 m down) crouched on the seated line (expected: sitting looks like a crouch);
  - Recentre seated: the origin taken at 1.20 m, the game stood, the eye **160.9 cm** above the feet;
  - leaning 0.30 m from the chair crouched (the eye 130.8, the real head), and sitting up stood;
  - a jump: 711 ms in the air, the camera within 0.5 cm of the body, no crouch action.
  The landing's MinEyeHeight is unchanged: the compensation is 0 when standing. **[H]** round 54.

### D64. The runtime's capabilities in the host's log -- Decided 2026-10-08 (GOAL B1)
- **Why:** remote testers on other headsets and runtimes send logs. Their first questions are what the runtime offers
  and what it wants.
- **What:** at start the host logs:
  - every instance extension offered, in one line;
  - the system's name, vendor, tracking (orientation, position) and swapchain / layer limits;
  - the recommended and maximum per-eye size;
  - the swapchain formats (DXGI numbers) and the reference spaces (STAGE = a floor);
  - the refresh rate, through `XR_FB_display_refresh_rate` (enabled only when offered).
  Diagnostics, always on. The extension list is kept as a set the later bindings use (B3).
- **Proven [S]:** the simulator: 9 extensions (including `XR_KHR_composition_layer_depth`); orientation and position
  tracking; swapchains up to 4096x4096 and 16 layers; 1280x1400 per eye recommended; formats 29 28 91 87 10 2 24 20 40
  45 55; spaces VIEW LOCAL STAGE; refresh rate "not exposed".

### D65. An R8G8B8A8 swapchain when the runtime offers no B8G8R8A8 -- Decided 2026-10-08 (GOAL B2)
- **Why:** the host stopped with "runtime offers no B8G8R8A8 swapchain format". SteamVR, Meta and Virtual Desktop offer
  it, but some runtimes may offer only R8G8B8A8. A raw copy between the two families is invalid, and CPU-packed pixels
  come out with red and blue swapped.
- **What:** the format order is BGRA sRGB, BGRA, then RGBA sRGB, RGBA. On the RGBA path:
  - the game's frame goes through a shader blit (`blit.cpp`: a texel `Load`; a shader read returns logical RGBA) into an
    R8G8B8A8_UNORM texture, then the raw per-eye copy as before (same family as the sRGB swapchain);
  - every render target copied into a swapchain is made in the swapchain's family (`formats.hpp` RtFormat): the menu's,
    the wrist HUD's atlas, the scope's;
  - CPU-packed pixels follow the format (Pack): the reticle and the rings; the vignette is black.
  `[Debug] ForceRgbaSwapchain=1` takes the RGBA path where BGRA is offered too. A fix for a hard failure: on (GOAL
  rule 3), no switch beyond the debug one.
- **Proven [S]** (the EA copy; `logs/shots/b2-*`): forced, the host took "format 29 (R8G8B8A8, sRGB) -- the game's frames are
  blitted into it". Against the BGRA run, gameplay / wrist HUD / menu captures matched:
  - mean RGB 31.4/30.0/27.1 against 31.4/30.1/27.1 in gameplay;
  - the menu's blue highlight 31.5/55.5/83.9 against 31.2/55.2/84.3, its orange title 179/152/87 against 182/154/88;
  - no blue pixels where red belongs.
  The XR frame was 11.11 ms on both, 0 late.

### D66. More controllers bound directly -- Decided 2026-10-08 (GOAL B3)
- **Why:** after D59 (the Reverb G2), the controllers testers may bring. Without their own bindings they depend on the
  runtime remapping Touch's.
- **What** (the paths from the OpenXR spec; each suggestion logged if refused):
  - **The Vive Cosmos** (`XR_HTC_vive_cosmos_controller_interaction`, enabled when offered: SteamVR does): Touch's
    buttons and sticks, the grip a click.
  - **The Pico 4** (`XR_BD_controller_interaction`, when offered: Pico's own runtime, not SteamVR): Touch's layout.
  - **First-generation WMR** (`microsoft/motion_controller`, core): sticks, the grip a click; no face buttons.
  - **The Vive wands** (`htc/vive_controller`, core): no sticks. The left trackpad's touch is the move stick, the
    right's the turn stick.
  - **The trackpads** (WMR and wands; `Pad::TrackpadSrc`): a click on the upper half is that hand's upper face button
    (B / Y), on the lower half its lower one (A / X), in the centre (under 35 %) its stick click. While a pad is
    clicked, that hand's stick reads still: on the wands the stick is the pad, so a low click mustn't also flick it.
    `FaceButton` sees the derived buttons too: the manual reload's release, and D60's wrist menu button (X = the left
    pad's lower half).
  - The MOHAVR menu on WMR and wands: the left menu button toggles; the left stick (wands: the left pad) navigates;
    the triggers select; the right menu button backs out.
- **Proven [S]:** the simulator accepted every suggestion (no "bindings not accepted"), still bound Touch ("bound as
  .../oculus/touch_controller"), and a harness cycle passed. SteamVR's `vrclient_x64.dll` on this PC names the Cosmos,
  WMR and Vive profiles and the Cosmos extension. **[H]:** a tester per device (round 54).

### D67. The wrist panels dim behind the gun -- Decided 2026-10-08 (GOAL C1)
- **Why:** the README's known issue: the panels draw over the gun or arm when those pass in front of the wrist. The panels
  are composition quads, always over the game's image. `XR_KHR_composition_layer_depth` would need the game's depth
  buffer, which the bridge doesn't share (only colour), and drawing the panels into the game's frame is a far larger change.
- **What:** geometry the host already has. For each panel, samples along the gun hand's aim line, from 30 cm behind the
  hand (the forearm) to 60 cm ahead (the barrel), every 5 cm, are tested against the eye's line to the panel's centre. A
  sample within 8 cm of that line, between 5 % and 95 % of the way along it, puts the panel behind the gun. It dims to
  15 % (not hidden: still readable through) at the panels' fade rates, and comes back when clear. `[HUD] WristOcclusion=1`:
  a fix, on (GOAL rule 3).
- **Proven [S]** (`logs/shots/c1-*.png`): the wrist panels up; the gun hand pointing across between the eyes and the
  wrist logged "the left panel behind the gun -- dimmed", the same for the right; moved away, "clear of the gun again".
  The capture shows the gun over the wrist with the panels faint, then the panels full again.

### D68. The HUD in the desktop mirror in wrist mode -- Decided 2026-10-08 (GOAL C2)
- **Why:** the README's known issue. In wrist mode the game's HUD pass goes into the mod's own texture (D55), so the frame
  the mirror shows has no HUD: onlookers and streamers saw none.
- **What:** when the slot's HUD flags say the eyes are HUD-free and the texture holds this frame's pass (bits 0 and 1;
  `WristHud::MirrorHud`), the mirror draws that texture over its whole picture with premultiplied blending (a
  full-screen triangle, linear sampling), as the flat game would. `[Bridge] MirrorHud=1`: a fix, on.
- **Proven [S]** (`logs/shots/c2-mirror.png`, the mirror in its own window): "mirror: the HUD drawn over the mirror (wrist
  mode: the frame has none)". The capture shows the compass, health, grenades, weapon icon and 50/90 over the scene, where
  the flat game draws them.

### D69. Falling magazines, cases and rounds stop at what is in their way -- Decided 2026-10-08 (GOAL C3)
- **Why:** the README's known issue, dropped magazines and ejected rounds showing through walls and tables. The cause, in
  two parts:
  - **No collision:** the falls (the magazine and the M18's spent case in `reload.cpp`, the rack-ejected round in
    `rackround.cpp`) were ballistic paths down to the feet's height, so they passed straight through anything in the way.
  - **The depth group:** they're drawn in the first-person foreground group, the magazine as a bone of the gun's mesh
    and the round as a carrier with the arms' settings. So once inside or behind a surface they still showed in front
    of it. The magazine can't take another depth group apart from its gun.
- **What:** `falltrace.cpp` traces each fall's path once, at the drop, on the game thread (`aim::WorldTrace`, ignoring the
  pawn, in 20 ms steps):
  - a step that hits mostly going down has landed on what it hit (it rests there);
  - a step that hits mostly sideways has met a wall: the sideways motion stops 3 units short, and it drops to what is
    under that point.
  In left-hand mode the points are mapped through the frame's mirror for the traces; the times and heights carry over
  (the mirror is a vertical plane). A start at or under the feet' plane rests there as before. Objects so stay in front
  of surfaces, and the eye's line to them is clear. `[ManualReload] FallTrace=1`: a fix, on. A test command, `mohavr
  falltrace <m/s> [up]`, throws a path from the eye along the heading.
- **Proven [S]** (the EA copy, the save's street):
  - the test path, open street: "landed on TOP of something", the ground 13 units above the feet' plane; facing the
    low wall: "a WALL, the sideways motion stops after 0.68 s";
  - a Thompson's magazine ejected 2 m from it: no hit, the floor as before;
  - walked up to it (the test path stopping after 0.17 s): "the magazine's fall meets something under it after 0.13 s --
    it rests at 3391 (the feet at 3264)", on top of the low wall instead of through it to the feet.

### D70. The StG44's dust cover stays shut -- Decided 2026-10-08 (GOAL C4: WONTFIX)
- **Why asked:** round 52, "The stg chamber does not open with the bolt ... the chamber clips through the cover."
- **Found** (within GOAL's 1 h box; `work/research/reload/psk`, `DE_STG44_Rigged`):
  - The mesh is **one material** (6,166 faces), so there's no material or section trick.
  - Its only moving action bone is `Bolt`: 194 vertices, the handle and the rod.
  - The faces around the ejection port's right side are skinned to `RootOffset` (151, the body: the bolt body and the
    dust cover) or to `Bolt` (242). No bone exists to move the cover, and no art exists of it open.
- **What it would take:** re-skinning the cover's vertices in memory to a driven bone, in each LOD's GPU vertex buffer and
  its chunk's bone map, then posing a hinge: days of reverse engineering, with a crash risk in every LOD. The other way
  is new art, which the mod can't ship (standing rule 1). Out of proportion for one gun's detail.
- **The README** keeps the known issue.

### D71. Mounted MG42s: the game draws and aims them, the head aims in VR -- Decided 2026-10-08 (GOAL D)
- **Why:** the mod had no handling of mounted guns (D33). Measured at a nest (MOUNTED-DESIGN 2): the view model pulled
  the mounted MG42 into the gun hand, off its mount, and the hand's ray aimed it.
- **What (Phase 1):** while a `MOHAMountedGunWeapon` is in hand, the view model treats it as "no gun drawn" (the game
  draws the parts, in true 3D) and the aim stands down. The gun sits on its mount, camera-locked as in the flat game:
  the head aims it, the stick turns the mount, and barrel, view and shots agree. `[Weapon] MountedGame=1` (a fix: on).
  Test commands: `mohavr mg list | goto [n] | use [n] | where` (a nest is a `MOHAPawn` in the pawn list; manning goes
  through its `MyCSA.UsedBy`).
- **Not built:** both hands on the handles (MOUNTED-DESIGN 3.2: `rMGRot` from the hands, the gun drawn off the camera,
  firing along the barrel). How it should feel is for the headset to judge; round 54 asks.
- **Proven [S]** (the EA copy, Husky's street, 10 nests found):
  - manned: the MG42 on the wall, the hands on it, in stereo;
  - a soldier 6 m ahead killed (Health 110 -> 0, `MOHAMGDamageType`), the belt 78 -> 60;
  - the head's pitch pitches the gun with the view; the stick turns the mount (`rMGRot` yaw 3632).

### D72. The mission sweep, and a fallback round where the borrowed one isn't loaded -- Decided 2026-10-08 (GOAL E)
- **Why:** the README's known issue: the knife and the rack-ejected rounds use other guns' models from the game, and they
  had been checked in the first levels only.
- **The sweep:** `mohavr sweep` logs the level and does a fresh look-up of every borrowed model (the MP40's knife mesh, each
  gun line's round template and bone). `work/research/tests/sweep.ps1` opens each campaign map in turn, runs it, screenshots
  and writes `work/research/goal/sweep.md`. Results:
  - 7 of 7 maps load with the mod, with no stand-down, error or crash.
  - `open` loads only the persistent level (an empty grid; the mission's areas stream in only through the campaign's own
    start), so the live results are a floor.
  - The cooked packages show every borrowed class somewhere in every mission.
  - Live Husky gameplay lacked the G43's and the M12's meshes: what is loaded depends on the area.
- **The fix:** `RackRoundAlt=<Attachment>.<bone>,<len>,<along>,<across>`, tried when RackRound's model isn't loaded. The StG44
  and the G43 fall back to the K98's `bullet2` (the same 7.92 mm cartridge) at the same size. The M12 has no other shell,
  so it keeps the game's spent shell there. The knife has no other model: not drawn where the MP40 isn't loaded.
- **Proven [S]:** the sweep (above); in Husky, the Garand's round pointed at the missing M12 shell threw "a live round
  thrown (RackRoundAlt)" with the Springfield's bullet, and the sweep reports both alternatives found there.

### D73. Render resolution presets for common headsets -- Decided 2026-10-08
- **Why:** the player (2026-10-08): "Can we have resolution options to match commonly used headsets?" The render size was
  only `[Render] ResX/ResY` (shipped 2880x1620: 1440x1620 per eye), set by hand.
- **What:**
  - `[Render] Preset` and the menu's General tab -> Resolution (saved in the player's ini; the game picks its size at
    start, so it applies at the next start).
  - The presets (`src/common/render_presets.hpp`): `custom` (ResX/ResY, the shipped default), `auto`, and the headsets'
    panels per eye: Quest 2 / 3S 1832x1920, Quest 3 2064x2208, Quest Pro 1800x1920, Pico 4 2160x2160, Index 1440x1600,
    Reverb G2 2160x2160, Vive Pro 2 2448x2448, Rift S 1280x1440, PS VR2 2000x2040, Bigscreen Beyond 2560x2560. The game
    renders both eyes side by side: ResX = 2 x the width.
  - **Auto:** the host saves the runtime's recommended per-eye size each session (`%LOCALAPPDATA%\MOHAVR\MOHAVR.headset.ini`),
    and the next start uses it (the game picks its size before the host runs). With no headset seen yet it falls back to
    Custom.
  - A cap of 2560 per eye.
  - The game DLL now reads the player's ini for this key: the first setting it takes from there. `[Render] UserPreset=0`
    ignores the player's choice; `deploy.ps1` adds it to any run that sets `Render.ResX/ResY`, so the harness's
    1920x1080 screen checks still hold.
- **Proven [S]** (the EA copy, the 4 GB flag on):
  - Custom: a harness cycle at 1920x1080 OK;
  - Auto: 2560x1400 ("what OpenXR Simulator asked for last time");
  - Quest 3: 4128x2208 in gameplay, the game frame 11.11 ms avg (7 of 900 over 20 ms), no XR frame late, 1.72 GB virtual;
  - Bigscreen Beyond: 5120x2560 in gameplay, 11.11 ms, 1.72 GB virtual (the larger targets live in the D3D12 device, not
    the game's address space);
  - the menu: Custom -> Auto -> Quest 2 / 3S, written to the player's ini and restored after.
  **[H]:** the sharpness and the frame rate on the player's own PC.

### D74. Pause when the headset loses focus; a pulse per shot -- Decided 2026-10-08
- **Why:** the player (2026-10-08, "Do it", after the feature list).
  - The host only reacted to a stopped session: taking the headset off or opening the runtime's dashboard left the game
    running.
  - Only the off-hand pistol pulsed per shot.
- **The pause:**
  - When the session leaves FOCUSED during gameplay (a head-tracked frame, no game menu open), the host sends the game
    `showmenu`, the game's own Start binding, through the console-command channel. The game runs it even while the host
    isn't drawing.
  - With a game menu already open it does nothing, since a second `showmenu` would close it.
  - `[Bridge] PauseOnFocusLoss=1` (a fix, on). The simulator never loses focus, so a host test command (`unfocus`, in
    host_cmd.txt) runs the same path.
- **The pulse:**
  - `muzzle.cpp` counts the player's weapon's muzzle flashes into the new `gunShots` counter (shared block v30, at 3860).
    The flash is re-activated per shot, so the count is per shot for every gun, the mounted MG42 too.
  - The host pulses the gun hand (`[Controls] ShotHaptics` = the strength, 0.6 shipped; 0 = off), and the foregrip hand at
    70 % when two-handed.
  - It needs the muzzle hook, installed unless `Weapon.MuzzleFlash` and `Brass` are both "game".
- **Proven [S]** (the EA copy):
  - a 1 s Thompson burst: ammo 50 -> 35, 15 shots seen, 15 pulses in the right hand;
  - `unfocus` in gameplay: "the game paused (its pause menu)", the screen check "pausemenu"; again with it open: "not in
    gameplay, no pause".
  **[H]:** the pulse's strength, and the pause when the headset really comes off.

### D75. The parachute steered by the hands -- Decided 2026-10-08
- **Why:** the player ("Do it", after the feature list's item 3). Every mission starts with the jump, and steering was the
  stick only.
- **What:** the menu's General tab -> Parachute (`[Controls] ChuteHands`, shipped **0**: how it should feel is for the
  headset to say).
  - **Hands:** with the chute open (`airdrop` >= 2: shared block v31, game -> host) and both grips held (on the risers),
    each hand's pull below the head (0 at 10 cm above the head, 1 at 35 cm below it) steers through the move stick, the
    game's own steering (ENGINE-NOTES 5bu). The difference between the hands turns (x = 1.5 x the difference); the
    average dives or slows (y = 0.6 - 1.2 x the average, never below 0: a canopy never flies backwards).
  - **The flare:** a quick deep pull of both, from under half to past 85 % within 0.4 s, pulses Xbox A, the game's
    FlareChute.
  - **The grips:** while the chute is open they are the risers', so they don't also press their mapped buttons (the
    right grip's A would flare by itself).
  - **The real stick** still wins when pushed.
- **Proven [S]** (the EA copy, `mohavr chute 300`):
  - hands up: 800 u/s forward;
  - the right riser pulled: ~800 u/s right, the heading turning;
  - both half pulled: slowing to 22-48 forward;
  - a fast pull of both: "flare", the game's phase 2 -> 3 -> 2, then slowing (266 -> 140 -> 44), never backwards.
  **[H]:** the feel, the depths, whether holding the risers for a whole descent is comfortable.

### D76. The recoil: the game's view kick on the gun in the hand -- Decided 2026-10-08
- **Why:** the player ("Do all 3", the feature list's item 6).
- **Measured first (ENGINE-NOTES 5bv):** the game's fire animations already shove the drawn gun ~5 cm back (the Colt
  also flips 17 deg). The game's view kick, the muzzle climb, was lost in VR, because the head turns the view.
- **What:** per shot of the main gun (`gunShots`), the gun turns up about the gun hand by the weapon's own
  `KickParams.PitchDistance`, a little aside (YawDistance, YawRandomness), capped at its cutoff (15 deg at most). It
  rises in ~18 ms and comes back at the game's `PitchRecenterRate`, but always within 0.3 s.
  - The arms follow (the IK's target is the kicked controller frame); the game's own shove stays.
  - `[Weapon] Kick` (shipped **1** = the game's; 0 off, up to 2) and the menu's Weapons tab -> Recoil (off / 50-200 %,
    live through shared block v32 `kickMode`).
  - `[Weapon] KickAim` (shipped 0): the shots follow the kicked barrel. Off by default: the aim stays on the hand's line,
    and the reticle with it.
- **Proven [S]:**
  - the drawn barrel's turn off its line per shot: Thompson 0.2 deg at Kick 0, 0.8-2.0 at 1 and 2.8 at 2;
  - the Garand 4.9;
  - the Colt 19 (17 of it the game's flip).
  **[H]:** the feel, and whether aiming down the sights tolerates it.

### D77. Taking weapons and crates by hand -- Decided 2026-10-08
- **Why:** the player (the feature list's item 4).
- **What:** a free grip closing within 35 cm of a weapon or a crate the game offers (its swap prompt) takes it, through
  the game's own `UsedBy`. A free grip is one no gesture took: not a holster, the pouch, the foregrip or the off hand's
  items.
  - A weapon is swapped for the gun of its kind you carry, which is dropped there. A crate gives its ammo or grenades.
  - Only what the game would take counts as in reach (`IsUsableBy`, asked at most every 0.5 s). A light tick when a hand
    comes within reach, a pulse when it's taken.
  - Either hand works; the game's hold-to-swap stays.
  - Shared block v32: `pickupNear` (game -> host), `pickupReqHand`/`pickupReqSeq` (host -> game), `pickupDone`,
    `pickupMode`.
  - `[Controls] GrabPickup` (shipped **1**) and the menu's Weapons tab -> Grab pickup.
  - Test commands: `mohavr pickup list | drop | take`.
- **Proven [S]:**
  - an M1 Garand dropped 1 m ahead (`mohavr pickup drop`): both hands "within reach" from 0.18 m;
  - the left hand on it, the grip: "taken", the Garand in hand, the Thompson left lying there and offered in turn;
  - a Thompson while carrying one: refused (IsUsableBy false), and no longer offered.
  **[H]:** the reach, and whether a stray grip ever takes something.

### D78. The mounted MG42 aimed by the hands -- Decided 2026-10-08
- **Why:** the player (the feature list's item 5; MOUNTED-DESIGN 3.2).
- **What:** while a manned MG42 is in hand and `[Weapon] MountedHands` is on (shipped **0**; the menu's Weapons tab ->
  Mounted MG42: head / hands, live through `mgMode`), the game side writes the pawn's `rMGRot` and its aim blends each Draw.
  They come from the host's aim line, against the body's heading and level, within the gun's 45 / 30 deg.
  - The game turns and fires the gun on its mount.
  - The camera turns with the gun (ENGINE-NOTES 5bv), so the eyes keep the controller's yaw, and their position is held
    where it was with the gun level. The controller's pitch is held at 0 meanwhile.
  - Fixed for D71 too: past 11 deg of the stick's mount turn the view was taken for a cutscene's (no head pitch on the
    gun).
- **Proven [S]** (Husky's street, a soldier 6 m ahead):
  - the hand line 25 deg right and left -> the barrel along it (0.0 deg off), two bursts miss (Health 110 -> 110);
  - the hand on him -> one burst kills him (110 -> 0);
  - pitch 15 deg -> rMGRot 2730, the barrel 0.2 deg off;
  - the view doesn't move as the gun swings (`logs/shots/d78-*.png`).
  **[H]:** whether a heavy gun swinging freely under the hand feels right; head or hands.

### D79. The mission loadout's list, workable with the controllers -- Decided 2026-10-08
- **Why:** the player: "When you start a new mission and get to the load out screen, you select the gun you want to swap
  out, it opens a list of guns to choose. Then any input just goes back to the load out screen."
- **Found (ENGINE-NOTES 5bw):** the game's own bug, the same without the mod. Its list takes its first input as the pick:
  Down gave the next gun, anything else the first one, and the third was never reachable. The flat game's mouse click
  hides it.
- **What:** `[Controls] LoadoutList` (shipped **1**, a fix). While a loadout scene's list has the focus, up and down (the
  D-pad and the left stick, repeating while held) are taken out of the pad state the game sees (the XInput hook, the
  host's pad or a real one). The list's index is moved with `UIList.SetIndex(..., notify)`, which moves the highlight and
  the stats. A picks the gun (the game's own submit), and B goes back.
- **Proven [S]** (Husky's loadout, the controller path through the host):
  - Primary's list: down, down -> MP40 highlighted with the list open, up / down, A -> Primary = MP40;
  - Secondary's list: down -> Thompson, A -> Secondary = Thompson.
  The keyboard keeps the game's behaviour.

### D80. The build check no longer stands down on the PE CheckSum alone -- Decided 2026-10-08
- **Why:** a tester's EA copy logged "build check: FAIL -- CheckSum is 0x00E458F7, expected 0x00E402F3" and stood down.
  It was the only mismatch: the timestamp, SizeOfImage, entry point and all 39 code signatures matched.
- **Cause:** the CheckSum is the whole file's checksum. Re-signing the exe or a header patcher (a third-party 4 GB tool
  recomputes it) changes it while the code is identical. Our own 4 GB step doesn't touch it (this PC's flagged exe still
  has 0xE402F3).
- **What:** a CheckSum difference is logged as a note. The other header fields and the signatures (every patched site's
  bytes, plus each hook's own prologue check, standing rule 4) still decide. `[Debug] TestWrongBuild=2` simulates it.
- **Proven [S]:** with TestWrongBuild=2, "build check: note -- the header's CheckSum is 0x00E454F7 ...", then
  "build check: OK", every hook installed, and the harness cycle reached gameplay.

### D81. The menu reorganised into six tabs -- Decided 2026-10-08
- **Why:** the player ("Do it", to cleaning up the menu). Since round 32's four tabs, General had grown to 15 items and
  Weapons to 21, the newest added at the bottom.
- **What:** six tabs, each item where a player would look for it and the ones changed most first:
  - **General:** the view and the setup;
  - **Comfort:** moving, turning, stance, the parachute;
  - **Weapons:** the gun in hand;
  - **Reload:** the manual reload and its pages;
  - **Hands:** the holsters, the off hand's items, the hand point and rings;
  - **HUD:** unchanged.
  The tab row has its own line, at a smaller size, to fit six. The menu opens on Recentre. No setting changed.
- **Tests:** `menu_cmd.py goto=<key>` (the item keys in `kItemKeys`) or `goto=<tab>` selects directly, so tests no longer
  count steps through the layout. The older scripts in work/research/tests count steps through the old one.
- **Proven [S]:** each tab captured (`logs/shots/d81-*.png`); `goto=recoil` -> the Weapons tab, row 2, and left / right
  changed it; `goto=hud` -> the HUD tab's row; an unknown key is logged and ignored.

### D82. The mounted MG42: the head's pitch, and the hand as a lever -- Decided 2026-10-08
- **Why:** the player's headset round:
  - "10. Only goes side to side": with the head aiming, the gun didn't pitch with the head;
  - "16. Hand aim is inverted".
- **Found:**
  - **Head:** the head's pitch reached the controller (Aim.HeadPitch), which didn't move the manned gun in the headset.
  - **Hands:** the session log showed the barrel within ~1 deg of the hand's pointing line, so the mapping did what D78
    designed. The design was wrong for a gun held by its rear handle: turning the wrist right swung the drawn handle
    away to the left. The right grip also pressed the game's use, which twice took the player off the gun.
- **What:**
  - **Head mode:** the controller is held level on a manned gun, and the head's pitch is written to `rMGRot` and the aim
    blend, the way D78's hands drive it. The eye stays where it was with the gun level, as in hands mode.
  - **Hands mode** (host): the gun hand's grip takes the handle. The pivot is put 40 cm along the gun's line from the
    hand, and while held the gun points from the hand through it: pushed left, the muzzle swings right; pushed down, it
    rises. Let go and it stays. While manned that grip is the handle's, not use; B still gets off.
- **Proven [S]:**
  - head pitch 20 / -15 deg -> rMGRot pitch 3640 / -2730, the camera pitch 20.00 / -14.99, the eye's height unchanged;
  - hand 10 cm left / right / down -> the gun's line +14.0 / -14.0 deg yaw, +14.0 pitch, back to 0 at the start;
  - let go, the hand moved: the gun stayed; taken again there and 10 cm left: +14.0;
  - holding the handle, a burst at a soldier 6 m ahead: Health 110 -> 0, still on the gun.

### D83. Simple grenades, the new default -- Decided 2026-10-08
- **Why:** the player: "I think I overcomplicated grenades. Make a new option that will be the default. Hold grip to grab
  from holster, press right trigger once to start cooking, then throw by letting go of the grip in a throwing motion."
- **What:** `[OffHand] GrenadeStyle=simple` (shipped; the menu's Hands tab -> Grenades: simple / classic):
  - **Off hand:** hold the grip at the grenade holster to take one; it stays while the grip is held, and let go with the
    pin in it goes back. One press of that hand's trigger pulls the pin and starts the fuse together (PIN and COOK). Let
    go of the grip with the throw.
  - **Gun hand:** the grip held when the trigger pulls the pin makes letting go the throw. Simple covers the gun hand
    whatever "Hand grenades" says.
  - **Classic** keeps everything as it was (GrenadeHold, Pin, Cook, Hand grenades); with simple on, the menu hides the
    classic-only items.
  - "That hand's trigger": the grenade's own hand (the left trigger for the off hand with the gun in the right). The
    player said "right trigger"; to be confirmed in the headset.
- **Proven [S]:**
  - off hand: the grip held 1 s and let go -> TAKE, PUT BACK; held, one trigger press, let go with a throw -> TAKE,
    PIN + COOK, THROW (a cooked stick grenade, the fuse 2.69 s);
  - gun hand (the player's GrenadePin=0): the grip held, one press, let go -> PIN + COOK, THROW, the next one in hand.

### D84. The damage flash back, as a switch (on) -- Decided 2026-10-08
- **Why:** the player: "When I take damage now, the screen no longer goes red. Is that a toggle somewhere now? If it isn't
  make it one. On by default."
- **Found (ENGINE-NOTES 5bx):** not a toggle. The game draws its damage tint in the depth-of-field (uber post-process)
  pass, which the mod has forced off since M2 (`Camera.DisableDepthOfField`, against the game's focus blur in VR). The
  tint was computed every hit and never drawn.
- **What:** `[Camera] DamageTint` (shipped **1**; the menu's Comfort tab -> Damage flash, live through shared block v33
  `damageTint`). While one of the game's screen tints runs (the post-process component's flags: a bullet or melee hit,
  low health, an explosion, dying, a medkit, a pickup), depth of field is allowed, so the pass draws the tint. Otherwise
  it stays off as before, so the normal image is unchanged.
- **Proven [S]** (a bullet hit held, the backbuffer's mean R / G / B):
  - on: 31.3 / 29.9 / 26.9 before, 148 / 63 / 62 during, 31.3 / 29.9 / 26.9 after;
  - off: unchanged throughout.
  **[H]:** whether the depth-of-field blur during a hit's ~1 s is noticeable.

### D85. The Valve Index's controllers work the MOHAVR menu -- Decided 2026-10-09
- **Why:** an Index player: the mod menu "does not respond to controls".
- **Found:** the menu's own actions (navigate, select, back) had bindings for every supported controller except the
  Index, whose suggestion list was empty. The menu opened (the wrist and the left A, D60) and nothing moved it.
- **What:** the Index's menu bindings, Touch's layout: the left stick navigates, either trigger or the right A selects, the
  right B backs out. The toggle stays the wrist's (the Index has no menu button for the game).
- **Proven [S]:** the bindings accepted (no "not accepted"), the harness cycle OK. The simulator emulates Touch only:
  **[H]** the Index player's confirmation.

### D86. The rings off by default, holster and reload rings separately -- Decided 2026-10-10
- **Why:** the player: "Need all visible reload and holster rings to be off by default, and able to be enabled separately
  in the first tab."
- **What:** `[Hands] HolsterRings` and `ReloadRings` (shipped **never**; each never / near / always), in place of the
  single `Rings`, which is no longer read.
  - **Holster rings:** the holsters and the belt pouch.
  - **Reload rings:** the gun's magazine, bolt, pump and foregrip spots.
  - The off hand's dot shows with either.
  - The menu's General tab: Holster rings and Reload rings. The Holsters page's ring item is the holster rings; the
    Holsters and Reload spots pages still show every ring while open.
- **Proven [S]:** with the defaults, hands at the belt pouch and a hip showed no ring; General -> Holster rings: near
  showed it ("markers: first rings shown").

### D87. The firing shake as a switch -- Decided 2026-10-10
- **Why:** the player: "screen shake when firing needs to be optional".
- **Found:** there is no firing camera shake. The game's per-shot view kick does it:
  - its pitch and yaw turn the controller, and the yaw turns the VR view's heading, so the world shakes side to side;
  - its push moves the camera.
  Both come from the weapon's WeaponKickComponents (KickComponent, IronsightsKickComponent), whose own copy of the
  ViewKickTuning their native side takes in UpdateParams.
- **What:** `[Camera] FireShake` (shipped **1**, the game's; the menu's Comfort tab -> Firing shake, live through shared
  block v34 `fireShake`). Off: the components' pitch, yaw and push (distance and randomness) are zeroed and re-taken
  (UpdateParams), per weapon and re-checked every second. The weapon's own KickParams, which D76's recoil in the hand
  reads, stay.
- **Proven [S]** (a 1.2 s Thompson burst; `mohavr shake trace`: the controller and the game camera per Draw):
  - on: the heading moved over 0.83 deg, the camera 2.5 units;
  - off: 0.00 deg and 0.5 units, the idle level.
