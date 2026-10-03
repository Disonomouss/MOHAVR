# Physical melee: design (2026-10-03)

The player's request (2026-10-03): "In game with most weapons clicking right stick whacks enemies with the butt of the gun.
Can the motion of hitting with the butt of the gun do the melee damage? If this system can be worked out, apply it for
special cases, such as upgraded guns that have bayonets."

Research: a seven-dimension workflow, with an adversarial check of each dimension's claims. The notes are kept in
work/research/melee/: flow, victim, upgrades, geometry, mod, testing and vrdesign. A three-lens critique of the first draft
followed (work/research/melee/design-critique.txt), and its findings are folded in below (section 5). The facts cite the
decompiled scripts (work/script) and measured data.

## 1. The game's melee

- **Input.** The right stick click (Xbox RS; F on the keyboard) runs `StartAltFire 2 | OnRelease StopAltFire 2 |
  AirDropMeleePress` (DefaultInput.ini:25, 118).
  - Fire mode 2 is the melee: every hand gun, both launchers and the three grenades have `FiringStatesArray[2] =
    'WeaponMeleeFire'`.
  - The mounted MG42, the Hellbox and the Comp B have no melee.
- **The state** (EALAWeapon.uc:1464-1660).
  - `BeginMelee` sends the pawn to state `Melee`, which plays the arms' `<gun>_melee` animation: a butt stroke for every
    long gun, a grip-first whip for the pistols.
  - It samples the pawn's speed and arms `PerformMeleeTrace` at `MeleeImpactInterval` (0.25-0.5 s).
  - Iron sights block a melee. A melee cancels a reload or a bolt cycle.
- **The hit** (`PerformMeleeTrace`).
  - It traces from the eye, 200 units along the aim. If that misses, it retries with a 1-unit box, then traces 160 units
    from the current eye.
  - On the hit actor it calls `TakeDamage(dmg, Instigator.Controller, HitLocation, impulse * RayDir,
    InstantHitDamageTypes[2], HitInfo, self)`.
  - Then it calls `GetWeaponAttachment().PlayImpactEffectsWithImpactInfo(Impact, GetMeleeImpactType())`, with type 24 for
    pistols, 25 for the other guns and 27 for grenades.
- **Damage.**
  - `InstantHitDamage[2]` is 50 for every single-player weapon. `UpdateWeaponVars` copies it from
    `InstantHitDamageThird_TUNE` at spawn.
  - Two level-2 upgrades rewrite it once: the MP40's "Dagger" to 150, the M12's "M1 16 Inch Bayonet" to 200.
  - The sprint rule: if the pawn moved faster than `GroundSpeed` (490) as the melee started, the damage is the target's
    Health.
  - The impulse is 30000, or 75000 at or above GroundSpeed.
- **The victim** (`MOHAAIPawn.TakeDamage`).
  - Bone multipliers apply to melee too: neck and head x4, hips x1.25, arms and legs x0.75, feet x0.5. A 50-damage head
    hit kills a 130 HP soldier; the torso takes 3.
  - **Allies:** the game reads the pawn's Kynapse controller, or, with none (a gunner on a mounted MG42 is unpossessed and
    owned by the gun), its owner's controller. If that controller is on team 0, or there is no Kynapse controller, the
    player's damage is zeroed and a friendly-fire line plays (MOHADamageType.uc:104-142). The hit reaction still plays.
  - Dead pawns ignore damage. Gag pawns and `bInvincible` pawns take none.
- **Credit.** `InstigatedBy` must be the player's controller. A melee kill goes to `OnMeleeKill` with Pawn.Weapon's type:
  3 weapon XP, the melee-kill stats (PRI +0x734) and the HUD notice. A non-lethal melee hit records nothing.
- **Props and level script.**
  - Der Flakturm's three vent-shaft covers have Kismet `TakeDamage` events that need exactly `MOHAMeleeDamageType` (or a
    Panzerschreck rocket). They are meant to be meleed open.
  - Unfiltered Kismet damage events (radios, PA speakers, conveyor controls) and fuel barrels accept melee too, as they
    accept the button melee.
- **Bayonets.**
  - The M12 at level 2 is the only gun with a blade: bone `altFire_bayonet`, hidden below that level.
  - The MP40's "Dagger" is not on the gun: the game hangs a knife on the arms' left hand only while `mp40_melee_3` plays.
  - The C96's shoulder stock (level 0) turns its whip into a stock strike.

## 2. The design, as built (src/mohavr/melee.cpp; host: hands.cpp, main.cpp, pad.cpp, menu.cpp)

### 2.1 The split
- **Speed is measured on the host's gun pose twice: in the room (LOCAL) and in the head's heading frame.** A swing moves
  the gun in both. Turning or stepping in the room moves it only in the room, and a glance or a duck only against the
  head, so neither counts (2.4). Walking and turning by stick move neither. Speed comes from a lever: each strike point's
  place in the gun pose's frame, taken from the drawn gun only while the weapon is quiet (Active for 0.3 s, no shot or ammo
  change for 0.5 s). The game's own gun animation, such as the fire kick or the raise, therefore never counts as speed. A
  gun strikes only once it has its levers; a gun-hand switch takes them again.
- **Contact is tested on the drawn gun in the world:** the mesh point x the gun component's LocalToWorld x the arm bake's
  move (catch-up included) x the left hand's mirror, as muzzle.cpp's MoveToDrawnGun does. A hit lands where the player
  sees the gun.
- **Damage, credit and effects are the game's own.** `TakeDamage` and the attachment's impact effects are called directly.
  The weapon never enters `WeaponMeleeFire`: no arms animation pulls the gun from the hand, no fire lockout, no view move.

### 2.2 Strike points (tools/melee_points.py -> src/mohavr/melee_points.inc)
Per gun and upgrade level, from the gun meshes (psk exports), in the gun mesh frame at the idle (+X left, +Y down, +Z muzzle).
A row applies at the highest level not above the gun's `CurrentUpgradeLevel`.
- **Long guns:** the butt plate (3 sample points: the centre, and 0.7 of the plate's radius above and below).
- **Pistols:** the grip bottom (the whip). The C96 with its shoulder stock also strikes with the stock.
- **The M12 at level 2:** the bayonet: the tip and the blade's middle.
- **The front** (a muzzle jab, a barrel swing) only with `[Melee] Muzzle=1`. The request was the butt.
- **The launchers strike with nothing.** Their ends sit behind the head and a metre out; the right stick's melee stays.

### 2.3 Speed (game side, per new host sample)
- The host frame comes from one seqlock read (`shared::ReadGun`): the gun pose, the head, both hands, the display time and
  the melee bits.
- Samples are kept for ~0.7 s. Each estimate compares the newest sample with the one ~30 ms back, which removes one
  frame's spike. A gate must hold on 2 estimates in a row.
- **The newest sample alone** must also move at half the gate. A swing is moving now; a tracking step (one sample's
  jump, then still) stays in the 30 ms window for a few samples, but its newest sample is still.
- **The hand speed** is that of the gun hand, or, two-handed, the faster of the two hands, in each frame.
- **The history restarts on:**
  - a gap over 0.1 s; the gun hand, the head or (two-handed, since it steers the gun) the off hand moving faster than
    20 m/s; the gun turning faster than 40 rad/s;
  - a weapon or upgrade change, or a gun-hand change;
  - the foregrip taken or let go;
  - the host's gun-pose epoch changing: the two-handed turn on or off, a hand held by HoldLost or back, a recentre.
- **Untracked samples never arm.** The host's bit is POSITION_ and ORIENTATION_TRACKED for the gun hand, and for the off
  hand too when it is on the foregrip, not merely VALID.

### 2.4 The gates ([Melee] keys)

| Strike | Arms when |
|---|---|
| Butt or grip | the point at `ButtSpeed=3.0` m/s, the hand at `HandSpeed=1.5`, and the point moved `Travel=0.12` m in 0.25 s |
| Bayonet thrust | the hand along the barrel at `ThrustSpeed=2.5` m/s (within `ThrustCos=0.8`), `ThrustTravel=0.15` m forward in 0.25 s, the gun turning under `MaxTurn=3` rad/s (a re-aim turns) |
| Bayonet slash | the tip at `SlashSpeed=8.0` m/s across the blade (\|cos\| <= `SlashCos=0.6`), the hand at `BladeHandSpeed=2.5`, Travel |

- **Both frames.** Each gate must pass in the room and against the head, as the same stroke (a thrust in both, or a slash
  in both). The numbers logged are the smaller frame's. `[Debug] MeleeTrace=1` logs a stroke seen in one frame only.
- An armed strike stays armed for `Hold=0.12` s of real time. Players brake before an impact they can't feel, and the
  drawn gun is a frame behind.
- Contact is tested only while the host's newest sample is under 50 ms old.
- After a hit, the strike is spent until its speed drops below half its gate: one swing, one hit.

### 2.5 Contact (each Draw, armed strikes only)
- **The sweep.** Each sample point is swept from last Draw's drawn position to this Draw's, from 10 units before to 10
  past. That covers a braked stroke and a start just inside a body.
- **A newly armed strike** is also swept from the hand out to each point. A point already inside a body is then entered
  from the hand's side.
- **Collision.** The trace is the bullets' own `CalcWeaponFireNative`: zero extent, the per-bone bodies, so the bone is
  known. aim.cpp's bullet hook stands down during it.
- **No sweep across a jump.** No sweep for 2 Draws after the pawn turns over 10 deg or moves over 0.6 m between Draws (a
  snap turn's carry, a teleport), or after a reset. A segment over 0.6 m is never traced.
- **Line of sight.** The game's own eye must see the gun hand, and the hand must see the contact. These are the shots'
  rules for a hand through a wall. A refusal starts no cooldown.

### 2.6 What a contact does
- **A living enemy:** the hit (2.7). The swing is spent, the weapon's re-melee interval starts
  (max(MeleeInterruptInterval, MeleeImpactInterval), 0.5-1.0 s), and that soldier waits `TargetCooldown=0.5` s.
- **An ally** (the game's rule, 1): left alone. No damage, no friendly-fire line, no hit reaction.
- **A dead soldier:** nothing.
- **A prop meant for melee:** the game's damage and effects. That is an actor with a Kismet damage event listing the melee
  damage type (the vent covers), or a physics actor (a push). `[Melee] Props=1` extends this to every prop.
- **Anything else** (the world, a fuel barrel, a radio, a door): the impact effect and a short pulse, once per swing. The
  swing is not spent, and the sweep looks on past the hit for a soldier, as over a low wall.

### 2.7 Damage and credit (`PerformMeleeTrace`'s, one impact)
- **Damage:**
  - butt, grip, front: the weapon's base melee damage, `InstantHitDamageThird_TUNE` (50);
  - bayonet: `InstantHitDamage[2]` (200 on the M12 at level 2);
  - `ChargeKill=1` keeps the sprint rule, judged on the pawn's top speed over the last 0.3 s (the game samples it as its
    melee starts).
- **Impulse:** `MeleeImpulse_Min` or `_Max` along the swing.
- **TakeDamage:** `TakeDamage(damage, the player's controller, HitLocation, impulse * RayDir, InstantHitDamageTypes[2],
  HitInfo, the weapon)`, with the parameter names probed per class. Pawn.Weapon is the gun in hand, so the credit (the
  melee-kill count, XP) goes to it. `fMeleeStartingSpeed` is set first.
- **Effects:** `PlayImpactEffectsWithImpactInfo(Impact, GetMeleeImpactType(), None)` after the damage, on the weapon and
  attachment read again (a kill may change them).
- **Feedback (v23):**
  - the hit increments `meleeHits`, with `meleePower` and `meleeKind`;
  - the host merges a pulse into that frame's others: 50 ms at 0.6-1.0 for a soldier or a prop, on both hands when
    two-handed; 25 ms at 0.35 for the world.

### 2.8 Not while
- **Hard gates** (the history restarts): the switch off, no host or gun pose, a grenade or no gun in hand.
- **Soft gates** (contact refused, the history kept):
  - the host's busy bit:
    - a holster or the pouch pressed by the gun hand in the last 0.4 s, or a draw by the other hand (it becomes the gun
      hand);
    - the manual reload working the gun: a magazine grabbed or in the hand, the action held, the pump taken by its grip
      or stroked on the foregrip, a box magazine out with one to fetch;
    - a menu open or just closed (0.15 s).

    A gun waiting for the player is not busy and still strikes: a bolt or pump gun before its bolt or pump, a Garand
    whose clip the game threw out, a gun with an empty reserve;
  - a game menu, the pause;
  - offhand.cpp's base gates: cinematics, the parachute, ladders, mounted guns, weapons disabled;
  - a weapon switch;
  - a weapon state other than Active, firing or the sights: no button melee, reload, rechamber, equip or put-down.

### 2.9 What stays
- **The right stick click stays the game's melee:** its animation, its 2 m camera trace, the MP40's dagger stab.
- **The MP40's "Dagger" is not on the gun,** so the gun's own strikes do the base 50. The dagger stays on the button; an
  off-hand knife is a question for the player (answered: yes, D51 -- the off hand draws it from the lower back).

### 2.10 Shared block v23 (2528 -> 2544)

| Offset | Field | Direction | Meaning |
|---|---|---|---|
| 2528 | `meleeOn` | host -> game, in the view seqlock | bit0 the switch; bit1 really tracked; bit2 busy; bits 8-15 the gun pose's epoch |
| 2532 | `meleeHits` | game -> host | +1 per strike |
| 2536 | `meleePower` | game -> host | 0..1, the strike's speed over its gate |
| 2540 | `meleeKind` | game -> host | 1 a soldier, 2 a prop, 3 the world |

`shared::ReadGun` also returns the head, the hands, the display time and `meleeOn`, from the same frame (no layout change).

### 2.11 Switches
- `[Melee] Physical=1`: shipped on once proven in the simulator (rule 7). The menu's Weapons tab "Physical melee" is the
  player's switch. The tab now scrolls with the selection; it had outgrown the panel.
- The other `[Melee]` keys (2.4, 2.6) and `[Debug] MeleeTrace`: each armed swing, its speeds, contacts and refusals.

## 3. Tests [S] (work/research/tests/melee1-9.ps1)

### Tools
- **pad_cmd:** `hand=... dur=S` keyframes, interpolated on the host per XR frame, so swings have a known speed.
  `handframe=room` keeps the test hands still in the room while the head moves (`tools/sim_pose.py`); `handframe=head`
  follows the head again. `hand=l,@fore,0,0,0,0,0,0,pin` puts the hand at a reload spot and keeps it there as a plain pose
  (the host logs where), so the foregrip hand doesn't chase the spot the gun turns.
- **`mohavr melee enemy [dist] [ally]`:** moves the nearest axis soldier (or ally) in front of the player with
  `Pawn.ClientSetLocation`. His AI is turned off and his stasis cleared.
  - He must stay at least 0.85 m centre to centre: closer, the move is refused as an encroachment.
  - Only the first placement in a run sticks; later moves snap him back to that spot.
- **`mohavr melee enemy at <kind> [depth]`**, **`where`**, **`status`**, **`hit`** (the executor alone): `status` reads his
  Health, his last hit's type and bone, and the player's melee-kill count.

### Results
Elite soldier, 200 HP. Unless noted, the hits are on the Spine chain (x1) and the shoulder (x1).

| Case | Result |
|---|---|
| BAR butt stroke at 4.5 m/s | 50: 200 -> 150 |
| Butt stroke on the arm | 37 (x0.75): 200 -> 163 |
| Same stroke in 0.9 s | nothing |
| Stroke in and held | one hit |
| Two strokes 0.3 s apart | one hit |
| A stroke at 30 HP | the kill: the melee-kill count 61 -> 62 |
| M12 bayonet thrust at 2.9 m/s | 200 |
| M12 bayonet slash at 10 m/s | 200 |
| M12's butt | the base 50 |
| A re-aim sweeping the tip through him, hand still | nothing |
| An 8 cm push to aim | nothing |
| The butt into the floor | the world's effects and a 0.35 pulse, the swing not spent |
| An ally thrust at | left alone |
| Left-hand mode | 50, the lever's sideways sign mirrored, the player's user ini byte-identical after |
| Walking and stick-turning into him | nothing |
| Kar98k butt stroke (the manual reload on) | on; 37: 200 -> 163 |
| The same after a shot, the bolt not worked | on; 37: 163 -> 126 |
| M12 thrust after a shot, the pump not worked | on; 200 |
| A thrust started 0.27 m high (a diagonal stroke, 38 deg off the barrel) | nothing (ThrustCos) |
| A glance: the head 40 deg right and left at once, the gun still in the room | nothing; the trace: "against the head only" |
| An 18 cm step of the gun hand into him in one sample (under the jump test) | nothing; the trace: "a step, not a swing" |
| The M1 Garand fired empty (the game throws its clip) | on; 50 |
| The M12 two-handed (the foregrip held): both hands thrust | 200 |
| The foregrip hand 12 cm, then 30 cm, sideways in one sample (the gun turns, the tip ~0.4 m) | nothing; "a step, not a swing" |
| Harness cycles with the switch off and on | OK |

Seen in passing: held at the hip, the M12's foregrip lies inside the belt pouch's ring (`MagPouchSpot` 0 -60 14, 12 cm),
and the manual reload's spots come before the foregrip. A grip there is then a pouch press ("the tube is full"), not
the foregrip. This is the reload's existing order, so the tests hold the gun higher. If it bothers the player, the
foregrip could win over a full tube's pouch.

## 4. [H] questions for the player
- Did every deliberate strike count? Did anything count that you didn't mean (holstering, reloading, re-aiming, turning,
  a quick look around or a duck with the gun held)?
- The Kar98k and the Springfield: does the butt strike, also before you work the bolt?
- Are 3 torso hits per soldier right (a head hit kills)? Should a hard swing do more?
- The bayonet: are thrust and slash both natural?
- Should the muzzle or the barrel count too (`Muzzle=1`)?
- Keep the right stick's melee? Does its animation pull the gun out of your hand?
- The MP40's dagger: should it be an off-hand knife? **Yes** (the player, 2026-10-03): D51, OFFKNIFE-DESIGN.md.

## 5. The critique's findings and what changed
- **Speed:** the first draft rebuilt each lever from the animated gun every sample, so a kick or a head nod became speed.
  The levers are now constants of the held gun, taken while it is quiet.
- **Bayonet:** a 1.33 m blade met its first gates on any re-aim. The thrust and the slash now each have their own gates
  (2.4).
- **Props:** every prop took the game's damage, so a fuel barrel blew up at a touch. Props now get effects only, except
  the ones meant for melee.
- **The world:** a wall contact spent the swing. It no longer does, and the sweep looks past it.
- **Body motion:** a body turn or step in the room counted. Speeds are now measured in the head's heading frame.
- **Spikes:** one-sample spikes armed a strike. There is now a 30 ms window and 2 estimates in a row.
- **Tracking:** VALID was taken as tracked. The bit is now TRACKED, and the host's gun-pose epoch resets the history on
  pose jumps.
- **Holds:** they ran on display time, which stops with the host. They now run on real time, with a 50 ms freshness check.
- **Contact:** a strike starting inside a body missed. The sweep now runs from 10 units before the point, and a newly
  armed strike also from the hand.
- **Snap turns:** their carry and resets were caught a Draw early. The pawn's turn and move are now checked across Draws,
  with a 2-Draw skip.
- **Effects:** they ran on the pre-damage attachment. They now use the weapon and attachment read again after the damage.
- **The menu:** the Weapons tab had no room. The tab now scrolls with the selection.
- **The pulse:** a pulse on the same hand cut the melee pulse short. It is now merged into the frame's other pulses.
- **Line of sight:** the check started at the tracked eye. It is now two legs, the game's eye to the hand and the hand to
  the contact.

The code review after (4 findings confirmed, all fixed):
- **Busy:** the host's busy bit used the pouch reload's "magazine out", which is a bolt gun's resting state and the M12's
  after a shot. Physical melee never struck with the Kar98k or the Springfield, nor with the M12 once fired. Busy is now
  the manual reload working the gun (`Reload::GunHandBusy`).
- **Head motion:** speeds were measured only against the head, so a fast glance or a duck with the gun still could arm a
  strike. Each gate now passes in both frames, and a head jump resets the history too.
- **Levers:** a gun without levers took them from the raise's first frame. They now come only from a quiet gun. Until
  then the gun can't strike, and a gun-hand switch takes them again.
- **Props:** a physics actor now counts only with its impulse on, and not when it is a destructible prop. This finding
  was refuted, but the narrower rule is harmless.

A second review, of those fixes (7 of 8 confirmed, all fixed):
- **Busy, again:**
  - a Garand or a level-0 C96 fired empty (the game throws the clip) was busy until reloaded;
  - so was a box magazine out with an empty reserve;
  - the old gun's bolt or pump hold survived a switch and kept the next gun busy (it also posed the new gun's action:
    the reload now drops those holds on a switch);
  - with `PumpTrigger=0` the two-handed M12 was always busy;
  - a draw by the other hand skipped the 0.4 s hold-off.
- **Steps:** a tracking step under 20 m/s (the off hand on the foregrip turning the gun, or the gun hand) armed a strike,
  because the step stays in the window for a few samples. The newest sample alone must now move too. The jump test also
  watches the off hand when two-handed and the gun's turn rate.
- **Tests:** `aim=` now ends the keyframes it overrides.
- **Refuted:** that a gaze within about 5 deg of straight down makes the heading frame drop a real thrust. It needs a
  head turn during the thrust and a target under the face, which a living soldier at reach never is.
