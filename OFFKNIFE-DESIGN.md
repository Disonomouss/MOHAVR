# The off-hand knife (D51)

The player, 2026-10-03: "The mp40 has an upgrade that is a knife for its melee attack, can we make the knife an off hand
equip with melee functionality? Add a holster to lower back for it."

The research and the staged design are in `work/research/knife-scope/` (design.txt section 2; knife-game.txt and
knife-host.txt are the code readers' reports). This document is what was built.

## 1. Behaviour
- **Taking it:** with a gun in the gun hand, the off hand's grip at the lower-back holster draws the MP40's Dagger into the
  off hand. This works only once the Dagger is earned: the save's MP40 upgrade level is 2 or more (`[Knife] Require=earned`;
  `carried` also wants an MP40 in the inventory). Otherwise the press gets a short refusal pulse.
- **Holding it** (`[OffHand] KnifeHold=toggle`): a press at any holster puts it back. With `KnifeHold=grip`, the knife is
  held while the grip is, and letting go puts it back.
  - While it is held, the off hand is busy: no foregrip, no reload spots, no grenade or pistol take (the press chain gives
    the knife the off hand's presses first, as the off-hand pistol has them). Scopes need two hands, so they stay off too.
- **The grip** (`[OffHand] KnifeGrip`):
  - `forward` (the default) holds the blade out of the thumb side. It is turned about the blade's flat to point along the
    controller, `KnifeTilt` (10) degrees up.
  - `icepick` is the game's own: the arms' `KnifeSocket` on LeftHand, with the blade out of the little finger's side.
- **Strikes:** a stab along the blade, or a slash across it, into a soldier does the game's melee with the Dagger's
  damage:
  - `[Knife] Damage=150` with `MOHAMeleeDamageType`, the hit bone, the impulse and the melee-kill credit, as the gun's
    strikes do.
  - The off hand pulses on a hit.
  - The gun hand's butt strikes go on as before.
  - The knife strikes whatever the gun hand is doing, and whether or not the menu's "Physical melee" is on (the knife has
    its own switch).
- **The switch:** the menu's Weapons tab "Off-hand knife" is the player's (`[OffHand] Knife` in their ini). It is shipped
  on (1), proven in the simulator.
- **The holster:** `[Holsters] LowerBack=Knife`, `LowerBackSpot=0 -58 -22 16` (cm from the head in its heading: right, up,
  forward; then the radius).
  - "Knife" is a host-only holster command. It never reaches the game.
  - The Holsters page can move it, and any holster can be set to hold the knife ("knife (off hand)").
  - A knife holster with the knife switched off counts as empty: no ring, no pulse.

## 1a. After round 50 (D52)
- `[Knife] Require=any` is the default: the knife needs no MP40 upgrade.
- The menu's Weapons tab "Knife grip": the grip (forward / icepick), and the knife moved (forward, right, up: cm in the off
  controller's frame) and turned (tilt, turn, roll: degrees about the handle's middle), saved in the player's ini
  ([OffHand] KnifeGrip, KnifeAdj). The host publishes `knifeAdj[6]` (shared block v26); the game applies it to the placed
  knife as it changes. The same numbers give the mirror image in left-hand mode. [S] r51a: +2 cm and tilt +20 applied (the
  captures), the knife drawn with the MP40 at upgrade level 0.

## 2. How
### 2.1 The knife in the hand (game: src/mohavr/knife.cpp)
- **The template:** `Object.FindObject("MOHAGameNonNative.Default__Attachment_MP40.KnifeMeshComponent")` finds the class
  default MP40 attachment's knife component (a MOHASkeletalMeshComponent, the mesh `DE_MP40_altFire_Knife`, depth group 2).
  It is found once per pawn.
- **The carrier:** `carrier::AttachTemplate` clones it (the outer is the pawn) onto the arms' Camera bone. The arm bake
  draws it as a third carrier, after the off-hand pistol and grenade (`BakeCarrier`: mesh x `CarrierFrame` x the catch-up).
- **Placement:** `CarrierFrame` = the knife in the off controller's frame x the drawn off-controller frame
  (`viewmodel::HandFrames`).
  - The knife in the controller is taken at the draw: the knife's mesh in the hand (the RotOrigin, then the socket, then
    the forward flip about the handle's middle at mesh z -2) x the free hand's relation to its controller at that moment
    (`armsik::FreeHandRel`). The hand doesn't jump when it takes the knife.
  - The hand closes with the stick grenade's handle fingers ("offnade").

### 2.2 The hold (host: src/host/offknife.cpp; shared block v25)
- **Level-triggered:** the host holds whether the knife is held, and the game draws or sheathes to match.
  - This is simpler than the off-hand pistol's event ring (design.txt A.3). The knife has no state besides held, so there
    is nothing an event would carry.
- **Host to game:** `knifeFlags` (bit0 on, bit1 held, bit2 the icepick grip), written per XR frame inside the view seqlock.
- **Game to host** (once per Draw, seqlock `knifeSeq`):
  - `knifeCaps`: bit0 installed, bit1 a draw can happen now, bit2 earned, bit3 drawn, bit6 the gun in hand is an MP40 at
    level 2 (its button melee hangs the game's own knife).
  - `knifeState` bits 8-15: the refusal (1 unavailable, 2 not earned, 3 no template, 4 the draw failed).
  - `knifeDamage`.
  - `knifePawnSeq`: +1 on a new pawn.
- **The host lets go of a held knife** on:
  - the switch turned off;
  - a new pawn;
  - the Dagger no longer earned;
  - a draw the game hasn't made within 2.5 s (wall time).
- **Once drawn, the knife may hide** (a weapon switch: `HandFrames` needs a drawn gun) and stays held.
- **The game draws when it can** (a gun up, no cinematic: `offhand::BaseAvailable`).
  - While a draw is pending, the arm bake frees the off hand (`knife::Pending`) so that its relation to the controller
    exists even with `[Weapon] FreeOffHand=0`. A draw without that frame yet is tried again 100 ms later; a failed attach
    1 s later.

### 2.3 The strikes (game: src/mohavr/melee.cpp)
- **Channels:** the gun's per-hand state (the pose history, the strikes, the re-melee interval, the gates) moved into
  `struct Channel` (`g_gun`). The gun's behaviour is unchanged: the regressions melee6 and melee9 gave the same strikes.
  - `g_knife` is a second channel on the off hand's aim pose.
- **Strike points:** the tip (mesh 0,0,32.5) and the blade's middle (0,0,16).
  - Their lever and the blade's axis come straight from the knife in the controller's frame, mapped into the aim pose's
    frame (the right un-mirrored in left-hand mode). The knife's place in the hand is a constant of the hold, so there is
    no quiet gun to wait for.
- **Gates:** the bayonet's estimate (two frames, the room and the head; the newest sample alone at half the gate) at the
  knife's numbers:
  - a stab is `ThrustSpeed` 2.0 m/s along the blade (cos `ThrustCos` 0.8), `ThrustTravel` 0.12 m over 0.25 s, under the
    gun's turn limit;
  - a slash is `SlashSpeed` 4.0 m/s at the tip across the blade (cos `SlashCos` 0.6), the hand at `HandSpeed` 2.0, and the
    melee travel.
  - The bayonet's 8 m/s slash is for a tip a metre out on a rifle; a hand-held blade can't reach it.
- **Contact:** swept from last Draw's drawn points (the knife's carrier frame x the catch-up x the mirror), and on arming
  from the off hand out. The bullets' collision is used.
  - `Recently()` is shared across both channels: one soldier is struck once per 0.5 s by either hand.
  - The knife's re-melee interval is 0.4 s.
- **Host bits:**
  - `meleeOn` bit3: held.
  - bit4: the off hand really tracked.
  - bit5: the off hand busy (the knife drawn or put back in the last 0.4 s, or a menu). A squeeze that does nothing doesn't
    count: a stab may start with one.
  - bits 16-23: the off hand's pose epoch (+1 on a draw or put back, HoldLost on the off hand, a gun-hand change, a
    recentre).
  - The knife's strikes have their own counter (`knifeHits`, `knifeKind`, `knifePower`), so a gun hit in the same window
    can't take the knife's pulse. The host pulses the off hand only for them.

## 3. Tests [S] (work/research/tests)
- **knife1 (spike):**
  - the template resolves (Var_Flk_P);
  - the knife is drawn in the off hand, forward and icepick.
- **knife2:**
  - the lower back draws it (a capture);
  - a press at the foregrip while it is held doesn't take the foregrip;
  - a press at the chest holster puts it back;
  - the gun hand at the lower back draws nothing;
  - `mohavr upgradelevel 3 1` makes it "not earned", and the press is refused;
  - a second draw works;
  - `-Hold grip`: letting go puts it back.
- **knife3:**
  - a slow push into a placed soldier does nothing;
  - a stab (0.35 m in 0.12 s) does 150 (a thrust);
  - a slash (0.6 m in 0.1 s) does 150;
  - the host pulses the off hand.
- **knife4:**
  - left-hand mode: the right hand draws it (a capture) and a right-hand stab does 150;
  - the player's user ini and freehand file are unchanged.
- **Gun regressions:** melee6 and melee9 gave the same strikes as before the refactor (melee5's slash is under today's
  8 m/s gate, as before).
- **Harness cycle:** OK with the shipped defaults (knife on).
- **The code review** (four reviewers, a verifier each; 7 findings confirmed, all fixed):
  - with `FreeOffHand=0` a draw could never succeed (no free hand frame): now drawn about 110 ms after the first try
    (knife2 `-Extra Weapon.FreeOffHand=0`);
  - the game's 1 s retry came after the host had let go (90 frames);
  - any off-hand squeeze held strikes off for 0.4 s;
  - a gun hit in the same window took the knife's pulse;
  - the test command's draw was undone on the next Draw;
  - a config comment.
  - Re-run after the fixes: knife3 (stab and slash 150, the off hand's pulse on its own counter), melee6 (same strikes and
    pulses), the harness cycle.

## 4. Open, for the headset [H]
- Can the player reach the lower back without seeing it, and does the entry pulse find it? A glance swings the spot,
  because the heading is the head's yaw.
- Is the forward grip right? Is the 10 degree tilt right? Or is the game's icepick better?
- Click or grip hold?
- The stab and slash thresholds; does an icepick downward stab register?
- **Not done yet** (design.txt A5, each its own switch when wanted):
  - the swish sound;
  - the credit to the MP40;
  - masking the button melee while the knife is held with an MP40 at level 2 in the gun hand (the game hangs its own knife
    on the left hand for the animation: `knifeCaps` bit6 already says when);
  - fingers from the game's own knife grip (a "socket" mode in tools/reload_grips.py).
- **Gap:** the template was found in the harness level only. Every level cooks the MP40's classes, but that is unproven.
