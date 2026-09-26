# MOHAVR: VR for Medal of Honor: Airborne

MOHAVR turns the Steam PC version of *Medal of Honor: Airborne* into a VR game: a stereo, head-tracked
view, playable with motion controllers, plus an in-headset settings menu. It is an **early test
build**. Read "Known issues" before playing.

## What you need

- *Medal of Honor: Airborne* from Steam. The mod checks the game build (3648) and stays inactive on any
  other version.
- Windows 10 (1703 or later) or Windows 11, and a graphics card with Direct3D 12.
- A PC VR headset with an OpenXR runtime: Virtual Desktop, SteamVR, or Meta Quest Link. MOHAVR uses
  whichever OpenXR runtime is active.

## Install

1. Unzip the whole folder anywhere.
2. Double-click `install.cmd`. It finds the game through Steam. If your game is somewhere Steam
   doesn't know about, run `install.ps1 -GameDir "<game folder>"` instead.

It adds three files next to `MOHA.exe`: `dinput8.dll`, `MOHAVR-host.exe` and `MOHAVR.ini`. Nothing
of the game's is changed. If another mod's `dinput8.dll` is already there, the installer stops and
changes nothing.

## Play

1. Start your VR runtime (for example, connect with Virtual Desktop).
2. Start the game from Steam as usual.
3. Put the headset on. The picture appears in front of you. Once you are in the game, the world is in 3D
   and follows your head.

The monitor shows what the headset shows (a mirror over the game's window), while the game is in front.

## Controls

You play with the VR controllers. Aiming up and down follows your head. To use an Xbox controller or
mouse and keyboard instead, set `Input.Controllers=0` in `MOHAVR.ini`.

| Controller | Game |
|---|---|
| Left stick | Move |
| Left stick click | Sprint (click to start, click again or stop moving to stop) |
| Right stick left/right | Turn (smooth, or snap: see the menu) |
| Right stick flicked down | Crouch / stand up |
| Right trigger | Fire |
| Left trigger | Aim down the sights |
| A | Jump |
| B | Reload |
| Right grip | Interact (doors, pick-ups) |
| Y | Switch weapon |
| X | Grenade |
| Left grip | Alternate fire |
| Right stick click | Melee |
| Left menu button (tap) | Pause |
| Left menu button (hold) | MOHAVR menu |

B and the right grip both press the game's reload/use button; the game decides which action happens.
In the game's menus the face buttons work as labelled: A selects, B goes back.
The whole table can be changed in `[Controls]` in `MOHAVR.ini`.

## The in-headset menu

Hold the left menu button to open the menu (with `Input.Controllers=0`, a tap is enough). Use the
left stick to move and change values, and the trigger or A to select. B closes it. The menu has:

- **World scale**: how big the world feels (higher = smaller).
- **Height**: raise or lower yourself for seated or standing play.
- **Turning**: smooth, snap 30° or snap 45°.
- **Recentre**: face forward from where you are now.

Your choices are saved in `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and kept across updates.

## Settings (`MOHAVR.ini`)

These are on by default: VR controllers, menus on a flat screen in front of you, the HUD as one panel, the
headset view mirrored on the monitor, and 2880×1620 rendering. Settings you may want to change:

| Setting | What it does |
|---|---|
| `Render.ResX` / `Render.ResY` | Rendering resolution (default 2880×1620). Lower it (for example 1920×1080) if the game stutters. |
| `Camera.CinemaScreen=2` | Also shows cutscenes on the flat screen (default `1`: menus only). |
| `HUD.Width`, `Distance`, `Down`, `Scale` | Size and position of the HUD panel. |
| `Weapon.HideViewModel=1` | Hides the first-person gun. |
| `Bridge.Mirror=2` | Puts the monitor mirror in its own window (`0` turns it off). |
| `Controls.SprintToggle=0` | Hold the click to sprint instead of toggling. |

## Known issues

- **Aiming:** up/down follows your head, but left/right follows your body (the direction you turn with the
  stick), not where you look or point. Controller aiming is the next big step.
- The first-person gun sits very close to your eyes and can look doubled. `Weapon.HideViewModel=1` hides
  it.

## Uninstall

Double-click `uninstall.cmd`. It removes only MOHAVR's files. To also remove your saved in-headset
settings, run `uninstall.ps1 -RemoveSettings`.

## If something goes wrong

The mod writes `MOHAVR.log` and `MOHAVR-host.log` next to `MOHA.exe`. If the mod can't start VR, the game
still runs as a normal flat game, and the logs say why.
