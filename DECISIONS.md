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
