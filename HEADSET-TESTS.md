# Headset test rounds

After each batch of **[H]** work, add a round here, newest first. Keep the questions short and
answerable with yes or no.

After every session, ask the player for their MOHAVR log; bugs often show up there that the
simulator never shows.

## Round 2: prepared 2026-09-25, M3 (head tracking, mono)
**Changed:** your head now moves the camera. Looking around, up and down, and tilting all move
the view; leaning moves it too. The picture fills your view instead of floating on a screen. It's
still mono (the same image in both eyes), so it won't look 3D yet; that's M4. Mouse turning still
turns your body. The rifle stays where your body aims, so it slides away when you look aside, and
the HUD stays glued to your view. Both are expected for now.

**How to try it:** Claude deploys (`Bridge.D3D9On12=1`, `Bridge.Host=1`, `Camera.HeadTracking=1`,
RuntimeJson empty). Launch from Steam (the desktop window stays white), then Campaign → Continue.
Look around slowly during and after the parachute landing, then quickly. Turn with the mouse too.
Quit, and tell Claude.

**Questions:**
1. When you turn your head, does the world stay still, rather than swimming or lagging behind? (yes/no)
2. Does the world look the right size: not giant, not tiny? (yes/no)
3. Is the horizon level when your head is level? (yes/no)
4. Any discomfort or eye strain in the first minute? (yes/no; mono is expected to feel flat)
5. Does leaning your head move the view naturally, not too much or too little? (yes/no)

**Attempt 1 (~19:05): VOID.** Not deployed; Claude asked instead of deploying (process fixed:
Claude deploys immediately).

**Attempt 2 (~19:10):** deployed. Answers: 1 **world stays still** · 2 **world looks larger than it
should** · 3 horizon fine · 4 **some tearing when moving the head, making things look hazy** ·
5 not tested: after alt-tabbing out to read the questions, the player couldn't get back in or click
anything, and Claude had to kill the game.
Logs (`logs/modlogs/20260925-191356-*`): alt-tab → `Present` = `D3DERR_DEVICELOST`, and the game hung in
its Reset loop.
**Findings and fixes (v0.4.1):**
- **Hang:** D3D9 refuses `Reset` while any D3DPOOL_DEFAULT resource exists, including the
  bridge's copy render target. Fixed with an `IDirect3DDevice9::Reset` hook that releases it first
  and recreates it after. Verified in fullscreen with a scripted alt-tab: Reset → S_OK, frames flow again.
- **Larger world:** `UnitsPerMeter` was 50 (the "UE3 = 2 cm" assumption). MOHA's soldier is 192 units
  tall (CollisionHeight 96), so about 100 units per metre. Head translation was half what it should
  be. The default is now 100.
- **Haze/tearing on head motion:** the game's motion blur (on in the player's settings) blurs by
  camera motion, which head motion now is. It's now forced off in memory while head tracking, along
  with depth of field (FSystemSettings, ENGINE-NOTES §5i). The remaining suspect is resolution:
  the mono union FOV spreads 1440 px over the whole eye.

**Attempt 3 (~19:25):** deployed with the fixes.
**Answers:** 1 world stays still · 2 **world too big: "I am now double the height of other soldiers"**,
player's call: back to 50, revisit with stereo · 3 horizon level · 4 **haze gone** · 5 leaning feels good.
**Log** (`logs/modlogs/20260925-193943-*`): the head-position origin was captured from Virtual Desktop's
**placeholder pose** (identity orientation, y = −1.187 m) before tracking started. The real head
then sat ~1.19 m above the origin, lifting the camera ~119 units. That, more than the scale,
explains "double height". Fixed in v0.4.2: the origin is taken only from a pose with position
TRACKED (host flag), with an auto-recentre if the head is >1 m from the origin. `UnitsPerMeter`
is back to 50 as the player asked.

**Verdict: M3 [H] PASSED** (world-lock, horizon, haze, leaning). Scale is deferred to M4 by the player.

---

## Round 3: prepared 2026-09-25, M4 (stereo)
**Changed:** real 3D. Each eye now gets its own view, rendered by the game's own split-screen system
with that eye's position and field of view. The HUD appears in each eye. Scale is back at 50 as
you asked, and the head-height bug is fixed (the origin now comes from real tracking only).
Menus: the background is 3D, and the menu text is drawn across both eyes, which will look wrong.
Menus move to a floating screen in M5.

**How to try it:** Claude has deployed (`Camera.Stereo=1` + head tracking + host, RuntimeJson
empty). Launch from Steam (white desktop window), then Campaign → Continue. During the parachute
and after landing, look at near objects (your rifle, rubble) and far ones (the tower). Quit and
tell Claude.

**Questions:**
1. Does it look 3D, with depth and near things clearly nearer? (yes/no)
2. Is it comfortable to look at: no double vision, no eye strain? (yes/no)
3. World size now, with stereo: too big, too small, or right? (and your height compared to other soldiers)
4. Does the world still stay still when you turn your head? (yes/no)
5. Is the HUD (compass, health, ammo) readable and comfortable, or does it hurt to look at? (describe)

**Attempt 1 (~19:55):** stereo basically works. Guns are doubled (the equipped gun is seen twice). **The right
eye flickers heavily.** World size is hard to judge with the flicker. The world stays still. The HUD is hard to read:
things at the edge of view, in only one eye, or cross-eyed.
**Findings:** the right-eye flicker came from both eyes sharing one FSceneViewState. Fixed with a second one from
the engine's AllocateViewState, measured 2.79 → 1.04 (ENGINE-NOTES §5j). The doubled gun is expected until M7
(controller weapon). The HUD is expected until M5 (HUD layer). The origin was again first taken from VD's
placeholder, which VD flags as tracked; it recentred 20 ms later, and identity-orientation poses are now ignored.

**Attempt 2 (~20:40):** deployed with the flicker fix (v0.5.1).
**Answers:** **the right eye is good now** · height is good · **world scale is too big** · the player asks for an
in-game menu to adjust it.
**Log:** the origin was set from a real pose on the first try, the right-eye view state was allocated, and an
alt-tab at ~60 s recovered (Reset → S_OK, frames flowing again).
**Verdict:** the flicker is fixed; stereo is working. Scale: the player tunes it with the new in-headset menu (round 4).

---

## Round 4: prepared 2026-09-25, in-headset menu (world scale)
**Changed:** an in-headset menu, drawn by MOHAVR-host, not the game. **Left controller menu button (≡)** opens
and closes it; **left stick** up/down chooses and left/right adjusts; **trigger** (or A) selects; B closes.
Items: **World scale** (live: higher = smaller world, 5 per step), Reset to default (50), Close. Your value
is saved to `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and used next time.
(Also: stereo eye separation no longer depends on the head origin being set.)

**How to try it:** Claude has deployed (stereo + head tracking + host). Launch, Campaign → Continue, open the
menu, and raise World scale step by step until soldiers and doors look right. Close the menu, play a
minute, adjust again if needed. Quit and tell Claude the value you settled on.

**Questions:**
1. Did the menu open with the left menu button, and do the stick and trigger work? (yes/no)
2. Is the menu text readable, and is the panel at a comfortable distance and size? (describe)
3. Which world scale looks right? (number)
4. At that scale, how are your height and the gun (still doubled until M7)? (describe)

**Answers (~21:05):** the menu is good. **Height and scale look right at 100.** The gun is hard to judge.
**Log:** the host loaded the player's saved 100 and the game applied it; the menu opened and closed twice.
**Verdict: PASSED.** The shipped default is now UnitsPerMeter=100.

**Log received:** yes (`logs/modlogs/20260925-210540-*`).

---

## Round 5: prepared 2026-09-25 overnight, controllers, menus/HUD, comfort, mirror, resolution
**Changed** (everything from the overnight checklist; UNATTENDED-REPORT.md has the details):
- **Play with the Touch controllers** (a virtual Xbox pad). The mapping is MOHA's own pad layout: left stick
  moves, right stick turns, right trigger fires, left trigger aims, A reload/use, B switch weapon, X crouch,
  Y jump, grips = alt-fire / grenade, stick clicks = sprint / melee.
- **The left menu button is now shared: a TAP = the game's pause menu, HOLD it (0.6 s) = the MOHAVR menu.**
- The MOHAVR menu has new items: **Height** (seated/standing), **Turning** (smooth / snap 30° / snap 45°) and
  **Recentre** (face forward from where you are). The panel is smaller.
- **Menus are on a flat screen** in front of you instead of split across the eyes. Cinematic cameras go flat
  too (mode 2): you may see this during the parachute landing roll.
- **The HUD is one panel** about 2 m ahead, slightly low, the same in both eyes (no more cross-eyed HUD).
- **The monitor shows the game** (a mirror of the headset's view over the game window) instead of white.
- **Sharper image:** the game now renders at 2880×1620 (each eye 1440×1620, was 960×1080).
- If the VR host ever crashes, the game drops back to a normal flat game instead of freezing the view.

**How to try it:** Claude has deployed this configuration (Virtual Desktop runtime). Start Virtual Desktop,
launch MOHA from Steam, put the headset on.
1. Main menu: it should be a flat screen in front of you. Navigate it with the left stick and A (the game's
   own pad navigation), or the mouse.
2. Campaign → Continue. During the parachute descent and landing, notice whether the view goes flat for a
   moment (the landing roll) and whether that's OK.
3. Play for a few minutes with the controllers: walk, turn, shoot, reload, crouch, jump, throw a grenade.
4. Tap the left menu button: the game's pause menu (flat screen). Tap again or B to resume.
5. Hold the left menu button: the MOHAVR menu. Try **Turning** → snap 30 and snap 45 (flick the right stick),
   **Height**, and **Recentre** (turn your chair first, then Recentre).
6. Look at the HUD panel (compass bottom left, ammo bottom right).
7. Quit the game from the pause menu.

**Questions:**
1. Controllers: does everything in step 3 work? Anything mapped badly or missing? (describe)
2. Stick feel: are the deadzone and smooth-turn speed OK? (describe)
3. Tap = pause and hold = MOHAVR menu: does it work reliably? (yes/no)
4. Turning: smooth, snap 30 or snap 45: which do you prefer? (answer)
5. Height and Recentre: do they work as expected? (yes/no)
6. Menus on the flat screen: readable and comfortable, and do they switch back to 3D when you resume? (describe)
7. The landing roll going flat, and any cutscene you saw: better flat, or leave it 3D? (answer)
8. HUD panel: readable? At a good depth and size? Too low or high? (describe)
9. Is the image sharper than before, and still smooth (no stutter)? (describe)
10. Monitor: does it show the game while you play (not white)? (yes/no)
11. Anything that got worse compared with round 4? (describe)

**Answers (2026-09-26, ~17:00):**
1. Controls need remapping: jump A, reload B, crouch = flick the right stick down, sprint a toggle instead of
   hold, switch weapon Y, grenade X, interact the right grip. Other bindings can wait.
2. Deadzone and turn speed are OK for now.
3. Tap = pause / hold = menu: works.
4. **Smooth** turning is best; the snaps are good options.
5. Height and Recentre: yes.
6. Flat-screen menus work well. At the first title screen the player was way higher than the screen; it was
   in front of them when they returned to the title screen to exit.
7. Cutscenes/landing roll: **leave in 3D**; comfort can be refined later.
8. HUD panel: good.
9. Image better and smooth.
10. Monitor shows the game: yes.
11. The gun got stuck aiming up. After tabbing out (and moving the window while out) and back in: cross-eyed
    double vision.

**Log received:** yes (`logs/modlogs/20260926-170931-*`). XR steady at 90 Hz; snap turns, the Recentre
and the menu logged; 55 s of flat screen while tabbed out; no device reset or resize.

**Verdict: PASSED** (controllers, menu button, turning, Height/Recentre, menus on the screen, HUD, sharpness,
mirror). Shipped defaults now: Input.Controllers=1, Camera.CinemaScreen=1, HUD.Mode=1, Bridge.Mirror=1,
Render 2880×1620. **Follow-ups (round 6):** the new layout; the gun/double vision fixed with Aim.HeadPitch
(stray mouse input changed the game's pitch, which the view ignores but the gun follows; tab-out + window
move didn't reproduce in the simulator at 1080p or 2880×1620); the title-screen height (it came from Virtual
Desktop's untracked start-up pose).

**Deployed for this round** (`tools\deploy.ps1 deploy -Set ...`): `Input.Controllers=1`,
`Camera.CinemaScreen=2`, `HUD.Mode=1`, `Bridge.Mirror=1`, `Render.ResX=2880`, `Render.ResY=1620`
(RuntimeJson empty, so the headset runtime is used). If anything is bad, Claude can turn single features
off; each has its own switch. Not on: `Weapon.HideViewModel` (the gun stays; say if
you want it hidden) and `Comfort.SnapTurn` (choose in the menu).

---

## Round 6: prepared 2026-09-26, the new control layout, head-pitch aim, title-screen height
**Changed:**
- **Controls, as you asked:** A jump, B reload, right grip interact, Y switch weapon, X grenade, **flick the
  right stick down = crouch / stand**, **left stick click = sprint toggle** (click again, or stop moving, to
  stop). Triggers, left grip (alt fire), right stick click (melee) and the menu button are as before.
  (B and the right grip both press the game's reload/use button, so the game picks reload or interact.)
- **Aim up/down follows your head.** The game's pitch is now taken from the headset, so the gun no longer
  drifts up, and shots go up/down where you look. Left/right still follows your body.
- The flat screen at the title screen is placed at your real head height (it used Virtual Desktop's start-up
  placeholder before).
- Now the defaults (no special setup): controllers, menus on the flat screen (cutscenes stay 3D), HUD panel,
  mirror, 2880×1620.

**How to try it:** Claude has deployed. Start Virtual Desktop, launch MOHA from Steam, headset on.
1. Title screen: is it at eye level from the start?
2. Campaign → Continue. After landing: jump (A), crouch and stand (flick right stick down twice), sprint (click
   left stick while moving forward, then click again), switch weapon (Y), grenade (X, then fire), reload (B),
   open a door or pick something up (right grip).
3. Look up and down while shooting at something: does the gun point where you look, and do the shots land
   there (up/down)?
4. Tab out, move the window, tab back in (like last time): any double vision?

**Questions:**
1. Title screen at eye level at start? (yes/no)
2. Do all the new buttons do what they should? Anything to change? (describe)
3. Sprint toggle: comfortable? Does it stop when it should? (describe)
4. Does the gun stay pointing where you look, and do shots go up/down where you look? (yes/no)
5. After tab-out and back: any double vision? (yes/no)
6. Anything worse than round 5? (describe)

**Answers (2026-09-26, ~17:40):**
1. Title screen at eye level: yes.
2. New buttons: yes. **Can the A button select in menus?**
3. Sprint toggle: good.
4. The gun aims up and down (with the head).
5. **Double vision repeated** (after tab-out + moving the window).

**Log received:** yes (`logs/modlogs/20260926-174244-*`). The tab-out is 17:38:22–34, with a 1.1 s stall while
the window was dragged; no device reset.

**Found (simulator, reproduced):** the game window at 2880×1620 is bigger than the 2560×1440 desktop. Dragging it
lets Windows shrink it to fit, and UE3 then draws the viewport into the top-left of the unchanged 2880×1620
backbuffer, so each half the host sends to an eye holds parts of both eyes. **Fixed:** `Render.LockWindow`
(default 1) subclasses the game window once its client equals the backbuffer and refuses size changes (moves
are fine). Verified: a resize to 1936×1119 → refused, client stays 2880×1620, stereo intact. **A in menus:**
while a game menu is open (the cursor test) the pad uses `[ControlsMenu]` (A select, B back, X/Y as
labelled). Held buttons are ignored across the switch. Verified: title screen → A Campaign → down → A
Continue into play; pause → B → resume → A jumps (+121).

**Verdict: PASSED** except the double vision (fixed now, round 7).

**Deployed for this round:** the shipped defaults, no overrides (RuntimeJson empty = the headset runtime).

---

## Round 7: prepared 2026-09-26, double vision after moving the window; A in menus
**Changed:**
- The game window can no longer be resized (it can still be moved). Resizing it was what caused the double
  vision after tabbing out and moving it.
- In the game's menus, the face buttons work as labelled: **A selects, B goes back**. In play they're your
  layout again (A jump, B reload, ...). A button you're still holding when a menu opens or closes is ignored
  until you let go.

**How to try it:** Claude has deployed. Launch as usual.
1. Title screen: use the left stick and **A** to pick Campaign → Continue; B to go back a screen.
2. In play, tap the menu button (pause), then B to resume, then A: you should jump (not select anything).
3. Tab out, drag the game window around (try to make it smaller too), tab back in.

**Questions:**
1. A selects and B backs out in the game's menus? (yes/no)
2. After leaving the pause menu, do A/B work normally in play right away? (yes/no)
3. After tab-out and dragging the window: any double vision? (yes/no)
4. Anything else odd? (describe)

**Answers (2026-09-26, ~18:08):**
1. A selects / B backs out in menus: yes.
2. A/B normal in play right after the pause menu: yes.
3. No longer double vision after tab-out + moving the window, but **"like looking through a glass bowl"**.
4. **Bullet holes in walls only appear in the left eye.**

**Log received:** yes (`logs/modlogs/20260926-181009-*`). The only lock event was a refused resize to 160×28:
the window being **minimized**. The lock forced the full size onto a minimized window. XR stayed at 90 Hz and
game frames kept coming after the restore.

**Found and fixed (simulator):**
- **Glass bowl (likely):** the lock no longer touches a minimized window (minimize and restore pass
  through). Simulator: minimize → restore → client back to 2880×1620, stereo normal. The distortion itself
  didn't reproduce in the simulator, so this is the probable cause, not a proven one.
- **Bullet holes, left eye only:** reproduced (holes in one eye). The engine's decal screen-box test
  (`0x10A2ABB0`) projects to absolute pixels but clamps to `[0, SizeX]`, so every decal is culled in a view
  that starts at x = 1440. Experiments proved it: swapping the eye order made no difference; swapping the
  halves moved the holes to the other eye. Fix `Render.DecalFix` (default 1): for such a view, the function
  runs with the view's X at 0 and its box is shifted back. Simulator: the same hole cluster now shows in both
  eyes (ENGINE-NOTES §5r).

**Deployed for this round:** the shipped defaults, no overrides.

---

## Round 8: prepared 2026-09-26, bullet holes in both eyes; minimize/restore
**Changed:**
- Bullet holes (and other decals, like scorch marks) now show in **both** eyes.
- Minimizing the game (or tabbing away so it minimizes) no longer fights the window-size lock. That's the
  likely cause of the "glass bowl" look after tabbing back.

**How to try it:** Claude has deployed. Launch as usual.
1. Shoot a wall up close: are the holes in both eyes?
2. Tab out the way you did last time (and minimize the game if that's what happened), move the window, tab
   back in.

**Questions:**
1. Bullet holes in both eyes? (yes/no)
2. After tabbing out and back: normal, double vision, or the glass-bowl look? (describe)
3. If the glass bowl is back: is it the whole view or only part of it, does it go away if you pause/unpause,
   and how did you leave the game (Alt+Tab, Windows key, taskbar, Virtual Desktop menu)? (describe)
4. Anything else odd? (describe)

**Answers (2026-09-26, ~18:55):**
1. Bullet holes in both eyes: **yes**.
2. The glass-bowl look is back. The player left with a **double press of the Quest menu button** (Virtual
   Desktop's "back to the desktop") and took a Quest screenshot
   (`%TEMP%\VirtualDesktop.Android-20260926-185331.jpg`): a normal-looking eye image with a strip of a second
   view at its right edge.

**Log received:** yes (`logs/modlogs/20260926-185818-*`). The game kept rendering stereo views throughout
(`stereo eye 0/1`). The tab-out shows as flat-screen mode 18:52:16–30. The session never left FOCUSED except a
12 ms blip at 18:54:05. No resize or minimize.

**Not reproduced:** in the simulator, focus loss and return (window, minimize, move) leaves the composited
headset view normal. The strip at the right edge may just be Virtual Desktop's screenshot covering more than
the eye image (sampling past its edge), so it doesn't by itself show the fault. **Next:** change logging added
(game: each eye's view rect and final FOV; host: stereo/hasView and the submitted FOVs, and the runtime's own
eye FOVs) to catch what changes. Round 9 = reproduce with a screenshot before and after.

**Verdict:** bullet holes **PASSED**; glass bowl open.

**Deployed for this round:** the shipped defaults, no overrides.

---

## Round 9: prepared 2026-09-26, "glass bowl" diagnostics
**Changed:** nothing you'll notice. The mod now logs what changes in the picture it sends (field of view, eye
rectangles), so the log can show what goes wrong when you come back from the desktop.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Before leaving: take a Quest screenshot while everything looks right.
2. Leave exactly as last time (double press the Quest menu button), do what you normally do on the desktop,
   come back.
3. If the glass bowl appears: take another screenshot, then pause and unpause (menu button tap) and say whether
   that changes anything. Keep playing ~30 s, then quit.
4. Tell Claude the times roughly (or just the screenshot file names).

**Questions:**
1. Did the glass bowl appear? (yes/no)
2. Did pause/unpause change it? (yes/no)
3. What did you do on the desktop (click the game window, move it, other apps)? (describe)

**Answers (2026-09-27, ~17:45):** the player double-pressed the menu button, moved the MOHA window,
**minimized it, maximized it**, went back into play: the glass bowl appeared. Screenshots of both eyes before
and after (`Downloads/GunVR-r154/VirtualDesktop.Android-20260927-1741..1743*.jpg`): after, each eye is magnified
and the left eye shows a strip of the other view.

**Log (`logs/modlogs/20260927-174851-*`), the diagnostics caught it:** `diag: eye 0 view rect x 0 y 0 1280 x 1369
(was 0 0 1440 1620)`. The game's viewport had become the maximized window's client area (2560×1369 on the 2560×1440
desktop) inside the unchanged 2880×1620 frame. The host kept splitting at 1440, so each eye got a smaller,
stretched image plus part of the other. The FOVs and the headset runtime were unchanged.

**Why the lock let it through:** restoring from minimized into a maximized window arrives while the window still
has `WS_MINIMIZE` (let through since round 8). And a maximize can't be held at the render size anyway: asked for
2896×1659, Windows made it 2576×1408 (traced). **Fix:** no maximizing. The maximize box is removed, `SC_MAXIMIZE`
is swallowed, a window that is maximized anyway is restored at once, and only the move into the minimized size is
let through. The style change is posted to the window's own thread: done from the render thread it deadlocked
the game (seen once in the simulator, killed, user data restored). [S]: maximize command → refused; `ShowWindow`
maximize → restored; minimize → maximize → restored; the client stays 2880×1620 and the eye rects never change.

**Verdict:** glass bowl found; fix in round 10.

**Deployed for this round:** the shipped defaults, no overrides.

---

## Round 10: prepared 2026-09-27, the glass bowl (maximizing the game window)
**Changed:** the game window can't be maximized any more: no maximize button, and double-click or Win+Up does
nothing. If something maximizes it anyway (like restoring from the taskbar), it snaps straight back.
Maximizing was what shrank the picture and gave the glass bowl. Minimizing and moving the window still work.

**How to try it:** Claude has deployed. Launch as usual.
1. Do exactly what you did last time: double press the menu button, move the window, minimize it, try to maximize
   it (button, double-click on the title bar, restore from the taskbar), go back into the game.

**Questions:**
1. Any glass bowl or double vision after coming back? (yes/no)
2. Anything else odd? (describe)

**Answers (2026-09-27, ~18:30):** "Fixed".

**Log received:** yes (`logs/modlogs/20260927-183010-*`). The lock took hold at 2896×1659 (client 2880×1620), the
maximize button was removed on the window's own thread, and the eye view rects stayed 1440×1620 for the whole
session (no change logged after the first frame). No refusal was logged, so with the button gone nothing even
asked to maximize.

**Verdict:** glass bowl **PASSED**. `Render.LockWindow=1` stays the default.

**Deployed for this round:** the shipped defaults, no overrides.

---

## Round 11: prepared 2026-09-27, aiming with the right controller (M7)
**Changed:** your shots now go where your **right controller points**, not where your body faces. A small red
dot shows where they will land, on whatever the controller points at. The gun model still sits in front of your
face as before (putting it in your hand is the next step), and the game's own crosshair on the HUD still shows the
middle of your view, so go by the red dot. [S] proven in the simulator: the bullet holes land on the aim point.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Point the right controller at things near and far, and watch the red dot.
2. Shoot at walls and enemies, pointing well to the side of where you're looking too.
3. Try aiming down the sights (left trigger) once.

**Questions:**
1. Does the red dot sit on what the controller points at, and follow it smoothly? (yes/no, describe)
2. Do the shots (bullet holes, hits on enemies) land on the red dot? (yes/no)
3. Dot size and colour: fine, too big, too small, hard to see? (describe)
4. Hand aiming or head aiming (shots land where you look, no dot): which do you want to try next or keep? (hand / head / the old way)
5. The gun in front of your face while you aim elsewhere: bearable until it moves to your hand, or hide it for now? (keep / hide)

**Answers (2026-09-27, ~19:10):** 1. yes. 2. Yes. 3. Good. 4. Hand. 5. keep for now.

**Log received:** yes (`logs/modlogs/20260927-191114-*`): the hook was in, the player's shots took the hand aim, and the
XR frame held 90 Hz (11.1 ms average, 0–1 late frames in 900) with the per-frame trace.

**Verdict:** hand aiming **PASSED**. `Aim.Mode=3` (right hand) becomes the shipped default, with the reticle; the gun
stays visible until M8 puts it in the hand.

**Deployed for this round:** the shipped defaults plus `Aim.Mode=3` (right hand; the reticle is on by default).

---

## Round 12: prepared 2026-09-27, the gun in your hand (M8, first step)
**Changed:** the gun (with both arms holding it) is now drawn at your **right controller**, pointing where it
points, instead of in front of your face. It's also drawn in true 3D, so it shouldn't look doubled any more.
Only the drawing moved; the game itself still thinks the gun is in front of the camera. The arms come along as
one piece, so they may look detached from your body. [S]: the simulator shows the gun at the test hand, in both eyes.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Hold the controller as if it were the gun's grip; move it around, look at the gun from the side.
2. Shoot at a few things; check that the barrel points at the red dot.
3. Switch weapons (Y) once, and aim down the sights (left trigger) once.

**Questions:**
1. Is the gun in your hand and does it follow the controller smoothly? (yes/no)
2. Is it still doubled? (yes/no)
3. Where does it sit relative to your real hand: too far forward/back, left/right, up/down, tilted? (describe)
4. Does the barrel line up with the red dot? (yes/no, which way it's off)
5. Anything odd with other weapons or aiming down the sights? (describe)

**Answers (2026-09-27, ~19:40):** 1. Yes. 2. No. 3. feels ok. 4. "A bit low", with a screenshot of the pistol against a
wall (`OneDrive/Pictures/VirtualDesktop.Android-20260927-193819.jpg`). 5. no.

**Log received:** yes (`logs/modlogs/20260927-194043-*`). The pistol's origin sits at 37.6 / 11.3 / −12.5 in the camera
frame (the rifle's 34.2 / 11.2 / −16.7), so the fixed grip holds each weapon slightly differently.

**Reading the screenshot:** the red dot (the shots) is ~217 px below the line of the pistol's barrel on a wall about
as far away as the gun (slide ≈ 21 cm = 540 px) -> the barrel runs ~8 cm above the controller's aim ray, parallel to
it. **Fix:** the aim ray is raised to the barrel, `Aim.RayUp=8` (cm along the controller's up; with the gun in the
hand only), in the game's aim and in the host's reticle alike.

**Verdict:** gun in hand **PASSED** (not doubled, fits); the aim line follows in round 13.

**Deployed for this round:** the shipped defaults plus `Weapon.ViewModel=2` (grip 34 / 11 / −17).

---

## Round 13: prepared 2026-09-27, the aim along the gun's barrel, and the Gun fit page
**Changed:** the aim (and the red dot) now runs 8 cm higher, along the gun's barrel instead of from the controller's
tip. The gun in your hand is now the default. **New, at your request:** a **Gun fit** page in the MOHAVR menu (hold
the menu button → Gun fit). It shows the gun you're holding, and the left stick adjusts, live: gun forward/back,
right/left, up/down, gun angle, aim line up/down and right/left. Each gun keeps its own fit (saved in your settings);
"Reset this gun" goes back to the defaults. The red dot stays visible while the menu is open, so you can line it up
with the barrel. [S]: in the simulator the game named the StG 44, Colt 45 and BAR, the menu's changes reached the
aim straight away, and the saved fit came back when the gun did.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. With the pistol, look along the barrel at the red dot on a wall. If they don't line up, open Gun fit and move the
   aim line (and the gun, if its place in your hand feels off) until they do.
2. Do the same with the rifle (Y switches weapons; switch with the menu closed).

**Questions:**
1. Did the barrel line up with the red dot before you adjusted, with the pistol? (yes / low / high)
2. And the rifle? (yes / low / high / left / right)
3. Was the Gun fit page easy to use; is anything missing (a step too big or small, another adjustment)? (describe)
4. After fitting, do the shots land where the barrel points? (yes/no)

**Answers:** not played -- the player asked for the hand features first; round 14 carries these questions.

**Deployed for this round:** the shipped defaults (now `Weapon.ViewModel=2`, `Aim.RayUp=8`), no overrides.

---

## Round 14: prepared 2026-09-27 night, hand features (the player's list) + round 13
**Changed** (all [S]-proven in the simulator; the new ones are switched on for this round only):
- **Holsters:** squeeze a grip with your hand over your **right shoulder** = long gun 1, **left shoulder** = long gun 2,
  **right hip** = pistol, **left hip** = grenade. **Either hand can draw; the hand that draws holds the gun** -- the gun
  is drawn at it and its trigger fires. A short buzz tells you the hand is at a holster.
- **Foregrip:** with the gun in one hand, squeeze the other hand's grip at the gun's foregrip (about 30 cm ahead) to
  hold it with two hands -- the gun then points from your gun hand through the other. Where the foregrip sits is
  adjustable per gun in Gun fit ("Foregrip forward", "Foregrip up / down").
- **Reload gesture:** the other hand's grip squeezed at the gun's magazine (just under and ahead of the gun hand).
- **Throwing grenades:** with a grenade in hand, swing and let go of the trigger -- it flies with your hand's speed and
  direction (x1.5). A slow release (under 1 m/s) throws the game's own way.
- **Menu:** "Sticks" (move with the right stick and turn with the left) and "Gun hand" (the hand at start).
- From round 13 (not played yet): the aim along the barrel (8 cm above the controller) and the **Gun fit** page.
- Known: in your left hand the gun is still held by a right arm reaching across (the arms model is right-handed).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Draw each weapon from its holster, with each hand. Switch hands by drawing with the other one.
2. Hold the rifle with two hands via the foregrip; aim, shoot. Try the Gun fit foregrip settings if it's off.
3. Reload with the other hand at the magazine.
4. Draw a grenade (left hip) and throw it overhand, underhand, softly.
5. Round 13: look along the pistol's and the rifle's barrel at the red dot; fix it in Gun fit if needed.
6. Try Sticks swapped for a minute.

**Questions:**
1. Holsters: do the four spots sit where you expect; do they trigger when you mean them to and not otherwise? (describe)
2. The hand that draws holds the gun: does that work in play; is the right arm reaching across OK for now? (describe)
3. Foregrip: does two-handed aiming work and feel steady; is the foregrip point in the right place? (describe)
4. Reload gesture: does it trigger reliably, and only when you mean it? (yes/no, describe)
5. Grenades: do they go where and as far as you throw them? Too weak / too strong (ThrowScale)? (describe)
6. Sticks swap and Gun hand in the menu: work as expected? (yes/no)
7. (Round 13) Before adjusting, did the pistol's barrel line up with the red dot? (yes / low / high) And the rifle's?
8. (Round 13) Was the Gun fit page easy to use; anything missing? After fitting, do shots land where the barrel points?
9. Anything else odd? (describe)

**Answers (2026-09-28):**
1. "Holsters are not quite where I want them, can you add a ring around where they are, make a menu toggle to have the
   ring be visible (always, when hand is near and never visible) and make the holster size and position be adjustable."
2. "works well, arm IK is part of next milestone right?"
3. "feels good, will fine tune after arm IK"
4. "It feels good, happens accidentally sometimes, but only because you can't tell where your offhand is."
5. "Needs further testing"  6. "Untested"
7. "Don't remember, I will adjust weapons in game later and when I tell you it's done we will use those values as
   defaults."  8. "All good"

**Log received:** yes (`logs/modlogs/20260928-155246-*`): every holster fired (RightShoulder, LeftShoulder, RightHip,
LeftHip, with both hands; the gun hand followed), foregrip taken 8-11 cm from the point (the 12 cm catch is doing the
work: the default point sits a little off), two reload gestures. No grenade release reached the throw code (not tested).
The XR frame fell to 10 Hz from 05:15 with the session still FOCUSED: the headset was off the head (Virtual Desktop idles).

**Verdict:** holsters, the drawing hand, foregrip, reload gesture **PASSED** as mechanics; holster placement needs
visible, adjustable spots (round 15); the reload gesture needs to show where the off hand's gesture spots are;
throwing and the stick swap still to test; per-gun fits later from the player's own values.

**Deployed for this round:** the shipped defaults plus `Holsters.Enabled=1`, `Hands.Foregrip=1`,
`Hands.ReloadGesture=1`, `Hands.Throw=1`.

---

## Round 15: prepared 2026-09-28, holster rings and adjustable holsters
**Changed:**
- **Rings** around the gesture spots: each holster, and the other hand's foregrip and magazine spots on the gun (so
  you can see where your off hand's gestures are). A ring lights green while your hand is inside it -- a squeeze
  there acts. A small white dot marks your off hand (the game shows nothing there).
- **Menu -> Holsters** (new page): pick a holster (right shoulder, left shoulder, right hip, left hip) and move it
  right/left, up/down, forward/back (cm from your head) and change its size. Every ring shows while the page is open.
  "Rings: never / near / always" -- near (the default) shows a ring when a hand comes within twice its size.
  Your holster places and the ring setting are saved.
- Holsters, foregrip and the reload gesture are now on by default (round 14 passed). Throwing is on for this round.
- [S]: in the simulator the foregrip ring showed amber with the off hand close and green with it inside, the dot on
  the off hand; the Holsters page showed all four rings, and moving a holster moved its ring.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Open the menu -> Holsters. Put each holster where you want it (look down to see the hips' rings), and size them.
2. Play with Rings on near; try always and never too.
3. Reload with the off hand now that you can see the magazine spot.
4. Still to test from round 14: throw grenades (left hip, swing, let go of the trigger), and Sticks swapped.

**Questions:**
1. Can you put each holster where you want it and size it? Anything missing on the page? (describe)
2. Rings: which setting do you like; are they too big, too bright, in the way? (describe)
3. Reload gesture: fewer accidents now that the magazine spot shows? (yes/no)
4. Grenades: do they go where and as far as you throw them? Too weak / too strong? (describe)
5. Sticks swapped: works as expected? (yes/no)
6. Anything else odd? (describe)

**Answers (2026-09-28):** 1. "Holsters working great" 2. "Rings are fine" 3. "Yes" (fewer accidental reloads)
4. "A little too weak" 5. "Yes" (sticks swapped)

**Log received:** yes (`logs/modlogs/20260928-162210-*`): 11 hand throws at 3-9.5 m/s (x1.5 -> ~1100-1400 units/s,
where the game's own full throw is ~1900). One release caught a pooled grenade lying still 250 units away (velocity 0)
instead of the new one, so that throw went the game's way. **Fixes:** `ThrowScale` 2.2 (a typical 8.5 m/s throw =
the game's full strength); a thrown grenade must be within 60 units of the eye and already moving (measured: 4-14
units). [S]: two test throws caught (the pooled grenade relaunched the second time), 1760 units/s forward.

**Verdict:** rings, holster adjustment, fewer accidents, stick swap **PASSED**; throwing PASSED with the strength
raised (now on by default, `Hands.ThrowScale=2.2`).

**Deployed for this round:** the shipped defaults (now holsters, foregrip and the reload gesture on) plus
`Hands.Throw=1`.

---

## Round 16: prepared 2026-09-28, arm IK
**Changed:** the arms now reach from your shoulders to the gun instead of moving with it as one piece. Your hands stay
on the gun; the shoulders sit where yours are (18 cm to each side, 22 cm below your eyes, turned with your body), each
elbow bends naturally, and if you reach further than the arm is long the shoulder follows. The body/legs under you stay
under you. (Also in this build: grenades throw at 2.2x, and a thrown grenade can no longer be mixed up with one lying
around.) [S]: in the simulator the shoulders land where they should and the arms connect without stretching.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Hold the rifle one-handed and two-handed (foregrip); move it around, high, low, across your body; look at your arms.
2. The pistol, reaching out and close to your chest.
3. Throw a grenade once or twice (stronger now).

**Questions:**
1. Do the arms look connected to you and follow naturally? (describe)
2. Shoulders in the right place (too wide / narrow / high / low / forward / back)? (describe)
3. Elbows bending the right way? Any strange stretching or twisting? (describe)
4. Any lag or wobble of the arms when you move fast? (yes/no)
5. Grenades now strong enough? (yes / too weak / too strong)
6. Anything else odd? (describe)

**Answers (2026-09-28):** 1. "Yes" 2. "Seem ok" 3. "When twisting arm, shoulder and bicep twist all the way around
when hand is only twisted 90 degress or so." 4. "Didn't notice, needs further testing." 5. "All good".
The player also asked: take the off hand off the gun unless it holds the foregrip, and have it follow its controller
when not in use (not on the foregrip; pistol / grenade).

**Log received:** yes (`logs/modlogs/20260928-172408-*`).

**Cause of 3:** the upper arm was turned from its gun-moved pose, which carries the hand's whole roll. **Fix:** the upper
arm and the forearm bone are turned from the body's pose (no twist); only the forearm roll bones take the hand's twist.

**Verdict:** arm IK **PASSED** (connected, shoulders OK; now on by default); the twist and the free hand in round 17.


**Deployed for this round:** the shipped defaults plus `Weapon.ArmIK=1`.

---

## Round 17: prepared 2026-09-28, the twist fix and the free off hand
**Changed:**
- **Twist:** turning your wrist no longer spins the bicep and shoulder -- only the forearm turns with the hand.
- **The off hand is free:** unless you're holding the foregrip, the model's other hand leaves the gun and follows your
  other controller (with a pistol or a grenade too). Take the foregrip and it goes back on the gun.
- A hand just out of reach now stretches the arm a little (up to 30%) before the shoulder follows -- a far reach had
  torn the sleeve into a flat "sail".
- Arm IK is on by default now.
- [S]: in the simulator the free hand follows the other controller with its arm reaching to it, goes back on the gun
  on the foregrip, the far foregrip pose shows one sleeve with no sail, and a rolled wrist turns the forearm only.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. With the rifle: turn your gun wrist both ways; hold and let go of the foregrip; wave the free hand around.
2. With the pistol and a grenade: the free hand.

**Questions:**
1. Wrist twist: does the upper arm stay put now? (yes/no)
2. The free hand: does it follow your controller, and does it look right (its angle, where it sits)? (describe)
3. Does it snap back onto the foregrip cleanly when you grab it? (yes/no)
4. Any stretching, sails or odd shapes? (describe)
5. Arm lag or wobble on fast moves? (yes/no)
6. Anything else odd? (describe)

**Answers (2026-09-28):** 1. "Yes" (the upper arm stays put). 2. "It follows the hand, position is off, could use menu
to adjust or just make it relative to other hands position. With long gun equipped, arm jutters when gun is moved, like
it's trying to move with it but being pulled back. With pistol equipped, wrist twists constantly and arm twists
excessivley with controller twist." 3. "Yes" 4. "Just what I mentioned already" 5. "Seems ok".
Also: "let the grenade position be rotated further than 45 degrees"; "guns lose the ability to aim where I fire them at
distance. If the crosshair isn't there, they fire from and to somewhere else"; "Remove foregrip hold from pistols and
grenades and reload from grenades."

**Causes and fixes (round 18):**
- The jitter: the arms' pose was solved with the gun's move from the previous frame but drawn with the current one --
  the free arm moved with the gun for a frame and snapped back. Now the move is baked into the arms' AND the gun's bone
  matrices at the same point of the same frame (just before the pose goes to the renderer), and the renderer draws them
  as they are.
- The free hand's place and the pistol's constant wrist twist: it followed the game's animated support hand. Now it is
  the mirror image of the gun hand's grip, on the other controller.
- The arm twisting too much: the forearm roll bones took all the wrist's roll; now 60% of it.
- Shots off at distance: MOHA's weapon spread (hip fire is wide and grows while turning -- the head always moves in VR).
  `Aim.Spread` (default 0) scales it for the player's shots.
- Gun angle up to +-180 degrees; the foregrip for long guns only; no reload gesture with grenades (the game now tells the
  host what the weapon is: a long gun, a pistol or a grenade, by its class).

**Verdict:** the twist fix PASSED; the free hand, the jitter, spread and the weapon rules in round 18.


**Deployed for this round:** the shipped defaults (now `Weapon.ArmIK=1`, `FreeOffHand=1`), no overrides.

---

## Round 18: prepared 2026-09-28, steadier arms, the free hand mirrored, no spread
**Changed:**
- **No more arm jitter:** the arms and the gun are now moved together, in the same frame.
- **The free hand** is the mirror image of your gun hand's grip, on your other controller (not the game's animated hand).
- **The forearm** turns with 60% of your wrist's roll (it was 100%: too much, especially with the pistol).
- **Shots go to the red dot, even far away:** the game's spread is off (`Aim.Spread=0`; 1 brings it back).
- **Gun angle** in Gun fit goes to +-180 degrees (grenades).
- **Foregrip:** long guns only. **Reload gesture:** not with grenades.
- [S]: in the simulator the gun is drawn in the hand with the arms baked in the same frame; the free left hand shows a
  mirrored grip behind its controller and rolls with it; shots land exactly on the aim (the spread had moved one ~1 degree).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Rifle: move the gun fast with the free hand away and on the foregrip; watch both arms.
2. Pistol: twist the controller; look at the wrist and forearm. Try to take a foregrip (it shouldn't).
3. Shoot at something far away with each gun; check the hits against the red dot.
4. Grenade: rotate it in Gun fit past 45 degrees if you want; the reload gesture shouldn't fire.

**Questions:**
1. Any arm jitter left? (yes/no)
2. The free hand: in the right place now? (yes / describe)
3. Pistol: wrist and forearm twist OK now? (yes / describe)
4. Far shots land on the red dot? (yes/no)
5. Foregrip and reload rules OK? (yes/no)
6. Anything else odd? (describe)

**Answers:** 1 "Gone" · 2 "The hand points back at the player as if it had been bent back. It is in the right position
for guns but not the grenade." · 3 (not answered) · 4 "No, issue remains. Sometimes close shots don't work either. I
cannot tell what triggers this." · 5 "Yes" · 6 (not answered).

**Log received:** yes, `logs/modlogs/20260928-182307-MOHAVR.log` and `-MOHAVR-host.log`. 6 of 37 logged aim rays
hit at 0.0 m (the ray starting inside something).

**Verdict:** arm jitter PASSED; foregrip/reload rules PASSED; free hand with grenades and shot landing FAILED (round 19).

**Found (round 19):**
- **Shots:** the host held the shared view lock for its whole hands update (OpenXR calls included). A game read that
  met it gave up, and for that frame the shot went back to the game's own aim (straight ahead of the body), and a torn
  gun read counted as "no gun", so the aim fell back to the raw controller. Random, near or far: what the player saw.
  Also the aim trace hit volumes and could start inside geometry (the 0.0 m hits).
- **Free hand:** the long-gun grip it copies was re-taken on every frame of "a long gun" -- including the long gun's
  put-away animation during the switch, so the grenade kept a frame with the hand bent away.

---

## Round 19: prepared 2026-09-28, shots that stay on the red dot, the free hand's own menu
**Changed:**
- **Shots:** the host now holds the lock only for the copies, and the game retries a read instead of giving up; a
  read that still fails keeps the last frame's aim. The aim trace ignores volumes, and a ray starting inside
  something is traced again from 20 cm on.
- **Free hand:** its grip is taken from a long gun held still (not the switch animation), then kept for pistols and
  grenades.
- **New menu page, Free hand:** tilt, turn, roll (5 degrees a step) and forward/back (1 cm) for your free hand, saved
  as yours. Reset puts it back.
- [S]: in the simulator the grenade's free hand sits where the rifle's does; the Free hand page turns it live and
  saves `FreeHand` in your settings; 0 torn reads in ~850 frames per 5 s (the log now counts them, so your headset
  log will show how often it happened).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Shoot at things far and near with the rifle and the pistol, moving around and along walls; check the hits against
   the red dot.
2. Switch to a grenade with your other hand in view. If it still sits wrong, open the menu -> Free hand and tilt / turn
   / roll it until it matches your real hand.
3. Pistol: twist the controller; look at the wrist and forearm.

**Questions:**
1. Do shots land on the red dot now, far and near? (yes / when not)
2. The free hand with the grenade and the pistol: right? If you used the Free hand page, what values? (yes / describe)
3. Pistol: wrist and forearm twist OK? (yes / describe)
4. Anything else odd? (describe)

**Answers:** "Bring shoulders in a bit" · 1 "no, issue persists" · 2 "I turned the off hand around 180 degrees" (saved
`FreeHand=180 0 0 0`: tilt 180) · 3 two screenshots of the off-hand arm, pistol and grenade
(`VirtualDesktop.Android-20260928-184535.jpg`, `-184618.jpg`: the upper arm big in the lower left, the forearm long).

**Log received:** yes, `logs/modlogs/20260928-185105-MOHAVR.log` and `-MOHAVR-host.log`. 0 torn reads in every
5-second line (the lock was not the cause). With the BAR, ~50 s of walking gave "hit at 0.2 m" in every line: the
ray started inside something and the re-trace from 20 cm started inside it too -- the aim then pointed from the eye
at the hand.

**Verdict:** shots FAILED again (cause found in the log, round 20); free hand fixed by the player's tilt 180.

---

## Round 20: prepared 2026-09-28, the aim ray steps out of what it starts in; shoulders in
**Changed:**
- **Shots:** when the aim ray starts inside something, it now steps along 20 cm at a time (up to 1 m) until it's
  out, instead of aiming at a point by your hand. The log names what it started inside, so if it still happens the
  next fix is exact.
- **Shoulders** 6 cm narrower (`ShoulderWidth` 36 -> 30).
- **Free hand:** your tilt 180 is now the default too (your saved setting is unchanged); Reset goes back to it.
- **Elbow** (the player, before playing this round: "In the pistol screenshot the elbow is twisted"): the forearm is
  now carried by the upper arm and bends from there (`Weapon.ElbowHinge=2`), instead of each turning on its own.
- [S]: in the simulator a ray starting inside the roof mesh during the landing steps out and hits beyond it; the
  shoulder and free-hand defaults load. Elbow, looking down at the free arm with the pistol in three poses: the old way
  folds and creases at the elbow; both on a shared hinge (1) was clean at the elbow but pinched the shoulder; carried
  (2) is smooth at both (`logs/shots/r20-elbow-*.png`).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Walk around with the BAR and the rifle, shooting at things near and far, and along walls and past teammates.
2. Look at your arms with each weapon.

**Questions:**
1. Do shots land on the red dot now? (yes / when not)
2. Shoulders: better? Still too wide, or too narrow now? (describe)
3. The elbow: still twisted? Both arms, with each weapon. (yes / describe)
4. Anything else odd? (describe)

**Answers:** 1 "Yes, make red dot toggle for menu. Also, can we remove the in game crosshair?" · 2 "Better" · 3 "Yes,
with pistol, arm is constantly twisting and jittering."

**Log received:** yes, `logs/modlogs/20260928-201303-MOHAVR.log`. The ray started inside **Trigger** actors
(`Trigger_1`, `Var_Flk_Global_Aff_Trigger_8`): standing in one, every 20 cm step hit it again and the aim point
ended 1 m out (3610 "started inside" in one 5-s line); otherwise StaticMeshActors of the level (stepped out of).

**Verdict:** shots PASSED (the player); shoulders better; pistol arm FAILED (round 21).

---

## Round 21: prepared 2026-09-28, red dot switch, no game crosshair, a steady free arm with the pistol
**Changed:**
- **Menu: Red dot** on/off (saved as yours).
- **The game's own crosshair is hidden** (`HUD.Crosshair=0`; 1 brings it back).
- **Shots:** the aim passes through the game's trigger zones, exactly as the game's own bullets do (your log showed
  standing inside one sent the aim to a point 1 m ahead).
- **Free arm with the pistol / grenade:** it now starts from the rifle's arm pose instead of the pistol's own
  (`Weapon.FreeArmPose=1`); the elbow no longer takes its bend from a nearly straight pose (noise flipped it), and
  the forearm's twist no longer flips side to side near 180 degrees.
- [S]: red dot off/on from the menu; the crosshair's bars gone from the game's image (`logs/shots/r21-xh-zoom.png`);
  the aim passes through `Trigger_1` in the landing area; the free arm with the pistol moves 3.7x less between
  frames (0.82 vs 3.00 mean pixel change, hands held still) and sits straighter (`logs/shots/r21-arm-ab.png`).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Menu -> Red dot: off and on again.
2. Pistol: hold it and move/twist both controllers; watch both arms. Then the grenade.
3. Shoot as usual.

**Questions:**
1. Red dot toggle works, and the game's crosshair is gone? (yes/no)
2. Pistol: any twisting or jitter left, in which arm? (yes/no, describe)
3. Shots still on the red dot? (yes/no)
4. Anything else odd? (describe)

**Answers:** 1 "Red dot toggle works." · 2 "Looks good" · 3 "Issue persists."

**Log received:** yes, `logs/modlogs/20260929-164154-MOHAVR.log`. The last minute: aim points at **0.0 m** (at the gun)
with up to 11,296 trigger pass-throughs in 1,066 frames -- two overlapping triggers: each was set back before the
next trace, so they hit in turn until the loop gave up at the gun.

**Verdict:** red dot switch PASSED; pistol arm PASSED; shots FAILED (round 22).

**Found (round 22), with Ghidra:** the player's bullets don't use the script's Trace: EALAWeapon.CalcWeaponFire calls
the native CalcWeaponFireNative (0x10F0CF10), which traces with flags **0x268BF** -- per-poly collision (0x20000),
volumes and material -- where the aim trace used the simple-collision 0x60BF without volumes. The red dot's point came
from simple collision hulls (bigger or smaller than the meshes the bullets hit), so the shot from the eye towards it
met other surfaces. The native passes triggers by keeping every one it passed switched off until it's done.

---

## Round 22: prepared 2026-09-29, the shot and the red dot on the same ray
**Changed:**
- **The red dot now uses exactly the bullets' collision** (the flags the game's own bullet trace uses).
- **Triggers:** passed the way the game's bullets pass them (all kept off until the trace is done) -- no more aim point
  stuck at the gun.
- **Shots start at the gun** and run along the red dot's ray (`Aim.ShotFromGun=1`), unless something is between your
  eyes and the gun (a hand through a wall): the same ray, start and collision as the dot.
- **Every shot is logged** (`Aim.ShotLog=1`, the first 400): where it started, what it hit, and how far from the red
  dot -- so if anything is still off, the log says exactly what and where.
- [S]: 18 shots at three gun angles (2-8 m, slabs and roof): all from the gun, **0 cm** from the red dot's point;
  the game's own shot line within 0.03 deg of the aim (no hidden spread or recoil). With the old start (from the
  eye) and the new collision: 0-2 cm.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Shoot everything: near, far, while walking, along walls, over and around cover, rifle, BAR and pistol.
2. If a shot goes wrong, just keep playing -- the log records every shot.

**Questions:**
1. Do shots land on the red dot now? (yes / when not)
2. Is the game's crosshair gone? (yes/no)
3. Anything else odd? (describe)

**Answers:** 1 "Yes, I only played a bit but was able to shoot where I wanted." · 2 "It is gone, could you also get rid
of the hit detection marker, a red cross shows on screen when you shoot an enemy." · 3 "Sprinting has an animation
that overrides the arms. Guns have a visible tracer that does not come from the gun barrel, I'd like to either hide it
or have it come from the barrel."

**Log received:** yes, `logs/modlogs/20260929-171948-MOHAVR.log`: 225 shots, all from the gun; 210 within 2 cm of the
red dot; the rest: both ray and dot hitting nothing (the weapon's range 163.8 m vs the dot's 300 m), an enemy stepping
into the line (21 m / 36 m), an ammo pickup (14 cm).

**Verdict:** shots PASSED; crosshair gone PASSED. New: the hit marker, sprint, tracers (round 23).

---

## Round 23: prepared 2026-09-29, no hit marker, no tracers, the gun stays in the hand while sprinting
**Changed:**
- **The red hit cross is hidden** (`HUD.HitMarker=0`), the same way as the crosshair.
- **Tracers are off** (`Weapon.Tracers=0`): the game starts them at the gun's barrel in its own pose (round 26: not a separate
  third-person gun as thought here; the unseen body's
  hand), not at the gun in your hand. Starting them at your gun's barrel is possible later (more work).
- **Sprinting:** the sprint animation no longer carries the gun and arms away (`Weapon.SprintLock=1`): the gun hand is
  held where it was just before, eased in and out.
- [S]: sprinting in the simulator, the gun hand stays within 2 cm of where it was in its controller's frame (without
  the lock it swung 4-38 cm); the rifle's tracers switch off (CreateTracers 1 0 -> 0 0). The hit cross only shows on a
  hit, which the simulator's start has no enemy for: [H].

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Shoot some enemies: the red cross shouldn't appear.
2. Sprint with the rifle, the BAR and the pistol; watch the gun and both arms, and when you stop.
3. Fire long bursts: no tracers.

**Questions:**
1. Hit cross gone? (yes/no)
2. Sprinting: the gun stays in your hand, arms look right, starting and stopping smooth? (yes / describe)
3. Tracers gone -- and do you want them back from the barrel later? (yes/no)
4. Anything else odd? (describe)

**Answers:** 1 "Yes" · 2 "The gun stays in hand at first but is jittering as if it is being pulled to sprint animation
position. Occasionally it enters the animation." · 3 "Gone and can stay gone."

**Log received:** yes, `logs/modlogs/20260929-175244-MOHAVR.log` and `.prev.log` (two game runs). The speed-detected
lock fired only during the parachute glide (a false positive) and **never on the ground**: the game's sprint threshold is
per weapon (GroundSpeed x MoveSpeedMultipler x 1.01: Stg44 435.5, BAR 420.7, Colt 494.9) and the lock's was ~500, so the
sprint animation mostly played with no lock; when the speed hovered near 500 the lock's 100 ms ramp-in restarted on every
flicker -- the pull toward the sprint pose.

**Verdict:** hit cross PASSED; tracers off PASSED (stay off); sprint FAILED (round 24).

---

## Round 24: prepared 2026-09-29, the arms no longer play the sprint animation
**Changed:**
- **Sprinting:** instead of fighting the game's sprint animation, the first-person arms simply don't play it: while you
  sprint they hold the gun as when standing (`Weapon.SprintArms=idle`; `walk` and `game` are the alternatives). The gun
  can't be pulled out of your hand, and the sprint's view shake goes too. Sprinting itself (speed, zoom, sound) is
  unchanged.
- The old speed-detected sprint lock is gone.
- [S]: sprinting in the simulator, the gun hand turned up to **1 deg** in its controller's frame with `idle` (84 deg with
  the game's animation); side-by-side screenshots standing vs sprinting show the gun in the same place
  (`logs/shots/r24-sprint-ab.png`). The log now records every sprint: "armik: sprint of N ms -- the gun hand moved up to
  X cm and turned up to Y deg". An adversarial review of the change (3 lenses, each finding checked by a skeptic)
  confirmed two minor issues -- the per-sprint log across a level change, and the switch being skipped with
  ViewModel=0 -- both fixed.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Sprint with the rifle, the BAR, the pistol and a grenade: straight, diagonally, with the stick half pushed.
2. Start and stop sprinting a few times; fire or reload right after a sprint.

**Questions:**
1. Does the gun stay in your hand while sprinting, with no jitter or pull? (yes / describe)
2. Do the arms look OK while sprinting, and when you start and stop? (yes / describe)
3. Anything else odd? (describe)

**Answers:** 1 "Looks good. There is some slight jitter in general movement not just sprinting, maybe a slight rubber
banding feeling." · 2 "Feels good." · 3 "Left grip is add/remove attachment button, unmap it. After 10 seconds idle, gun
goes back to double vision until you move or input. Possibly an idle animation." Then: "Currently when you unholster a
weapon with the left hand, an arm extends from the right shoulder to the left hand position and holds the gun. I'd also
like the holster system to allow dual wielding" (dual wielding: researched, then parked by the player -- ENGINE-NOTES 5af).

**Log received:** yes, `logs/modlogs/20260929-194803-MOHAVR.log` and `-MOHAVR-host.log`. 21 sprints: the gun hand turned
0-3 deg in its controller's frame (once 71 deg, at 19:40:47 -- the walk animation's, see round 25) and moved 1.3-8.4 cm
(once 20.5): the walk pose giving way to idle, plus the move's one-frame lag. Both aim poses went "none" for 5.3 s at
19:43:55: the Quest had stopped tracking the idle controllers, and without a tracked hand the gun fell back to the game's
own placement in front of the eyes (the double gun). The headset's XR loop was steady: 11.11 ms, worst 12-13 ms, 0 late;
the game ran uncapped at 122-146 fps.

**Verdict:** sprint PASSED. General movement jitter, the left grip, the double gun after idling and left-hand mode:
round 25.

**Deployed for this round:** the shipped defaults, no overrides.

---

## Round 25: prepared 2026-09-29, left-hand mode, steadier movement, controllers that sleep
**Changed:**
- **Left-hand mode:** with the gun in your left hand, the arms are drawn mirrored -- the left arm holds the gun, the
  right hand is free on its controller (`Weapon.LeftHandMirror=1`). The game's own animations (reload, bolt, pin pull)
  come out left-handed. [S]: a left-hand draw of the BAR from the right-shoulder holster -- the left arm holds it from
  the left, all solid (`logs/shots/r25-left-arms-ab.png`).
- **The left grip no longer toggles the weapon attachment** (`[Controls] LB=none`).
- **Controllers that stop tracking** (the Quest drops an idle controller after ~10 s): the hand now stays where it was,
  relative to your head, until it's tracked again (`Hands.HoldLost=1`) -- no more double gun. [S]: a hand switched to
  "lost" looks the same as a tracked one; without the hold the gun jumps to the game's placement.
- **Movement**, three changes:
  1. **Walking no longer sways the view or the gun:** the arms play their idle animation while walking, as they already do
     while sprinting (`Weapon.WalkArms=idle`). The game's camera is a bone of the first-person arms, so the walk and run
     animations swayed the whole world against your eyes -- up to 3 cm side to side and 2 cm back and forth at your step
     rate -- and the gun 3-9 cm in your hand. [S]: running, the camera now moves 0.0 / 0.0 / 0.5 cm against the body
     (forward / right / up; with the walk animation up to 4.7 / 20.4 / 1.5), and a 71-degree turn of the gun during a run
     (seen once in round 24's log) is gone. Walking speed and footsteps are unchanged.
  2. **The gun and arms keep up with you** (`Weapon.CatchUp=1`): they were placed from the previous frame's view, so while
     moving they trailed by a frame's movement -- a few cm with the gun held to the side, and it changed with every
     frame. [S]: with the gun held 40 degrees to the side while running, strafing and sprinting, the gun hand stayed within
     0.4 cm of its controller (0.5 in the left hand; 2-8 cm before).
  3. **Frame pacing, new in the menu, off to start:** "Frame pacing" makes the game draw exactly one frame per headset
     frame, right after your poses arrive, so the world steps evenly from frame to frame. Off, the game runs uncapped and
     each frame shows the world at a slightly uneven moment. [S]: it works -- one game frame per headset frame (2-18
     before), about 60% less rendering, the world shown 4 ms fresher, switchable while playing. The simulator can't judge
     whether it's smoother (its own frame loop stutters); that's your call.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. **Left hand:** draw a gun with your left hand (the holsters). Watch both arms; reload; fire. Then the pistol and a
   grenade in the left hand.
2. **Walking:** walk and run around (stick), strafe, stop and start -- with the gun in front, then held off to the side.
3. **Frame pacing:** open the menu (left menu button) -> Frame pacing -> on (left stick right), close, and do the same
   walking; then turn it off again. Your choice is kept.
4. **Idle controllers:** put the controllers down for 20 s, look around, pick them up: the gun should stay put.
5. **Left grip:** squeeze it with a gun in hand: nothing should happen.

**Questions:**
1. Left-hand mode: the left arm holds the gun, the right hand free, animations OK? (yes / describe)
2. Walking: any jitter or rubber banding left, gun in front and to the side? (describe)
3. Frame pacing on vs off while moving: smoother, the same, or worse (stutter, lag)? Which do you want as the default?
4. Controllers put down: no more double gun? (yes / describe)
5. Left grip: nothing happens now? (yes/no)
6. Anything else odd? (describe)

**Answers:** 1 "Left hand is still wrong. Arms are crossed, arm extends from right shoulder to position of left hand and
vice versa. Position is also wrong, like right hands were before my adjustments." · 2 "No jitter, looks good." · 3 "Noticed
no difference. Your recommendation as default." · 4 "Untested" · 5 "Nothing happens." · Also: "Muzzle flash is visible in
front of player instead of on gun barrel. Move to gun barrel if possible, hide otherwise." "A similar animation to sprint
happens when walking over rough terrain or falling a small distance."

**Log received:** yes, `logs/modlogs/20260929-221539-MOHAVR.log` and `-MOHAVR-host.log`. Frame pacing, on vs off in the
same session: the world shown 10.9 ms behind each XR frame with a spread (sd) of 0.4-1.0 ms paced, 14-21 ms with 1.3-3.2
ms uncapped; paced at exactly 90 Draws a second, ~8 ms to spare each, no timeouts in steady play. Walking: the camera
against the body 0.0 fwd, 0.4-0.7 cm up (walk arms idle); but windows with 14-46 cm and up to 14 deg, and per-walk gun turns
of 64-72 deg right after landings -- the jump and landing animations. Left hand drawn 6 times.

**Verdict:** jitter PASSED (walk arms, catch-up); left grip PASSED; frame pacing -> on by default (D16); left-hand mode
FAILED (crossed arms, the grip mirrored wrong -- round 26); idle controllers untested (again in round 26).

**Deployed for this round:** the shipped defaults (Frame pacing starts off; the menu turns it on).

---

## Round 26: prepared 2026-09-29, left-hand mode fixed, no jump animations, the flash and brass
**Changed:**
- **Left-hand mode, fixed:** the arms no longer cross -- each arm comes from its own shoulder -- and the gun sits in your
  left hand the way it sits in your right (your gun fits apply, mirrored; the red dot's line too). [S]: the same rifle
  drawn right- and left-handed at mirrored poses: the left-hand picture flipped matches the right-hand one
  (`logs/shots/r26-lh-fix-compare.png`); the pistol and a grenade in the left hand too.
- **Jumps and falls:** stepping off a ledge or over rough ground played the game's jump and landing animations, which
  swung the view and the gun; the arms now keep their idle pose (`Weapon.JumpArms=idle`). A jump also lifted the view up
  to 8 cm and dropped the gun as far in your hand; that lift is gone (`Camera.JumpLift=0`). [S]: a fall with a landing --
  the view 0.5 cm, the gun <= 2 deg in the hand (with the animation: 11-18 cm, 68 deg); a jump -- the view 0.5 cm, the gun
  0.3 cm (8 cm before).
- **Muzzle flash hidden, brass from your gun:** the game put both where it holds the gun itself, in front of your face.
  The brass now flies out of the gun in your hand (`Weapon.Brass=gun`). The flash could be moved to your barrel, but
  there it doesn't show (the simulator), so it's hidden (`Weapon.MuzzleFlash=hide`; `barrel` is there to try).
- **Frame pacing is on by default** (your menu setting was already on).

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Draw guns with your left hand (rifle, pistol, a grenade): arms, gun position, reload, fire.
2. Walk over rough ground, step off ledges, jump.
3. Fire long bursts with the rifle and the BAR, gun in front and to the side: no flash in front of your face, brass from
   the gun.
4. Put the controllers down for 20 s, look around, pick them up: the gun should stay put (untested in round 25).

**Questions:**
1. Left hand: arms from the right shoulders, the gun sitting right in your hand? (yes / describe)
2. Rough ground, ledges, jumps: any animation or view bump left? (describe)
3. Firing: no flash in front of your face? Brass coming from your gun? Do you miss the flash? (yes/no, describe)
4. Controllers put down: no more double gun? (yes / describe)
5. Anything else odd? (describe)

**Answers:** (the player's words) "Move the flash." -- the only reply; questions 1, 2, 4 and 5 carry over to round 27.

**Log received:** the host log only, `logs/modlogs/20260930-0006-MOHAVR-host.prev.log` (Virtual Desktop, 23:24:47-23:28:02,
3 min 15 s: the gun drawn in the left hand with the StG44, the Colt and the BAR, hands switched five times, a grenade;
paced, the world 10.9 ms behind each XR frame with a spread of 0.4-0.8 ms). The game log is lost: a simulator test
config Claude left deployed after an interrupted test (~23:41) was still in the game folder when the player launched at
23:54 and 00:06, so those two launches ran on the simulator, not the headset, and overwrote it. `tools\deploy.ps1
deploy` now keeps any MOHAVR logs it finds in the game folder (`logs/modlogs/<stamp>-predeploy-*`) before deploying.

**Deployed for this round:** the shipped defaults.

---

## Round 27: prepared 2026-09-30, the muzzle flash at your barrel
**Changed:**
- **The muzzle flash is at the barrel of the gun in your hand**, either hand (`Weapon.MuzzleFlash=barrel`, now the
  default). Round 26 said it didn't show there -- wrong: its flame lasts about one frame (the game's own too), and the
  simulator captures missed it. [S]: the world paused right after a shot, the flame at the BAR's muzzle in the right hand
  (`logs/shots/004827-frz3-def.png`) and in the left (`logs/shots/004448-frz3-l2.png`). The muzzle light (it lights the
  surroundings; it isn't the flame) still comes from where the game holds the gun.
- Otherwise as round 26: left-hand mode fixed, jumps and landings without the animation, brass from your gun, pacing on.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Fire single shots and long bursts with the rifle, the BAR and the pistol, gun in front and out to the side, in each
   hand.
2. Draw guns with your left hand (rifle, pistol, a grenade): arms, gun position, reload, fire.
3. Walk over rough ground, step off ledges, jump.
4. Put the controllers down for 20 s, look around, pick them up.
If the game stays black at the start, quit and relaunch (a rare startup hang, seen twice in testing), and
say so.

**Questions:**
1. The flash: at the barrel of the gun in your hand, in both hands? Any flash left in front of your face? (yes/no,
   describe)
2. Brass coming from your gun? (yes/no)
3. Left hand: arms from the right shoulders, the gun sitting right in your hand? (yes / describe)
4. Rough ground, ledges, jumps: any animation or view bump left? (describe)
5. Controllers put down: no more double gun? (yes / describe)
6. Anything else odd? (describe)

**Answers:** (the player's words) 1. "Flash is good" · 2. "Brass is good right handed, but with left hand the brass comes
out of the wrong ride of the gun and flys off in the wrong direction." · 3. "Looks good" · 4. "Looks good" · 5. "Fixed"

**Log received:** yes, kept by the next deploy's pre-deploy copy: `logs/modlogs/20260930-093750-predeploy-MOHAVR.log`
and `-MOHAVR-host.log` (Virtual Desktop, 09:23:34-09:28:23: the gun drawn in the left hand three times in the last
minute; no errors). The 1.5 KB `-MOHAVR.prev.log` is Steam relaunching the exe 0.7 s earlier.

**Verdict:** the flash at the barrel, left-hand mode, jumps and falls, and idle controllers (Hands.HoldLost) **passed**;
the brass passed right-handed and **failed left-handed** (thrown the right-hand way) -- round 28.

**Deployed for this round:** the shipped defaults.

---

## Round 28: prepared 2026-09-30, the brass from your left-hand gun
**Changed:**
- **Left-hand brass:** with the gun in your left hand, the casings now fly out of the mirrored gun's side and away from
  it, the mirror image of your right hand (`Weapon.BrassMirror=1`). Before, they flew the right-hand way, across the gun.
  [S]: frozen 15 frames after a shot with the BAR in the left hand, the casing is out to the left of the gun
  (`logs/shots/095427-frz5-l-def.png`); before the fix it crossed to the right (`logs/shots/094007-frz5-l-old.png`).

**How to try it:** Claude has deployed. Launch as usual, get into play. Fire the rifle, the BAR and the pistol with the
gun in your left hand, then in your right.

**Questions:**
1. Left hand: does the brass come out of the gun's side and fly away from it, like your right hand's, mirrored? (yes /
   describe)
2. Right hand: still right? (yes / describe)
3. Do the casings look normal (solid brass, not hollow or see-through)? (yes / describe)
4. Anything else odd? (describe)

**Answers:** (the player's words)

**Log received:** (after `tools\deploy.ps1 undeploy`)

**Deployed for this round:** the shipped defaults.

---

## Template

### Round N: YYYY-MM-DD, <milestone>
**Changed:** what's new since the last round (one line each).

**How to try it:** where to go in the game and what to do.

**Questions:**
1. ... (yes/no)
2. ...

**Answers:** (the player's words)

**Log received:** yes/no, and the file name kept under `logs/`.

---

### Round 1: prepared 2026-09-25, M2 (mono quad through the 64-bit host)
**Changed:** the game's image now reaches the headset. MOHAVR-host.exe (64-bit) runs OpenXR and
shows the game on a flat screen floating about 2 m in front of where your head was at start. It
is not stereo and doesn't follow your head yet: it's a cinema screen.

**How to try it:**
1. Start Virtual Desktop and connect the headset (it's the system OpenXR runtime).
2. `tools\deploy.ps1 deploy -Set 'Bridge.D3D9On12=1','Bridge.Host=1'` (RuntimeJson left empty,
   so the headset runtime is used).
3. Launch MOHA from Steam normally. The desktop window will be white; that's expected.
4. Campaign → Continue, then play the parachute landing for a minute.
5. Quit the game. Then `tools\deploy.ps1 undeploy` (which keeps both logs in `logs\modlogs`).

**Questions:**
1. Do you see the game on a floating screen in the headset? (yes/no)
2. Is the image stable, with no flicker or tearing? (yes/no)
3. Does the game feel as smooth as on the monitor, with no stutter? (yes/no)
4. Are the colours and brightness right: not washed out, not too dark? (yes/no)
5. Is the screen too close or too far, too high or too low? (describe)

**Attempt 1 (2026-09-25 ~18:40): VOID, the mod was not deployed.** The last deploy had been undone at
18:33, no MOHAVR logs were written, and the game ran unmodded. The player saw Virtual Desktop's
streamed desktop, not MOHAVR's quad. Their answers (1 yes, 2 yes, 3 slight stutter, 4 yes,
5 "directly in front, large but comfortable") describe VD's desktop view, **not** the mod.
Lesson: Claude deploys and undeploys for headset rounds. Tell the player the check that the mod
is live: the game's desktop window stays white.

**Attempt 2 (2026-09-25 ~18:48):** deployed by Claude with RuntimeJson empty (Virtual Desktop).

**Answers:** 1 yes · 2 yes (stable) · 3 yes (smooth, no stutter) · 4 yes (colours right) · 5 "all good".

**Log received:** yes, `logs/modlogs/20260925-184906-MOHAVR.log` and `-MOHAVR-host.log`. They
confirm runtime "VirtualDesktopXR" 1.0.10 on a Meta Quest 3, 90 Hz, 98.8% of XR frames carrying a
new game frame (6,224/6,300), and the game in the player's own fullscreen 2560×1440 mode. 79 s
session, and the host exited cleanly with the game.

**Verdict: M2 [H] PASSED.**
