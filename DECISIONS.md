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
