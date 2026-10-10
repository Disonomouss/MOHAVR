# MOHAVR: VR for Medal of Honor: Airborne

MOHAVR turns the PC version of *Medal of Honor: Airborne* (Steam or the EA app) into a full 6DoF VR game: a stereo,
head-tracked view, guns held in your hands, holsters on your body, reloading by hand, melee swings, scopes you raise to
your eye, the parachute and MG42 nests by hand, and the HUD on your wrist.

**[Download the latest release](https://github.com/Disonomouss/MOHAVR/releases/latest)**: `MOHAVR-<version>-Setup.exe`
(recommended) or the zip.

## What you need

- *Medal of Honor: Airborne* from Steam or the EA app.
- 64-bit Windows 10 or 11, a graphics card with Direct3D 12.
- A PC VR headset with an OpenXR runtime: Virtual Desktop, SteamVR or Meta Quest Link.

## Install and play

1. Run `MOHAVR-<version>-Setup.exe`. It finds the game and adds the mod's files next to `MOHA.exe`. Leave **"Let the game
   use up to 4 GB of memory"** ticked (without it the game runs out of memory in VR).
2. Start your VR runtime, then start the game as usual (the EA app shows the game's settings window first: press Play).
3. Put the headset on. Menus show on a flat screen in front of you; in missions the world is in 3D around you.

## Controls

### Buttons

| Controller | Action |
|---|---|
| Left stick | Move (the way you look) |
| Left stick click | Sprint (click to start; click again or stop to stop) |
| Right stick left / right | Turn (smooth or snap: see the menu) |
| Right stick click | Crouch / stand up |
| Right trigger | Fire the gun in your gun hand |
| Left trigger | The free hand's trigger: fires the off-hand pistol, primes a grenade |
| A | Jump |
| B or right grip | Reload / use / interact |
| Y | Switch weapon |
| X | Throw a grenade (the game's way) |
| Left menu button, tap | Pause |
| Left menu button, hold | The MOHAVR menu |

Aim by pointing the gun: a red dot shows where your shots land. In the game's own menus, A selects and B goes back.
Left-handed? Draw your gun with the left hand (or set it in the menu): the triggers and grips swap with it.

### Your hands

Either hand can hold the gun: the hand that draws from a holster holds it, and the other hand is free. A buzz tells you a
hand is at a holster or grab spot (rings showing them can be turned on in the menu's General tab).

**Holsters:** squeeze the grip with a hand at a spot on your body.

| Spot | Holds |
|---|---|
| Over your right shoulder | Long gun 1 |
| Over your left shoulder | Long gun 2 |
| Right hip | Pistol |
| Chest | Nothing (off by default; pick what it holds in the menu's Holsters page) |
| Left hip | Grenades |
| Lower back | Knife (the free hand) |
| Front of your belt | Ammunition pouch |

| To... | Do this |
|---|---|
| Hold a gun with two hands | Squeeze the free hand's grip on the gun's foregrip |
| Reload by hand | B drops the magazine (Y with the gun in your left hand); take a new one from the belt pouch; push it into the gun; rack the slide, bolt or pump with your free hand |
| Reload quickly | Grip the belt pouch with the hand holding the gun |
| Throw a grenade | Hold the grip at your left hip to take one, press that hand's trigger once (the fuse starts), let go of the grip in a throwing motion. Let go with the pin in and it goes back |
| Draw the off-hand pistol | Free hand at a pistol holster, squeeze; its trigger fires it |
| Use the knife | Free hand at your lower back, squeeze; stab or slash |
| Put an off-hand item away | Squeeze at any holster |
| Melee | Swing the gun: its butt, a pistol's grip or a bayonet |
| Look through a scope | Hold the gun with both hands and raise the scope to your eye |
| Pick up a weapon or ammo | Reach for it (a dropped gun, a rack, a crate) and squeeze a free hand's grip |
| See your HUD | Turn your free hand palm down and look at the wrist |
| Steer the parachute by hand | Menu: Comfort -> Parachute: hands. Hold both grips, pull one down to turn, both to slow, a quick hard pull of both to flare |
| Fire a mounted MG42 | Use it as in the game. Your head aims it; or menu: Weapons -> Mounted MG42: hands, then grip the handle and push it. B gets you off |
| Choose your loadout | In the loadout screen: A opens a slot's list, the left stick moves, A picks |
| Walk around your room | Your soldier walks with you (walls stop him; the view fades if you keep going into one). Turn around for real or with the right stick |

### The MOHAVR menu

Hold the left menu button, or turn your free hand palm down, look at the wrist and hold X (A on Valve Index controllers,
or with the gun in your left hand). In the menu: the left stick moves and changes values, a trigger or A selects, B goes
back. Push up past the first item to reach the tabs.

| Tab | Settings |
|---|---|
| General | Recentre, world scale, height, starting gun hand, resolution (headset presets), frame pacing, holster and reload rings |
| Comfort | Smooth / snap turning, room-scale walking, move direction, sticks, vignette, damage flash, firing shake, seated play, physical crouch, parachute |
| Weapons | Gun fit, red dot, recoil, scopes, physical melee, grab pickup, mounted MG42 |
| Reload | Manual reload, pouch reload, a spare magazine shown in the pouch, reload grips and spots, racking out a round |
| Hands | Holsters, grenades (simple / classic), the off-hand pistol and knife, the hand point |
| HUD | Wrist or a floating panel, and its layout |

Your choices are saved in `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and kept across updates.

### Other controllers

- **Valve Index:** the left controller has no menu button for the game: open the MOHAVR menu from your wrist (palm down,
  look at it, hold the left A); a tap pauses. In the menu, the left stick moves, a trigger or the right A selects, the
  right B goes back.
- **HP Reverb G2 under SteamVR:** SteamVR keeps the left menu button: use the wrist and X as above.
- **First-generation WMR and Vive wands:** no A/B/X/Y. Click a trackpad's upper half for that hand's upper button (B right,
  Y left), its lower half for the lower one (A right, X left), its centre for the stick click. On the wands, touching
  the left trackpad moves you and the right one turns you.
- The whole button table can be changed in `[Controls]` in `MOHAVR.ini`.

## If something goes wrong

The mod writes `MOHAVR.log` and `MOHAVR-host.log` in the game's `UnrealEngine3\Binaries` folder (the previous session's
are kept as `.prev.log`). Include both with a bug report, and say which headset and runtime you use. If the mod can't
start VR, the game runs as a normal flat game and the logs say why.

The full guide (every option, known issues, uninstalling) is in the README included with each release.
