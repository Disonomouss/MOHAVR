# Mounted guns in VR (GOAL D, 2026-10-08)

The MG42 nests: 47 placed in 6 missions (`work/research/goal/inventory.md` §1.6). Until now the mod had no handling for
them (D33): the manual reload left their belt reload to the game, and the off-hand features stood down there.

## 1. How the game does it (`work/script/MOHAGame`)

- **The nest** is a `MOHAMountedGun`, which extends `MOHAPawn`, so every nest is in `WorldInfo.PawnList`. It carries
  its weapon (`MOHAMG42Weapon`, a `MOHAMountedGunWeapon`), its use trigger `MyCSA` (`MOHAMountedGunCSA`) and its gunner.
- **Manning it:** `MOHAMountedGunCSA.UsedBy(Pawn)` checks that the player stands and the nest isn't mounted. It
  overrides an AI's claim, claims the nest, and sends the controller to `PlayerMountingMG`.
- **`PlayerMountingMG`** (`MOHAPlayerController.uc:4897`):
  - the player's weapon is put away and look and move input are ignored;
  - over the weapon's PutDownTime the first-person arms are moved to the gun (`LerpAnimOffsetPos`, toward
    `GetDesiredVMAnimOffset`) and the view turns to the gun's rotation;
  - then `TakeWeaponFromMG` and `LoadMG42Weapon`, and the controller goes to `PlayerMountedMG`.
- **`PlayerMountedMG`:** `ProcessViewRotation` adds the look input to the pawn's `rMGRot` (the gun against its mount),
  clamped to the weapon's `fMaxYaw` / `fMaxPitch`. That drives `fAimRightBlend` / `fAimUpBlend` (the arms-and-gun aim
  pose). No jumping, ducking or weapon switching. `PlayerExitingMG` hands the gun back.
- **The belt:** 100 rounds, infinite belts, the game's own reload (D33).

## 2. What the mod did there (measured, the EA copy, Husky's street, nest `MOHAMG42_0`)

`mohavr mg list | goto | use | where` (`src/mohavr/mounted.cpp`) found 10 nests in the level, moved the player to one
and manned it ("the controller now in state PlayerMountingMG", then `PlayerMountedMG`, `bMountedOnMG 1`). Then:
- **The gun:** the view model took the mounted MG42 as "weapon in hand: long gun" and drew it in the gun hand, off its
  mount (`logs/shots/065312-d1-mounted.png`: only the bipod's pole on the wall).
- **The aim:** the hand's ray, from the barrel in the hand.
- **The off-hand features** stood down ("a mounted gun"), as designed.

## 3. Options

1. **The game's way, in 3D (Phase 1, done):** the gun stays where the game draws it, on its mount and locked to the
   camera as in the flat game, and the game aims it. In VR the head then aims: barrel, view and shots agree, and the
   stick turns the mount within its limits.
2. **Both hands on the handles (Phase 2, designed, not built):** the gun pivots on its mount toward the hands.
   - Each frame, from the gun hand's aim (or the line between the hands), compute yaw and pitch against
     `MountedGun.Rotation`, clamp them to `fMaxYaw` / `fMaxPitch`, and write `rMGRot` and the two aim blends after the
     controller's `ProcessViewRotation`.
   - Draw the gun from its mount and `rMGRot`, not locked to the camera. The view-model delta (`viewmodel.cpp`
     `s.d`) would be the mount's pivot.
   - Fire along the barrel (aim.cpp's ray from the drawn gun, as for a held gun).
   - The hands' IK would close on the handles.
   - Open questions only the headset can answer: does a heavy gun pivoting freely under the hands feel right, or should it
     lag? Should the head keep the view free while the hands aim? Does stick turning stay?
3. **Head aims, the gun drawn off the camera on its mount:** a middle way. Only worth it if Phase 1's camera-locked gun
   feels wrong in the headset.

**Recommendation:** ship Phase 1 (on, behind `[Weapon] MountedGame`). Ask in headset round 54 how it feels, then build
Phase 2 behind `[Weapon] MountedHands` if the player wants to aim with the hands.

## 4. Phase 1 (D71)

While a `MOHAMountedGunWeapon` is in hand (`mounted::GameHandles`):
- `viewmodel.cpp` treats it as "no gun drawn": the parts are drawn where the game puts them, in true 3D, as for the
  parachute;
- `aim.cpp` doesn't take the aim (no hand ray, the host's reticle hidden).
`[Weapon] MountedGame=1`.

**[S]** (`logs/shots/065630-d2-*.png`, `065656-d2-target*`, `065724-d2-pitch15.png`):
- the MG42 drawn on the wall, the player's hands on it, the belt hanging, in stereo;
- a burst: the muzzle flash at the sights;
- a soldier placed 6 m ahead (`mohavr melee enemy 6`): Health 110 -> 0, "last hit MOHAMGDamageType on Spine1", the belt
  78 -> 60;
- the head pitched 15 deg: the gun pitches with the view;
- the stick: the mount turns (`rMGRot` yaw 3632, `fAimRightBlend` 0.44).

## 5. Phase 2 (D78): the hands aim it

Built behind `[Weapon] MountedHands` (shipped 0; the menu's Weapons tab -> Mounted MG42).
- **The aim:** per Draw (`mounted::OnDraw`), the host's aim line (the gun hand, or the line to the foregrip hand) in the
  world gives yaw against the controller's yaw and pitch against level. Clamped to the weapon's `fMaxYaw` / `fMaxPitch`
  (45 / 30 deg), it is written to `rMGRot` and `fAimRightBlend` / `fAimUpBlend`. The game turns, draws and fires the gun
  from them, so the gun stays the game's, on its mount, and no drawing change was needed.
- **The camera turns with the gun** (yaw and pitch, ENGINE-NOTES 5bv), so:
  - the eyes keep the controller's yaw, read that frame (a frame-late rMGRot made it oscillate);
  - the controller's pitch is held at 0 (the hand's pitch is all of the gun's);
  - the eyes stay where the camera was with the gun level (the Cam socket drops 15 units at 15 deg up).
- **[S]:** the barrel within 0.2 deg of the hand's line; bursts 25 deg off miss, on the soldier kill (D78).
- Not done: the hands' IK on the handles; a heavy gun's lag (for the headset to ask for).

## 6. The headset's verdict and the lever (D82)

- Head mode pitched no more in the headset ("only goes side to side"): the head's pitch now goes to `rMGRot` directly.
- Hands mode felt inverted: the gun had pointed where the controller pointed. It is now a lever: grip the handle, and the
  gun points from the hand through a pivot 40 cm along its line; let go and it stays. The grip is the handle's while
  manned (it had pressed use and dismounted the player).
