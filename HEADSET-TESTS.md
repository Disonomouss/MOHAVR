# Headset test rounds

After each batch of **[H]** work, add a round here, newest first. Keep the questions short and
answerable with yes or no.

After every session, ask the player for their MOHAVR log; bugs often show up there that the
simulator never shows.

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

**Answers:**

**Log received:**
