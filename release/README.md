# MOHAVR 0.8.3: VR for Medal of Honor: Airborne

MOHAVR turns the PC version of *Medal of Honor: Airborne* (Steam or the EA app) into a VR game. You get a stereo,
head-tracked view, motion controllers with guns held in your hands, and physical interactions: holsters on your body,
reloading by hand, melee swings, scopes you raise to your eye, an off-hand pistol, grenade and knife, and the HUD on your
wrist. An in-headset menu adjusts all of it. This is a **test build**: read "Known issues" before playing.

## What you need

- *Medal of Honor: Airborne* from Steam or the EA app (both sell the same build). The mod checks the game build (3648)
  and stays inactive on any other version.
- 64-bit Windows 10 (1703 or later) or Windows 11, and a graphics card with Direct3D 12.
- A PC VR headset with an OpenXR runtime: Virtual Desktop, SteamVR, or Meta Quest Link. MOHAVR uses whichever OpenXR
  runtime is active.
  - Controllers bound directly: Meta Touch, Valve Index, the HP Reverb G2's (for example through SteamVR with the Oasis
    driver; SteamVR must be the active OpenXR runtime: SteamVR Settings > OpenXR), the Vive Cosmos, the Pico 4, and
    the first-generation Windows Mixed Reality controllers and Vive wands. Others go through the runtime's remapping.
  - **WMR (first generation) and Vive wands** have no A/B/X/Y: click a trackpad's upper half for that hand's upper
    button (B right, Y left), its lower half for the lower one (A right, X left), its centre for the stick click. On
    the Vive wands, touching the left trackpad moves you and the right one turns you. The right menu button backs out
    of the MOHAVR menu.

## Install

**Setup program (recommended):** run `MOHAVR-0.8.3-Setup.exe`.
- It finds the game through Steam or the EA app. If it can't, browse to the game's folder: the one that contains
  `UnrealEngine3\Binaries\MOHA.exe`.
- It adds three files next to `MOHA.exe`: `dinput8.dll`, `MOHAVR-host.exe` and `MOHAVR.ini`. Nothing of the game's
  is changed.
- If another mod's `dinput8.dll` is already there, setup stops and changes nothing. Quit the game before installing.
- **"Let the game use up to 4 GB of memory"** is ticked by default; leave it ticked. The game is 32-bit and can only use
  2 GB, and in VR the first mission runs out (the game crashes after a few minutes).
  - It sets one flag in `MOHA.exe`'s header; the original exe is kept and put back when you uninstall.
  - If Steam's "Verify integrity of game files" or the EA app's "Repair" ever restores the original exe, run setup again.
- Windows may warn that the program is from an unknown publisher, because it isn't code-signed. Choose
  "More info", then "Run anyway".

**Zip (manual):** unzip `MOHAVR-0.8.3.zip` anywhere and double-click `install.cmd`. If your game is somewhere neither
Steam nor the EA app knows about, run `install.ps1 -GameDir "<game folder>"` instead. It also sets the 4 GB flag
(`-Keep2GB` leaves `MOHA.exe` alone). The EA app installs under Program Files: right-click `install.cmd` (and later
`uninstall.cmd`) and choose "Run as administrator".

**Updating:** install the new version over the old one. Your in-headset settings are kept. If the shipped `MOHAVR.ini`
changed, your old copy is saved to `%LOCALAPPDATA%\MOHAVR\MOHAVR.ini.previous`.

## Play

1. Start your VR runtime (for example, connect with Virtual Desktop).
2. Start the game from Steam or the EA app as usual. (The EA app first shows the game's own settings window: press Play.)
3. Put the headset on. Menus appear on a flat screen in front of you. In the game, the world is in 3D around you.

The monitor shows the headset view (a mirror over the game's window) while the game is in front. If the game window
shows the game normally instead of staying white, the mod isn't running: check the logs (below).

## Controls

| Controller | Game |
|---|---|
| Left stick | Move (the way you look) |
| Left stick click | Sprint (click to start; click again or stop to stop) |
| Right stick left / right | Turn (smooth, or snap: see the menu) |
| Right stick click | Crouch / stand up |
| Right trigger | Fire the gun in your gun hand |
| Left trigger | The free hand's trigger: fires the off-hand pistol, pulls a grenade's pin |
| A | Jump |
| B or right grip | Reload / use / interact (the game decides which) |
| Y | Switch weapon |
| X | Throw a grenade (the game's way) |
| Left menu button (tap) | Pause |
| Left menu button (hold) | MOHAVR menu |
| Look at the wrist HUD, then X (tap / hold) | Pause / MOHAVR menu (for controllers whose menu button the VR runtime keeps, like the Reverb G2 under SteamVR) |

You aim by pointing the gun: a red dot shows where shots land. In the game's menus the face buttons work as
labelled: A selects, B goes back. The whole table can be changed in `[Controls]` in `MOHAVR.ini`.

## Your hands

Either hand can hold the gun: the hand that draws from a holster holds it, and the other hand is free. A short buzz
tells you a hand is at a holster or a grab spot. Rings show where the spots are (green = a squeeze there acts).

**Holsters.** Squeeze the grip with a hand at a spot on your body:

| Spot | Holds (change it in the menu) |
|---|---|
| Over your right shoulder | Long gun 1 |
| Over your left shoulder | Long gun 2 |
| Right hip | Pistol |
| Left hip | Grenade |
| Chest | Pistol |
| Lower back | Knife (the free hand) |
| Belt, front | Ammunition pouch |

**Two hands.** Squeeze the free hand's grip at the gun's foregrip: the gun points through that hand.

**Reloading by hand.** Every gun is reloaded with your hands:
- Drop the magazine with B (Y if your left hand holds the gun).
- Take a new one from the pouch on your belt.
- Push it into the gun.
- Work the slide, bolt or pump with your free hand.

Bolt rifles are worked after every shot, the M12's pump is its foregrip, and the Garand takes an en-bloc clip.
Racking a loaded closed-bolt gun throws its live round out, and that round is spent.

**Pouch reload** (on by default): grip the pouch with the hand holding a gun, and it reloads at once.

**Grenades.** Take one with the free hand from the grenade holster while the gun stays in your other hand:
1. The free hand's trigger pulls the pin.
2. A second squeeze lets the spoon go (the fuse burns).
3. Swing and let go of the grip to throw.

The gun hand's grenades work the same way.

**Off-hand pistol.** Draw it with the free hand from a pistol holster; its trigger fires it. A squeeze at any holster
puts it back. With the pistol in your gun hand, the free hand draws its twin.

**Knife.** Draw it with the free hand from your lower back. Stab along the blade or slash across it. A squeeze at any
holster puts it back.

**Melee.** Swing the gun: its butt (or a bayonet, a pistol's grip) striking an enemy does the game's melee.

**Scopes.** Hold a scoped gun with both hands and bring the scope up to your eye: that eye looks through it. This
covers the Springfield, the G43, the StG44 and the M18. The zoom is realistic, or the game's, set in the menu.

**HUD on your wrist.** Look at your free wrist with the palm flat and face down:
- Health and the compass are on its left.
- Weapon, ammunition and grenades are on its right.
- Hit indicators, objectives and prompts stay in front of you.

Switch it to a panel in front of you in the menu's HUD tab.

## The in-headset menu

Hold the left menu button to open it. Or turn the off hand palm down, look at the wrist HUD and hold X (A when the gun
is in your left hand); while the menu is open, a tap of X closes it. The left stick moves and changes values, the
trigger or A selects, and B goes back or closes. Up from the first item reaches the tabs.

- **General**
  - World scale (higher = smaller world)
  - Height (seated or standing)
  - Turning (smooth, snap 30 or 45 degrees)
  - Sticks (swap them)
  - Move direction (where you look, or your body)
  - The starting gun hand
  - The red dot
  - Frame pacing
  - Recentre
  - Vignette (none by default; light, strong): darkens the edges while the stick moves or turns you
  - Seated (off by default): sit, then Recentre, and your seated head becomes standing height (true with it off too);
    on, physical crouch needs only a 25 cm lean
  - Physical crouch (off by default): crouch for real and the game crouches too, for low cover. Recentre standing first.
- **Weapons**
  - Gun fit (fit each gun to your hand and line up its aim)
  - Manual reload, Pouch reload
  - Physical melee
  - Scopes and Scope zoom
  - Grenades (Hand grenades, Off-hand grenade, Grenade hold)
  - Off-hand pistol
  - Off-hand knife and Knife grip
  - Reload grip and Reload spots (per gun)
  - Rack ejects a round, and whether the ejected round is lost or kept
- **Hands**
  - Holsters and pouch (move, size, choose what each holds, show or hide each ring)
  - The hand point (the white dot)
  - The foregrip and reload ring sizes
  - The free hand's pose
- **HUD**
  - Wrist or screen
  - When the wrist panels show
  - Their layout and backing
  - Wrist panels (move and size each)
  - Screen HUD (distance, size, height)

Your choices are saved in `%LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini` and kept across updates.

## Settings (`MOHAVR.ini`, next to `MOHA.exe`)

Most things are in the menu. A few you may want to change in the ini:

| Setting | What it does |
|---|---|
| `Render.ResX` / `Render.ResY` | Rendering resolution (default 2880x1620). Lower it (for example 1920x1080) if the game stutters. |
| `Input.Controllers=0` | Play with an Xbox controller or mouse and keyboard instead of VR controllers. |
| `Camera.CinemaScreen=2` | Also show cutscenes on the flat screen (default `1`: menus only). |
| `Bridge.Mirror=2` | Put the monitor mirror in its own window (`0` turns it off). |
| `Controls.SprintToggle=0` | Hold the stick click to sprint instead of toggling. |

## Known issues

- **Mounted MG42s:** you aim a manned MG42 with your head (and turn it with the stick), as the flat game does; the gun
  stays on its mount.

- **StG44:** working the cocking handle doesn't open the ejection port. The game's model has the bolt and the dust
  cover built into the gun's body, so only the handle and its rod move.
- **Wrist HUD:** the panels dim when the gun passes in front of them, but the off hand's own fingers can still show
  through.
- **Ejected rounds and objects:** a dropped magazine or an ejected round lands on a table or stops at a wall now; one
  held right against a wall can still show through it.
- **Levels:** the knife and the ejected rounds use other guns' models from the game, so they depend on what the level has
  loaded. Every mission loads with the mod; the StG44's and G43's rounds fall back to the K98's. Where no shotgun is
  loaded the M12 throws the game's spent shell, and where no MP40 is loaded the off-hand knife isn't drawn.

## Uninstall

Use Windows Settings > Apps > "MOHAVR", or run the uninstaller from the Start menu's MOHAVR folder (zip installs:
double-click `uninstall.cmd`). Uninstalling removes only MOHAVR's files and logs. Your saved in-headset settings stay in
`%LOCALAPPDATA%\MOHAVR`; delete that folder to remove them too.

## If something goes wrong

The mod writes `MOHAVR.log` and `MOHAVR-host.log` next to `MOHA.exe`. If the mod can't start VR, the game still runs
as a normal flat game, and the logs say why.

If a controller button does nothing, `MOHAVR-host.log` says which profile each hand was bound as
(`host: /user/hand/left bound as ...`). Under SteamVR, buttons can be rebound in SteamVR > Controller Bindings.

If the game crashes after a few minutes in a mission, check that `MOHAVR.log` says
`memory: MOHA.exe large address aware: yes`. If it says `no`, run setup again with the 4 GB option ticked.

## License

MOHAVR is free software under the GNU General Public License v3.0 (`LICENSE` in the source repository). Medal of Honor:
Airborne and its files belong to their owners; the mod ships none of them.
