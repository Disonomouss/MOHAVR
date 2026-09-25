# Roadmap

Milestones in order. Each has a one-paragraph technical summary and its acceptance test:
**[S]** means the simulator can prove it, **[H]** means only the headset can judge it. Update
the summary with what was actually done when a milestone closes.

---

### M0. Headless test rig — [S] — **DONE 2026-09-25**
*As built:* `tools/harness.ps1 cycle` backs up the player's Config and Saved, launches through
Steam windowed at 1920×1080, and detects the main menu and campaign menu by screenshot crops (the
engine writes no log, D9). It presses Campaign → Continue (verifying the highlight first), proves
gameplay with an Esc/pause-menu probe, records memory, quits with WM_CLOSE, and restores the user
data byte-exactly. **Passed 4 of 4 consecutive runs, about 30 s each.** Log-driven waits arrive
with the mod's own log in M1.

*Original plan:*
Scripts to kill, deploy, launch (through Steam, keeping the SteamStub wrapper), bring the
window to the front, drive the front end into live gameplay, inject input, and take
screenshots (the preview window through PrintWindow). Progress is detected from UE3 log lines,
not timers, with the `-log` switch. The game runs windowed. The harness backs up and restores
the player's ini around every run and keeps a copy of every log.
**Accept:** one command takes a cold machine to live gameplay and back out, unattended, three
times in a row.

### M1. The mod loads, and stands down safely — [S]
The x86 proxy DLL (D5) loads under the wrapped exe, checks the build hash (D4), writes its own
log (paths anchored to the exe's folder, not the working directory), and reads its ini. It
installs one trivial hook with prologue verification. With a wrong hash or wrong prologue it
logs and does nothing.
**Accept:** the log shows init before `Direct3DCreate9`, and a deliberately wrong prologue makes
the mod stand down cleanly.

### M2. D3D9 → OpenXR bridge, mono — [S] then [H]
Hook `Direct3DCreate9` to go through `Direct3DCreate9On12`. Force a windowed, headset-shaped
backbuffer with vsync off at device creation. Copy the finished frame into a shared D3D12
texture with a shared fence; on the XR thread (D3D11), copy it into the swapchain. First target:
the game's own view on a world-locked quad. Measure address-space headroom (the 2 GB limit).
**Accept [S]:** the frame appears in the simulator's preview window. **[H]:** the image is stable
and has no stutter.

### M3. Head tracking and per-eye projection — [S] then [H]
Compose the HMD pose onto the game camera: heading only, with the game keeping pitch and roll
(lessons §3). Find where UE3 turns `GetPlayerViewPoint`/`GetCameraViewPoint` into native view
and projection matrices. Rewrite the frustum arguments for the headset's asymmetric
field of view per eye. Handle both near planes (culler and projection).
**Accept [S]:** the camera follows scripted head poses. **[H]:** scale and FOV feel right, and
near objects clip rather than vanish.

### M4. Stereo — [S] then [H]
First check whether UE3's own multi-view support (split-screen, scene captures) can render two
eye views. Only if it can't, re-run the scene pass per eye, saving and restoring
once-per-frame state between passes. Pair left and right images from the same frame before
submitting.
**Accept [S]:** the two eyes differ by the expected parallax, and nothing appears in only one
eye. **[H]:** no flicker, and depth reads correctly.

### M5. Layers: menus, cutscenes, HUD — [S] then [H]
Show menus, cutscenes and "no live player" states on a world-locked cinema-screen quad. Use a
one-frame device-call trace to find where the HUD is drawn, redirect it to its own texture, and
show that on a quad (wrist or fixed). Per-eye overlays (reticle, vignette) get their own draw
at the end of each pass.
**Accept [S]:** capture-window shows the quads. **[H]:** nothing is stuck to the face.

### M6. Controller input (virtual pad) — [S] then [H]
Hook DirectInput `GetDeviceState` (and XInput, if the game uses it) and synthesize controls
from the OpenXR actions. Map controller buttons to the game's own controls (from
`DefaultInput.ini`), not to keys, and make the mapping remappable.
**Accept [S]:** scripted controller actions move, crouch, fire, and navigate menus.

### M7. Controller aiming — [S] then [H]
Find where the engine builds the shot ray and drive it from the controller pose. Turn the body
towards the controller with a gentle servo.
**Accept [S]:** a shot fired along a scripted controller ray hits a known target. **[H]:** aim
feels 1:1.

### M8. Hands and held weapons — [H]
Hand frame, per-weapon fits adjustable in the in-headset menu and saved to the player's ini,
two-handed weapons, and arm IK. Expect many rounds of adjustment.

### M9. Comfort and in-headset menu — [H]
An in-headset menu (ImGui, with stable IDs for controller focus), snap or smooth turning, a
vignette, and a seated offset. MOHA-specific: comfort during the parachute jump.

### M10. Release — [S]
A release zip with an install script that finds the game folder, backs up the player's ini, and
uninstalls cleanly.
