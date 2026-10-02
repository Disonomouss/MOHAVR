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
