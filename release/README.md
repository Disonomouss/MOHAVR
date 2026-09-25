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

The game's own window on the monitor stays **white** while the mod runs. That is expected. Set
`Bridge.Mirror=1` in `MOHAVR.ini` to show the headset's view there instead.

## Controls

The default is to play with a normal Xbox controller or mouse and keyboard. To play with the VR
controllers, set `Input.Controllers=1` in `MOHAVR.ini`. The mapping follows the game's own gamepad
layout:

| Controller | Game |
|---|---|
| Left stick | Move |
| Right stick | Turn (smooth, or snap: see the menu) |
| Right trigger | Fire |
| Left trigger | Aim down the sights |
| A / B / X / Y | Reload/use / switch weapon / crouch / jump |
| Left grip / right grip | Alternate fire / grenade |
| Left / right stick click | Sprint / melee |
| Left menu button (tap) | Pause |

The whole table can be changed in `[Controls]` in `MOHAVR.ini`.

## The in-headset menu

Hold the left menu button to open the menu (with `Input.Controllers=0`, a tap is enough). Use the
left stick to move and change values, and the trigger or A to select. B closes it. The menu has:

- **World scale**: how big the world feels (higher = smaller).
- **Height**: raise or lower yourself for seated or standing play.
- **Turning**: smooth, snap 30° or snap 45°.
- **Recentre**: face forward from where you are now.

Your choices are saved in `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and kept across updates.

## Settings worth trying (`MOHAVR.ini`)

| Setting | What it does |
|---|---|
| `Camera.CinemaScreen=1` | Shows menus on a flat screen in front of you instead of split across your eyes. Set it to `2` to show cutscenes that way too. |
| `HUD.Mode=1` | Shows the HUD (compass, ammo) as one panel in front of you. Adjust it with `Width`, `Distance`, `Down` and `Scale`. |
| `Weapon.HideViewModel=1` | Hides the first-person gun, which can look doubled in VR. |
| `Render.ResX=2880`, `Render.ResY=1620` | Renders at a higher resolution for a sharper image. |
| `Bridge.Mirror=1` | Mirrors the headset's view on the monitor. |

## Known issues

- **Aiming follows your body, not your head or hands.** Shots go where the game's crosshair would point
  (straight ahead of your body), not where you look. Controller aiming is the next big step.
- The first-person gun sits very close to your eyes and can look doubled. `Weapon.HideViewModel=1` hides
  it.
- The HUD and menus look odd in stereo unless `HUD.Mode=1` and `Camera.CinemaScreen=1` are set.
- Each eye renders at half the game's resolution. Use `Render.ResX/ResY` for a sharper image.

## Uninstall

Double-click `uninstall.cmd`. It removes only MOHAVR's files. To also remove your saved in-headset
settings, run `uninstall.ps1 -RemoveSettings`.

## If something goes wrong

The mod writes `MOHAVR.log` and `MOHAVR-host.log` next to `MOHA.exe`. If the mod can't start VR, the game
still runs as a normal flat game, and the logs say why.
