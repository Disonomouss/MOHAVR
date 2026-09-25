# Lessons from the Gun (2005) VR mod, for the Medal of Honor: Airborne VR mod

Written by the agent that built GunVR (a native 6DoF motion-controlled VR mod
for a 2005 D3D9 game: stereo, OpenXR, controller aiming, holsters, dual
wield, manual reloading, wrist HUD). Most of the Gun *engine* facts won't
carry over: Airborne is a 2007 Unreal Engine 3 game, while Gun ran on
Neversoft's own engine. The methods, the working process and the D3D9 →
OpenXR plumbing do carry over. Check every technical claim below against
your game before relying on it.

---

## 1. Process

**Build a headless test rig first. It paid for itself many times over.**
- Get a 32-bit build of an OpenXR simulator and select it per process with
  `XR_RUNTIME_JSON`, so the machine's real runtime is untouched.
- Write scripts that kill, deploy, launch and bring the window to the front;
  drive the front end into live gameplay; inject input; and take screenshots.
- Detect progress from **log lines** (for example "input context switched"),
  not timers. Load times vary, and timers make flaky tests.
- With this in place, nearly every feature can be proven before the player
  puts the headset on.

**The player's verdict in the headset is the acceptance test.**
- Mark each item **[S]** (the simulator can prove it) or **[H]** (only the
  headset can judge it).
- After each batch, write a "headset test round": what changed, how to try
  it, and yes/no questions. Short, concrete questions get the most useful
  answers.
- Give every new behaviour an ini switch. Turn it on by default only once
  the simulator has proven it.
- **Ask the player for their log after each session.** Several bugs were
  obvious from their log and invisible in the simulator. For example, "I
  can't swap guns" turned out to be a logged "both hands full" refusal.

**Logging discipline.**
- Log state *transitions* and first occurrences, not every frame. Every line
  should say what happened and why it matters.
- Keep an ini switch for heavy research instrumentation, off by default.
- Keep copies of every log before relaunching. The game overwrites its log.

**Documents that kept a long project coherent.**
- STATUS: where things stand.
- ROADMAP: milestones, each with a one-paragraph technical summary.
- DECISIONS: why this way and not that.
- ENGINE-NOTES: measured facts about the engine, with addresses and
  evidence.
- HEADSET-TESTS: the test rounds.
- A PLAN document with a checklist that an unattended session can work
  through.
- Update them after every milestone and commit with the code.

**Time-box research-heavy items** (two hours worked well). When the box runs
out or a fact can't be found, write up what's known and move on. Record
blocked items as blocked, with the reason, rather than looping.

**Standing rules that prevented disasters.**
- Never modify game files. Patch in memory only; the mod ships only its own
  DLL and ini.
- Keep all engine addresses in one header, each with a comment saying what
  it is and how it was verified.
- **Verify the prologue bytes of every hook** before installing it, and stand
  down with a log line if they differ.
- Keep the player's settings in a separate file (here `GunVR.body.ini`) that
  overrides the shipped defaults.
- Back that file up before every test and restore it after. Never delete or
  edit the player's file for a test.
- Match the game's bitness. Gun was 32-bit only; check Airborne's executable
  before choosing toolchains.

---

## 2. D3D9 → OpenXR plumbing

**Use D3D9On12 for the zero-copy path.**
- Create the game's `IDirect3D9` through `Direct3DCreate9On12` by hooking
  `Direct3DCreate9` at import time. Every D3D9 resource is then a D3D12
  resource.
- Per eye: `StretchRect` the backbuffer into your own A8R8G8B8 render
  target, then `UnwrapUnderlyingResource`, `CopyResource` into a **shared**
  D3D12 texture, signal a **shared fence**, and `ReturnUnderlyingResource`.
- On the XR thread, the session's D3D11 device opens the shared textures and
  fence, waits, and copies into the swapchain image.
- Keep a system-memory readback path as a fallback (it costs about 2–4 ms a
  frame).
- The bridge choice must be made before the device exists.
- If Airborne renders through D3D10 on some settings, force its D3D9 path
  first.

**Hook device creation from the ASI's init, before WinMain.** That is the
only place to change the backbuffer size or format.
- Windowed mode is mandatory for a non-standard, headset-shaped resolution.
- **Turn vsync off at creation.** A windowed, vsynced Present under 9On12
  waits for the desktop refresh and caused heavy stutter.
- **Relative ini paths break this early.** The game hadn't set its working
  directory yet, so anchor paths to the executable's folder.

**Stereo.** Gun has no stereo support, so the mod runs the game's
scene-render function twice per frame, with a different eye camera each
time. The left eye is copied out between the two passes, which avoids
redirecting render targets. The costly surprises all came from state that
is used up once per frame:
- lists drained as they draw (radar markers);
- particle "draw this frame" bits;
- frame stamps and counters.

Each of these made one eye lose something. The fix is to save and restore
that state between passes. UE3 has a separate render thread and its own
view setup, so look first at whether the engine can already render two
views (split-screen, reflections, scene captures) before re-running
passes.

**Projection.**
- Render the headset's field of view, per eye and asymmetric, by rewriting
  the frustum *arguments* where the engine builds the projection.
- When moving the near plane, scale the frustum edges by the same factor so
  the FOV doesn't change.
- **There may be two near planes:** the culler's and the projection's. Lower
  one without the other and near objects vanish instead of clipping.
- Depth-precision problems show up as shimmer on coplanar decals under head
  motion. A still simulator never shows it.

**Layers.**
- **Menus and cutscenes:** a world-locked cinema-screen quad with the game's
  own camera. Detect them from the input context, the cutscene update
  function, and "no live player".
- **HUD:** trace one frame's device calls (see §4). Find where the HUD starts
  (in Gun, right after one full-screen composite draw), switch the render
  target to your own texture for the rest of the pass, and show that on a
  quad (wrist or fixed).
- Give your per-eye overlays (reticle, rings, vignette) their own draw at the
  end of each pass.

**The frame loop.** Pair the left and right images from the *same* frame
before submitting. Mismatched pairs read as flicker in the headset.

---

## 3. Input and interaction

- **Check how the game reads input.** Gun read one DirectInput keyboard and
  mouse state per frame, and `SendInput` / `keybd_event` never reached it.
  Hook `GetDeviceState` and write synthesized controls there. That became
  the whole VR "virtual pad".
- **Find the game's own control table** (action names → control ids → keys)
  in the executable or its configuration. Map buttons to *controls*, not
  keys, and make the map configurable. Players will want to remap.
- **Aiming.** Drive the bullets from the controller ray: find where the
  engine builds the shot ray and override it there. Turn the body towards
  the controller with a gentle servo, and compose head tracking onto the
  game camera heading-only (the game keeps pitch and roll).
- **Two-handed items.** The hand frame, per-item fits (adjustable in the
  headset menu and saved), finger curl, and arm IK from the controllers all
  mattered to how things felt. Expect many rounds of "the hand sits wrong".
  Put the adjustments in the in-headset menu from the start. A keyboard
  numpad is useless with a headset on.
- **Call the engine's own functions** (member functions, script spawns)
  rather than reimplementing game logic, but test what context they run in.
  Scripts the mod spawned in Gun could not reach the hero's components.
- **The game's own assets have limits.** A "visible round in the hand" needed
  a model the engine couldn't build safely. When a model isn't possible,
  haptics and hand poses (a fist while holding rounds) are good feedback.
- **Resource budgets can surprise you.** Gun kept one weapon sound bank per
  weapon type, so a second gun of the same type played thin. The fix was
  keeping every holstered gun's bank loaded. Look for "one per type" caches
  whenever you let the player hold several things the base game never
  combined.

---

## 4. Research tools that earned their keep

- **A one-frame device-call trace** (behind an ini switch). Log every draw's
  caller address, plus render-target changes, StretchRects and Clears,
  grouped by caller, between pass markers. It answered "where is the HUD
  drawn and what comes after" in one run.
- **Probes that count calls per pass**, via a hook that records the return
  address. This found state consumed between the two eye passes.
- **Hardware breakpoints with stack capture** (Cheat Engine or x64dbg) for
  call chains that go through virtual calls, where static cross-references
  stop.
- A small capstone disassembler script, an xref and call finder, and Ghidra
  headless for scripted analysis.
- Engine scripts decompiled to text and grepped: event handlers, per-state
  handler tables, checksums. For UE3, UnrealScript packages can be
  decompiled (UE Explorer), and the defaults in its ini files are worth
  reading early.
- Compute and verify the engine's name-hash function against a known pair
  before trusting searches with it.

---

## 5. Simulator gotchas (OpenXR Simulator, 32-bit build)

- Its screenshot is taken *before* quad layers are composited, so a frame
  with both a projection and a quad never shows the quad. Capture the
  preview **window** instead (PrintWindow).
- The preview window shrinks over a session; reset its size by script.
- The head pose resets on every relaunch; set it again.
- Controller poses can't **roll**, so any "turn the wrist" feature can't be
  fully proven there.
- A perfectly still head hides shimmer and flicker, so only the headset
  shows those.
- Its flicker-capture tool worked only for D3D12 apps.

---

## 6. Agent-environment pitfalls (Windows, Claude Code)

- Write edit scripts with the file-writing tool, not shell heredocs.
  Heredocs mangled `\n` escapes in Python strings more than once.
- Nested `powershell` calls need `-ExecutionPolicy Bypass`, and robocopy
  should run from PowerShell (Git Bash mangles `/MIR`).
- `Select-String` is case-insensitive by default. Use `-CaseSensitive` when
  grepping logs for words like "error".
- Keep game launches short and separate. An interrupted launch can hang the
  session.
- In an ImGui menu driven by a controller, give buttons whose label changes a
  stable ID (`"label###id"`), or keyboard and controller focus is lost after
  one press.

---

## 7. What mattered most to the player

- Hands and held items that sit right, adjustable in the headset.
- Holsters on the body that are forgiving, and movable and resizable in the
  menu.
- Comfort options: snap or smooth turning, vignette, a seated offset.
- Menus, cutscenes and the HUD kept off the player's face (a cinema screen,
  a wrist HUD).
- Clear feedback for physical actions: haptics and hand poses.
- A release zip with an install script that finds the game folder, backs up
  the ini, and uninstalls cleanly.
