# Manual reload, Step 1 (box magazines): the implementation design

Design only. Nothing was run in the game; nothing in the repo or the game folder changed. Built from the four research
reports in this folder, checked against the repo at HEAD 34af8e1 (D21 committed; the code is as at a916d37) and against
the exe. The checks I added are in `design/` (scripts and their outputs).

**Corrected after an adversarial verification (2026-09-30).** Every change is marked **[corr ID]** and every
question the verification left open **[open ID]**, with the in-game test that settles it; the ids (C.., E.., X..)
are those of the verification, listed with the outcome in §10. The verifier's scripts and outputs are in `verify/`; the
extra checks made while correcting are in `verify2/` (`fold.py`/`fold.txt`, `ctx.py`, `ctx2.py`/`ctx2.txt`).

---------------------------------------------------------------------------------------------------------------------

## 0. Sources, conventions, the player's decisions, conflicts

### 0.1 Sources and tags

| Tag | What |
|---|---|
| `SK` | `skeletons.md` (+ `skel/`: rawskel, tracks, upgrades, trees2, upgclasses) |
| `AN` | `anims.md` (+ `segs/`, `magpaths.txt`, `upgmesh.txt`, `bonegeo_all.txt`) |
| `RL` | `reload_logic.md` (+ `nonnative/` = `NN/`, `natives_disasm.txt`) |
| `IN` | `integration.md` |
| `D/tracks` | `design/tracks_check.txt` (`tracks_check.py`): idle / fire / reload positions re-read from `skel/tracks/<mesh>.txt` |
| `D/grab` | `design/grabpoints.txt` (`grabpoints.py`): grab points from the psk vertices (engine space, bind pose) |
| `D/taped` | `design/taped.txt` (`taped.py`): where the magazine in the well sits in taped state A and B |
| `D/pe` | `design/peread.txt` (`peread.py`): bytes read from `work/MOHA.exe.unpacked.exe` (image base 0x10900000) |
| `EW`, `SA`, `IM` | `work/script/MOHAGame/EALAWeapon.uc`, `EALASmallArms.uc`, `MOHAInventoryManager.uc` (repo) |
| `V` | the verification's own re-reads, `verify/`: `myskel.txt` (independent RefSkeleton parse), `checkz.txt`, `magpath2.txt`, `rotcheck.txt` (own psa reader), `weights.txt`, `handles.txt`, `bulletgeo.txt` (own psk reader), `treenodes.txt`, `otherlevel.txt` (Nep_Azv_P cross-check) |
| `V2` | this correction pass, `verify2/`: `fold.txt` (every reference to 0x10E45CC0 in the exe), `ctx2.txt` (the per-class native lists resolved through the named native table) |

Confidence: **H** read directly (code, bytes, data); **M** inferred and consistent with what was read; **L** plausible,
unverified.

### 0.2 Conventions

- **Gun mesh space** (the space of every number in §1): +X = the gun's left, +Y = down, +Z = towards the muzzle
  (SK "Conventions", AN §0). All Step-1 magazine and action bones hang directly off an identity root at the origin, so a
  bone's local translation is its mesh-space position, and a mesh-space move is an add to the translation row of its
  SpaceBases matrix (SK, AN §0). SpaceBases = mesh space is **assumed** (RotOrigin applied through LocalToWorld, stock
  UE3); milestone M0 checks it. **[open C18]** Nothing static settles it (ENGINE-NOTES 5x says only "component space";
  reproducing the bind positions from the psk proves nothing about the runtime space): M0's per-bone translations do.
- **Back** = −Z. Every Step-1 action bone moves along mesh Z only (SK, AN §1; D/tracks: the action bones' Y is constant in
  every idle, fire and reload anim of the Step-1 guns, and their X at the start and end of each; V `checkz.txt`,
  `rotcheck.txt`: X and Y constant and no rotation in every listed anim, BAR ≤ 0.03°).
- **Units:** mesh units are world units (no component scale; AN §0), ~1 cm at the default 100 units per metre (the models
  are ~10% oversize: SK "Units"). The host works in metres: metres = units / upm (the live world scale).
  **[open C46]** Only the script side is known: the attachment mesh subobjects set no `Scale`/`Scale3D`
  (`NN/Attachment_Stg44.uc:41-49`, archetype `SmallArmsAttachment.uc:1171-1172`); the runtime LocalToWorld scale is not
  measured. **[corr C46]** So every length that crosses between mesh and world goes through `A` (§5.2, §5.5) instead of
  assuming scale 1, and M0 logs the norms of the L2W rows.
- **Sign pitfall [corr]:** the umodel `.psk`/`.psa` exports have **Y negated** relative to the engine-space numbers in
  this document (psk StG44 `Bolt` (0.38, +11.50, 15.00) = engine (0.38, −11.50, 15.00); `single_magazine` (0, −2.50,
  17.68) = engine (0, 2.50, 17.68)). Anyone re-deriving data from the exports must negate Y, as D/grab did
  (`verify_mod/pskbones.txt`).
- **Bone tree walks [corr]:** `NumChildren` in the cooked RefSkeletons is stale (StG44 `single_magazine` and
  `upgrade_02_tapedMagazines` nc 2 with no children, Thompson `Root` nc 12 for 10 real children: V `myskel.txt`). Code
  that walks children uses `ParentIndex` (+56 in memory, ENGINE-NOTES 5x) only.
- **Host gun frame:** the host's `gunPose` (the gun hand's aim pose pitched by the fit's angle, turned by the foregrip,
  `hands.cpp:110-119, 213-219`): x right, y up, z back, metres. The game's `G` = viewmodel's `ctrlFrame · carry` (rows:
  forward, right, up; `viewmodel.cpp:447-456, 483`; `arms_ik.cpp:296-300`). host = (right, up, −forward) / upm.
- **SAVE** = the player's save: every upgrade fitted (ENGINE-NOTES 5ak).

### 0.3 The player's decisions (D21) and how the design meets them

| Decision | Design |
|---|---|
| Pouch in the middle of the belt | A 5th spot on the Holsters page, `[Holsters] MagPouchSpot=0 -60 14 12` (cm from the head in its heading frame: centre, 60 below, 14 ahead, radius 12). Off hand only, only while the gun's magazine is out (§3.7). |
| Drop: a gun-hand button AND the off hand pulling it out | The gun hand's upper face button (B right, Y left), masked from the pad while active (§3.6); or the off hand squeezes at the magazine and pulls it 4 cm along its out axis (§3.2). |
| Racking required after an empty magazine | An empty gun (closed bolt: clip 0; open bolt: not cocked) takes no rounds from a new magazine until the action is racked; the trigger gives the game's own dry click meanwhile (§2.2, §2.4). |
| Rounds in a dropped magazine go back into reserve | At the drop, the clip (minus a chambered round on closed-bolt guns) goes back to the reserve. **[corr E5]** Rounds the reserve can't take (it is at its cap: pistol 100, rifle 120, `DefaultPlayer.ini:190-191`) are carried by the mod (`owed`, per ammo class) and credited first at the next INSERT/RACK, so none are lost (§2.2). |
| The gun model shows empty (slide/bolt back, chamber open) | While empty, the action is held at its empty position every tick and the visible top round is hidden (§5.3, §5.4). Per gun in §1; see P1/P2. |

### 0.4 Questions for the player (defaults chosen; each is a switch)

- **P1. Thompson and MP40 show the bolt FORWARD when empty.** They fire from an open bolt: at rest and loaded the bolt is
  back (cocked); the game's own empty pose (reload frame 0) is bolt forward (SK, AN §1, D/tracks). "Back" would look the
  same as loaded. Default: forward. `Empty=none` per gun turns the cue off. [H]
- **P2. StG44 and BAR have no hold-open** (none in the art, none on the real guns). Default: the charging handle is held
  at its rack position (StG44 −2.33, BAR −2.80) as the empty cue; on the BAR it may read as "cocked" (AN §1). [H]
- **P3. Closed-bolt guns keep the chambered round on a drop** (StG44, G43, Colt, C96: clip 1, one more shot, no rack
  after the new magazine). The approved plan said "the loaded ammo goes to 0": `KeepChambered=0` gives exactly that. [H]
- **P4. The release button is B (right hand) / Y (left hand).** B is today's "reload"; Use stays on the gun hand's grip
  (`A=b,rgrip`). A left gun hand loses Y ("switch weapon") while a converted gun is drawn; holsters still switch. [H]
  **[corr E2]** The grip's Xbox A still runs the game's `StartReload`, which makes the reload noise for the AI before it
  finds nothing to reload (§2.6); harmless, as today with a full clip.
- **P5. One magazine model per gun.** The drawn magazine in the hand is the gun's own magazine bones, so a spare can come
  from the pouch only while the gun's magazine is out (drop first, then take). [H]

### 0.5 Conflicts between the reports, and what the design does

1. **Chamber on a drop.** IN §3.3 keeps one round in every gun; RL §7.5 keeps one only on closed bolts and tracks
   `cocked` for the open-bolt Thompson, MP40 and BAR. **Design: RL's per-gun action type** (an open bolt holds no round).
2. **Racking a loaded closed-bolt gun.** RL §7.7 ejects the chambered round (lost or back to reserve); IN §2.3 changes
   nothing. **Design: no ammo change** (the extracted round isn't modelled; nothing is lost). **[Revised by D54,
   2026-10-03]** the player asked for RL's: a full stroke of a loaded closed-bolt action throws the chambered round out,
   seen, and spent (`[ManualReload] RackEject`, `RackEjectKeep` for the reserve); the RACK BACK row in §2.2.
3. **Max+1.** RL §1.3 allows it (SetAmmoCount has no clamp; the next SubtractAmmo clamps to Max); IN caps at Max.
   **Design: cap at Max** (`ChamberPlusOne=0`) until the HUD has been seen with Max+1 [S].
4. **Hook site.** RL §2.2: a MidHook at 0x10E45CF1 or the vtable slot 0x115883E4; IN §3.4: an inline hook on the exec's
   entry 0x10E45CC0 that calls the original, then zeroes `*Result`. **Design: IN's**, the `muzzle.cpp` pattern (59 bytes
   pinned, re-read in D/pe). It catches every script caller and no C++ caller. **[corr, HIGH]** The linker folded this
   function: the same code is also `AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent`, so the hook also runs
   with ECX = the GameInfo (§2.5).
5. **Bolt data.** IN §3.2/§5 gives one `Move` vector per gun with `f = max(rack, HoldOpen)` and reads the Thompson/MP40
   "+Z" fire deltas as travel signs. That cannot describe an open-bolt gun whose empty pose is forward while its rack
   pulls back: the same vector would push the Thompson's bolt forward when racked. **Design: absolute Z positions per
   action bone (idle, empty, full back) from the tracks; the rack always pulls toward −Z from where the bolt is held.**
6. **Magazine axes.** IN puts each out axis in the magazine bone's own frame. In taped state B the BAR pair is turned
   180° about Z (SK; D/tracks `bar_gun_idle_2` r180), which flips a bone-frame axis to point up. **Design: out axes and
   grab points in mesh space; the grab point is an absolute point on the magazine below the well, about the same in both
   taped states** (D/taped: StG44 (0, 11.3-12.1, 19.7), BAR (0.1-1.0, 8.2-9.7, 20): 0.8 and 1.8 u apart; C29 open for
   the grab points themselves).
7. **StG44 scope and the alternate-fire ammo** (left open in IN §5). The scope doesn't use it: `MOHAStg44` Active's
   `ToggleAlternateFireMode` goes to `ScopeModeIn`/`ScopeModeOut` (`NN/MOHAStg44.uc:199-213`), and only
   `AlternateFireModeIn/Out` write `bAlternateFireMode` (`SA:1465, 1537`; `ScopeModeIn/Out` at `SA:2034-2105` don't).
   **The StG44 stays converted with its scope on.**
8. **Empty look on open bolts.** IN §3.2: "bolt back whenever the chamber is empty"; SK/AN: that is the loaded look on the
   Thompson and MP40. **Design: per-gun empty position** (P1).
9. **Rack events.** IN: RACK_MAG / RACK_EMPTY decided by the host. **Design: one RACK; the game decides** (it owns
   `magIn`/`pending`); plus TAKE (a pouch magazine, for the logs and the top-round visual).
10. **Units.** SK "~1.1 u per real cm" vs AN "~1 cm per unit": not a conflict (a ~10% oversize model, drawn at the
    player's world scale).

---------------------------------------------------------------------------------------------------------------------

## 1. Per-gun data

### 1.1 The table (mesh space, units; Z = the action bone's translation Z)

| Gun (key) | Action | Magazine bones (all move as one group; SAVE-visible first) | Seat = magazine pivot | Out axis | Grab point = insert target, radius | Action bone(s) | Z idle / empty / full back | Rack travel | Handle grab (offset from the action bone) | Top round | Empty look |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Thompson (`Attachment_Thompson`) | open | `upgrade_03_drum` (50, lvl 2, SAVE); `upgrade_03_hide_magazine` (30 stick, replaced by the drum) | (0, 2.00, 12.00) both | drum (1, 0, 0) (slides out to the gun's left); stick (0, 1, 0) | drum (0, 1.5, 11.5) r 9 cm; stick (0, 11.2, 9.6) r 7 | `Bolt` (knob on top) | −3.50 / **+8.18** / −3.50 | 11.68 (empty → back) | (0, 1.0, 0.5) | none | bolt forward (P1) |
| MP40 (`Attachment_MP40`) | open | `upgrade_02_64rdMagazine` (lvl 1, SAVE); `magazine` (32), `upgrade_01_tapedMagazine` (lvl 0) (both replaced at lvl 1) | (−0.40, 5.54, 31.30) all | (−0.06, 0.99, −0.08) | (−1.2, 14.4, 30.8) r 7 | `Bolt` + `chamber_slide` | Bolt 17.31 / **27.61** / 10.44; chamber_slide 25.73 / 29.91 / 25.04 | 17.17 (Bolt) | (4.5, 0.0, −3.4) | `bullet` (idle (−1.16, −4.03, 32.92)) | bolt forward (P1) |
| StG44 (`Attachment_Stg44`) | closed | `single_magazine` (always) + `upgrade_02_tapedMagazines` (lvl 1, SAVE) | (0, 2.50, 17.68); taped B: Δx −3.64 | (0, 0.93, −0.36) | (0, 11.5, 19.6) r 7 | `Bolt` (handle, left) | 15.00 / **−2.33** (not in the art) / −2.33 | 17.33 | (2.9, 0.0, 0.0) | none | handle back (P2) |
| BAR (`Attachment_Bar`) | open | `magazine` (always) + `upgrade_03_20rdMag` (lvl 2, SAVE) | (−0.02, 0.73, 20.84); taped B: (2.58, 8.92, 21.34), 180° about Z | (0, 1.0, −0.06) | (0, 8.5, 20.2) r 7 | `Bolt` = the charging handle (bone turned 90° about Y; travel in mesh Z) | 12.30 / **−2.80** (not in the art) / −2.80 | 15.10 | (1.7, −0.1, 2.9) | none | handle back (P2) |
| G43 (`Attachment_G43`) | closed | `upgrade_01_20rdMag` (lvl 0, SAVE) + `magazine` (10, always) | (0, −3.03, 18.48) | (−0.08, 1.0, 0.04) | (0, 3.0, 20.0) r 7 | `Bolt` (carrier + handle, left) | 19.70 / **8.34** / 7.08 | 12.62 from idle, 1.26 from empty | (2.4, −1.3, −4.9) | `bullet` (idle (0, −7.25, 14.59)) | locked back |
| Colt .45 (`Attachment_Colt45`) | closed | `magazine` (bone tilted 20° about X) | (0, −1.23, 0.42) | (0, 0.94, −0.34) (the bone's local +Y) | (0, 6.3, −1.3) (base plate; 0.9 u exposed) r 6 | `gunSlide` | 2.50 / **−1.85** / −1.85 | 4.35 | (0.1, −0.3, −4.0) (rear serrations) | `bullet` (static (−0.02, −5.74, 0.83)) | slide locked back |
| C96 (`Attachment_Mauser`) | closed; **only at CurrentUpgradeLevel ≥ 1** | `upgrade_02_magazine` (lvl 1 "20 Round Magazine") | (0, 0, 10.56) | (0.14, 0.99, −0.03) | (−0.1, 6.7, 11.3) r 7 | `Bolt` (top) | 0.65 / **−4.55** / −4.55 | 5.20 | (0, 0.5, −0.2) (rear wings) | `clip` (idle (0, −3.58, 9.92)) (M) | bolt locked back |

Notes (all H unless marked):
- **[corr E1/X9] The table's positions are ANIMATION values, not bind (RefSkeleton) values.** For the action bones, the
  top rounds and the seats they are the idle / empty / back poses of the anims; several differ from the reference pose:
  MP40 `Bolt` Z bind 18.575 vs idle 17.31, `chamber_slide` Z 29.909 vs 25.73, MP40 `bullet` bind (4.347, −4.034, 32.839)
  vs idle (−1.16, −4.03, 32.92), G43 `bullet` bind (0, 0, −37.5) vs idle (0, −7.25, 14.59), C96 `clip` bind (0, 0, −55.0)
  vs idle (0, −3.58, 9.92) (V `myskel.txt` vs `checkz.txt`). The guard in §1.3 therefore carries the RefSkeleton values
  separately.
- **Hold direction** follows from the data: empty < idle → the hold clamps back (`min`), empty > idle → forward (`max`).
  **[corr E9]** The clamp (not an offset) makes the last shot look right: `ConsumeAmmo` drops C to 0 at the shot, so on a
  closed bolt the hold applies from the fire anim's first frame and **the action snaps to the hold at the shot** (Colt
  fire f0 2.50 is clamped to −1.85 at once; the anim's own extreme is the hold value anyway: Colt fire f2-4 −1.85, G43
  fire 7.08 then held at 8.34, Thompson fire 8.18 = the hold) (D/tracks). On an open bolt `cocked` goes false at the next
  `OnDraw`, one bake late. The difference from "caught as the anim returns" is 1-2 frames.
- **MP40:** `chamber_slide` (the bolt face in the port, 12 vertices) is driven from the `Bolt`'s Z, piecewise-linear
  through the three pairs (27.61→29.91, 17.31→25.73, 10.44→25.04) (D/tracks). Neither bone has children.
  **[corr]** This is an approximation: in the fire anim `chamber_slide` is not a function of the `Bolt`'s Z (f3: `Bolt`
  15.59 ↔ `chamber_slide` 25.04, while the reload maps 10.44 ↔ 25.04). Cosmetic, 12 vertices; the mapping is only used for
  the hold and the rack, where the reload's pairs apply.
- **MP40 `bullet` [corr]:** at the idle pose its geometry sits just FORWARD of the magazine top (Z 32.9-36.0 against the
  magazine's Z 29.2-32.8; V `bulletgeo.txt`): a round at the feed / chamber, not strictly the magazine's top round.
  Hiding it when empty (§5.4) is still right. The G43 and Colt `bullet` do sit at the magazine lips.
- **G43 magazine [corr]:** its pose differs slightly between the idle (Y −3.03) and the fire / reload / `_4` anims (Y
  −3.14/−3.15, rotation 0.2°) (V `checkz.txt`, `magpath2.txt`). The Grabbed override uses `saved[j]`, so it follows either;
  it only matters where exact seat values are compared at runtime (M0 accepts both).
- **Thompson drum:** out along +X (the anim slides it in along −X over 3.6 u, reload_3 f50-54; D/tracks, SK). Its grab
  point is the disc's bounding-box centre (bonegeo_all: X −9.3..9.3, Y −7.7..10.8, Z 8.8..14.2); the disc is ~18.6 u
  across, hence r 9. Which side a real drum goes in from was not settled (AN §6); the anim is followed.
- **BAR:** the handle doesn't move when firing (SK; D/tracks: no Bolt motion in `bar_gun_fire_1`); a rack pulls it to
  −2.80 and it returns home.
- **Taped pairs (StG44, BAR):** both bones form one group; the game flips `TapedMagMode` only in `WeaponReload.EndState`
  (`NN/MOHAStg44.uc:227-238`, `NN/MOHABar.uc:154-165`, RL §3.1), which a converted gun never enters, so the pair stays in
  whatever state it is in. The grab point works in both states (D/taped: the below-well centroid of the pair moves 0.8 u
  between states on the StG44, (0, 12.1, 19.7) vs (0, 11.3, 19.7), and 1.8 u on the BAR, (0.1, 8.2, 20.4) vs (1.0, 9.7,
  19.9): well inside the 7 cm grab radius). The idle follows `TapedMagMode` (`NN/MOHAStg44.uc:136-147`): in state B the
  StG44 pair idles at (−3.64, 2.50, 17.68) (`stg_gun_idle_3b`, V `magpath2.txt`).
- **C96:** its 20-round magazine is upgrade index 1 (`WeaponUpgradeClasses` = _0 Shoulder Stock, _1 20 Round Magazine,
  _2 712 Conversion Kit: `skel/upgclasses.txt`, `NN/MOHAUpgradeMauser_1.uc:30`, `upgmesh.txt`). Below it, the gun
  reloads by stripper clip and stays on the gesture reload. The `clip` bone sits in the receiver at idle and is not moved
  by `mauser_gun_reload_3` (AN §2.7); treating it as the visible rounds is an M. **[corr]** The game's own art jumps it
  0.95 u: (0, −3.58, 9.92) in `mauser_gun_idle`, (0.05, −4.18, 10.67) in the fire and `reload_3` anims (V `checkz.txt`);
  irrelevant to the collapse. Its geometry at idle (X −0.5..0.5, Y −6.4..−0.4, Z 9.7..12.1; V `bulletgeo.txt`) sits
  inside the magazine's upper volume, which supports the "visible rounds" reading (still M; M5 capture).
- **Grab points** are absolute mesh points on the seated magazine below the well (psk vertices beyond the well edge,
  D/grab), so they stay valid while the magazine is out: they are also the insert target. Handle grabs are offsets from
  the action bone's current position (they move with the bolt).
- **Fire-mode:** the G43's rifle grenade (lvl 2) is `bAlternateFireMode` and keeps the game's reload (§2.5); the StG44
  scope is not alt mode (§0.5.7).

### 1.2 Evidence per column

| Column | Evidence |
|---|---|
| Bone names, parents, bind positions | SK per-gun tables (from the cooked RefSkeleton, `skel/rawskel.txt`); `bonegeo_all.txt` (ref t per bone); **V `myskel.txt`** (independent parse; the bind values are in §1.3, not in §1.1); every named bone exists in the psk skeletons (`pskbones.txt`); V `otherlevel.txt`: the seven RefSkeletons are identical in `Nep_Azv_P` |
| Upgrade level, SAVE visibility | `upgmesh.txt` (Default__MOHAUpgrade* UpgradeMeshesToAdd/Replace → control → bone), SK per-gun tables, `skel/upgclasses.txt`. **[open C23]** SAVE visibility can't be read from the decompiled scripts (`UpgradeMeshesTo*` shows as "Array type was not detected"); not critical, because the visible variant is chosen at runtime by `|det|` (§5.1). M0 logs it. |
| Z idle / empty / back | D/tracks: Thompson Bolt reload_1 f0 8.18, fire max 8.18, no idle motion (= bind −3.50); MP40 idle 17.31 / 25.73, reload_1 f0 27.61 / 29.91, min 10.44 / 25.04; StG44 fire min −1.46, reload min −2.33, idle = bind 15.00; BAR reload min −2.80, idle = bind 12.30; G43 reload_1/3 f0 8.34, fire min 7.08; Colt `colt45_reload` f0 −1.85; C96 `mauser_gun_reload_3` f0 −4.55. Agree with AN §1 deltas (e.g. G43 −11.36 = 8.34 − 19.70) and SK's summary |
| Out axes | AN §1 "magwell axis" (from `magpaths.txt`: the insert paths of the SAVE reload anims); Colt = the bone's local +Y = mesh (0, 0.94, −0.34) (SK Colt table). **[open C29, partly settled]** Re-derived in this pass from V `magpath2.txt` (the first frames of each removal, or the last of each insert): Thompson drum (1, 0, 0) and stick (0, 1, 0) exact; MP40 (−0.06, 0.99, −0.08) exact (`reload_1` f0→f17); BAR (0, 1.0, −0.04..−0.09); G43 (−0.11, 0.99, 0.06) at the insert (`reload_3` f37→f38); Colt (0, 0.94, −0.34) exact; C96 (0.14, 0.99, −0.03) exact. StG44: the removal runs (0, 0.99, −0.16) (`reload_3a` f0→f18) and the insert (0.05, 0.91, −0.40) (`reload_3b` f23→f24); the table's (0, 0.93, −0.36) lies between, ≤ 12° from either, inside the 40° insert cone. **M** (the art swings the magazine; no single axis is exact). |
| Grab points, depths | D/grab (inserted depth along the out axis: MP40 6.7, StG44 6.7, BAR 9.5, G43 5.1, Colt 12.0, C96 6.2; the drum slides 3.6); D/taped for the taped states. **[open C29]** Not re-derived by the verification or this pass (M); M3/M5 settle them with the geometry log (the drawn grab point against the controller). |
| Handle grabs | D/grab (knob vertices); consistent with AN's picked knobs: MP40 (3.3, −5.2, 15.5), StG44 (3.4, −11.9, 14.8), BAR (4.1, −6.7, 15.8), G43 (2.8, −10.3, 14.8). V `handles.txt` (own reader): StG44 (2.90, −0.01, 0.01), MP40 (4.50, 0.03, −3.42), BAR (1.72, −0.14, 2.91), G43 (2.37, −1.32, −4.87), Thompson (0.29, 1.06, 0.52) from the bind position = the table's offsets. The Colt and C96 offsets are deliberate other picks (the rear serrations / wings, not the highest-X knob). |
| Top rounds | SK/AN: MP40, G43 and Colt `bullet` never move in any anim; C96 `clip` idle position (SK Mauser table) |
| Action type | RL §7.3 (open bolt: Thompson, MP40, BAR); D/tracks (Thompson and MP40 rest back, fire throws forward) |

### 1.3 The per-gun lines, as shipped (`config/MOHAVR.ini`, new `[ManualReload]`)

The Windows profile API splits a line at its first '=', so each value is a string of `Key=Value` tokens. Vectors are
comma-separated mesh-space values (units); `;` separates the magazine variants (one value = all variants).

```ini
[ManualReload]
Attachment_Thompson=Action=open Mag=upgrade_03_drum;upgrade_03_hide_magazine MagOut=1,0,0;0,1,0 MagGrab=0,1.5,11.5;0,11.2,9.6 MagR=9;7 Bolt=Bolt BoltZ=-3.50,8.18,-3.50 BoltGrab=0,1.0,0.5
Attachment_MP40=Action=open Mag=upgrade_02_64rdMagazine;magazine;upgrade_01_tapedMagazine MagOut=-0.06,0.99,-0.08 MagGrab=-1.2,14.4,30.8 Bolt=Bolt,chamber_slide BoltZ=17.31,27.61,10.44 Bolt2Z=25.73,29.91,25.04 BoltGrab=4.5,0,-3.4 TopRound=bullet
Attachment_Stg44=Action=closed Mag=single_magazine;upgrade_02_tapedMagazines MagOut=0,0.93,-0.36 MagGrab=0,11.5,19.6 Bolt=Bolt BoltZ=15.00,-2.33,-2.33 BoltGrab=2.9,0,0
Attachment_Bar=Action=open Mag=magazine;upgrade_03_20rdMag MagOut=0,1,-0.06 MagGrab=0,8.5,20.2 Bolt=Bolt BoltZ=12.30,-2.80,-2.80 BoltGrab=1.7,-0.1,2.9
Attachment_G43=Action=closed Mag=upgrade_01_20rdMag;magazine MagOut=-0.08,1,0.04 MagGrab=0,3.0,20.0 Bolt=Bolt BoltZ=19.70,8.34,7.08 BoltGrab=2.4,-1.3,-4.9 TopRound=bullet
Attachment_Colt45=Action=closed Mag=magazine MagOut=0,0.94,-0.34 MagGrab=0,6.3,-1.3 MagR=6 Bolt=gunSlide BoltZ=2.50,-1.85,-1.85 BoltGrab=0.1,-0.3,-4.0 TopRound=bullet
Attachment_Mauser=Action=closed MinUpgrade=1 Mag=upgrade_02_magazine MagOut=0.14,0.99,-0.03 MagGrab=-0.1,6.7,11.3 Bolt=Bolt BoltZ=0.65,-4.55,-4.55 BoltGrab=0,0.5,-0.2 TopRound=clip
```

Optional tokens: `Empty=none` (no empty cue; P1/P2), `MagR=` (cm, default 7). `BoltZ=` = idle, empty, full back.

**Ref-pose guard [corr E1/X9]** (per mesh, at resolve). The lines above carry no bind values, and the §1.1 values are
animation poses (a guard built from them would have rejected the MP40, G43 and C96: their bind positions lie 1.3 u (MP40
`Bolt`) to 65 u (C96 `clip`) from the table's values). The guard gets its own key per gun, with the **RefSkeleton**
values (V `myskel.txt`, engine space, units):

```ini
Attachment_Thompson.Ref=Bones=11 Bolt=0,-11,-3.5 upgrade_03_drum=0,2,12 upgrade_03_hide_magazine=0,2,12
Attachment_MP40.Ref=Bones=14 Bolt=-1.412,-5.232,18.575 chamber_slide=-3.186,-5.287,29.909 upgrade_02_64rdMagazine=-0.401,5.538,31.296 magazine=-0.401,5.538,31.296 upgrade_01_tapedMagazine=-0.401,5.538,31.296 bullet=4.347,-4.034,32.839
Attachment_Stg44.Ref=Bones=9 Bolt=0.385,-11.5,15.004 single_magazine=0,2.496,17.676 upgrade_02_tapedMagazines=0,2.496,17.676
Attachment_Bar.Ref=Bones=9 Bolt=2,-6.6,12.3 magazine=-0.017,0.732,20.835 upgrade_03_20rdMag=-0.017,0.732,20.835
Attachment_G43.Ref=Bones=11 Bolt=0,-9.158,19.701 upgrade_01_20rdMag=0,-3.03,18.477 magazine=0,-3.03,18.477 bullet=0,0,-37.5
Attachment_Colt45.Ref=Bones=9 gunSlide=0,-7,2.5 magazine=0,-1.225,0.421 bullet=-0.019,-5.743,0.835
Attachment_Mauser.Ref=Bones=9 Bolt=0,-8.42,0.651 upgrade_02_magazine=0,0,10.556 clip=0,0,-55
```

(For reference, not checked: the BAR `Bolt` is turned 90° about Y, q (0, 0.7071, 0, −0.7071); the Colt `magazine` 20°
about X, q (0.1736, 0, 0, −0.9848).) Two layers:
1. **Fingerprint** (offsets already verified: Name +0, ParentIndex +56, stride 68; ENGINE-NOTES 5x, `addresses.hpp:241-243`):
   the RefSkeleton's bone count equals `Bones`, every listed name exists, and each listed bone's parent is the identity
   root (`Root` on the Thompson, `RootOffset` on the others). Always on.
2. **Positions:** each listed bone's `BonePos.Position` within **0.05 u** of the listed value (the values are the cooked
   floats rounded to 3 decimals). The in-memory offset +28 (Name 8, Flags 4, Orientation 16; consistent with stride 68 and
   ParentIndex +56: Orientation 16 + Position 12 + Length and sizes 16 = 44 bytes from +12) is still an **M**: M0 checks
   it; until it passes, layer 2 is off and layer 1 alone decides.

A failure unconverts that gun (the gesture reload, the game's own reload) and logs which bone failed. Only `Var_Flk_P`
was read by the design; V `otherlevel.txt` found the seven RefSkeletons identical in `Nep_Azv_P` too (and the gun
AnimSequences the same size), so another mesh version is less likely than first feared, not excluded (R19).

---------------------------------------------------------------------------------------------------------------------

## 2. Game rules and the control scheme

### 2.1 The game-side model (per weapon object; `src/mohavr/reload.cpp`)

| Field | Meaning | Initial / after a re-sync |
|---|---|---|
| `magIn` | a magazine is in the gun | true |
| `pending` | a magazine was inserted into an EMPTY gun and not yet fed: its rounds are still in the reserve | false |
| `cocked` | open-bolt guns: the bolt is back on the sear | true |
| `lastClip`, `wrote` | the clip seen at the end of the last Draw, and whether the mod wrote it this Draw | C |
| `heldRounds` | rounds in the magazine now in the hand (visual only) | 0 |
| `owed[ammoClass]` **[corr E5]** | per pawn, not per weapon: rounds returned by an EJECT that the reserve could not take (at its cap, or no entry for the class); credited first by the next draw of that class | 0; cleared with the pawn |

**Keys [corr]:** the per-weapon state is keyed by the weapon pointer **plus its UObject Index** (+0x04, ENGINE-NOTES 5u,
`addresses.hpp` "Object names"), so a new weapon that GC puts at a freed weapon's address (a Garand at an old StG44's)
never inherits its state or its "converted" verdict. Every per-weapon and per-class cache (the §2.5 filter's, the §5.1
bone cache) is cleared on a new local pawn (`reloadPawnSeq`).

The clip `C` = `AmmoCount[0]` counts everything that can be fired without a rack (chamber + magazine). The game already
treats magazines as a pool; so does this design. Derived, published to the host (§4): **ready** = closed: `C ≥ 1`, open:
`cocked` (a gun without an action bone always counts as ready); **rack needed** = closed: `C == 0 && magIn && pending`,
open: `!cocked`.

### 2.2 The arithmetic per event (mode 0 only; w = `pawn.Weapon`)

Notation: `M` = `MaxAmmoCount[0]`; the reserve entry `e` = the `AmmoStorage[i]` (i < `NumAmmoClasses`) whose class is
`w.AmmoClass[0]`; `R` = its amount (+4), `cap` = its max (+8); `inf` = `bInfiniteAmmo`; `O` = `owed[w.AmmoClass[0]]`
(§2.1). What the game does (`AddAmmo`, `EW:621-641`): it reads the available count with the native
`GetReserveAmmoCount` (0x10F0D7D0, which matches an entry whose class **is or derives from** the asked class: it walks
the entry class's SuperField, +0x3C), and writes with the script `AddReserveAmmo` (`IM:76-104`), which matches the class
**exactly**, clamps to `[0, cap]`, and **appends a new, unclamped entry** when none matches (`IM:96-104`); the reserve is
only touched when `!bInfiniteAmmo` (`EW:634-637`). The game never returns box-magazine rounds to the reserve: its
`FillClip` → `AddAmmo(Max)` only tops the clip up (`SA:1991-2000`), so an unmodded reload never loses a round.

**[corr E5, reserve bookkeeping]** Two helpers, written so that no round is ever lost:
- `ToReserve(k)`: if `inf`: nothing. Else, if the exact entry `e` exists: `put = min(k, max(cap − R, 0))`, `R += put`,
  `O += k − put` (the overflow at the cap is carried, not lost). No exact entry: `O += k` (no entry is appended: that
  would need the class default object's `MaxAmmoCount`, an offset not verified in this build).
- `FromReserve(n)`: if `inf`: return `n`. Else `a = min(n, O)`, `O −= a`; `b = min(n − a, R)`, `R −= b`; return `a + b`.
- Published to the host as `ammoReserve` = `R + O` (what this gun can still draw; the HUD shows `R` only, so at the cap
  the carried rounds are invisible until used). Example: the Colt (M 7) holding 5, the pistol reserve at its cap of 100
  (`DefaultPlayer.ini:190`): EJECT (`KeepChambered=1`) → C 1, `R` 100, `O` 4; INSERT (ready) adds M − C = 6: 4 from `O`,
  2 from `R` → C 7, `R` 98. The old `clamp(R + k, 0, cap)` would have lost the 4; the unmodded reload keeps them.
- `O` is lost only with the pawn (death, level load, a loaded save), where the game restores the reserve from its
  checkpoint or save anyway (M: not traced; what matters is that `O` never outlives the reserve it belongs to).
- M0 logs, for each Step-1 `AmmoClass`, which entry the exact match picks and which the native (subclass) read picks; if
  they ever differ, the design's exact match follows the writer (`AddReserveAmmo`), and the difference is logged.

| Event | Precondition | Effect |
|---|---|---|
| **EJECT** (button or pull-out) | `magIn` | `k = C`; closed bolt with `KeepChambered=1`: `keep = min(k, 1)`, else `keep = 0`; `C = keep`; `ToReserve(k − keep)`; `magIn = false`; `pending = false`; `heldRounds = k − keep`. Open bolt: `cocked` unchanged. |
| **TAKE** (pouch) | `!magIn` | `heldRounds = inf ? M : min(M, R + O)` (no ammo moves; the host only offers the pouch when `ammoReserve = R + O > 0 || inf`). |
| **INSERT** | `!magIn` | `magIn = true`. Ready (closed: `C ≥ 1`; open: `cocked`): `add = ChamberPlusOne ? M : M − C` (≥ 0); `C += FromReserve(add)`; `pending = false`. Not ready: `pending = (inf || R + O > 0)`; **no ammo moves**. |
| **RACK** | — | Closed: if `C == 0 && magIn && pending`: `C = FromReserve(M)`, `pending = false`; otherwise nothing (an empty gun without a fed magazine stays back; a loaded one keeps its round -- D54: unless RACK BACK threw it at the arm point). Open: if `!cocked`: `cocked = true`, and if `magIn && pending`: `C = FromReserve(M)`, `pending = false`; if already cocked: nothing. |
| **DROP** (the held magazine let go) | — | `heldRounds = 0`; log only (its rounds went back at EJECT). |
| **RACK BACK** (D54, v27; the action at RackArm, not a tug) | RackEject on, a gun with a `RackRound`, closed bolt, `C ≥ 1`, the game's reload blocked, not yet this stroke | `ConsumeAmmo(0)` (`C −= 1`; else a direct write); `RackEjectKeep`: `ToReserve(1)`; the round thrown from the drawn port (a carrier, else the gun's brass). EjectOnEmpty (the Garand) at `C == 0` with the clip in: `EjectClip`, the ping, `magIn = false`. RACK ends the stroke. |

**The rack eject per gun (D54; the bolt and pump guns of GOAL A2/A3 too):**

| Gun | Action | A full stroke of the loaded action | The round drawn (`RackRound`) |
|---|---|---|---|
| Colt M1911A1 | closed (slide) | RACK BACK: the chambered round out, `C − 1` | its own `bullet` |
| C96 (every level) | closed (bolt) | RACK BACK, as the Colt (at level ≤ 0 racked to `C 0`: the magazine out) | the Colt's `bullet` x1.05 / 0.85 |
| StG44 | closed (handle) | RACK BACK | the G43's `bullet` x0.6 along |
| G43 | closed (handle) | RACK BACK | its own `bullet` |
| M1 Garand | closed (op-rod) | RACK BACK; at its last round the empty clip pings out | the Springfield's `bullet01` |
| K98, Springfield | bolt | BOLT BACK on a live round (`chamberEmpty` tracked: the next stroke feeds, the one after throws it) | own `bullet2` / `bullet01` |
| M12 | pump | PUMP BACK on a live shell (a spent hull as before, no count change) | its own `shell` |
| Thompson, MP40, BAR | open bolt | nothing (the chamber is empty at rest) | none |
| M18, Panzerschreck | breech / tube | nothing (no extractor) | none |

Every write is a plain int on the game thread (the clip at `w + off("AmmoCount")`, the reserve at `inv + off("AmmoStorage")
+ 12·i + 4`). `wrote = true` for the Draw. Consequences: a save, a death or a weapon switch never loses rounds that
were "in a magazine" (they stay in the pool until fed); re-inserting a part-used magazine gives `min(M, R + O)` (the
pool).

### 2.3 What the game does on its own (detected per Draw), and the re-sync

- **`C` rose without a mod write** (a capacity upgrade gained **[corr E4]**: the per-upgrade subclasses set
  `MaxAmmoCount[0]` and `AmmoCount[0]` to the new capacity: `NN/MOHAUpgradeThompsonDrum.uc:5-6`,
  `NN/MOHAUpgradeMP40.uc:9-10`, `NN/MOHAUpgradeMP40_1.uc:11-12`, `NN/MOHAUpgradeBar_2.uc:9-10`,
  `NN/MOHAUpgradeG43.uc:11-12`, `NN/MOHAUpgradeMauser_1.uc:12-13`; the base `MOHAWeaponUpgrade.StaticUpgradeWeapon`,
  `MOHAWeaponUpgrade.uc:35-45`, only shows/hides parts and raises `CurrentUpgradeLevel`, and non-capacity upgrades such as
  `STG44_1` or `Colt45_*` leave C alone; the upgrade sequence refill `EW:728-735`; cheats; a save loaded; the equip refill
  `SA:1924-1927` if the hook wasn't blocking): re-sync: `magIn = true`, `pending = false`, `cocked = true`. The host
  adopts it (§3.2 reconcile). Unaffected by the correction.
- **`C` fell to 0 without a mod write** (the last shot): open bolt → `cocked = false` (the bolt shows forward / the BAR
  handle back). Closed bolt: nothing to do (the hold is `C`-driven).
- **A new weapon object** for a key (a pickup swap) → fresh state; **a new local pawn** (death, level load) → every state
  cleared and `reloadPawnSeq++` (the host resets all guns).
- An event that arrives while the weapon is in `WeaponReload` (only possible if the hook was not blocking) → rejected,
  acknowledged, logged, and a re-sync follows when `C` rises.
- `pending` is not saved: a save/load while a magazine waits for its rack comes back as a fired-dry gun (drop, take,
  insert, rack again). Minor; documented.
- **[corr] `magIn = false` is not saved either** (the game saves `AmmoCount` and the reserve, not the mod's state, and a
  loaded weapon is a new object with fresh state). A gun saved with its magazine out comes back as "magazine in, empty"
  (C 0, or 1 with a kept round): the magazine is drawn in the gun, and the player must eject (0 rounds), take, insert
  and rack. `owed` is lost the same way (it is per pawn). Minor; documented with R14.

### 2.4 Blocking fire until racked

`C == 0` → `HasAmmo` false → `SmallArmsBeginFire` → with `HasReserveAmmo()` false → `WeaponEmpty()` →
`SmallArmsAttachment.WeaponIsEmpty` → `PlayWeaponDryFireSound()` (`SA:612-627`, `EW:810-814`,
`SmallArmsAttachment.uc:824-829`). No reload, no switch; the weapon stays in `Active`/`WeaponIronsights`. The rounds of an
unfed magazine stay in the reserve (RL §4). **H** (script logic).

### 2.5 Keeping the game from reloading itself: one hook

- **Where:** `UEALAWeapon::execHasReserveAmmo` **0x10E45CC0**, native-table entry 0x11612330 (name
  "intAEALAWeaponexecHasReserveAmmo" at 0x11569F88, function 0x10E45CC0: D/pe). Thiscall, ECX = the weapon, `[esp+4]`
  FFrame&, `[esp+8]` Result, `RET 8`; after `P_FINISH` it calls the C++ virtual at vtable +0x324 and stores the UBOOL
  into `*Result`. Its 59 bytes (D/pe, = IN E5): `8B 44 24 04 83 40 1C 01 56 8B F1 8B 48 1C 80 39 41 75 12 83 C1 01 6A 00
  89 48 1C 8B 48 18 50 FF 15 C4 35 6B 11 8B 06 8B 90 24 03 00 00 8B CE FF D2 8B 4C 24 0C 89 01 5E C2 08 00`. The first 8
  bytes are two whole instructions with no relative operands (a 5-byte detour relocates cleanly), the same boilerplate as
  `execActivateSystem`, which `muzzle.cpp:154-165, 247-270` already hooks this way.
- **[corr, HIGH safety] The function is shared.** The linker folded identical code: 0x10E45CC0 is also the native of
  `AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent` (native-table entry 0x11612438 → name 0x115695F0
  "intAMOHAGameInfoexecInitialLoadCompleteKismetActionPresent" → function 0x10E45CC0; the per-class native lists hold it
  at 0x11611854 (among AEALAWeapon's natives) and 0x11611954 (among AMOHAGameInfo's); no direct call or jump reaches it;
  V2 `fold.txt`, `ctx2.txt`; **H**). Its script caller is `MOHAGameInfo.uc:62` (declaration), called at `:368` in state
  `ActualRespawn` (level start and respawn). So the hook also runs with **ECX = the GameInfo**, and there the virtual at
  +0x324 is a different function. Rules: `Blocking(w)` first compares `w` with `pawn.Weapon` **by pointer** (no reflection
  on `w` before that: `PropertyOffset` on a GameInfo would look up the wrong class), and the hook never zeroes `*Result`
  for any other `this`. The `addresses.hpp` comment for `kExecHasReserveAmmo` says so. M0's hook-call counters split the
  calls by caller (the pawn's weapon / anything else). The exe folds widely (2480 named natives share 1831 distinct
  functions, V2 `ctx2.txt`): every future exec hook in this mod must filter on `this` the same way.
- **How:** `safetyhook::InlineHook` → `g_hook.thiscall<void>(w, stack, result)`, then `if (Blocking(w)) *(uint32_t*)result
  = 0`. Verify the 59 bytes first (standing rule 4); address + bytes + a `kSignatures` row in `addresses.hpp`.
  **[corr X2]** `reload::Install` installs the hook only when `Hook_Draw` is installed (stereo on and the Draw hook
  created, `vr_view.cpp:1052-1057`) **and** `armsik::Install` returned true (today its result is ignored,
  `vr_view.cpp:1071`; the call site must keep it) **and** `Weapon.HideViewModel=0` (see below).
- **Filter `Blocking(w)`** (in this order): `w == pawn.Weapon` (pointer; see above) && the host's live flags (read each
  player view): `reloadFlags` bit0 (the toggle) **and bit4 "engaged" [corr X2]** (the host sees the game's reload
  pipeline alive, §3.1) && **game-side liveness [corr X2]**: `reload::OnDraw` ran within 250 ms and a first-person gun
  bake ran within 250 ms (any gun, so the equip refill of the next gun stays blocked during a switch) && the class is
  converted: **[corr X4]** `names::Name(names::ReadPointer(w + PropertyOffset(w, "AttachmentClass")))` (`EW:95`; the
  class object's own name, e.g. `Attachment_Stg44`: `names::ClassName` of a UClass would return "Class") has a
  `[ManualReload]` line, `CurrentUpgradeLevel` (`EW:98`) ≥ `MinUpgrade`, the ref-pose guard passed (cached per class from
  the first bake; assumed passed until then, and a later failure only stops the blocking, so the game reloads) &&
  `!bAlternateFireMode` && `viewmodel::HandFrames` valid (≤ 250 ms, `viewmodel.cpp:514-526`). The class test (not the
  host's 250 ms-polled `weaponKey`) is exact at equip time, when `Instigator.Weapon` is already the new gun
  (`Engine/InventoryManager.uc:555, 567`, RL §2.2). Cache the class verdict per (weapon pointer, UObject Index) (§2.1).
  Why the liveness terms: with `Camera.Stereo=0` nothing runs in `Hook_Draw` (no caps, no geometry, no events, no
  `RunHostCommand`, so no gesture reload either), while the view hook, installed regardless of stereo (`vr_view.cpp:1045`,
  `786`), keeps `HandFrames` valid; the old filter blocked there and left the gun **unloadable** (the pad's A runs
  `Reload` → `CanManualReload` → the blocked `HasReserveAmmo` → false). The same happened if the Draw or UpdateTransform
  hooks failed to install.
- **What it stops** (every one tests `HasReserveAmmo()` in script; grep of `work/script`, RL §2.1): the empty trigger
  pull (`SA:620`), entering `Active` empty (`SA:1103`), ironsights entered empty (`SA:1340`: it now stays in
  ironsights), the end of a burst (`SA:1621-1626`), the end of a single shot (`SA:1699`, 1744-1756), **the instant
  refill on equip** (`SA:1924`), and the manual reload (`CanManualReload`, `SA:1192`). **[corr]** Also tested, with no
  effect: `MOHAInventoryManager.SetCurrentWeapon` (`IM:1192`) tests the previous weapon's `HasReserveAmmo` and calls
  `TryPutDown` in both branches (`PutWeaponDownWhenEmpty` is true, `EW:816-820`; only `EALAGrenade.uc:508` overrides it).
- **What it leaves alone:** C++ callers of the virtual (e.g. the inventory manager's "small arm to return to", RL §2.2),
  `GetReserveAmmo` (the HUD's reserve count), grenades and unconverted guns, the rifle grenade (alt mode), the GameInfo's
  folded call, and every gun whenever the pipeline is not live: the host gone or not engaged, the hands stale, `OnDraw`
  or the gun bake stale, stereo off, a view-model hook missing (the game reloads by itself again). **[corr X2]** With the
  liveness terms and the install condition, "no gun is ever left unloadable" holds for the cases above.
  **[open X2]** `Weapon.HideViewModel=1` calls the pawn's `HideWeapon` → `Weapon.Mesh.SetHidden(true)`
  (`MOHAPlayerPawn.uc:1786-1795`, re-issued by `vr_view.cpp:316-328`); whether a hidden gun is still updated (baked) is not
  known. The install condition excludes it; M6 can test whether the liveness term alone would suffice.
- Side effects read in script: `EALAWeapon.Active.BeginState` re-issues a held trigger only if `HasAnyAmmo ||
  HasReserveAmmo` (`EW:1348`): an empty gun no longer auto-refires (correct). `EALASmallArms.Active.BeginState`
  (`SA:1100-1116`) toggles alt mode off only when `bAlternateFireMode` (unchanged for mode 0).

### 2.6 The game's reload button

`R` and the pad's A (= the right controller's B and the right grip, `A=b,rgrip`, `config/MOHAVR.ini:206`) run exec
`Reload` → `StartReload` → `CanManualReload()` = `!HasFullClip && HasReserveAmmo && GameplayAllowsReload` (`SA:1190-1194`)
→ false for a converted gun: no reload happens. **[corr E2]** Not "nothing": `EALASmallArms.Active.StartReload` calls
`super(EALAWeapon).StartReload(FireModeNum)` **before** it tests `CanManualReload` (`SA:1196-1198`), and that emits
`MOHAPawn.NoiseRadius(WeaponReloadNoiseRadius)` (`EW:822-830`; 1500, `DefaultWeapon.ini:143`). `WeaponIronsights`
extends `Active`, so it applies there too. So every R / pad-A press (including the gun-hand grip) makes the reload noise
for the AI, as it does today on a full clip. Harmless; recorded as R7b. While the manual reload is active the host masks
the gun hand's B from the pad (it is the magazine release, §3.6); the grip still sends Xbox A, so Use/flare/skip keep
working.

### 2.7 Properties, functions, threads

| Datum / function | Where | How |
|---|---|---|
| pawn | `aim::LocalPlayerPawn()` | |
| weapon, inventory manager | `Pawn.Weapon` (+0x3A8), `Pawn.InvManager` (+0x3A4) | reflection (`names::PropertyOffset`), ENGINE-NOTES 5v |
| clip, capacity | `EALAWeapon.AmmoCount[3]` (+0x2D4), `MaxAmmoCount[3]` (+0x2E0) | reflection, element 0; static cross-check logged in M0 (D/pe: `GetAmmoInClip` 0x10F0D5F0 reads `[ecx+eax*4+0x2D4]`) |
| alt mode, infinite | `bAlternateFireMode` (bit 0x400 of +0x2EC), `bInfiniteAmmo` (bit 0x1 of +0x2EC) | `names::BoolProperty` |
| ammo class | `AmmoClass[3]` (+0x2FC) | reflection |
| attachment class, upgrade level | `AttachmentClass` (`EW:95`), `CurrentUpgradeLevel` (`EW:98`, −1 = none) | reflection; **[corr X4]** the key is `names::Name(ReadPointer(w + off))` (the class object's name), not `names::ClassName` (which returns the object's class: "Class"). The drawn part's key stays `ClassName(Outer(comp))` (`viewmodel.cpp:376`) |
| object identity | UObject Index +0x04 (ENGINE-NOTES 5u) | cache keys (§2.1) |
| reserve | `MOHAInventoryManager.AmmoStorage[10]` (+0x228, 12 bytes: class +0, amount +4, max +8), `NumAmmoClasses` (+0x2A0) | reflection; D/pe: `GetReserveAmmoCount` 0x10F0D7D0 (subclass match); the writer `AddReserveAmmo` matches exactly (§2.2) |
| state name | `StateFrame` +0x18 → `StateNode` +0x2C → Name +0x2C | ENGINE-NOTES 5ad |
| taped half | `TapedMagMode` (MOHAStg44/MOHABar) | reflection (logged only) |

| Code | Thread / when | Does |
|---|---|---|
| `Hook_ExecHasReserveAmmo` | game thread, inside the script VM (also for the GameInfo's folded native, §2.5) | the §2.5 override |
| `reload::OnDraw(hdr)` in `Hook_Draw` right after `RunHostCommand` (`vr_view.cpp:390`) | game thread, once per Draw | detect (§2.3), apply the host's events in order (§4), write ammo, publish caps/ammo/state and the geometry of the last bake **[corr]** only when a bake of the current gun happened since the last Draw (§5.5); stamps the `OnDraw` time for the §2.5 liveness term |
| **[corr X3]** `viewmodel::OnPlayerView`: `ReadHands` (`viewmodel.cpp:489`) is extended to copy `reloadFlags`, `reloadKeyHash`, `magPull`, `magPose` and `rack` **in the same seqlock pass as `hand[]`** (or one combined reader of gun, hands and reload) | game thread, each player view | `ReadGun` (`viewmodel.cpp:434`) and `ReadHands` are separate passes today and the host rewrites the whole view block every XR frame (`main.cpp:711-726`), so separate passes can straddle a host frame; a third pass would put the held magazine on a different host frame than the free hand it is drawn in (drawn from `hand[o]`). Map and mirror the pose like the off frame |
| `reload::OnGunBake(...)` from `arms_ik.cpp` right after the bake loop (line 589), gun only | game thread, every gun update (MidHook 0x10CFAFAD) | the bone overrides (§5) and the geometry sample; stamps the bake time for the §2.5 liveness term |
| `reload::Install(cfg)` from `view::Install` after `armsik::Install` (`vr_view.cpp:1071`) | start-up | bytes check, hook, parse `[ManualReload]`; **[corr X2]** only when the Draw hook exists and `armsik::Install` returned true (`vr_view.cpp` keeps that result) and `HideViewModel=0` |
| host `src/host/reload.cpp` (owned by `Hands`) | host, each XR frame | the state machine (§3) |

No console command and no ProcessEvent are needed (both optional later: §8 R7/R8).

---------------------------------------------------------------------------------------------------------------------

## 3. Host interaction design (`src/host/reload.cpp/.hpp`, class `ManualReload`, owned by `Hands`)

### 3.1 Active

The manual reload drives the gun in hand when all hold: the menu's "Manual reload" on && `reloadCaps` bit0 (converted
gun) and bit2 (the hook is installed) && the game's `reloadKey` is fresh (its `reloadGeoSeq` advanced within 250 ms) &&
`gunValid` && `in.gestures` (no menu open) && the frame has a view (`lastMeta.hasView`) && not alt mode (bit3).
Otherwise today's gesture reload runs (`hands.cpp:198-203`). The state map is keyed by `reloadKey` (exact per Draw),
not by the 250 ms `weaponKey`.

**[corr X2] "Engaged" (`reloadFlags` bit4), distinct from the toggle (bit0) and from Active:** the host sets it while the
toggle is on, `reloadCaps` bit2 (installed) is set and `reloadGeoSeq` advanced within 250 ms, whatever gun is in hand.
The game blocks (§2.5) only while engaged, so a pipeline that never runs (stereo off, a failed hook) never blocks. Every
Active frame is also an engaged frame (Active's conditions include engaged's), so the game blocks whenever the host
drives a gun (one frame of lag at the start, harmless: the host has no magazine out yet). **[corr]**
`reloadGeoSeq` is bumped only on a Draw that has a fresh bake of the current gun (§5.5), so "fresh" means baked, not just
"`OnDraw` ran".

**[corr] When Active drops mid-gesture** (the MOHAVR menu opens, the game hitches past 250 ms, tracking is lost, the
gun leaves the hand): a Grabbed magazine goes back InGun (`magPull` 0, no event); a magazine InHand is DROPped (DROP
event, state Out: the game still has `magIn` false, its rounds already went back at EJECT); a held action is let go
without a RACK (`rack` 0, even if armed). The release-button mask and the consumed grips stay on until those presses end
(§3.6), so no stray Xbox A or grip reaches the pad. When Active returns, §3.2's reconcile brings the host to the game's
`magIn`.

### 3.2 The magazine (per gun: InGun | Grabbed | InHand | Out)

`o` = the off hand's aim pose, `gun` = the final gun pose (after the foregrip turn, `hands.cpp:213-219`). World spots:
`magGrabW = gun.p + R(gun)·magGrab`, `magOutW = R(gun)·magOut` (likewise the bolt).

| From | Input | Condition | To | Event | Haptics |
|---|---|---|---|---|---|
| InGun | release button (gun hand), press edge | — | Out | EJECT | gun 40 ms 0.6 |
| InGun | off grip press | the magazine is the nearest candidate (§3.4), `|o − magGrabW| < magGrabR` | Grabbed (`start = o.p`) | — | off 30 ms 0.5 |
| Grabbed | each frame | `pull = clamp(dot(o.p − start, magOutW), 0, PullOut)` → published as `magPull` | Grabbed | — | — |
| Grabbed | `pull ≥ PullOut` | — | InHand, `heldRel = inv(o) · Pose(R(gun), magGrabW + magOutW·PullOut)` (kept as grabbed: no snap) | EJECT | off 40 ms 0.6 |
| Grabbed | release button | — | InHand at the current pull | EJECT | both 40 ms 0.6 |
| Grabbed | off grip released | — | InGun (`magPull` 0) | — | — |
| Out | off grip press in the pouch | `ammoReserve > 0` (= `R + O`, §2.2) or infinite | InHand, `heldRel = Pose(q(fit.angle about x), Hold')` (the magazine sits in the off hand the way the gun sits in the gun hand); **[corr]** `Hold'` = `Hold` with its sideways (x) component negated when the off hand is the right hand (a left gun hand), as `hands.cpp:228` does for the fit's `rayRight` | TAKE | off 30 ms 0.5 |
| Out | off grip press in the pouch | reserve 0 | Out | — | off 60 ms 0.2 |
| InHand | each frame | `magPose = o · heldRel`; insert when `|magPose.p − magGrabW| < InsertRadius` and `angle(R(magPose)·magOut, magOutW) < InsertAngle` | InGun | INSERT | both 50 ms 0.9 |
| InHand | off grip released | — | Out (the magazine is gone; nothing spawned) | DROP | — |
| Grabbed / InHand | the gun leaves the hand (key change, holster draw) | — | old gun: Grabbed → InGun; InHand → Out | DROP | — |

**Reconcile with the game:** when every event sent has been acknowledged (`reloadEvtAck` = the last sent) and the game's
`reloadState` bit0 (`magIn`) disagrees with the host for 2 frames, adopt the game's (InGun, or Out; a held magazine is
dropped). `reloadPawnSeq` changed → every gun InGun.

### 3.3 The action (rack)

| Situation | Gesture | Visual (game, §5.3) | Event |
|---|---|---|---|
| The action is not held back (`reloadState` bit3 clear: ready guns; Thompson/MP40 empty = forward) | off grip at `boltGrabW` (`< BoltGrabR`), pull along `boltBackW`: `rack = clamp(dot(o.p − start, boltBackW) / max(boltTravel, RackMin), 0, 1)`; **armed** at `rack ≥ RackArm` | the action follows the hand from its held Z toward full back | RACK on grip release, or when the hand comes forward again (`rack < 0.3`) after arming; unarmed release: nothing |
| The action is held back (bit3: G43/Colt/C96 locked; StG44/BAR handle held; remaining travel < 2 u) | "tug and let go": grab, pull ≥ `RackTug` (armed; 0 = at once), release | stays back while held, snaps home when the rack feeds a round | RACK on release |

Haptics: armed 20 ms 0.4 (off); RACK: both 40 ms 0.8 when `rackNeeded` (bit4) was set, else a light 30 ms 0.3 (a
press-check on a loaded gun). The host allows a rack any time; the game decides its effect (§2.2).

### 3.4 Press priorities (off hand)

At an off-hand grip press: collect the reload candidates within their radius — the pouch (only while Out; the hand
inside the pouch sphere), the magazine (only while InGun), the action (any time a bolt bone exists) — and take the
nearest by `distance / radius`. They win over the foregrip, and over a holster zone the hand is also in (the existing
code tests holsters first, `hands.cpp:180-204`; the pouch touches each hip spot by ~1.4 cm: centres 26.6 cm apart,
radii 16 + 12). No reload candidate → today's order (holster zone, then foregrip), **[corr X11] and the fixed-spot
gesture reload only while not Active**: while Active it is off (§3.8), since for a converted gun its `Reload` is blocked
anyway yet would still consume the grip, pulse, and make the AI reload noise (`SA:1196-1198` → `EW:822-830`). A press
used here is `consumed_` (kept from the pad until released), as today.

### 3.5 Thresholds and tuning (all [H]; `[ManualReload]`, cm and degrees)

| Key | Default | Why |
|---|---|---|
| `MagR` (per gun, published as `magGrabR`) | 7 (drum 9, Colt 6) | covers the exposed magazine (D/grab) |
| `PullOut` | 4 | the drum slides 3.6 u; shallow enough for a quick pull (inserted depths 5-12 u) |
| `InsertRadius`, `InsertAngle` | 5, 40 | forgiving snap ("within a few cm it snaps in", the approved plan) |
| `BoltGrabR` | 5 | handle knobs are small |
| `RackArm`, `RackMin` | 0.85, 4 | IN §2.3 |
| `RackTug` | 1 | a deliberate tug on a locked action |
| `Hold` | 0 0 0 | where the magazine's grab point sits from the off hand's aim origin, authored for the left off hand; mirrored (x negated) for a right off hand (§3.2) |
| `ReleaseButton` | upper | B (right) / Y (left); `lower` = A/X; `none` |
| `KeepChambered`, `ChamberPlusOne` | 1, 0 | P3, §0.5.3 |

### 3.6 The release button and the pad

- `Pad::FaceButton(session, hand, upper)` reads B/Y (upper) or A/X, and the raw test values (like `GripValue`,
  `pad.cpp:585-593`); edge with hysteresis 0.6/0.4.
- `Pad::SetMaskedFace(hand, upper, on)` zeroes that source at the top of `Map()` next to the consumed grips
  (`pad.cpp:248-250`). Set it before `pad.Update` in the same frame (`main.cpp` order: `hands.Update` → view seqlock →
  pulses/`SetConsumed` → `pad.Update`), so Xbox A never sees the press. Mask only while Active, and keep masking a press
  that began while Active until it is released (no stray Xbox A when Active ends mid-press). `LeftHanded` swaps triggers
  and grips only (`pad.cpp:257-260`), so the face buttons are taken by physical hand.
- `Pad::Pulse(session, hand, amp, ms)`: an overload; today's `Pulse` is fixed 30 ms at 0.5 (`pad.cpp:606-614`).

### 3.7 The pouch, the Holsters page, rings, the menu

- **Pouch:** `kSpots = kHolsters + 1`; `SpotName(4)` = `L"MagPouch"` → `[Holsters] MagPouchSpot=0 -60 14 12` (same format
  and frame as the holsters, `hands.cpp:56-73`). The holster zone loops stay on `kHolsters` (`hands.cpp:129, 146, 159`);
  the pouch runs no command. The Holsters page cycles 5 spots (`menu.cpp:32` label "magazine pouch", 172, 427) and saves
  it in the player's ini (`SaveHolster`, `menu.cpp:215-220`).
- **[corr X8] Every place sized or looped by `kHolsters` that must see the 5th spot** (grep of `src/host`):
  `main.cpp:401-403` (`defaults[kHolsters]` for `LoadHolsters`) and `main.cpp:701` (the per-frame `SetSpot` loop: without
  it the pouch moved in the menu never reaches `Hands`); `menu.hpp:64` (`LoadHolsters` signature) and `menu.hpp:110`
  (`spots_`, `spotDefaults_`); `menu.cpp:32` (labels), `171-172` (`LoadHolsters`), `427` (the selector's modulo), `654`
  (`RenderHolsterPage` indexes `spots_[holsterSel_]` and the labels); `hands.hpp:89-90` (`zones_`, `spots_`,
  `defaultSpots_`); `hands.cpp:45-46` (`SpotName`'s `kNames`) and `57-58` (the built-in defaults); `markers.cpp:89`
  (`draws`) and `markers.hpp:32` (`quads_`). The zone loops at `hands.cpp:128-129, 146, 159` stay on `kHolsters` (the
  pouch runs no command). Everything else uses `kSpots`.
- **Rings** (`Hands::SpotKind` + `kPouch`, `kBolt`; `Output::spots[kHolsters + 4]`, `hands.hpp:63`; `Markers::quads_` and
  `draws` `kHolsters + 5`, `markers.hpp:32`, `markers.cpp:89`; the pouch in `showAll`): the magazine ring at `magGrabW`
  (InGun: grab spot; InHand: the insert target, lit within `InsertRadius`); the bolt ring when `rackNeeded`; the pouch
  ring while Out. **[corr X8]** The pouch spot is emitted into `Output::spots` whenever the Holsters page is open (not only
  while Out), and `markers.cpp:94`'s `showAll` test accepts `kPouch` as well as `kHolster`; otherwise the player could not
  see the pouch while moving it. Layers: 1 projection + 1 reticle + 9 rings/dot + 1 menu = 12 of 16 (`main.cpp`
  layers[16]).
- **Menu:** a main-page item "Manual reload < on / off >" (`menu.cpp:20` enum), the player's own once toggled (saved as
  `[Weapon] ManualReload` in their ini), default the shipped `[Weapon] ManualReload` (the Frame pacing pattern,
  `menu.cpp:155-159`). Published live as `reloadFlags` bit0.

### 3.8 The fallback: today's gesture reload

For any gun that isn't Active (unconverted: Garand, K98, Springfield, M12, launchers, the C96 below upgrade 1, a gun
whose data failed the guard; the switch off; no geometry yet) the off hand's grip at the fixed magazine spot (8 cm
below, 10 cm ahead of the gun hand, `hands.cpp:120, 198-203`) sends the game's `Reload`, exactly as today
(`[Hands] ReloadGesture=1`). **[corr X11]** Only then: while Active the fixed-spot gesture is off (§3.4). Note that the
gesture needs `Hook_Draw` too (`RunHostCommand` runs only there, `vr_view.cpp:390`): with stereo off neither the manual
reload nor the gesture can run, so only the game's own reload is left, which is why the hook must not block there (§2.5).

### 3.9 Test channels (tests only)

- `pad_cmd.txt` `reload=eject|take|insert|rack|drop`: the host puts that event into the ring for the gun in hand
  (M1: the game rules before any gesture exists).
- The host logs the spots (`magGrabW`, `boltGrabW`, the pouch) in the head's heading frame about once a second while a
  test pose is active; optional targets `hand=l,@mag|@bolt|@pouch[,dx,dy,dz]` in `Pad::LocateHands` (IN §6).
- `Debug.ReloadTrace=1`: per Draw (game) and per frame (host): flags, pull, rack, the drawn magazine grab point against
  the off controller (world units), the drawn action Z.

---------------------------------------------------------------------------------------------------------------------

## 4. Shared block v14 (`src/common/shared_frame.hpp`)

`kVersion` 13 → **14** ("14: manual reload"). The host refuses a mismatch (`main.cpp:166`): both binaries ship together.
Appended after `slotViewQpc` (v13 ends at 1248, `shared_frame.hpp:200-201`); 4-byte fields only, so x86 = x64:

```cpp
    // --- v14: manual reload ([Weapon] ManualReload; src/mohavr/reload.cpp, src/host/reload.cpp) ---------------------
    // game -> host, once per Draw (seqlock reloadGeoSeq: odd while the game writes). Geometry in the host's gun frame
    // (x right, y up, z back, metres), already un-mirrored for a left gun hand.
    volatile std::uint32_t reloadGeoSeq;   // 1248
    std::uint32_t          reloadCaps;     // 1252 bit0 converted gun in hand, bit1 action bone(s) found, bit2 hook
                                           //      installed [corr: was "installed and blocking now", which with the
                                           //      engaged bit would be circular], bit3 alt-fire mode, bit4 the §2.5
                                           //      filter passes for this gun now (diagnostic: logs and trace only)
    char                   reloadKey[48];  // 1256 the attachment class the geometry is for
    float                  magGrab[3];     // 1304 the in-gun magazine's grab point = the insert target
    float                  magOut[3];      // 1316 unit: the way the magazine leaves the well
    float                  magGrabR;       // 1328 metres: its grab radius
    float                  boltGrab[3];    // 1332 the handle / slide grip point at its held position (without the pull)
    float                  boltBack[3];    // 1344 unit: the pull direction
    float                  boltTravel;     // 1356 metres from the held position to full back
    std::int32_t           ammoClip, ammoMax, ammoReserve;  // 1360 1364 1368 (ammoReserve = R + owed, §2.2)
    std::uint32_t          reloadState;    // 1372 bit0 magIn, bit1 pending, bit2 ready (closed: C >= 1; open: cocked),
                                           //      bit3 action held back (rack = tug and let go), bit4 rack needed,
                                           //      bit5 open-bolt gun, bit6 infinite ammo
    volatile std::uint32_t reloadPawnSeq;  // 1376 bumped on a new local pawn: the host resets every gun to InGun
    volatile std::uint32_t reloadEvtAck;   // 1380 the last event the game processed (applied or rejected)
    // host -> game, per XR frame INSIDE the view seqlock (viewSeq), next to gunPose
    std::uint32_t          reloadFlags;    // 1384 bit0 manual reload on (the player's toggle); bits 1-2 the magazine
                                           //      (0 in gun, 1 grabbed, 2 in the off hand, 3 out); bit3 action held;
                                           //      bit4 engaged (the pipeline is alive, §3.1; the game blocks only then)
    std::uint32_t          reloadKeyHash;  // 1388 FNV-1a 32 of the attachment class these flags are for
    float                  magPull;        // 1392 metres the grabbed magazine is drawn out along magOut
    Pose                   magPose;        // 1396 the held magazine's grab-point frame in LOCAL (the gun frame's axes)
    float                  rack;           // 1424 0..1 of boltTravel pulled
    // host -> game events, ordered: reloadEvt[seq % 8] written, then reloadEvtSeq bumped (the cmdSeq pattern)
    volatile std::uint32_t reloadEvtSeq;   // 1428
    std::uint32_t          reloadEvt[8];   // 1432 low byte: 1 EJECT, 2 INSERT, 3 RACK, 4 TAKE, 5 DROP;
                                           //      high 24 bits: the low 24 bits of the key hash
};                                         // sizeof 1464 (a multiple of 8)
```

Static asserts: `reloadGeoSeq`==1248, `reloadKey`==1256, `magGrab`==1304, `magGrabR`==1328, `boltTravel`==1356,
`ammoClip`==1360, `reloadState`==1372, `reloadEvtAck`==1380, `reloadFlags`==1384, `magPose`==1396, `rack`==1424,
`reloadEvtSeq`==1428, `reloadEvt`==1432, `sizeof(Header)`==1464 (`Pose` is 28 bytes, align 4: 1396 → 1424).

Readers: **[corr X3]** no separate `ReadReload` pass: `ReadHands` (`shared_frame.hpp:287-303`) grows to copy `reloadFlags`,
`reloadKeyHash`, `magPull`, `magPose` and `rack` in the same seqlock pass as `hand[]` (or one combined reader of gun,
hands and reload replaces `ReadGun` + `ReadHands`), so the held magazine and the free hand it sits in always come from the
same host frame. `ReadReloadGeo(h, ...)` for the host (a seqlock on `reloadGeoSeq`, the `ReadFit` pattern). The game
processes at most 8 events per Draw (`while (seen != reloadEvtSeq)`), skips an event whose hash doesn't match the weapon
in hand (log + ack), and writes `reloadEvtAck`. Why a ring: an EJECT and an INSERT can land between two game frames, in
order. **[corr] Overrun rule** (IN §3.3 had "log and resync"; the first draft dropped it): the host never writes while
`reloadEvtSeq − reloadEvtAck ≥ 8` (it holds the event for the next frame; if the game has not acked within 250 ms, the
host drops it with a log, and the reconcile of §3.2 settles the state); the game, if it ever finds
`reloadEvtSeq − seen > 8`, logs an overrun, sets `seen = reloadEvtSeq`, acks, and lets the reconcile resync.

---------------------------------------------------------------------------------------------------------------------

## 5. Game-side bone overrides in the bake (`reload::OnGunBake`, called from `arms_ik.cpp` after line 589, gun only)

The bake today: `bones[i] = saved[i] · kMove`, `kMove = A · inv(L2W)`, `A = L2W · D'` (row vectors; `arms_ik.cpp:579-589`);
the game's pose is put back after the render copy (`Hook_UpdateTransform`, `arms_ik.cpp:609-620`), so game code, sockets,
the flash and the brass never see an override. Overrides replace `bones[j]` for the listed bones only.

### 5.1 Resolve (per mesh, cached by mesh pointer **[corr]** plus its bone count, cleared on `reloadPawnSeq`)

The gun component is the first-person part whose Outer isn't the pawn; its Outer's class is the key (ENGINE-NOTES 5u,
`ClassName(Outer(comp))`, `viewmodel.cpp:376`). Apply only when **[corr X4]** `names::Name(AttachmentClass)` of
`pawn.Weapon` (§2.5) equals it (during a switch the old gun draws as the game has it). Find the table's bones by name in
the RefSkeleton (mesh +0x7C, stride 68, `addresses.hpp:241-243`), plus descendants by ParentIndex (+56) (none for Step
1; never by the stale `NumChildren`, §0.2), run the ref-pose guard (§1.3). A cache keyed by the mesh pointer alone could
be fooled by another level's mesh (R19) or a GC-reused address: the key is (pointer, bone count), and the cache is dropped
on a new pawn. The **visible magazine variant** = the first listed whose `saved` 3x3 has `|det| > 1e-3` (hidden upgrade
parts get BoneScale 0: `MOHAWeaponUpgrade.uc:59-92` sets `ControlStrength` 1, `SetSkelControlActive(true)`, `BoneScale =
visible ? 1 : 0`; `BoneScale` is `SkelControlBase.uc:31`; SK, IN E7). **[open C22]** How the engine applies `BoneScale`
to SpaceBases (the 3x3 only, or the translation too) was not read in the exe. The `|det|` test and the Out collapse work
either way; M0 logs `|det|` and the translation of every hidden variant.

### 5.2 The magazine group (every listed variant moves together; hidden ones stay collapsed)

The state used: the host's (`reloadFlags` bits 1-2) when `reloadKeyHash` matches this class; otherwise the game's
`magIn` (InGun or Out).

| State | Override |
|---|---|
| InGun | none (the game's pose) |
| Grabbed | `M_j = saved[j] · T(out · d)` (a mesh-space slide along the variant's `MagOut`), `bones[j] = M_j · kMove`. **[corr C46]** `d` = the host's pull in mesh units, through `A`: `d = magPull · upm / |out · A₃ₓ₃|` (= `magPull · upm` when the component scale is 1) |
| InHand | `bones[j] = saved[j] · A · inv(Fgrab) · Fheld · inv(L2W)`, with `Fgrab` = G's axes at `grabW = MagGrab · A` (the in-gun grab frame) and `Fheld` = the host's `magPose` mapped to the world (`view::PoseFrameToWorld`, mirrored with `MirrorFrame` when mirrored, like the off frame, `viewmodel.cpp:489-492`) `· carry`. Check: a magazine held exactly at the in-gun grab frame gives the identity. |
| Out | `saved[j]` with its 3x3 zeroed (origin kept), `· kMove`: the same collapse as the game's own upgrade hiding (if M0 shows the engine also scales the translation, this is still a collapse to a point; nothing depends on where) |

**[corr X3]** The magazine's frame is copied in `OnPlayerView` **in the same seqlock pass as the hand frames** (`ReadHands`
extended, §4), so it comes from the same host frame as the free hand it is drawn in. It was claimed to be "the same host
frame as `D` and the hand frames"; that was false for a separate reader, and even today `ReadGun` (`viewmodel.cpp:434`)
and `ReadHands` (`:489`) are two passes that can straddle a host frame (the host rewrites the view block every XR frame,
`main.cpp:711-726`). Merging `ReadGun` into the same pass would also tie `D` to it (a small, separate improvement).
Reading it later in the bake would put the magazine a host frame ahead of the drawn hand while moving (IN §3.1).

### 5.3 The action group: the empty hold and the rack

For the action bone `b` (translation Z only; D/tracks):
1. `z_s = saved[b].row3.z` (the game's animated position).
2. Hold: active when (closed: `C == 0`) or (open: `!cocked`), and the gun's `Empty` isn't `none`.
   `z_h = hold ? (zEmpty < zIdle ? min(z_s, zEmpty) : max(z_s, zEmpty)) : z_s`.
3. Rack: when the host's bit3 (action held) is set for this key: `z_d = z_h + rack · (zBack − z_h)`; else `z_d = z_h`.
4. `M_b = saved[b]` with `row3.z = z_d`; `bones[b] = M_b · kMove`. The BAR's rotated bone frame doesn't matter: the
   delta is on the translation row (mesh space).
5. MP40: `chamber_slide` gets `row3.z = f(z_d)`, piecewise-linear through (10.44 → 25.04), (17.31 → 25.73),
   (27.61 → 29.91).

On the Draw that applies a RACK (or an INSERT into a cocked open-bolt gun), the hold ends and the host's rack drops to
0: the action snaps to the game's idle position (forward on closed bolts, back on the Thompson/MP40). At the last shot the
hold takes over at once (closed bolts: C is 0 from the shot; open bolts one bake later, when `OnDraw` clears `cocked`):
**[corr E9]** the action snaps to the hold at the shot rather than being "caught" as the fire anim returns.

### 5.4 The top round

`TopRound` (MP40, G43, Colt `bullet`; C96 `clip`) is drawn only when the magazine is InGun and has rounds (closed:
`C ≥ 2 || pending`; open: `C ≥ 1 || pending`); otherwise collapsed like §5.2 Out. It never moves with a held magazine
(simplest; it sits at the top of the magazine only while seated). This finishes "chamber open, empty" (AN §3).
**[corr]** V `bulletgeo.txt` (geometry at the idle pose): the G43 and Colt `bullet` sit at the magazine lips (the top
round, as the rule assumes); the MP40 `bullet` sits just forward of the magazine top (a round at the feed / chamber),
where hiding it when empty is still right; the C96 `clip` sits inside the magazine's upper volume (M, §1.1).

### 5.5 The geometry for the host (sampled every update, published in `OnDraw` from the last one)

The parts update up to 6 times a frame while moving and only the last pair is drawn (ENGINE-NOTES 5ad), so the sample is
stored per update and published once per Draw. **[open C32]** That count is ENGINE-NOTES 5ad's measured runtime fact, not
re-measured here; the design does not depend on the number (it always publishes the last sample of the frame). M0 can
count gun bakes per Draw. **[corr]** `reloadGeoSeq` is bumped only when a bake of the current gun happened since the last
Draw; a gun that stopped being baked therefore goes stale for the host within 250 ms (it used to look fresh because
`OnDraw` republished the last bake every Draw). With `upm` the live world scale (`view::HeadInWorld`) and `G` = the gun
controller frame `· carry`:
- `grabW = MagGrab · A` (the visible variant's grab point), `outW = normalize(MagOut · A₃ₓ₃)`, `magGrabR = MagR/100`
  (cm → m).
- `p = saved[b].row3` with its z replaced by `z_h` (the held position, without the host's own pull), plus `BoltGrab`
  (a mesh-space offset); `boltGrabW = p · A`; `backW = normalize((0, 0, −1) · A₃ₓ₃)`; **[corr C46]** `boltTravel =
  |(0, 0, zBack − z_h) · A₃ₓ₃| / upm` (through `A`, not assuming a unit component scale; the host's `rack` is a fraction
  of it, so the game side needs no conversion back); `reloadState` bit3 = `|zBack − z_h| < 2` u (mesh units).
- Into the host frame: `local = (p − G.origin)` on G's rows (forward, right, up) → host `(right, up, −forward) / upm`
  (directions without the origin and the scale).

### 5.6 The left hand (Weapon.LeftHandMirror)

The bake runs in the mirror world: G is `MirrorFrame(ctrl, R)` (own Y row negated, then the world reflected,
`viewmodel.cpp:53-58`), and the parts are drawn through `R`. A point with local coordinates (f, r, u) in the mirrored G
is, after the draw's reflection, the point (f, −r, u) of the real controller frame (IN §2.2; `MirrorFrame(T(g)·Gr) =
T(S g)·MirrorFrame(Gr)`, S = diag(1, −1, 1)). So the game **negates the right component of every published point and
direction when mirrored**, and the host does not deal with the mirror for anything the game publishes. **[corr]** One
exception on the host's own side: the `Hold` offset of a pouch magazine is in the off hand's frame and is authored for
the left off hand; with a left gun hand (right off hand) the host negates its sideways component, as `hands.cpp:228`
does for the fit's `rayRight` (§3.2). The held magazine's pose comes in real and is mirrored by the game like the off
frame. Mesh-space overrides (§5.2 Grabbed, §5.3) are unaffected by the mirror.

---------------------------------------------------------------------------------------------------------------------

## 6. Switches and fallbacks

| Switch | Default | What |
|---|---|---|
| `[Weapon] ManualReload` | **0** until M6 passes in [S] (standing rule 7), then 1 for the headset round | the default of the menu's "Manual reload" (the player's own once toggled); published live |
| `[ManualReload] Hook` | **[corr] 0 until M1 passes in [S], then 1** (standing rule 7: an inline code patch, even a pass-through one on a function the GameInfo shares, is not on by default before the simulator has proven it) | install the `execHasReserveAmmo` hook (with `Weapon.ArmIK=1`, `ViewModel=2`, the Draw hook present, `armsik::Install` succeeded, `HideViewModel=0`: §2.5); it passes through while the toggle is off or the pipeline is not engaged. 0 = never installed (hard off: caps bit2 stays clear, the host is never Active; the menu shows the item as unavailable). M0 installs it count-only (never writes `*Result`) under `Debug.ReloadProbe=1` |
| `[ManualReload] KeepChambered`, `ChamberPlusOne` | 1, 0 | P3; §0.5.3 |
| `[ManualReload] ReleaseButton`, `PullOut`, `InsertRadius`, `InsertAngle`, `BoltGrabR`, `RackArm`, `RackMin`, `RackTug`, `Hold` | §3.5 | host tuning |
| `[ManualReload] Attachment_<Gun>=...`, **[corr]** `Attachment_<Gun>.Ref=...` | §1.3 | per-gun data; `Empty=none` per gun; the ref-pose guard's RefSkeleton values |
| `[Holsters] MagPouchSpot` | `0 -60 14 12` | the pouch (the player's own from the Holsters page) |
| `[Hands] ReloadGesture` | 1 | today's gesture, the fallback |
| `[Debug] ReloadTrace`, `ReloadProbe` | 0, 0 | per-frame trace; M0 logging |

| Fallback (each logged once) | Result |
|---|---|
| No `[ManualReload]` line, no magazine bone found, the ref-pose guard fails (fingerprint or positions, §1.3), or the C96 below upgrade 1 | caps bit0 clear: the gesture reload; the hook doesn't block; the game reloads |
| **[corr X10]** Hook bytes differ | **the whole mod stands down**: the bytes are a `kSignatures` row, and `CheckBuild` runs in `DllMain` and stops everything on any mismatch (`dllmain.cpp:49-52`, `build_check.cpp:34-39`). Same convention as the muzzle and arm-IK signatures; the per-feature row below is unreachable for this cause |
| `Hook=0`, `ArmIK=0`, `ViewModel≠2`, **[corr X2]** `Camera.Stereo=0` or the Draw hook failed, `armsik::Install` failed, `HideViewModel=1` | the hook is not installed; caps bit2 clear: never Active (no host "magazine out" while the game auto-reloads); the game's own reload works |
| No action bone | bit1 clear: the rack isn't needed: INSERT into an empty gun feeds at once |
| `bAlternateFireMode` (G43 grenade) | bit3: the game's own reload |
| No hands (HandFrames stale), host gone or not engaged, **[corr X2]** `OnDraw` or the gun bake stale (> 250 ms) | the hook stops blocking; the game reloads by itself; the host/game re-sync on the next rise of `C`. A hitch over 250 ms counts too: the first tick after it may let the game reload an empty gun; the re-sync adopts it |
| **[corr]** Active drops mid-gesture (menu, hitch, tracking lost) | §3.1: Grabbed → InGun, InHand → DROP (Out), a held action released without a RACK; masks kept until the presses end |
| `Weapon.FreeOffHand=0` | works, but the drawn support hand stays on the gun while the magazine follows the controller (ini comment) |

---------------------------------------------------------------------------------------------------------------------

## 7. Milestones (in order; one commit each with STATUS / ENGINE-NOTES / DECISIONS, rule 10)

Harness setup for every [S] step (CLAUDE.md): back up the user data; `& tools\deploy.ps1 deploy -Set
'Render.ResX=0','Render.ResY=0','OpenXR.RuntimeJson=<x64 simulator json>','Debug.GameCommands=1',...`; `harness.ps1
launch`, `to-gameplay` (the save resumes in Var_Flk with the StG44, BAR and Colt: ENGINE-NOTES 5u, §4); keep every log
before a relaunch; undeploy and restore at the end. The [H] questions of M1-M6 go into one headset round for Step 1.
**[open C43]** The save's level and weapons are a runtime fact from earlier sessions (ENGINE-NOTES.md:128-129, 5u: the
save resumes over the flak tower; `Attachment_Stg44`, `Attachment_Colt45`, `Attachment_Bar` seen, Y cycles three
weapons); the citation is accurate but not re-checkable statically. M0's first log line (the drawn keys) confirms it.

**M0 — logging run, no behaviour change (`Debug.ReloadProbe=1`) [S].** Settles the assumptions in one short session:
- For each drawn gun (once, at its 10th bake): every table bone's `saved` translation and `|det|`, and its RefSkeleton ref
  position at the assumed +28. Pass (animated `saved`, within 0.05 u): StG44 `Bolt` (0.38, −11.50, 15.00), `tag_barrell`
  (0, −8.31, 64.00), **[corr E8]** `single_magazine` and `upgrade_02_tapedMagazines` at (0, 2.50, 17.68) **or** (−3.64,
  2.50, 17.68) (taped state B idles there, `stg_gun_idle_3b`, `NN/MOHAStg44.uc:136-147`), `single_magazine` |det| 1,
  `upgrade_02_tapedMagazines` |det| 1; BAR `Bolt` (2.00, −6.60, 12.30), `magazine` at (−0.02, 0.73, 20.84) or (2.58,
  8.92, 21.34) (taped B); Colt `gunSlide` (0, −7.00, 2.50), `bullet` (−0.02, −5.74, 0.83) → SpaceBases = mesh space
  **[open C18]** and the visible variants as expected **[open C23]**. (For the G43 later: `magazine` Y −3.03 or −3.15.)
- **[corr E1/X9]** The RefSkeleton positions read at +28 against the §1.3 `.Ref` values (not the §1.1 animation values):
  e.g. MP40 `Bolt` Z 18.575, G43 `bullet` (0, 0, −37.5), C96 `clip` (0, 0, −55). A match settles R2 and turns the guard's
  layer 2 on. Also log the bone count and each listed bone's ParentIndex (the layer-1 fingerprint).
- **[open C22]** Every hidden variant's `saved` 3x3 `|det|` and translation (is BoneScale 0 applied to the 3x3 only?).
- **[open C46]** The norms of the gun component's L2W rows (the component scale), and of `A`'s 3x3 rows.
- **[open C32]** Gun bakes per Draw while standing and while moving (ENGINE-NOTES 5ad says up to 6).
- The weapon's `AmmoCount`/`MaxAmmoCount`/bools/`AmmoClass`/`CurrentUpgradeLevel`/`TapedMagMode` by reflection against
  +0x2D4 / +0x2E0 / +0x2EC / +0x2FC; the reserve entries (+0x228, stride 12, `NumAmmoClasses` +0x2A0), each entry's
  class, amount and cap; **[corr]** for each Step-1 `AmmoClass`, the entry the exact match picks against the one the
  native subclass match picks (§2.2).
- The 59 hook bytes in the running (SteamStub-wrapped) process. **[corr, HIGH]** The hook installed count-only (never
  writes `*Result`): calls counted per `this` class; the level start / respawn must show the GameInfo's folded call
  (`MOHAGameInfo.uc:368`, state `ActualRespawn`) and the pointer test must reject it.
- `EnableCheats`, `GiveWeapon MOHAGameNonNative.MOHAThompson` (then `MOHA_MP40`, `MOHAG43`, `MOHAMauser`) and `UpgradeWeapon 3`
  through `Debug.GameCommands` (`Engine/CheatManager.uc:293-306`, `MOHACheatManager.uc:910`): do the other four guns
  appear, fully upgraded? Decides M5's route. **[corr]** `UpgradeWeapon` acts on the gun in hand (`Outer.Pawn.Weapon`,
  `MOHACheatManager.uc:910-938`), so each `GiveWeapon` is followed by a switch to that gun before `UpgradeWeapon 3`. It
  also grants weapon experience (`GiveWeapExp`), which a save could capture: the harness restores `Saved\` afterwards
  (D8), so the player's save is safe; no save is made during the test.
- [H]: none.

**M1 — the game rules (hook + ammo + events; no visuals, no gestures) [S].** `ManualReload=1`; events from the host's
test channel (`reload=...`, §3.9).
- StG44 fired empty: the log shows no `WeaponReload` state and the hook's blocks; the HUD clip 0 (capture the HUD panel);
  R and pad A reload nothing (**[corr E2]** they still run `StartReload`'s AI noise, not observable in [S]); switch to the
  Colt and back: still 0 (no equip refill).
- StG44: `eject` → clip 1, reserve +k−1; `insert` → min(1+n, 30); fire out; `eject` (0 back), `insert` → pending, clip 0;
  `rack` → 30, reserve −30. BAR (open): `eject` with k → clip 0, reserve +k, cocked; `insert` → 20 at once. Colt: as the
  StG44 with 7. A mid-burst `eject` (StG44) is applied at once (the chambered round still fires).
- **[corr E5]** At the cap: the Colt holding 5 with the pistol reserve at 100 (`GiveAmmo pistol 100`,
  `MOHACheatManager.uc:970-990`, which goes through `AddReserveAmmo` and clamps at the cap): `eject` → clip 1, HUD reserve
  100, the log `owed 4`; `insert` → clip 7, reserve 98, `owed 0`. No round lost.
- `ManualReload` toggled off in the menu → the game's own reload again. Alt mode (G43, if M0 gave it): passes through.
- After M1 passes: `[ManualReload] Hook=1` (rule 7). (The `Camera.Stereo=0` pass, where the hook must not be installed,
  is in M6.)
- [H]: the dry click on an empty trigger sounds right; the reload is now silent (R7) — acceptable for Step 1?

**M2 — the visuals in the bake (holds, magazine collapse, top round) [S].**
- `sim_shot.py` captures + the trace's drawn Z: StG44 fired dry → handle at −2.33; Colt → slide −1.85; BAR → handle −2.80;
  `eject` → the magazine gone (both taped bones); `insert` → back; `rack` → the action home.
- The last shot in slow motion (`SloMo 0.05`, ENGINE-NOTES 5aj): **[corr E9]** the Colt slide snaps back at the shot and
  stays (the hold applies from the fire anim's first frame); the Thompson bolt goes forward one bake after the shot.
- [H] P1/P2: does each empty gun read as empty (locked back; Thompson/MP40 forward; StG44/BAR handle back)?

**M3 — the host's magazine: shared block v14, release button, pull-out, pouch, the magazine in the hand, insert, rings,
haptics, the menu toggle, the Holsters page [S].** IN S2-S4, S8, S9:
- `--raw --press b --dur 0.2` → EJECT, the HUD clip 1 (StG44), Xbox A not pressed (no Use).
- `hand=l,@pouch` + `raw=1 press=lgrip dur=4` → TAKE; `sim_window_shot.ps1`: the gun's own magazine (the taped pair) in
  the left hand; the trace: grab point within 0.5 cm of the controller + `Hold`.
- `hand=l,@mag` (grip held) → INSERT; the magazine back in the gun, the hand empty.
- `hand=l,@mag`, grip, move 6 cm along `magOut` → Grabbed then InHand, kept as grabbed (no snap); release → gone, no
  ammo change, no stray triangles.
- **[open C33 / R10]** Culling of the held magazine: with the magazine InHand, point the gun hand away (the game's gun
  spot stays in front of the game camera) and hold the magazine ~60 cm from the gun near the edge of the view: it must
  stay drawn. (The sphere's size is script, `MOHAPlayerPawn.uc:1190` sets the new gun's `fCustomBoundsSize` = 100, `:1163`
  resets the old one to 0, arms 1000 at `:5939`; that culling uses it at the component origin is ENGINE-NOTES 5ae's
  research finding, not re-checked in the exe.)
- **[corr X8]** The Holsters page: select "magazine pouch", move it; the pouch ring shows while the page is open (with
  the magazine in the gun) and the moved spot is used by the next TAKE (the `main.cpp:701` loop).
- **[corr]** Open the MOHAVR menu while holding a magazine → DROP, state Out; close it → reconcile, no stray Xbox A.
- [H] P4, P5; the pouch's place and size; the 4 cm pull; the 5 cm / 40° snap; the magazine in the fist (`Hold`); the
  haptics; accidental ejects while supporting an SMG by its magazine; the Colt's short base plate.

**M4 — the rack [S].** IN S5, S6, S10:
- Colt fired dry: tug and let go at `@bolt` → nothing without a fed magazine; eject, take, insert (pending), tug and let
  go → clip 7, the slide home. StG44 likewise (handle −2.33 → 15.00).
- An open-bolt gun (BAR in [S]; Thompson/MP40 if M0 gave them): fired dry → pull ≥ 85% of the travel, release → cocked;
  insert → full at once.
- [H]: the rack's travel and feel (Colt 4.35 cm, StG44 17 cm, Thompson 11.7 cm, MP40 17 cm); tug-and-let-go on locked
  actions.

**M5 — the other guns (Thompson drum sideways, MP40 two bones, G43, C96 at upgrade 1) [S].** Through `GiveWeapon` (M0),
else in a level that has them. Per gun: the geometry log (cm, gun frame), the holds, eject/insert/rack. C96 at upgrade
0: unconverted (the gesture reload).
- [H]: each gun once; the drum's sideways insert.

**M6 — the left hand and the fallbacks [S].** IN S11, S12:
- Draw with the left grip at the right-shoulder spot; release with `press=y`; the capture mirrored against the right-hand
  one (as `logs/shots/r26-lh-fix-compare.png`); the magazine ring on the mirrored gun's magazine.
- `ManualReload=0`, `Hook=0`, an unconverted gun: B reloads through the game (state `WeaponReload`), the gesture reload
  works; `harness.ps1 cycle` passes both ways.
- **[corr X2]** `Camera.Stereo=0` with `ManualReload=1`: the hook is not installed, the StG44 fired empty reloads through
  the game (auto and pad A). **[open X2]** `Weapon.HideViewModel=1`: the hook is not installed by rule; separately, log
  whether the hidden gun is still baked (decides whether the liveness term alone would do).
- **[corr]** The left gun hand takes a pouch magazine: the magazine sits mirrored in the right fist (`Hold` x negated).
- Then `[Weapon] ManualReload=1` for the headset round; ENGINE-NOTES 5am (the hook, the ammo layout, the bone data),
  DECISIONS D22 (§0.4-0.5 choices), STATUS, HEADSET-TESTS round.
- [H]: the left-hand round (Y as the release).

**M7 (optional, after the round) — sounds and the AI cue.** The mag-out / mag-in / rack cues per gun are known (RL §6.2,
`vm_reload_notifies.txt`); playing them (and the AI's `NotifyObservers('Reload')`) needs ProcessEvent/FindFunction
(0x10DB1FA0 / 0x109CE980, 0x109ED8D0, RL §8), never used by this mod (L): research time-boxed first.

---------------------------------------------------------------------------------------------------------------------

## 8. Risks and unknowns

| # | Risk / unknown | Status | How to settle |
|---|---|---|---|
| R1 | SpaceBases = mesh space (RotOrigin in LocalToWorld) **[open C18]** | assumed (SK, IN §8); consistent with the socket/brass work (ENGINE-NOTES 5aj) | M0 translations; if rotated, apply RotOrigin (P0, Y −16384, R 16384) to the table's vectors |
| R2 | `FMeshBone.BonePos.Position` at +28 (the ref-pose guard's layer 2) | M (consistent with stride 68 / ParentIndex +56) | M0 log against the §1.3 `.Ref` values; until then only the layer-1 fingerprint runs **[corr]** |
| R3 | Reflection offsets vs the static ones; `MaxAmmoCount` +0x2E0 | M (declaration order `EW:24-26`) | M0 |
| R4 | `GiveWeapon`/`UpgradeWeapon` for the Thompson, MP40, G43, C96 in Var_Flk | L (all 13 guns are cooked in `Var_Flk_P`, SK) | M0 |
| R5 | The HUD with clip 0 + a pending magazine; any "reload" prompt; Max+1 | not examined (the HUD is native) | M1 HUD capture; Max+1 only if `ChamberPlusOne=1` |
| R6 | The dry click under the override | H in script (§2.4); audio not observable in [S] | M1 [H] |
| R7 | **[corr E3]** Lost with the `WeaponReload` state (its `BeginState`, `SA:1935-1948`): the AI's `AxonObserver.NotifyObservers('Reload')` (`SA:1937`), `MOHAPlayerPawn.Weapon_ReloadEvent` (the telemetry), and the arms reload anim with its sound notifies. **Not** lost: the reload noise radius (`EW:822-830` is `EALAWeapon.StartReload`, the button path, still run on every press, §2.6; the state never emitted it); `WeaponAttachment.WeaponBeingReloaded`'s `WeaponReloadSnd` (`WeaponAttachment.uc:277-284`) is unset on every Step-1 attachment (none in the `NN/Attachment_*` defaults), so nothing is lost there | H (script) | [H] "does it feel silent / do enemies behave?"; M7 |
| R7b | **[corr E2]** The reload noise (1500, `DefaultWeapon.ini:143`) now follows button presses (R, pad A, the gun-hand grip) instead of reloads, since `StartReload` runs `super.StartReload` before `CanManualReload` (`SA:1196-1198`) | H (script); LOW | [H] only if enemies react oddly; M7 could move the noise to EJECT/INSERT |
| R8 | Direct clip writes skip `UpdateLowAmmoMix` (the low-ammo audio fader lingers until the next shot) | H (`EW:654-664`) | [H]; a `SetAmmoCount` call through ProcessEvent (L) |
| R9 | An empty current gun can be switched away from on a scripted weapon grant (`Engine/InventoryManager.uc:605-610`; rating −1 when empty, 313-325) | M | [S]: pick up / swap a weapon with the magazine out |
| R10 | The magazine in the hand is drawn by the gun component far from its bounds origin **[open C33]** | M: the size is script (`MOHAPlayerPawn.uc:1190`: 100 for the new gun, `:1163` resets the old to 0; arms 1000, `:5939`); that culling uses this sphere at the component origin (the game's gun spot, always in view) is ENGINE-NOTES 5ae's research finding, not re-checked in the exe | M3 capture (§7 M3: the magazine ~60 cm from the gun at the view's edge); lighting comes from the gun's spot (minor) |
| R11 | Where the magazine sits in the fist | [H] | `Hold`; a refinement: anchor it to the free hand's palm (the mirrored gun-hand grip arms_ik already computes, 5x) |
| R12 | Accidental ejects when an SMG is supported by its magazine; the foregrip vs magazine spots overlap | [H] | nearest-by-radius rule; per-gun `PullOut` if needed |
| R13 | The taped pair never flips (`TapedMagMode` only changes in `WeaponReload.EndState`) | H | [H] acceptable? a flip gesture later |
| R14 | `pending` is not saved; **[corr]** nor is `magIn = false` (a gun saved with its magazine out loads as "magazine in, empty") nor `owed` | by design | documented (§2.3) |
| R15 | The C96 `clip` as its visible rounds | M (geometry inside the magazine's upper volume, V `bulletgeo.txt`) | M5 capture |
| R16 | Two agents' numbers | **[corr E7]** Not all re-read by `design/tracks_check.py`: `skel/tracks/*.txt` truncate bone names to 14 characters (`tools/tracks.py`), so its full-name match printed nothing for `upgrade_03_drum`, `upgrade_03_hide_magazine`, `upgrade_02_64rdMagazine`, `single_magazine`, `upgrade_02_tapedMagazines`, `upgrade_03_20rdMag`, `upgrade_01_20rdMag` or `upgrade_02_magazine`; only `Bolt`, `bullet`, `gunSlide`, `chamber_slide`, `slideRelease` and `magazine` were re-read there. The verification's own psa/psk readers (V `magpath2.txt`, `myskel.txt`) re-read the magazine seats and the taped states and agree; the out axes hold with the §1.2 caveats (C29) | M0/M5 per-gun logs |
| R17 | Frame latency: the magazine follows the host's flags a tick later (like the hands); an event can land one Draw after its flag | by design (harmless: one frame of mismatch) | — |
| R18 | Shared block v14: host and DLL must match | H (`main.cpp:166`) | deploy both (build.ps1 builds both) |
| R19 | Only `Var_Flk_P` was read: other levels may cook other mesh versions | M, lowered: V `otherlevel.txt` found the seven RefSkeletons identical in `Nep_Azv_P` (the exports not byte-identical) and its gun AnimSequences the same size | the ref-pose guard (§1.3, corrected) |
| R20 | Taking a spare from the pouch needs the gun's magazine out (one model) | by design (P5) | [H] |
| R21 | **[corr, HIGH]** The hooked function is shared with `AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent` (linker folding): the hook runs with ECX = the GameInfo at level start / respawn | H (V2 `fold.txt`, `ctx2.txt`) | mitigated by design: pointer test first, never zero `*Result` otherwise (§2.5); M0 counts the calls |
| R22 | **[corr X2]** A gun left unloadable if the hook blocks while the reload pipeline is dead (stereo off, a failed hook) | H (code reading: `vr_view.cpp:1045, 1052-1071, 786`) | fixed by design (install condition + liveness + "engaged", §2.5, §3.1); M1/M6 tests |
| R23 | **[corr E5]** Rounds lost at the reserve cap, or when no exact entry exists | H (arithmetic; caps `DefaultPlayer.ini:190-191`; `IM:96-104`) | fixed by design (`owed`, §2.2); M1 at-cap test |
| R24 | **[corr]** Pointer-keyed caches fooled by GC address reuse (a Garand at an old StG44 address wrongly blocked; another mesh at a reused address) | M | keys (pointer, UObject Index) / (mesh, bone count), cleared on `reloadPawnSeq` (§2.1, §5.1) |
| R25 | **[corr]** Event-ring overrun | L (needs a stalled game with a live host) | the overrun rule (§4) |
| R26 | **[corr]** Active drops mid-gesture | by design | §3.1 rules; M3 menu test |
| R27 | **[corr]** A stale bake looked fresh (`reloadGeoSeq` bumped by `OnDraw` alone) | fixed by design | bump only with a fresh bake (§5.5) |
| R28 | **[open C46]** The component scale (L2W) is not measured | M (no Scale in the attachment subobjects) | lengths go through `A` (§5.2, §5.5); M0 logs the L2W row norms |
| R29 | **[open X2]** `HideViewModel=1`: is a hidden gun still baked? | unknown | excluded at install; M6 logs it |
| R30 | **[corr]** Data re-derivation pitfalls: umodel exports have Y negated (§0.2); cooked `NumChildren` is stale (walk `ParentIndex`) | H (V `myskel.txt`, `verify_mod/pskbones.txt`) | conventions in §0.2 |
| R31 | **[corr]** Small art inconsistencies: the MP40 `chamber_slide` mapping (fire vs reload), the C96 `clip` 0.95 u jump, the G43 magazine idle vs fire (0.12 u) | H (V `checkz.txt`); cosmetic | none needed (§1.1 notes) |

What the research could not settle and this design does not guess: the Colt slide stop's engaged angle (not used), the
real side a Thompson drum goes in from (the anim's +X is followed), where the game's own left hand holds the magazine
(not needed: the host places it), and whether native code ever plays the `*_empty_idle_4` sequences (not used).

---------------------------------------------------------------------------------------------------------------------

## 9. Claims the design relies on (for verification)

| # | Claim | Evidence |
|---|---|---|
| C1 | The clip is `EALAWeapon.AmmoCount[3]` at +0x2D4, indexed by `bAlternateFireMode` = bit 0x400 of +0x2EC | D/pe: `GetAmmoInClip` 0x10F0D5F0 = `8b 81 ec 02 00 00 c1 e8 0a 83 e0 01 8b 84 81 d4 02 00 00 c3`; `EW:24-25`; RL §1.1 |
| C2 | `MaxAmmoCount[3]` is at +0x2E0 and `bInfiniteAmmo` is bit 0x1 of +0x2EC | M: declaration order `EW:24-26`; D/pe `HasReserveAmmo` tests `[esi+0x2EC] & 1` |
| C3 | The reserve: `MOHAInventoryManager.AmmoStorage[10]` at +0x228, 12-byte entries (class +0, amount +4, max +8), `NumAmmoClasses` at +0x2A0; lookup by class or subclass | D/pe `GetReserveAmmoCount` 0x10F0D7D0 (`8b b9 a0 02 00 00`, `8d b1 28 02 00 00`, `83 c6 0c`, `[eax+0x3c]`); `IM:50-51`; `MOHAIncludeClass.uc:298-310` |
| C4 | C++ `HasReserveAmmo` (0x10F0D610) = `GetReserveAmmo() > 0 || bInfiniteAmmo`; the AEALASmallArms vtable slot 0x115883E4 holds it | D/pe (37 bytes; slot value 0x10F0D610) |
| C5 | `execHasReserveAmmo` = 0x10E45CC0 (native-table entry 0x11612330, name "intAEALAWeaponexecHasReserveAmmo"), thiscall, `RET 8`, 59 bytes as in §2.5; **[corr]** the same function is also `AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent` (entry 0x11612438) | D/pe; IN E5; V2 `fold.txt`, `ctx2.txt` |
| C6 | Every automatic reload, the equip refill and the reload button test `HasReserveAmmo()` in script | `SA:620, 1103, 1192, 1340, 1621, 1626, 1699, 1924`; RL §2.1 |
| C7 | With `HasReserveAmmo()` false an empty trigger pull calls `WeaponEmpty()` → the dry-fire sound; no reload, no switch | `SA:612-627`; `EW:810-814`; `SmallArmsAttachment.uc:824-829` |
| C8 | `AddAmmo(n)` takes min(n, reserve) unless infinite, clamps the clip to [0, Max], and returns the difference to the reserve only when `!bInfiniteAmmo` | `EW:621-641` (634-637) |
| C9 | `AddReserveAmmo` clamps to [0, entry max]; **[corr]** it matches the class exactly and appends a new, unclamped entry when none matches | `IM:76-104`; RL §1.2 |
| C10 | `SetAmmoCount` doesn't clamp; `SubtractAmmo` clamps to [0, Max] | `EW:643-664` |
| C11 | Same-class guns share one reserve (Thompson+MP40, StG44+BAR, rifles, pistols) | RL §1.2 (NN defaults) |
| C12 | An empty gun is refilled instantly on equip when `HasReserveAmmo()` | `SA:1918-1929` |
| C13 | `EALAWeapon.AttachmentClass` names the attachment class (**[corr X4]** read as the class object's name, `names::Name`, not `names::ClassName`); `CurrentUpgradeLevel` defaults −1 | `EW:95, 98, 1714`; `names.cpp:56-58` |
| C14 | `Instigator.Weapon` is set before `Activate()` on a switch (the equip refill sees the new gun as `pawn.Weapon`) | `Engine/InventoryManager.uc:555, 567`; RL §2.2 |
| C15 | The StG44 scope does not set `bAlternateFireMode` | `NN/MOHAStg44.uc:199-213`; `SA:1465, 1537, 2034-2105` |
| C16 | `TapedMagMode` flips only in `WeaponReload.EndState` | `NN/MOHAStg44.uc:227-238`, `NN/MOHABar.uc:154-165`, `NN/MOHA_MP40.uc:122-133` |
| C17 | The first-person gun is the attachment's Mesh = `EALAWeapon.WeaponSkeletalMesh`; its Outer's class is the key | `EW:937-938`; ENGINE-NOTES 5u, 5aj |
| C18 | SpaceBases hold mesh-space matrices (RotOrigin in LocalToWorld) | **[open]** assumed (SK Conventions; IN §8); M0 |
| C19 | Mesh axes +X left, +Y down, +Z muzzle | SK Conventions; AN §0 (tags, handles, sockets agree) |
| C20 | Every vertex has one bone at weight 1.0 and no triangle spans two bones: each part moves or hides on its own | SK (`skel/overlap.txt`, `skel/geo.txt`) |
| C21 | All Step-1 magazine and action bones are direct children of identity roots at the origin; no children of their own | SK Conventions and per-gun tables |
| C22 | Hidden upgrade parts are zero-scale bone matrices (SkelControlSingleBone BoneScale 0) | **[open]** script side H (`MOHAWeaponUpgrade.uc:59-92`, `SkelControlBase.uc:31`); how the engine applies it to SpaceBases unread; M0 `|det|` + translation log |
| C23 | Bone names and SAVE visibility per gun as in §1.1 | names H (all in the psk skeletons, `pskbones.txt`; V `myskel.txt`); **[open]** SAVE visibility (not in the decompiled defaults; chosen at runtime by `|det|`); M0 |
| C24 | The Z positions (idle, empty = reload f0, full back) per gun as in §1.1 | D/tracks; AN §1; SK |
| C25 | Action bones move along mesh Z only | D/tracks (X, Y constant in every listed anim) |
| C26 | Thompson and MP40 rest with the bolt back; the game's empty pose is bolt forward | D/tracks (idle −3.50 / 17.31; reload f0 8.18 / 27.61); SK; AN §1 |
| C27 | The BAR handle doesn't move on fire; the StG44 and BAR have no empty pose in the art | D/tracks; SK; AN §2.3-2.4 |
| C28 | No Step-1 empty idle / slide-lock sequence is used by the scripts | AN §3; RL §6.1 |
| C29 | Out axes and grab points per gun as in §1.1; the in-well magazine sits at the same place in both taped states | D/grab; D/taped; AN §1. **[open, partly settled]** out axes re-derived from V `magpath2.txt` (§1.2: within ~3°, the StG44 within 12°); taped states 0.8 / 1.8 u apart (D/taped); grab points not re-derived: M3/M5 |
| C30 | The C96's detachable magazine is upgrade index 1 | `skel/upgclasses.txt:9`; `NN/MOHAUpgradeMauser_1.uc:12-30`; `upgmesh.txt` |
| C31 | The bake is `bones[i] = saved[i]·kMove`, `kMove = A·inv(L2W)`, `A = L2W·D'`, and the game's pose is restored after the render copy | `arms_ik.cpp:579-589, 609-620` |
| C32 | The parts update up to 6 times a frame; only the last pair is drawn | **[open]** ENGINE-NOTES 5ad (a measured fact, not re-measured); the design does not depend on the count; M0 counts |
| C33 | Culling uses the component's bounds sphere (gun 100) at its origin | **[open]** size H (`MOHAPlayerPawn.uc:1163, 1190, 5939`); the sphere-at-origin rule is ENGINE-NOTES 5ae's research finding; M3 capture |
| C34 | `MirrorFrame` negates the frame's own Y row, then reflects; the gun and off frames are mirrored in `OnPlayerView`; hence a mirrored-G local point (f, r, u) is the real (f, −r, u) | `viewmodel.cpp:53-58, 447-456, 489-491`; IN §2.2 |
| C35 | Host: holster spots are cm from the head in its heading frame; the defaults; holster zones win today | `hands.cpp:56-73, 128-130, 180-204` |
| C36 | Host: today's gesture reload at (0, −0.08, −0.10) in the gun frame sends `Reload` | `hands.cpp:120, 198-203` |
| C37 | Pad: `A=b,rgrip`, `B=y`, `Y=a`, `RB=x`; `LeftHanded` swaps triggers and grips only; consumed grips are zeroed in `Map()` | `config/MOHAVR.ini:206-211`; `pad.cpp:246-260` |
| C38 | `Pad::Pulse` is fixed 30 ms at 0.5 | `pad.cpp:606-614` |
| C39 | The shared block is v13 with `sizeof(Header)` 1248 (`slotViewQpc` at 1224); the host refuses another version | `shared_frame.hpp:31, 200-201`; `main.cpp:166` |
| C40 | `Output::spots` is `kHolsters + 2`, `Markers::quads_` `kHolsters + 3`; the host has 16 layers | `hands.hpp:63`; `markers.hpp:32`; `main.cpp:889` |
| C41 | `Hook_Draw` runs `RunHostCommand` on the game thread before the views; `OnPlayerView` reads the gun and hands per view; `HandFrames` is valid for 250 ms | `vr_view.cpp:381-392, 786`; `viewmodel.cpp:410-512, 514-526` |
| C42 | The exec-hook pattern (inline hook, bytes pinned) exists for `execActivateSystem` | `muzzle.cpp:154-165, 247-270`; `addresses.hpp:223-227, 290` |
| C43 | The harness resumes the player's save in Var_Flk with the StG44, BAR and Colt | **[open]** ENGINE-NOTES §4 (lines 128-129), 5u (accurate citation of a runtime fact); M0's first log |
| C44 | `GiveWeapon <class>` exists in the engine's CheatManager; `UpgradeWeapon` in MOHA's (**[corr]** it acts on the gun in hand and grants experience) | `Engine/CheatManager.uc:293-306`; `MOHACheatManager.uc:910-938` |
| C45 | An empty gun no longer re-fires a held trigger on `Active` entry under the override (no other effect) | `EW:1340-1360` (1348) |
| C46 | Units: mesh units are world units (no component scale) | **[open]** AN §0; no Scale/Scale3D in the attachment subobjects (`NN/Attachment_Stg44.uc:41-49`, `SmallArmsAttachment.uc:1171-1172`); runtime L2W unmeasured; the design no longer depends on it (lengths through `A`); M0 logs |
| C47 **[corr]** | 0x10E45CC0 is shared by exactly two exec natives (AEALAWeapon's `HasReserveAmmo`, AMOHAGameInfo's `InitialLoadCompleteKismetActionPresent`); nothing calls or jumps to it directly | V2 `fold.txt` (4 data references: the two named entries and the two per-class lists; 0 rel32 call/jmp), `ctx2.txt`; `MOHAGameInfo.uc:62, 368` |
| C48 **[corr]** | `EALASmallArms.Active.StartReload` emits the reload noise before testing `CanManualReload` | `SA:1196-1198`; `EW:822-830`; `DefaultWeapon.ini:143` |
| C49 **[corr]** | Only the capacity upgrades write the clip (Thompson drum, MP40, MP40_1, BAR_2, G43, Mauser_1); the base `StaticUpgradeWeapon` doesn't | `NN/MOHAUpgrade*.uc` (grep: only these six assign `AmmoCount[0]`); `MOHAWeaponUpgrade.uc:35-45` |
| C50 **[corr]** | The single-player reserve caps: SMG 390, sniper 50, pistol 100, rifle 120, shotgun 80, rocket 7, auto-rifle 390; no Step-1 upgrade raises them (only the grenade and launcher upgrades call `AddReserveAmmoCapactity`) | `DefaultPlayer.ini:187-196`; `MOHAWeaponUpgrade.uc` `AddReserveAmmoCapactity`; its callers in `NN/MOHAUpgrade*.uc` (Gammon, MKII, M18, Panzerschreck only) |
| C51 **[corr]** | With `Camera.Stereo=0` the view hook (and so `OnPlayerView`/`HandFrames`) runs but `Hook_Draw` doesn't; `armsik::Install`'s result is ignored | `vr_view.cpp:1045, 1052-1071, 786` |
| C52 **[corr]** | `ReadGun` and `ReadHands` are separate seqlock passes; the host rewrites the view block every XR frame | `viewmodel.cpp:434, 489`; `shared_frame.hpp:217-239, 287-303`; `main.cpp:711-726` |

Appendix — file list (from IN §9, adapted; **[corr]** additions in bold): `shared_frame.hpp` (v14; **`ReadHands` extended
with the reload fields in the same pass**, `ReadReloadGeo`), new `src/mohavr/reload.cpp/.hpp`, `addresses.hpp`
(`kExecHasReserveAmmo` + bytes + signature row, **its comment naming the folded GameInfo native and the pointer-test
rule**), `arms_ik.cpp` (the call after line 589; **the bake time stamp**), `viewmodel.cpp` (the reload fields from
`ReadHands`, the mirrored magazine frame + getter), `vr_view.cpp` (OnDraw at 390, Install at 1071; **keep
`armsik::Install`'s result and install the reload hook only with the Draw hook and `HideViewModel=0`**), `config.hpp/.cpp`
(`[ManualReload]`, per-gun lines **and `.Ref` lines**), new `src/host/reload.cpp/.hpp`, `hands.cpp/.hpp` (candidates,
kSpots, kPouch, kBolt, Input/Output fields; **`hands.hpp:89-90`, `hands.cpp:45-46, 57-58`; the gesture off while Active;
`Hold` mirrored for a right off hand**), `pad.cpp/.hpp` (`FaceButton`, `SetMaskedFace`, `Pulse(amp, ms)`, `reload=` test
lines), `menu.cpp/.hpp` (toggle, 5th spot; **`menu.hpp:64, 110`, `menu.cpp:32, 171-172, 427, 654`**), `markers.cpp/.hpp`
(sizes, pouch in showAll; **`markers.cpp:89, 94`, `markers.hpp:32`**), `main.cpp` (read geometry, write flags and the
**engaged bit** in the seqlock, the event ring **with the overrun rule**, the mask, pulses; **`main.cpp:401-403, 701`**
for the 5th spot), `config/MOHAVR.ini` (**`Hook=0` until M1**), `CMakeLists.txt`.

---------------------------------------------------------------------------------------------------------------------

## 10. Verification record

The design was verified adversarially (2026-09-30); this pass applied the result. Sources: the verification's own
re-reads (`verify/`, tag V), and the checks made while correcting (`verify2/`, tag V2), plus direct reads of the repo
scripts and source named in each row. Confidence as in §0.1.

### 10.1 Refuted, and corrected

| ID | What the draft said | What is true (evidence) | Where fixed |
|---|---|---|---|
| E1 / X9 | The ref-pose guard compares each named bone's RefSkeleton position with "the bind values above" (§1.1), 0.5 u | §1.1 holds animation values; the ini lines carry no bind values; a guard built from them rejects the MP40, G43 and C96 (1.3 to 65 u off). RefSkeleton values: V `myskel.txt` (H) | §1.1 note, §1.3 (`.Ref` lines, fingerprint + positions), §7 M0 |
| E2 | R and pad A do "nothing" on a converted gun | `StartReload` runs `super.StartReload` first: the AI reload noise (1500) on every press (`SA:1196-1198`, `EW:822-830`, `DefaultWeapon.ini:143`; H) | §0.4 P4, §2.6, R7b, C48 |
| E3 | R7: the noise radius is lost with `WeaponReload` | The state never emitted it; lost are `NotifyObservers('Reload')`, `Weapon_ReloadEvent`, the arms anim and its sound notifies; `WeaponReloadSnd` is unset on the Step-1 attachments (`SA:1935-1948`, `WeaponAttachment.uc:277-284`; H) | R7 |
| E4 | C rises on an upgrade via `MOHAWeaponUpgrade.uc:36-45` | That function writes no ammo; the six capacity-upgrade subclasses do (H, grep of `NN/`) | §2.3, C49 |
| E5 | `ToReserve`: `R = clamp(R + k, 0, cap)` "as in the game" meets decision 4 | The game never returns magazine rounds, so it never loses them; the clamp loses them at the cap (pistol 100, rifle 120), and the draft also lost them when no exact entry exists (`IM:96-104` appends one) (H) | §0.3, §2.1 `owed`, §2.2 helpers and table, §4, §7 M1, R23 |
| E7 | R16: both agents' numbers were re-read and agree | `tools/tracks.py:41` truncates names to 14 chars, so eight magazine bones were never re-read by `tracks_check`; V's own readers confirm the seats and taped states (H) | R16 |
| E8 | M0 pass: StG44 `single_magazine` at (0, 2.50, 17.68) | Taped state B idles at (−3.64, 2.50, 17.68) (`stg_gun_idle_3b`, `NN/MOHAStg44.uc:136-147`; H) | §1.1 note, §7 M0 |
| E9 | The hold "catches" the action as the fire anim returns | C is 0 from the shot: the action snaps to the hold at the shot (open bolts one bake later) (M, from `ConsumeAmmo` order) | §1.1 note, §5.3 |
| X2 | The hook stops blocking whenever the game couldn't otherwise reload | With `Camera.Stereo=0` (or a failed Draw / arm-IK hook) `HandFrames` stays valid, nothing runs in `Hook_Draw`, and the gun became unloadable (`vr_view.cpp:1045, 1052-1071, 786`; H) | §2.5 (install condition, liveness, engaged), §3.1, §4 bit4, §6, §7 M6, R22 |
| X3 | The magazine frame read in `OnPlayerView` is in the same host frame as D and the hands | Separate seqlock passes can straddle a host frame (`viewmodel.cpp:434, 489`, `main.cpp:711-726`; H) | §2.7, §4 readers, §5.2 |
| X4 | `ClassName(w.AttachmentClass)` gives the class name | It returns "Class" (`names.cpp:56-58`); use `names::Name(ReadPointer(...))` (H) | §2.5, §2.7, §5.1, C13 |
| X8 | The file list covers the 5th (pouch) spot | More `kHolsters` sites than listed (`main.cpp:401-403, 701`, `menu.hpp:64, 110`, `menu.cpp:171, 654`, `hands.hpp:89-90`, `hands.cpp:45-46, 57-58`, `markers.cpp:94`; grep, H); without `main.cpp:701` a pouch moved in the menu never reaches `Hands`; the pouch must also show whenever the Holsters page is open | §3.7, appendix, §7 M3 |
| X10 | "Hook bytes differ → caps bit2 clear" is a per-feature fallback | A `kSignatures` mismatch stands the whole mod down in `DllMain` (`dllmain.cpp:49-52`, `build_check.cpp:34-39`; H) | §6 |
| X11 (uncertain) | (§3.4 vs §3.8) the gesture reload runs "after the reload candidates" | Inconsistent; while Active the gesture would consume the grip, pulse and make the noise for nothing | §3.4, §3.8: gesture off while Active |

### 10.2 Problems raised by the verification (not tied to one claim), and what was done

| Problem | Severity | Done |
|---|---|---|
| 0x10E45CC0 is folded with `AMOHAGameInfo::execInitialLoadCompleteKismetActionPresent` (re-checked: V2 `fold.txt`, `ctx2.txt`, H) | HIGH (safety) | §0.5.4, §2.5 (pointer test first, never zero `*Result` otherwise, the `addresses.hpp` comment), M0 counters, R21, C47 |
| Pointer-keyed caches vs GC address reuse | LOW-MEDIUM | keys (pointer, UObject Index) and (mesh, bone count), cleared on `reloadPawnSeq` (§2.1, §5.1), R24 |
| Event ring without an overrun rule | LOW | §4 overrun rule, R25 |
| Reserve bookkeeping (subclass read vs exact write; no append) | MEDIUM | §2.2 (exact match like the writer, `owed` when no entry, M0 logs both matches) |
| Unit scale assumed in `boltTravel`, the Grabbed slide, the rack | LOW-MEDIUM | through `A` (§5.2, §5.5), M0 L2W norms, R28 |
| "The host never deals with the mirror" | LOW | `Hold` mirrored for a right off hand (§3.2, §3.5, §5.6), M6 |
| Active dropping mid-gesture unspecified | MEDIUM | §3.1 rules, §6 row, §7 M3, R26 |
| `reloadGeoSeq` freshness = `OnDraw` liveness, not a fresh bake | LOW-MEDIUM | bump only with a fresh bake (§3.1, §5.5, §2.7), R27 |
| Y-negated umodel exports | LOW (implementer pitfall) | §0.2, R30 |
| `Hook=1` default-on before [S] proof (rule 7) | LOW | `Hook=0` until M1 passes (§6, §7 M1) |
| `UpgradeWeapon` acts on the gun in hand and grants experience | LOW | §7 M0 (switch first; `Saved\` restored), C44 |
| `IM:1192` (`SetCurrentWeapon`) missing from "what it stops" | INFO (no effect) | §2.5 |
| Save/load loses `magIn = false` | LOW | §2.3, R14 |
| Stale `NumChildren` | LOW | §0.2, §5.1, R30 |
| C96 `clip` jump; MP40 `bullet` at the feed; MP40 `chamber_slide` mapping; G43 magazine idle vs fire | LOW (cosmetic) | §1.1 notes, §5.4, R31 |

### 10.3 Confirmed (the verification raised no objection; stand at their stated confidence)

C1-C17 (C5, C9 and C13 extended in this pass, not contradicted), C19-C21, C24-C28, C30, C31, C34-C42, C44 (with the
§7 note), C45; the conflict resolutions of §0.5 (with §0.5.4 extended); the per-gun action data of §1.1 (Z idle / empty / back per gun, action along Z only: also re-read by V
`checkz.txt` and `rotcheck.txt`); the handle offsets (V `handles.txt`); every Step-1 vertex single-bone at weight 1 (V
`weights.txt`). Re-checked in this pass (V2, direct reads, H): the fold (C47), `SA:1196-1198` (C48), `IM:76-104` (C9),
`MOHAWeaponUpgrade.uc:35-45` and the six capacity subclasses (C49), `DefaultPlayer.ini:187-196` (C50),
`vr_view.cpp:1045-1071` (C51), `viewmodel.cpp:434/489` and `main.cpp:711-726` (C52), `names.cpp:56-58`, the `kHolsters`
sites, `dllmain.cpp`/`build_check.cpp`, `hands.cpp:228`, `MOHACheatManager.uc:910-938, 970-990`, `tools/tracks.py:41`,
and the out axes against the anim paths (§1.2, M).

### 10.4 Still open, and how the main session settles each (short in-game logging runs)

| ID | Question | Settled by |
|---|---|---|
| C18 | Are SpaceBases mesh-space (RotOrigin in L2W)? | M0: the animated translations of the StG44 / BAR / Colt bones against §7's values |
| C22 | Does BoneScale 0 zero the 3x3 only, or the translation too? | M0: `|det|` and translation of each hidden variant (the design works either way) |
| C23 | Which magazine variant is visible in the SAVE? | M0: `|det|` per variant (runtime choice; not critical) |
| C29 | Grab points (and the StG44 out axis within 12°) | M3/M5: the geometry log, the drawn grab point against the controller; [H] the snap |
| C32 | Gun bakes per Draw | M0 counter (the design doesn't depend on it) |
| C33 / R10 | Culling sphere at the component origin | M3: the held magazine ~60 cm from the gun at the view's edge stays drawn |
| C43 | The harness save's level and weapons | M0's first log (the drawn keys) |
| C46 / R28 | The component scale | M0: L2W row norms (the design no longer assumes 1) |
| R2 | `BonePos.Position` at +28 | M0 against the §1.3 `.Ref` values; until then only the fingerprint runs |
| X2 / R29 | Is a hidden gun (`HideViewModel=1`) still baked? | M6 log (the hook is not installed with it meanwhile) |
| C47 at runtime | The GameInfo call really goes through the hook, and the pointer test rejects it | M0 count-only hook: a call with a non-weapon `this` at level start / respawn |
| E9 | The shot-to-hold order on screen | M2 slow motion (`SloMo 0.05`) |

**Recommended first milestone: M0**, a single short logging run with the hook installed count-only (never writing
`*Result`) under `Debug.ReloadProbe=1`: it settles C18, C22, C23, C32, C43, C46, R2 and the fold at runtime, checks the
reflection offsets and the reserve entries (exact vs subclass match), and tries `GiveWeapon` + switch + `UpgradeWeapon 3`
for the other four guns. Nothing in M1 onwards should be built on the unverified assumptions before it.

Housekeeping: `verify/dec2/Nep_Azv_P.xxx` (142 MB, the decompressed cross-check level) is no longer needed and can be
deleted; its result is in `verify/otherlevel.txt`.

### 10.5 Implementation notes (as built)

- **M3, insert arming:** a magazine that comes away from the gun (pulled past `PullOut`, or the release button while
  grabbed) ends within `InsertRadius` of the well (4 cm < 5 cm), which §3.2's insert test would take at once. It is
  armed only once it has been more than `InsertRadius` + 2 cm from the well; a pouch magazine arms on its first frame.
- **M3, a gun change** adopts the game's magazine for the new gun (in or out); nothing is sent for the old one (its
  DROP would be rejected as meant for another gun, and its rounds are already in the reserve since EJECT).
- **M3, the press test** uses the previous frame's grab point (the press is handled before this frame's foregrip turn);
  the state machine then runs with this frame's final gun pose.
- **M3, "engaged"** also needs caps bit0 (a converted gun with this Draw's geometry); the game side blocks only for a
  converted gun anyway.
- **M3, test targets:** `pad_cmd.txt` `hand=l,@mag|@pouch|@bolt[,dx,dy,dz[,yaw,pitch,roll]]` puts a hand at the last
  frame's spot, offset in the head's heading frame.
- **M4, "held back"** (`reloadState` bit3, the tug) comes from the empty hold only; the game's own animation positions
  (a shot's recoil moves the G43's bolt back for a moment) never make it a tug.
- **M4/M5, the given guns:** from the harness save's G43, `GiveWeapon MOHAGameNonNative.MOHAThompson` is reached with 7
  `NextWeapon`s (BAR, Colt, three grenades, Comp B, then the given guns in order).
- **M7, the sound call:** `PlaySoundAt` (script: an AudioComponent of WorldInfo at the gun) rather than `PlaySound`
  (native) or `WeaponPlaySound` (its `bNoRepToOwner=true` goes through the owner-replication path MOHA never uses). The
  cues come from the arms' AnimSets by reflection (no object search). Per gun `SndOut`, `SndIn`, `SndRack` (per
  variant); `[ManualReload] SndTake` for the pouch; nothing for DROP.
- **Twin magazines, as built:** a used half with rounds left can go back in (flip back and insert); the player's
  "once before a new pair" is then a habit, not a rule ([H]: should the mod forbid it?). The MP40's upgrade-0 pair is
  not handled (no B pose measured).
- **Round 31, the grips:** a held magazine's pose comes from the game's reload animation (the hold grip), not `Hold`
  (which stays for guns without a hold grip). With the grip the magazine is turned in the hand as the animation holds
  it, so lining it up at the well is up to the player's wrist ([H]; the host logs near misses: "at the well but turned").
