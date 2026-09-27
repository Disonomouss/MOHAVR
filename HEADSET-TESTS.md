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

## Round 13: prepared 2026-09-27, the aim along the gun's barrel
**Changed:** the aim (and the red dot) now runs 8 cm higher, along the gun's barrel instead of from the controller's
tip. The gun in your hand is now the default. [S]: the simulator shows the aim ray starting 8 cm above the hand,
in the game and for the red dot alike.

**How to try it:** Claude has deployed. Launch as usual, get into play.
1. Shoot at a wall with the pistol, as in your screenshot, and with the rifle; look along the barrel at the red dot.

**Questions:**
1. Does the barrel line up with the red dot now, with the pistol? (yes / still low / now high)
2. And with the rifle? (yes / low / high / left / right)

**Answers:** (the player's words)

**Log received:** (after `tools\deploy.ps1 undeploy`)

**Deployed for this round:** the shipped defaults (now `Weapon.ViewModel=2`, `Aim.RayUp=8`), no overrides.

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
