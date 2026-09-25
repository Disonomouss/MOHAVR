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

**Attempt 3:** deployed ~19:20 with the fixes. Answers pending.

**Answers:**

**Log received:**

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
