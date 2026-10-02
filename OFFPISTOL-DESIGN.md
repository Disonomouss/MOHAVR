# Off-hand pistol ("dual wield", step 2): the design

(The research notes are in `work/research/offpistol/`, local; sections 0.1 and 0.2 were added after the spike ran.)

Design 2026-10-02, written by the designer of a research workflow from six research notes in this folder
(`script-fire.md`, `native-fire.md`, `effects.md`, `draw-hold.md`, `mod-integration.md`, `ux-prior-art.md`), checked
against the decompiled scripts (`work/script/`, the map-package classes in `work/research/reload/nonnative/`), the unpacked
exe (headless Ghidra, `MOHA.exe`, image base 0x10900000), the game's `DefaultWeapon.ini` (read only) and the mod's source
at HEAD def18d0. Read-only: no source, config, tool or doc was changed; nothing was built, deployed or launched.

The player's question (verbatim): "Is it possible to build similar system for using the pistol with the off hand?"
"Similar system" = the off-hand grenade (Phase 1, commit 518f9f3; `OFFHAND-DESIGN.md`, DECISIONS D40, ENGINE-NOTES
5bb/5bc): the mod drives an INACTIVE weapon object's own script functions through `AActor::ProcessEvent` while the gun stays
the pawn's active weapon, and draws the item in the off hand as a clone of its pickup mesh baked by the arm IK.

Tags: **[V]** verified by me (script text file:line, bytes / disassembly / decompile with address, mod source file:line,
game ini line); **[V-agent]** verified by a research note and consistent with everything I read; **[I]** inference;
**[R]** needs a runtime check (what to measure is said); **[S]/[H]** acceptance in the simulator / in the headset
(standing rule 8).

Short paths: `SC/` = `work/script/`, `NN/` = `work/research/reload/nonnative/`, `DW` = the game's
`UnrealEngine3/MOHAGame/Config/DefaultWeapon.ini`.

---------------------------------------------------------------------------------------------------------------------

## 0. Verdict and the design in one screen

**Feasible, with caveats.** The pistol can be held and fired from the off hand while the long gun stays the pawn's active
weapon, with no engine patch, no new hook and no new pinned address. Every engine function the shot needs is unnumbered,
and every one is statically verified. Two pairs of pistols can also work: two different pistols (Colt and C96) from
Phase 1, and two of the same pistol (one object fired twice) in Phase 3. The caveats are runtime facts the simulator
still has to confirm, listed in 7 and 9:

- the first out-array call through the mod's call helper;
- the credit swap around the damage call;
- whether the pistol's sound bank is loaded while it is holstered;
- whether the harness checkpoint has an enemy in reach to prove damage and kill credit.

- **Why it is harder than the grenade, and still works.** The grenade had one clean entry point, `SpawnProjectile`. The
  pistol has none. `StartFire`/`BeginFire` set the shared `PendingFire`. `FireAmmunition` fires the gun attachment's sound,
  flash and brass and the arms' fire event. `InstantFire`/`PerformWeaponTrace` call `SetFlashLocation`, which plays
  `Pawn.Weapon.PlayFireEffects`, and aim through the mod's hooked `GetBaseAimRotation`, so the shot would land on the main
  gun's red dot. So the mod rebuilds the shot from the layer below. [V 1.1]
  1. **The trace:** the static native `EALAWeapon.CalcWeaponFireNative(TraceOwner = the pawn, Start, End, Extents = 0, out
     ImpactList)`, the bullets' own native trace, which passes through triggers.
  2. **The damage:** `EALASmallArms.ProcessInstantHit(0, Impact)` for each impact, which runs native `ApplyDamage`, then
     `TakeDamage`, then the impact effects on the gun's attachment.
  3. **The AI:** the weapon's own `SpawnGunshotStimulus` and `SpawnImpactStimulus`, then `Pawn.NoiseRadius`.
  4. **The stats:** `playerStats.OnWeaponFire`.
  5. **The ammo:** a direct `AmmoCount[0]` write.
  6. **The sound:** `PlaySoundAt` at the off barrel.

  Around the trace, the mod's bullet-trace hook stands down. For the damage call only, the mod swaps `Pawn.Weapon` to the
  pistol (guarded), so kills and experience count for the pistol. Upgrades are applied at the draw with the game's own
  `upgrade()`. The gun in hand, its state, `FlashCount`, `PendingFire` and ammo are never touched.
- **Interaction (each a switch, `[OffHand] Pistol=0` until [S]).** This is the grenade's grammar.
  - **Draw:** the off hand's grip at the pistol holster (`RightHip=SwitchPistol`) takes the pawn's pistol into that hand.
    D40's rule applies: with the feature on, that press is never the holster's own `SwitchPistol`, so the long gun never
    jumps hands.
  - **Hold:** holding the grip keeps the pistol; letting go puts it back in the holster.
  - **Reload:** in the holster it refills after the pistol's own reload time.
  - **Fire:** the off trigger fires it, one shot per pull, at the pistol's own rate (the C96 with the 712 kit fires
    automatically while the trigger is held). Empty, it clicks.
  - **Aim:** each shot goes along the off pistol's own aim line, with a second dot.
  - **While held:** the foregrip, the main gun's manual reload and the off-hand grenade wait until the pistol is put back.
- **Drawing.** A clone of the pistol's own `DroppedPickupMesh` (the same skeletal mesh as its first-person attachment) is
  attached to the arms and baked at the off controller. The gun is anchored, not the hand: it sits at the weapon's own
  `[GunFit]`, mirrored, so the drawn bore lies on the off aim line. The off hand snaps onto it with the game's pistol grip,
  mirrored, and the trigger finger follows the trigger. The slide, hammer, lock-back, muzzle flash and brass come in
  Phase 2.
- **Contract.** Shared block v21 (2120 -> 2544 bytes) carries:
  - a game -> host status (caps, the pistol's key, clip, state, counters, the second dot's distance);
  - the off gun pose, aim line, fit grip, trigger value and flags inside the view seqlock;
  - an ordered 8-slot ring of DRAW / SHOT(ray) / HOLSTER events.
- **Spikes first.**
  - **S1, the shot:** test commands only. It must show the BAR untouched and the stand-down working, and (S1b) an enemy
    damaged and the pistol credited.
  - **S2, the pistol in the hand:** the carrier, the mirrored placement, the closed grip.
  - **S3 (optional), the report and the flash:** the cue and its sound bank, and a mod-owned muzzle-flash component.
- **Fallbacks.** Each step has a narrower substitute (2.7). The last resort is today's behaviour with the switch off: the
  pistol becomes the main weapon in the drawing hand.
- **Effort.** Spikes about 4 h, Phase 1 about 6-8 h with two adversarial reviews, Phase 2 about 4-6 h, Phase 3 about
  3-4 h. Roughly 2-3 sessions. The grenade went from research to Phase 1 shipped in one day; this has more parts (the
  grip, a second dot, the aim scope).

### 0.1 Spike S1, run (2026-10-02, the main session) -- PASS

Built as 7.1 says: `src/mohavr/offpistol.cpp/.hpp` (test commands only), aim.cpp `BeginOffShot()/EndOffShot()`, and the
call helper factored out of offhand.cpp into `src/mohavr/script_call.cpp/.hpp` (the grenade unchanged: `nade11`). Logs
`logs/modlogs/pistol1-*` to `pistol5-*`, scripts in `work/research/tests/`. The BAR in hand, the Colt holstered.
- **Resolve.** The pistol is `MOHAColt45_0` = `IM.PistolWeapon`, not `Pawn.Weapon` (`MOHABar_1`); vtable 0x115880C0
  (+0xF0 0x10DB1FA0, +0x344 0x10F0DF60); Instigator the pawn. Every function is callable with native index 0:
  `CalcWeaponFireNative` (flags 0x00422500, parms 0x74: TraceOwner +0, StartTrace +4, EndTrace +0x10, vTraceExtents
  +0x1C, ImpactList +0x28, ReturnValue +0x34 -- exactly as predicted), `ProcessInstantHit` (0x44: FiringMode +0, Impact
  +4), `SpawnGunshotStimulus` (0xC), `SpawnImpactStimulus` (0x18), `PlaySoundAt` (0x10), `Object.FindObject` (0x14),
  `MOHAPawn.NoiseRadius` (4), `MOHAPlayerStatsComponent.OnWeaponFire` (8: WeaponType +0, WeaponClass +1, NumShots +4),
  `MOHAWeaponUpgradeManager.upgrade` (0xC) and `GetAppliedUpgradeLevel` (8).
- **The shot** (`fire eye`, `fire hand`): ImpactList 1 entry whose HitActor is 0.0 units from `aim::WorldTrace` on the
  same segment; one `ProcessInstantHit`; `Pawn.Weapon` the pistol for the call and the BAR again after; the clip 7 -> 6.
  The BAR's state (Active), AmmoCount, FlashCount (0), PendingFire ([0 0]) and `Pawn.Weapon` unchanged; only its
  attachment's `NextImpactSoundIndex` +1 (the impact played on it, by design). No `shot N:` line from aim.cpp for the
  off shot; the BAR then fired three rounds 0 cm from its red dot.
- **The list buffer:** the engine's array was reused across seven shots (the same data, max 1); a shot with two impacts
  (a Trigger in the way) made the engine re-grow it (max 2), and later shots used the new one. No fault.
- **Upgrades:** the save's experience restore had already applied level 2 to the holstered Colt (CurrentUpgradeLevel 2,
  100 damage, RefireCheckTime 0.06, magnum) -- the verifier's correction, not -1. The shared magnum mix **was on with
  the BAR in hand** (finding 1, from the game's own restore); the rule (`DeActivate`) turned it off: IsActive 1 -> 0.
- **The report (R1 settled):** `Object.FindObject("Aud_GCm_FakeWAV.COLT.WpnFire_PC_Colt_Magnum", the SoundCue class)`
  found it; `PlaySoundAt` made a WorldInfo AudioComponent that **played: 1 wave instance, 1.00 s in**. The holstered
  Colt's sound bank is loaded.
- **S1b, damage and death from the pre-draw.** No axis soldier is in sight at the harness checkpoint (55 AI pawns, all
  behind the flak tower's roof) and `Summon` (MOHAGame.MOHAAxisAIPawn, MOHAGameNonNative.MOHAFallshirmjagerSTG44Pawn)
  spawned nothing, so the test shot soldiers out of sight **from a point 1.2 m from them** (`ApplyDamage`'s falloff runs
  from the pawn, so the damage is the real one). A Panzerschreck soldier: the hips, Health 130 -> 5; the neck, -> 0,
  dead (Physics 2, the controller gone). An elite MG42 gunner: 850 -> 450 -> 50 -> 0 (a Trigger on the way took its own
  damage call). `TakeDamage -> Died` and the death processing ran from Hook_Draw's pre-draw -- the new exposure -- and
  the game ran on normally in five runs.
- **Credit:** every kill was recorded in the stats' `KillInfo` as weapon type 9 (the Colt), not 13 (the BAR) -- **also
  in the `main` run, without the swap**: the stats seem to credit the weapon last reported fired (`OnWeaponFire`), not
  only `Pawn.Weapon` [R: a one-shot `main` kill right after a BAR shot]. The per-weapon experience didn't move: every
  weapon in the save is at its top level.
- **Not run:** 7.1 pass 3's capture case (the gun fired within 100 ms of the off shot), S2 (the pistol drawn in the off
  hand), S3 (the flash).

### 0.2 What the verifiers corrected (34 checks confirmed, 3 refuted, 1 open -- now settled)

- **Upgrades reach the holstered pistol without an equip:** the experience restore at a checkpoint or level load
  (`MOHAPlayerPawn.RestoreExperience` -> `upgrade(type, level - 1, GetWeaponOfType(type))`). Confirmed by S1: level 2 at
  the start. The draw-time `upgrade()` stays as a guard.
- **A death inside the swap:** in single player `Pawn.Died` tosses and discards nothing (Engine/Pawn.uc:1885-1897 behind
  `NM_Standalone`), so `Pawn.Weapon` stays the pistol on the dead pawn. Restore the gun whenever `pawn.Weapon == P` and
  the gun is live, alive or not.
- **A Summon'ed pawn gets a controller** (`SetupBot` -> `SpawnKynapseAIController`), so it would be a valid target --
  but in practice `Summon` spawned nothing at the harness checkpoint (S1b used near shots instead).
- **An off-hand level-up** runs the game's `AddUpgradeSequenceAmmo` on the pistol: `SetAmmoCount` -> `UpdateLowAmmoMix`
  on the shared low-ammo fader. Restore the gun's (`G.UpdateLowAmmoMix(G.CurrentFireMode)`) after, like the magnum rule.
- **The call helper always runs the class-level UFunction:** a state-dependent query through it is wrong (e.g.
  `IsUpgradeSequenceActive`); use `names::StateName` (and keep the "magic bullets" rule: no ammo used during an upgrade
  sequence).
- **`OnWeaponFire`'s NumShots must be passed** (1): through ProcessEvent an omitted optional parameter is 0 (no shots
  counted). S1 passes it.
- **A game-side pause gate:** the pre-draw runs while the world is paused, and the host freezes on the game's menu a
  frame later; a SHOT must be refused while `WorldInfo.Pauser` is set or a game menu is open.
- **`Weapon.FreeOffHand=1` is required** for the off hand's grip: arms_ik applies a support-hand grip only in the
  free-hand branch. (The grenade carrier has the same gap with FreeOffHand=0.)
- **The off aim line's sideways sign** must be mirrored for a left off hand regardless of `LeftHandMirror`, and
  left-hand mode needs an explicit rule.
- **A grenade as the main weapon** changes the impacts (grenade attachments aren't SmallArmsAttachments: no hit spang)
  and the flash recipe (mint the particle components lazily, once a SmallArmsAttachment is current).
- **The C96:** its live idle pose is unlogged, and its `clip` bone carries rounds at idle (and the top round at level >=
  1): place it, don't collapse it.
- `CreatePlayerKillStimulus` reads the killer's `Pawn.Weapon` (a sniper rifle differs); minor.

### 0.3 Spike S2, run (2026-10-02, the main session) -- PASS

Built as 7.2 says, with these differences:
- the carrier is shared code (`src/mohavr/carrier.cpp`, the grenade's too);
- the fit grip is read from the inis (the player's, else the shipped one) until the host publishes it in Phase 1;
- the C96 collapse is left for Phase 1.

Logs `logs/modlogs/pistol6-*` and `pistol7-*`; ENGINE-NOTES 5bf.
- **Pass 1, the clone.** Mesh `US_M1911A1_Pistol_Rigged` / `DE_Mauser_Rigged`, Outer P, PhysicsAsset none, bAttached 1,
  FOV 65. No gun-path bake of a clone.
- **Pass 2, placement** (Colt fit 42 11 -11). Mesh origin (-4.41, -0.41, -1.64) and muzzle (15.19, -0.41, 5.76) in the
  off controller's frame. The bore is along the controller's forward by construction (fit angle 0).
- **Pass 3, the hand.** The hand bone sits at (-14.95, -2.16, -3.13). The captures (`logs/shots/pistol6-on_zoom.png`,
  `-raised_zoom`) show the fingers closed on the grip.
- **Pass 4, catch-up.** Walking (`pistol6-walk`), the hand and the gun stay together. The distance was not logged.
- **Pass 5, a weapon switch.** The gun hand drew the Colt itself while the clone was attached, then went back to the
  BAR. No empty key, no parachute path (`pistol6-akimbo`: a Colt in each hand).
- **Pass 6, left-hand mode.** The clone is in the right hand, drawn solid (`pistol6-lefthand`).
- **Pass 7, lifetime.** Five cycles and a `Suicide`: no fault, and the next attach is refused with no pistol.
- **The C96** (`pistol7-pair`). `GiveWeapon` gives it at level -1. It is carried beside the Colt in the gun hand: the
  inventory chain's other pistol.
- **Not run:**
  - the C96's parts at levels -1..2 (Phase 1 handles its `clip` bone and upgrade parts, 3.1);
  - bakes per Draw;
  - `lastTwist[1]`;
  - the 20-cycle count.

---------------------------------------------------------------------------------------------------------------------

## 1. What I re-checked, and where the notes disagree

### 1.1 Re-verified for this design [V]

| Claim | Evidence (mine) |
|---|---|
| `UObject::ProcessEvent` 0x109CE980 refuses only: no Native/Defined flag, a probe-masked name, `vt+0x40` (pending kill), **iNative != 0** (`*(short*)(fn+0x90) == 0`), a remote native (`vt+0xFC`). Nothing about weapon state. | Ghidra decompile of 0x109CE980 (the `& 0x402`, `FUN_10919400`, `vt+0x40`, `fn+0x90`, `vt+0xFC` tests); matches OFFHAND-DESIGN 1.1. |
| **Out parameters land in the caller's parms block:** each CPF_OutParm gets a record whose PropAddr = `Property.Offset (+0x64) + Parms` (0x109CEAE3 `mov edx,[esi+0x64]` / `add edx,[ebp+0xC]` / 0x109CEAEB-0x109CEAEE). **The result goes to Parms + ReturnValueOffset** (0x109CEB38-0x109CEB4F). After the call, the ConstructorLink loop (0x109CEB51-0x109CEBB5) destroys only locals past ParmsSize and copies back only non-out value parms. **So an out `TArray` filled by the engine stays in the mod's buffer, owned by the mod.** | disassembly 0x109CEA60-0x109CEBBE |
| The fire-path natives have no index: `CalcWeaponFireNative` (`native static simulated`, SC/MOHAGame/EALAWeapon.uc:130), `ApplyDamage` (EALASmallArms.uc:233), `NoiseRadius` (MOHAPawn.uc:149), `OnWeaponFire` (MOHAPlayerStatsComponent.uc:92), `SoundRiotMixSelect.IsActive/DeActivate` (Engine/SoundRiotMixSelect.uc:22, 37), `Object.FindObject/DynamicLoadObject` (Core/Object.uc:1432, 1435). The decompiler prints `native(N)` for numbered natives (Actor.uc:772 `native(278) ... Spawn`). | script declarations (the research notes read iNative 0 from the package export tails too) |
| **AEALASmallArms::ApplyDamage 0x10F0DF60:** returns on a zero HitLocation; reads Instigator (+0x98) with **no null check** (0x10F0DFA9) and its Location (+0xE8). The distance / 100 is compared to ForceFalloffDistanceMax (+0x338): at or past it, no damage (0x10F0E03F). Falloff starts at +0x33C. Returns on no HitActor (0x10F0E091). Momentum and DamageType are indexed by **FiringMode** (+0x284, +0x290); Damage = f x InstantHitDamage (+0x278) indexed by **CurrentFireMode (+0x22C)** (0x10F0E151-0x10F0E164). The instigator is `Instigator.Controller` (+0x1EC, 0x10F0E169). Calls the TakeDamage helper 0x10D30DB0 (0x10F0E16F); `ret 0x44`. No read of `Pawn.Weapon`, the weapon's state or bStasis. | disassembly 0x10F0DF60-0x10F0E17F |
| A Colt / C96 object carries the **AEALASmallArms vtable 0x115880C0**. MOHAPistol (SC/MOHAGame/MOHAPistol.uc:1-4), MOHAColt45 and MOHAMauser (NN/MOHAColt45.uc:1-3) are script classes; EALASmallArms is `native` (EALASmallArms.uc:1-3). Slots: +0xF0 = 0x10DB1FA0 (AActor::ProcessEvent), +0x16C = 0x10F0CCF0 (stasis), +0x314 = 0x10F0CDD0 (GetTraceOwner), +0x318 = 0x10F0CDC0 (GetTraceRange), +0x344 = 0x10F0DF60 (ApplyDamage). *(mod-integration had read +0x314 only in the grenade's vtable.)* | read_memory 0x115881B0, 0x1158822C, 0x115883D4, 0x11588404 |
| **Stasis:** 0x10F0CCF0 returns 1 when `this != Instigator.Weapon (+0x3A8)` (0x10F0CD1A-0x10F0CD26). A holstered pistol doesn't tick and its timers don't run, so its refire timer can't time off-hand shots. | disassembly |
| `EALASmallArms.ProcessInstantHit`: standalone -> `ApplyDamage(FiringMode, Impact)`; then, unless the hit is a Trigger / TriggerVolume, `GetWeaponAttachment().PlayImpactEffectsWithImpactInfo(...)`. `GetWeaponAttachment()` = `MOHAPawn(Instigator).CurrentWeaponAttachment`: the gun in hand's. | EALASmallArms.uc:167-230; EALAWeapon.uc:214-227 |
| `PerformWeaponTrace` = GetAdjustedAim, `CalcWeaponFire`, then on Authority `SetFlashLocation` + `SpawnGunshotStimulus(Start)` + `SpawnImpactStimulus(Start, Hit)`, then `ProcessInstantHit(CurrentFireMode, ImpactList[i])` for each. `SetFlashLocation` -> `Pawn.SetFlashLocation` -> `FlashLocationUpdated` -> `Pawn.WeaponFired` -> **`Weapon.PlayFireEffects` on `Pawn.Weapon`**, whichever weapon called it. | EALAWeapon.uc:357-389; Engine/Weapon.uc:594-601; Engine/Pawn.uc:447-463, 504-513 |
| `MOHAPlayerPawn.GetAdjustedAimFor(W, ...)` ignores W and returns `GetBaseAimRotation()`. | MOHAPlayerPawn.uc:3413-3417 |
| The mod's aim hooks are single-gun. `Hook_GetBaseAimRotation` returns eye -> the main dot's point for **any** call on the local pawn and arms `g_shot`. `OnBulletTrace` takes any armed (<= 100 ms) trace whose Source is the pawn, with zero extent, within acos(0.95) = 18.2 deg of the armed aim, and rewrites Start/End onto the main gun's ray. `Hook_AddSpread` pulls the spread to the last aim. | src/mohavr/aim.cpp:267-296, 339-347, 365-379, 246-264 |
| **Credit:** bullet hits and kills go to `EALAWeapon(<killer>.Pawn.Weapon).WeaponType`. `GiveWeaponExperience` adds the points to the component **of that WeaponType** when it differs from the current one's. | MOHAAIPawn.uc:1272; MOHAAIController.uc:390, 394; MOHAVehicle.uc:361; MOHAPlayerStatsComponent.uc:178-198 |
| Player -> NPC damage adjustment reads no weapon (the hit bone, the headshot cheat, team 0 -> 0 damage). The only weapon read in MOHADamageType is the NPC -> player scale. | MOHADamageType.uc:104-145, 146-160 |
| An inventory weapon's Instigator is the pawn. | Engine/Inventory.uc:102-107 |
| **Upgrades are applied only at an equip.** `AttachWeaponTo` loops `upgrade(WeaponType, I, self)` for I = 0..applied level. `upgrade()` applies levels `W.CurrentUpgradeLevel+1 .. UpgradeLevel` and touches the **current** attachment only when `Attachment.WeaponClass == W.Class`. `StaticUpgradeWeapon` raises `CurrentUpgradeLevel`. A fresh weapon starts at -1. | EALAWeapon.uc:548-594, 1714; MOHAWeaponUpgradeManager.uc:120-160; MOHAWeaponUpgrade.uc:35-45 |
| A weapon not in hand can be leveled up by the game: `StartUpgradeSequence(W)` takes any weapon object; the sequence calls `W.AddUpgradeSequenceAmmo()` (a `SetAmmoCount` to max) in its BeginState and later `upgrade(..., W)`. The flat game does this for grenade kills. | MOHAWeaponUpgradeManager.uc:208-226, 244-345; EALAWeapon.uc:728-735 |
| If the player dies inside a call, `Pawn.Died` calls `Weapon.HolderDied()` and may `TossWeapon(Weapon)` on whatever `Pawn.Weapon` is then. | Engine/Pawn.uc:1885-1897 |
| UMOHAPlayerStatsComponent's vtable 0x1158E8F8 +0xF0 = UObject::ProcessEvent 0x109CE980 (the call helper's component path). | read_memory 0x1158E9E8 |
| The mod's call helper: by name, `iNative == 0`, `ParmsSize <= 256`, actor or component ProcessEvent, SEH. Every off-hand shot call fits: the largest is `CalcWeaponFireNative` (TraceOwner 4 + 3 vectors + a TArray + an ImpactInfo return of 0x40, about 0x74). | src/mohavr/offhand.cpp:98-144 [V]; the parameter offsets [R in S1] |
| The executors run in `Hook_Draw`'s pre-draw (UGameViewportClient::Draw 0x10C14230), on the game thread after the world tick. The main gun's shots complete inside the tick, so the two never interleave. | src/mohavr/vr_view.cpp:499-511; addresses.hpp:93 |
| Bookkeeping: viewmodel's part list has **4 slots** and skips only the grenade carrier. The arm bake hands any unclaimed FOV part to the reload as "the gun", and has 6 slots. | viewmodel.cpp:81, 373-397; arms_ik.cpp:220, 756-759, 804-806 |
| The grenade carrier is baked at the raw off-controller frame (+6/-2 units) **without** the body carry W that the hand's IK target gets. With `Weapon.CatchUp=1`, the drawn grenade trails the hand at a walk. | offhand.cpp:903-913 vs arms_ik.cpp:399-401, 484, 797 (draw-hold's finding confirmed) |
| Shipped config: `RightHip=SwitchPistol` at `22 -65 0 16`; the pouch `0 -60 14 12`; `LB=none`, `LT=none` (the off grip and trigger reach no game control); Xbox B (switch weapon) = the controller's `y`; the Colt's fit `42 11 -11 0 5.5 0.5`, the C96's `46 10 -10 0 5.0 1.5`. | config/MOHAVR.ini:312, 318, 322, 241, 243, 238, 531, 535 |
| Today any hand at a holster runs its command and becomes the gun hand. | src/host/hands.cpp:272-283 |
| The layer budget has room for a second dot (16 layers; the markers get `15 - layerCount`). | src/host/main.cpp:965, 1036-1060 |
| Test targets exist natively: `WorldInfo.PawnList` and `Pawn.NextPawn`. | Engine/WorldInfo.uc:295; Engine/Pawn.uc:22 |

### 1.2 New findings (not in the six notes, or corrected)

1. **Calling `upgrade()` on a holstered magnum Colt turns on a sound mix the game keeps for the gun in hand.**
   `MOHAUpgradeColt45_2.StaticUpgradeWeapon` ends with `W.MagnumRoundsMix.ActiveAsMixFader()` (NN/MOHAUpgradeColt45_2.uc:15).
   The mix (`Aud_GCm_MixSnapshot.Magnum_Fader`, shared by every weapon, EALAWeapon.uc:1710) is otherwise switched on only
   while a magnum Colt is equipped (WeaponEquipping, EALAWeapon.uc:1442-1444), and off at `PutDownWeapon` /
   `PlayerPawnDied` (:697-726). Applied to the off-hand Colt with the BAR in hand, it would stay on under the BAR. **Rule:**
   after any `upgrade()` the mod makes, and after a level-up sequence of the off pistol ends, keep the game's invariant: the
   mix is active only if `Pawn.Weapon.bHasMagnumRounds`. If not, call `MagnumRoundsMix.DeActivate()` (unnumbered native)
   [V script; R whether audible].
2. **The C96's full-auto rate is about 16.7 rounds/s, not 7.5.** The 712 kit sets `RefireCheckTime = 0.06` and
   `FiringStatesArray[0..1] = 'WeaponBurstFire'` (NN/MOHAUpgradeMauser_2.uc:16-24; DW:2985-2989).
   `EALASmallArms.GetEffectiveFireRate` uses RefireCheckTime whenever it is non-zero (EALASmallArms.uc:282-294);
   `FireInterval` 0.1333 is only the gun's fire animation. ux-prior-art's 7.5/s is wrong; script-fire is right [V].
3. **The pistol's report can't be found the way the reload finds its sounds.** `reload::BuildCues` collects only cues on
   the arms' AnimSets' sound notifies (src/mohavr/reload.cpp:491-532). The fire cue lives on the attachment class's
   default object (`WeaponFireSnd[0]`) and the magnum one on `MOHAUpgradeColt45_2`'s defaults. **Lookup:**
   `Object.FindObject("Aud_GCm_FakeWAV.COLT.WpnFire_PC_Colt", <the SoundCue class taken from any cue BuildCues found>)`
   (unnumbered static native), or a one-time probe of the attachment UClass for its `Default__Attachment_Colt45` pointer.
   No new address either way. [V decl; R which works]
4. **The impact list needs no GMalloc address.** native-fire proposed freeing the engine-allocated `ImpactList` through
   GMalloc (0x116CBF6C: a new pinned address). Instead the mod keeps one array header across shots. Each call it passes
   `{Data, Num = 0, Max}` from the last call and reads the new `{Data, Num, Max}` back. The engine's
   `TArray<ImpactInfo>::Add` grows it only past Max (0x10C92CE0 [V-agent bytes]). A bounded buffer (a few 0x40-byte
   entries) is never freed, and **a mod-allocated pointer is never passed in** (the engine may realloc / free it) [I; R in
   S1].
5. **The level-up runs inside the swap.** A kill that levels the pistol calls `NewExperienceLevelReached` ->
   `StartUpgradeSequence(P)` -> `GotoState('UpgradeSequence')`, whose BeginState runs at once: DOF, the magic-bullet mix,
   god mode, `P.AddUpgradeSequenceAmmo()` -> `SetAmmoCount(max)` on the pistol, which flips the shared low-ammo mix by
   the pistol's count, as the flat game does for grenade level-ups. All of it is on the manager or the pistol; nothing
   reads `Pawn.Weapon` [V script]. The later `upgrade()` (a timer, outside the swap) hits finding 1.
6. **A pawn spawned with `Summon` is no damage test.** With no controller, `AdjustDamagePlayertoNPC` reads
   `kaic.TeamIndex` from `none` (0) and zeroes the damage as friendly fire (MOHADamageType.uc:104-145) [I from the script].
   S1b needs a real enemy (7.1).
7. `StaticUpgradeWeapon` also runs `UpdateMeshVisibility(W.WeaponSkeletalMesh, ...)` (MOHAWeaponUpgrade.uc:35-45). On a
   pistol never equipped this level that pointer is unset or none, and the function returns on none (:59-68). On one put
   away earlier it is the destroyed attachment's mesh component, which is exactly what the flat game passes when a grenade
   kill levels up a grenade not in hand [I: the same exposure as the game's own path; R8].

### 1.3 Where the notes disagree, and what this design does

| Point | Notes | Decision and why |
|---|---|---|
| Which trace call | script-fire: `CalcWeaponFireNative`; native-fire: the script `CalcWeaponFire` | **`CalcWeaponFireNative`** with TraceOwner = the pawn and zero extents. The script version adds `GetTraceExtentsHelper()`, which is the melee box when the object is in `WeaponMeleeFire` (it matters for the same-object pair, 2.6). The native reads only its parameters [V-agent native-fire 2.1]. The script one is the fallback. |
| Freeing ImpactList | native-fire: GMalloc vt+0xC (a new data address); script-fire: reuse | **Reuse one engine-owned buffer** (finding 4): no new address. |
| Aim | native-fire / script-fire: stand down; mod-integration: a second AimFrame plus an override scope | **Stand down for the shot** (explicit Start/End; the hook ignores traces inside the scope and clears `g_shot.armed`/`followOn` at its end). **A second per-view trace** only for the second dot's distance and the shot's start rules. The override (making `GetBaseAimRotation` serve the off line) is needed only by fallback F4 (2.7). |
| Credit | script-fire, native-fire, ux: a one-call `Pawn.Weapon` swap; ux also: give the points to the pistol and take them back | **The swap**, around the `ProcessInstantHit` loop only, guarded (2.4). `PistolCredit=main` (the game's literal rule) is the fallback. The default `pistol` ships only after S1b/R8 prove it. |
| Two pistols | mod-integration: refuse in Phase 1, a second object via `CreateInventory` in Phase 3; ux: a mod-made twin; script-fire/native-fire: **the active pistol object can fire the off-hand shots itself** | **The same object**, with its own clip kept by the mod (2.6), in Phase 3. No `CreateInventory`, so no duplicate in saves, the weapon cycle or the loadout copy. Two different pistols (Colt + C96 after Give all) work in Phase 1 through the long-gun path. |
| Where the drawn pistol is anchored | the grenade: the hand (inv(grip) x the free hand); draw-hold: the gun | **The gun**, at the weapon's own fit mirrored (3.2). A gun must point where its aim line points, and this reuses the player's per-gun fits. The hand snaps on as the reload grips do (D23). |
| Reload | mod-integration: holstering reloads; ux: refill in the holster after the game's reload time, plus a pouch swap | **Phase 1: refill in the holster after the pistol's own `ReloadInterval[0]`** (Colt 1.5 s, C96 2.5 / 1.75 s) in game time. Drawn sooner, it keeps its count. `PistolRefill=instant` is the [H] alternative (GunVR refilled at once). **Phase 2: the pouch.** |
| C96 712 rate | ux 7.5/s; script-fire 16.7/s | **16.7/s** (finding 2). |
| A grenade as the main weapon while the pistol is held | mod-integration: allowed; ux: put the pistol back | **Allowed** (two different objects; credit swap works the same), [H]. The HellBox and the pistol itself end the hold. |
| Flash component | native-fire: cloned components; effects: never `Clone` an activated PSC, mint one with `InitHitspangPSC` | **effects' recipe** (Phase 2, S3). |
| The hold model | grip (mod-integration) / hold (ux); options sticky / toggle | **`PistolHold=grip`** default. **`toggle`** is the first [H] A/B: the player's GunVR habit was the toggle. |

---------------------------------------------------------------------------------------------------------------------

## 2. The shot

### 2.1 Why there is no one-call entry point [V]

| Entry point on the holstered pistol P | What goes wrong | Evidence |
|---|---|---|
| `StartFire` / `BeginFire` | sets `InvManager.PendingFire[mode]`: the **gun's** trigger (a full-auto gun keeps firing; `Active.BeginState` re-fires); P itself fires nothing (state Inactive, `GotoState` numbered) | Engine/Weapon.uc:258-266, 709-713; EALAWeapon.uc:1143-1163 [V-agent script-fire 5.A] |
| `FireAmmunition` | the arms' `Weapon_FireEvent` (the gun arm plays fire), `GetWeaponAttachment().WeaponFired()` = **the gun's** sound / flash / light / brass, `ConsumeAmmo` -> `SetAmmoCount` (shared low-ammo mix) | EALASmallArms.uc:296-321; EALAWeapon.uc:179-201 [V-agent] |
| `InstantFire` / `PerformWeaponTrace` | `SetFlashLocation` -> `Pawn.Weapon.PlayFireEffects` (the gun's fire animation, tracer data, whiz); the aim from the mod's `GetBaseAimRotation` (the main dot) | 1.1 |

So the shot is rebuilt from the functions below these, as the grenade was: what the weapon's own functions do, minus the
calls that reach the active weapon.

### 2.2 The sequence (game thread, `offpistol::OnDraw` in Hook_Draw's pre-draw, after `offhand::OnDraw`)

Objects, all by reflection (`names::PropertyOffset`): `pawn` = `aim::LocalPlayerPawn()`, `C` = its Controller, `IM` = its
`InvManager`, `G` = `pawn.Weapon` (the gun), `P` = the held pistol (fixed at DRAW), `stats` = `C.playerStats`, `mgr` =
`pawn.WeaponUpgradeManager`. All calls go through the call helper (factored to `script_call.cpp`), and every parameter is
filled (no defaults through ProcessEvent).

```
SHOT(ray)                                       // ray = the off aim line at the trigger pull (LOCAL), from the event ring
 0  gate     state Held; P alive (!bDeleteMe), P.Instigator == pawn (ApplyDamage dereferences it unchecked), P still IM's
             (PistolWeapon or the chain object recorded at DRAW); pawn alive; !bNoWeaponFiring; Available() (the factored
             4 Hz gate: IsWeaponDisabled, cinematic, mounted MG, not the parachute / landing)        -> else refuse (acked)
             rate: gameTime - lastShot >= (P.RefireCheckTime > 0 ? P.RefireCheckTime : P.FireInterval[0])
                                                                       -> else refuse "too soon" (no queue: the game's rule)
             clip: P.AmmoCount[0] (or the twin's count, 2.6) == 0  -> dry cue at the muzzle, pistolDry++, done
 1  ray      start/dir = view::PoseToWorld(ray). Trace it as aim::OnPlayerView traces the main line (factored TraceAimLine):
             TraceThrough from start, stepping 20 cm past a start inside geometry (up to 1 m) -> from, point.
             Aim.ShotFromGun=1 and the eye -> from segment clear: shotStart = from, dir as is.
             Else (a hand through a wall, or ShotFromGun=0): shotStart = the eye, dir = unit(point - eye)   (D12's rule)
             end = shotStart + dir x P.WeaponRange (16384)
 2  mode     saveMode = P.CurrentFireMode; P.CurrentFireMode = 0          // ApplyDamage indexes the damage by it
 3  scope    aim::BeginOffShot()                                          // OnBulletTrace / Done ignore traces now
 4  trace    Call(P, "CalcWeaponFireNative"): TraceOwner = pawn, StartTrace = shotStart, EndTrace = end,
             vTraceExtents = (0,0,0), ImpactList = {g_list.data, 0, g_list.max}
             -> g_list = parms.ImpactList (data, num, max); real = parms.ReturnValue (ImpactInfo, 0x40 bytes)
 5  scope    aim::EndOffShot()                                            // also clears g_shot.armed / followOn / active
 6  AI       Call(P, "SpawnGunshotStimulus"): Loc = shotStart
             Call(P, "SpawnImpactStimulus"): StartTrace = shotStart, Loc = real.HitLocation       (PerformWeaponTrace's order)
 7  credit   if PistolCredit=pistol and G != P: write pawn.Weapon = P                  (2.4)
 8  damage   for i < num: Call(P, "ProcessInstantHit"): FiringMode = 0, Impact = g_list.data[i]
             // ApplyDamage -> TakeDamage (hit bone, ragdoll, death, Kismet on triggers); impacts on G's attachment
 9  restore  if pawn.Weapon == P (nothing changed it) and G is live (!bDeleteMe, G.Instigator == pawn) and pawn alive:
                 pawn.Weapon = G
             else log "the shot changed the weapon in hand" and leave it (the death path discards the inventory itself)
10  mode     P.CurrentFireMode = saveMode if P is pawn.Weapon (the same-object pair), else leave 0
11  noise    Call(pawn, "NoiseRadius"): fNewRadius = P.WeaponFireNoiseRadius (5500)
    stats    Call(stats, "OnWeaponFire"): WeaponType = P.WeaponType, WeaponClass = P.WeaponClass, NumShots = 1
12  ammo     P.AmmoCount[0] -= 1 (direct write; never SetAmmoCount / SubtractAmmo / ConsumeAmmo: the shared low-ammo mix)
13  report   Call(P, "PlaySoundAt"): ASound = the report cue (3.5), SourceLocation = the off muzzle
             pistolShots++ (the host's recoil pulse; Phase 2: the slide, flash, brass, kick from the same counter)
             lastShot = gameTime; log "offpistol: shot N -- <P> from the off gun, hit <actor/bone> at X m, 0 cm from the off
             dot; clip 6/7; the gun MOHABar_1 unchanged"
```

Auto fire (`P.FiringStatesArray[0] == 'WeaponBurstFire'`, the C96 at its level 2): after the press's SHOT, while
`pistolFlags` says the trigger is held (and clip > 0, gate ok), the game repeats the sequence at the same rate along the
**current** off aim line (the newest per-view mapping). The 712's report is one cue per round (`WpnFire_PC_Mauser`) in
Phase 1; the game's loop and report cues are Phase 2 polish [I].

Nothing in this sequence writes the gun's state, `IM.PendingFire`, `pawn.FlashCount` / `FlashLocation` / `ShotCount`, G's
`AmmoCount` or its attachment's counters. The only shared things touched are `pawn.Weapon` for the length of step 8, which
is restored, and the impact-sound ring of G's attachment (exactly what the game's own impacts use) [V by construction; R4
measures it].

### 2.3 Rate, spread, kick

- Rate: the pistol's own `RefireCheckTime`, read at the shot (upgrades change it: Colt 0.25 -> 0.06 at level 1; C96 0.06),
  timed in game time (`WorldInfo.TimeSeconds`, so the game's pause stops it). A pull that comes too soon is refused, not
  queued: the game's single-fire state drops an early press (`WeaponSingleFire.RefireCheckTimer` -> `global.StopFire`,
  EALASmallArms.uc:1691-1742 [V-agent]).
- Spread: none (the hand is the spread, `Aim.Spread=0` parity). The holstered pistol's accuracy component is stale anyway
  (no Tick) [I].
- View kick: none from the game. Only `Pawn.Weapon`'s kick reaches the camera (0x10E82AF0 [V-agent effects 2.11]), which
  is right for VR. A drawn hand kick is Phase 2.

### 2.4 Credit: the one-call `Pawn.Weapon` swap, and its guard

Every script reader of "the killer's weapon" on the bullet path runs synchronously inside step 8:
`MOHAAIController.NotifyTakeHit` (hits, ally hits), `MOHAAIPawn.Died -> NotifyPlayerStats` (kills), the stats natives
`OnWeaponHit -> OnKill -> eventGiveWeaponExperience`, and `CreatePlayerKillStimulus` (a constant 5.0 for any EALAWeapon).
So with `pawn.Weapon = P` only for that loop, hits, kills, points and level-ups go to the pistol [V call chain; R3 / R8 for
the numbers].

What else could run inside the window, and why it is acceptable:
- **A pistol level-up:** the sequence starts at once and treats P as the upgraded weapon (finding 5). Afterwards
  `UpgradeTheWeapon` upgrades P on a timer, outside the window, and the mod restores the magnum mix (finding 1).
- **The player dying** (an exploding prop shot at point blank, if MOHA has any): `Pawn.Died` would call
  `P.HolderDied()` and maybe toss P instead of G, then `DiscardInventory` sets `pawn.Weapon = none`. The guard (step 9)
  never writes back over a change, and the dead pawn's level reloads from a checkpoint. **Cosmetic, rare.**
- **Nothing on the path switches weapons:** Kismet actions run in the next tick's sequence update, not inside
  TakeDamage [I].
- **The mod's own hooks don't read `Pawn.Weapon` inside the window:** `GetBaseAimRotation`, `AddSpread`, the bullet
  trace and `ActivateSystem` key on the pawn, the instigator or the current attachment. The 4 Hz viewmodel check and the
  reload's per-Draw work run outside it.

`PistolCredit=main` skips steps 7 and 9: the gun in hand earns the pistol's hits, as the flat game would.

### 2.5 Upgrades at the draw

At DRAW (not per shot): `applied = mgr.GetAppliedUpgradeLevel(P.WeaponType)`. If `P.CurrentUpgradeLevel < applied`:
- `mgr.upgrade(P.WeaponType, applied, P)`, one call that applies every missing level (MOHAWeaponUpgradeManager.uc:120-160);
- then the magnum-mix rule (finding 1);
- log CurrentUpgradeLevel, InstantHitDamage[0], RefireCheckTime, MaxAmmoCount[0], FiringStatesArray[0] and
  bHasMagnumRounds before and after.

The gun's attachment is not touched (its WeaponClass isn't P's). Without this, a pistol never equipped this level (the
usual case: the loadout equips the primary) fires at base stats. For the player's fully upgraded Colt that would be 50
damage and 0.25 s instead of 100 and 0.06 s [V script; R5 for the -1 at level start]. Switch `PistolUpgrades=1`.

### 2.6 Two pistols

- **Colt + C96** (both carried, e.g. after Give all): the long-gun path unchanged. P is the pistol not in the gun hand,
  found the way `GrenadeOf` walks the chain; the credit swap gives each its own experience. **Phase 1.**
- **Twin of the same pistol (Phase 3, `PistolPair`):** P = `pawn.Weapon` itself.
  - Shots: the same sequence. CurrentFireMode is saved and restored around it (the main pistol may be mid-melee), and
    `CalcWeaponFireNative` has zero extents whatever P's state.
  - No credit swap: it is already the pistol's.
  - The twin's clip is a **mod-side count**: P.AmmoCount is the gun hand's (HUD, D21). The twin is refilled in the
    holster like the single pistol.
  - Its drawing is a clone of P's own pickup mesh beside the attachment's mesh.
  - Nothing new in the inventory: no duplicate in saves, the weapon cycle or `CopyInventoryToWorldInfoLoadout`
    (script-fire 7) [V-agent / I].
  - While the twin is held, the main pistol's manual reload waits (it needs the off hand). That is the honest trade
    every dual-wield game makes.

### 2.7 Fallbacks, step by step (each logged; the first that fails decides)

| If at runtime | Then |
|---|---|
| F1 `CalcWeaponFireNative` refused, faults, or its list comes back empty with a hit in ReturnValue | the script `CalcWeaponFire(Start, End, ImpactList)` on P (the same native underneath; state Inactive gives zero extents); if the list is still unusable, process **ReturnValue only** (loses only the damage events of triggers passed on the way) |
| F2 `ProcessInstantHit` refused / faults | `Call(P, "ApplyDamage")` (native, iNative 0, the same damage) + `Call(G's attachment, "PlayImpactEffectsWithImpactInfo")` (`Impact`, `ImpactType` 0, `DamageType` = P.InstantHitDamageTypes[0]) when the attachment exists |
| F3 the swap misbehaves (R3 / R8) | `PistolCredit=main` |
| F4 the inactive pistol's damage path is unusable as a whole (not expected) | the grenade's fallback B analog: `P.PerformWeaponTrace(shotStart)` inside a whole-call `Pawn.Weapon = P`, with aim.cpp's **override** frame (GetBaseAimRotation and the bullet hook serve the off line) and `P.WeaponSkeletalMesh` set to none while it is holstered (`PlayWeaponAnimation` returns on none, EALAWeapon.uc:850) |
| F5 nothing works | **today's behaviour** (the switch off): the off hand at the pistol holster draws the pistol as the main weapon into that hand (hands.cpp:272-283). Against the request: the long gun is put away. A faster variant (shorter equip times, A2 in ENGINE-NOTES 5af) is possible but not proposed. |
| the report can't be found or its bank isn't loaded (R1) | silent shots plus the haptic pulse (logged); the impacts still sound |

### 2.8 What the shot keeps and skips

**Kept:**
- the game's bullet trace (per-poly, through triggers and portals), its damage rules (falloff, hit bone, friendly fire,
  momentum, ragdoll, death, `SeqEvent_TakeDamage`), its impact effects (decal, impact sound, hitspang: weapon-agnostic,
  `ImpactType` 0);
- the AI's hearing and stimuli, the stats (shots fired, hits, kills, experience, level-ups), the pistol's damage, rate,
  clip and upgrades.

**Skipped on purpose:**
- the gun's fire effects, the arms' fire event, the view kick, the eye start and the hooked aim;
- the pistol's own state machine (it can't run);
- the low-ammo mix (the shared mix follows the gun in hand).

---------------------------------------------------------------------------------------------------------------------

## 3. Presentation

### 3.1 The drawn pistol (the carrier)

The grenade's recipe (offhand.cpp:328-379), with the pistol's differences [V-agent draw-hold 2-3; V the grenade's S2]:
- **Mesh:** `Object.Clone(InOuter = P)` of `P.DroppedPickupMesh`. It is the attachment's own skeletal mesh
  (`US_M1911A1_Pistol_Rigged` / `DE_Mauser_Rigged`, NN/MOHAColt45.uc:74-79 vs NN/Attachment_Colt45.uc:28-29). The
  attachment itself exists only while the pistol is `pawn.Weapon`.
- **Before attaching:**
  - copy the arms' depth group, light environment, LOD and FOV (65); set `fCustomBoundsSize` 200; turn collision off;
  - **new:** clear `PhysicsAsset`. The pistols' pickup meshes have one; no instance is made without
    `bHasPhysicsAssetInstance`, but one would make UpdateTransform skip the bake's MidHook;
  - **new:** publish the carrier pointer **before** `AttachComponent`, so a first update can never take it for the gun.
- **Attach:** `FPArms.AttachComponent(clone, 'Camera', 0, 0, (1,1,1))`.
- **Lifetime:** one clone per hold, detached at HOLSTER or when P is gone. A new pawn drops it.
- **Requirements:** only with the arm bake (`Weapon.ArmIK=1`, `ViewModel=2`); hidden with `Weapon.HideViewModel=1`.
- **Bookkeeping fixes:**
  - skip the clone by pointer in `viewmodel::UpdateWeaponKey` (its Outer is P, a weapon, not a WeaponAttachment, so
    mid-switch it would raise `g_noGunDrawn` and drop the rig onto the parachute path);
  - raise `g_parts` 4 -> 8 and `kBakeSlots` 6 -> 8;
  - add the bake branch before the move test (else `reload::OnGunBake` takes it for the gun).
- **The C96's parts** (no AnimTree on a pickup clone, so no SkelControls): `clip` is parked 55 units behind the grip in
  the reference pose and is collapsed always. The upgrade parts are collapsed by `CurrentUpgradeLevel`: the stock below
  level 0, `upgrade_02_magazine` below level 1. The bone names are to be checked against the main C96's collapsed bones
  [R]. **Phase 1** (it is visible).

### 3.2 Where it is drawn: the gun anchored, the fit mirrored

```
Gw_off = K(t) x M_left(key) x F_off x W
M_left = S x M_right x M_y       S = diag(-1,1,1) (the mesh's own mirror), M_y = diag(1,-1,1) (the controller's)
M_right(key) = [ the pistol's idle pose in the game camera's frame (rows = the camera's axes), origin O_idle(key) - fit.grip ]
```
- `F_off`: the host's `offGunPose` (the off aim pose pitched by the pistol fit's angle, as `gunPose`), mapped into the
  world, and into the mirror world in left-hand mode as the arms are.
- `W`: `viewmodel::BodyMoveSinceView`, the same carry the hand's IK target gets. **The grenade carrier lacks it (1.1); fix
  both.**
- `K(t)`: the kick replay, identity in Phase 1.
- **`O_idle`:** Colt (37.59, 11.41, -12.64), C96 (38.05, 11.34, -12.94) units in (forward, right, up). Computed offline
  from `colt45_idle` / `mauser_idle`, and the Colt's matches the in-game log (37.6, 11.4, -12.6) to 0.05 units
  [V-agent draw-hold 4.1]. Generated by `tools/reload_grips.py` beside the grip rows; viewmodel's once-per-weapon log is
  the live check.
- **Result with the shipped Colt fit:**
  - the mesh origin is at (-4.4, -0.4, -1.6) and the muzzle at (15.2, -0.44, 5.8) in the off controller's frame;
  - the bore is 0.3 units from the mirrored aim line [V-agent draw-hold 4.2: arithmetic on the logged pose, the fits and
    the psk];
  - so the off pistol is the mirror image of the main pistol's hold, and its bore is on its own dot's line.

### 3.3 The off hand on it

- The game's own pistol grip: the right hand in `colt45_idle` / `mauser_idle` frame 0 (rigid over the loop: 0.00 units,
  <= 0.06 deg). It is mirrored with `reload_grips.py`'s MIRRORED rule with part = identity (`H_left = MIRROR_D x H_right x
  MIRROR_S`); the mirror convention was re-checked on the parachute's symmetric poses [V-agent draw-hold 5].
  - Rows: draw-hold appendix A (`offgun` for the Colt and the C96; `offgun_pull` = `colt45_fire` f0, Index3 curled
    23.6 deg). **Regenerate them with the tool:** the scratchpad copy is temporary.
- **arms_ik:** a third grip source before `reload::GripNow` (arms_ik.cpp:486-490). `target = H_left x Gw_off`, computed
  from the same inputs as the clone's bake (the same view's hand frames, W, fit, K), so the hand and the gun can't
  separate. The 15 fingers come from the grip, with the trigger finger blended idle -> pull by `pistolTrigger`.
- The snap from today's free hand is 28.6 deg, of which 0.8 deg is forearm twist. The round 17-21 "pistol wrist twist"
  should not return [V-agent; R].
- The free hand's hold (D25) is unchanged and not drawn while the pistol is held.

### 3.4 Moving parts (Phase 2)

Posed in the carrier bake like the reload's action bones (reload.cpp:1621-1663) [V-agent draw-hold 9]:

| Part | Colt | C96 |
|---|---|---|
| Hammer | cocked always: 56 deg about mesh X (the clone's reference pose has it down) | 63 deg |
| Slide / bolt per shot | `gunSlide` Z 2.50 -> -1.85 at about 67 ms, home by 233 ms | `Bolt` Z 0.65 -> -4.55, home by 0.1 s |
| Lock-back at clip 0 | `gunSlide` held at -1.85, the top round hidden (the `[ManualReload]` line's `BoltZ`, `TopRound`; config/MOHAVR.ini:502) | `Bolt` held at -4.55 (:509-510) |

- **The kick:** the game's own `colt45_fire` flips the muzzle 22 deg at 0.067 s. It is replayed mirrored as `K(t)`,
  scaled by `PistolKick` (0..1), default from the player's verdict on the main pistol's kick [H].
- **The clock:** the shot counter plus the game time.

### 3.5 Sound

- **Report:** `P.PlaySoundAt(cue, offMuzzle)`, the reload's proven path (reload.cpp:590-635, through AActor::ProcessEvent
  on the weapon; `PlaySoundAt` uses only `WorldInfo`). It plays at the drawn muzzle, which is the off aim line's start
  plus the bore length (Colt about 15 units).
- **Cue:**
  - `Default__Attachment_Colt45.WeaponFireSnd[0]` = `Aud_GCm_FakeWAV.COLT.WpnFire_PC_Colt`;
  - with `P.bHasMagnumRounds`, `MOHAUpgradeColt45_2.UpgradedWeaponFireSound` = `..._Magnum`;
  - C96: `WpnFire_PC_Mauser`;
  - looked up as in finding 3.
- **Dry click:** `WeaponDryFireSnd` (`WpnDryFire_PC_Colt` / `_Mauser`), the same way. Phase 1.
- **[R1]** whether the pistol's SoundRiot bank is loaded while it is holstered. The class default object references
  `AudioBank = Aud_Weapons_All_SoundRiot.COLT` and no script unloads banks, so it probably is [I]. GunVR's lesson
  ("a second gun of the same type played thin", LESSONS-FOR-NEXT-VR-MOD.md:164-168) says measure it. Pass: a WorldInfo
  AudioComponent with the cue, wave instances > 0 (reload's CheckCue); no hit at the SoundRiot missing-bank path 0x1119863A
  [V-agent effects].

### 3.6 Muzzle flash and brass (Phase 2; spike S3)

- **Component:** a mod-owned ParticleSystemComponent minted by the game's own `SmallArmsAttachment.InitHitspangPSC(out
  PSC)` on the current attachment, the way ShotgunAttachment mints extra ones.
- **Re-parent:** `att.DetachComponent` then `pawn.AttachComponent`, so it outlives the gun's next switch.
- **Setup:** `SetTemplate(<the attachment CDO's MuzzleFlashParticleSystemTemplate.PSystem>)`, `DeactivateSystem`,
  `SetDepthPriorityGroup(2)`.
- **Per shot:** `SetTranslation` / `SetRotation` at the drawn muzzle (un-mirrored in left-hand mode), then `ActivateSystem`.
- **muzzle.cpp:** its ActivateSystem hook leaves non-attachment components alone (muzzle.cpp:79-93).
- **Brass:** the same, with the `ShellEject` template. Which socket looks right in the hand is [H].
- **Never `Object.Clone` an activated PSC:** a shared emitter-instance array is possible [V-agent effects 3].

### 3.7 Impacts, AI, haptics

- **Impacts:** free with `ProcessInstantHit`, on the gun's attachment. They are weapon-agnostic, so the decal, impact sound
  and spang are exactly the pistol's own [V-agent effects 2.2].
  - Two shots in one frame share the attachment's one spang component (cosmetic).
  - With no attachment (mid-switch) the effects call is an "Accessed None" and is skipped [I; R7].
- **AI:** step 6 and step 11 of 2.2.
- **Haptics (host, on the off hand):** each shot 40 ms 0.8 (on `pistolShots`), dry click 15 ms 0.3, too soon 10 ms 0.15,
  draw 30 ms 0.5, refused 60 ms 0.2, holster 20 ms 0.2, refilled 20 ms 0.3. The game's rumble never reaches the
  controllers (xinput_hook.cpp:23-77).

### 3.8 Minimum that reads as real, and later polish

| Phase 1 (the first build) | Phase 2 (before or after the first headset round, the player's choice) | Not planned |
|---|---|---|
| the pistol in the hand at the mirrored fit; the hand closed on it with the trigger finger; the C96's parked clip and upgrade parts collapsed; the report and the dry click at the muzzle; the impacts; AI hearing; the recoil pulse; the second dot | the hammer cocked, the slide / bolt cycling and locking back empty, the top round; the muzzle flash; brass; a hand kick; the pouch reload; an off-hand fit page if the mirrored fit is wrong [H] | tracers (pistols have none: `CreateTracers[0] = 0`); a view kick; the game's arm fire animation; a HUD count (a host counter only if asked) |

---------------------------------------------------------------------------------------------------------------------

## 4. Interaction

### 4.1 Draw

**Where:**
- Every holster zone whose command is `SwitchPistol` (shipped: RightHip `22 -65 0 16`): the player's muscle memory and
  no new spot.
- Right-hand mode: a cross-draw past the gun arm. Left-hand mode: the off hand's own side.
- `PistolSpot=chest` (a 6th spot, a cross-draw shoulder holster) is the [H] option. The other [H] option is GunVR's
  layout: pistols on both hips, with the grenades moved to the chest.

**The press, D40's rule:** an off-hand grip press in a pistol zone belongs to the off-hand pistol whenever the switch is
on, the game's side is alive and a gun is in the other hand.

| Situation | What happens |
|---|---|
| A take can happen | DRAW (a pulse) |
| A take can't happen now: the landing, a weapon switch, a cinematic, weapons disabled, a mounted gun, the HellBox, a grenade held in the off hand, the only pistol in the gun hand (without `PistolPair`) | the press is consumed with a refusal pulse; **never** the holster's own `SwitchPistol` (it would move the long gun to the off hand, nade3's lesson) |
| Switch off, game quiet, no gun hand | today's behaviour |
| The gun hand at the pistol holster while the off pistol is held | refused (pulse): `SwitchPistol` would equip the object the off hand holds |

### 4.2 Hold and put back

| Switch | Default | Behaviour |
|---|---|---|
| `PistolHold=grip` | yes | held while the grip is held; let go puts it back in its holster (nothing lost, never dropped: it is the pawn's only pistol) |
| `PistolHold=toggle` | [H] A/B | a squeeze at a pistol zone draws; it stays without the grip; a squeeze at any holster zone puts it back; other off-grip presses are consumed while held. The player's GunVR habit. |

### 4.3 Fire

- One shot per pull: press >= 0.6 after < 0.4, the grenade's and the reload's hysteresis.
- **D40's latch:** at the draw and after a freeze, a trigger already above 0.15 must rest below 0.15 before a pull counts.
  A fist closing on the draw must not fire.
- The SHOT event carries the off aim line of that XR frame, so the shot goes where the pistol pointed at the pull.
- The game enforces the rate (2.3). Empty -> a click.
- The C96 at level 2: held fires (the game repeats while the flag says held).
- While held, the off trigger is masked from the pad (`SetMaskedTrigger`), and stays masked until let go after the hold.

### 4.4 Reload (one hand: the gun hand never lets go of the main gun)

| Mode | Behaviour |
|---|---|
| **R1 holster refill** (Phase 1, `PistolRefill=game`) | back in the holster for the pistol's own `ReloadInterval[0]` (game time), it is refilled: `AmmoCount[0] = MaxAmmoCount[0]` by a direct write, with a click and a pulse when full. Drawn sooner, it keeps its count. `instant` refills at once (GunVR's rule), `off` never. If the gun hand equips the pistol before the refill, the refill is cancelled (the game's equip refill and D21's reconcile take over, reload.cpp:2162-2183). |
| R2 pouch (Phase 2, `PistolPouch=1`) | the held pistol's hand point in the pouch ring, slower than about 0.6 m/s for about 0.2 s, with the pistol not full: the old magazine drops (the reload's falling magazine) and a full one seats, with the ClipOut / ClipIn cues. A locked-back slide is then released by the next trigger press without a shot (the Colt's `TriggerRack` rule, D24, round 37). |
| `PistolRefill=auto` / `none` | accessibility options: a dry press reloads after the reload time / never needs one (infinite reserve anyway) |

### 4.5 The off hand's other uses while it holds the pistol

| Use | While held |
|---|---|
| Foregrip | impossible (the grip is busy); its ring and approach pulse hidden; the long gun aims one-handed |
| The main gun's manual reload (pouch, magazine, bolt, pump, breech) | presses impossible, rings hidden; the gun hand's release button still drops the magazine; bolt / pump guns wait to be worked (D29 / D30) until the pistol is put back |
| Off-hand grenade | refused (a pulse); the pistol is refused while a grenade is held |
| The off trigger's contextual uses (twin flip, MP40 grab, M12 pump, grenade pin) | inert: each needs the foregrip held or a magazine in the hand |
| X (Xbox RB, the game's grenade switch) | allowed: the main weapon becomes a grenade, the pistol stays [H] |
| Y (Xbox B, switch weapon) | when the game says the next weapon would be the held pistol (from the secondary), Xbox B is masked (`PistolKeep=1`); otherwise it works |
| The menu's Gun hand | waits until the pistol is put back |

### 4.6 The main gun one-handed

While the pistol is out, the long gun is one-handed (no foregrip). The game's view kick reaches the camera only from
`Pawn.Weapon`, and the pistol adds none. Balance is the physical trade the prior art converged on: no foregrip, no
off-hand grenade, the main gun's reload waits [V-agent ux].
- An unupgraded Colt adds roughly 30 % to a BAR's damage per second (4 x 50 vs 7.5 x 85); fully upgraded it about
  doubles it [I arithmetic, ux 7.8].
- If the player finds it too strong: the game's own spread for off-hand shots (`PistolSpread`) or a damage scale, not
  shipped unless asked.

### 4.7 Freeze, menus, tracking, death

- **Frozen** (no shots): a menu open, the off grip action asleep (a sleeping controller), **or the off hand not really
  tracked**. Unlike the grenade's release, a shot from a stale pose would aim wrong.
- **Let go during a freeze:** it is put back at the unfreeze.
- **The game's pause:** frozen; the refill timer stops with game time.
- **The game ends the hold** when the pistol is gone (a death discards the inventory; a pickup swap replaces the pistol),
  when weapons become disabled for 2 Draws, or when a switch takes the pistol into the gun hand (refusal code 6). The host
  reconciles after two polls. No live ordnance, so the game may end it alone.

### 4.8 Left-hand mode

- The off hand is the real right controller, the rig's left hand in the mirror world. The same formulas apply and the
  pistol is drawn back through the mirror: a **mirror-image** pistol in the right hand, as the main gun is mirrored in
  that mode.
- The shot needs no un-mirroring: its ray is the host's real-world aim line.
- Only the Phase 2 effects positioned from the drawn mesh (flash, brass) need un-mirroring (muzzle.cpp:123-139's
  pattern).
- The off aim line keeps the fit's `rayRight` for a right off hand and negates it for a left one (hands.cpp:389's rule)
  [V pattern; I applies].
- A true-handed off pistol in that mode would need a per-part determinant flag: only if noticed [H].

### 4.9 Two pistols (Phase 3, `PistolPair=1`)

With the pistol as the main weapon, the off hand at the pistol holster takes its twin (2.6): GunVR's pair grab, which
the player used 6 times in their last session [V-agent ux 1.3]. Each trigger fires its own gun; each refills in the
holster. Phase 1 refuses the twin with a pulse. Colt + C96 works in Phase 1.

### 4.10 The second dot

- Drawn while the pistol is held and the menu's Red dot is on (`PistolDot=1`): a second `Reticle` instance with a colour
  parameter (white, [H]).
- It sits along `offAimRay` at `pistolAimDistance`. The game publishes that distance from a per-view trace of the off
  line, done in `aim::OnPlayerView` beside the main one, only while held.
- The shots land 0 cm from it by construction: the same line, the same start rule, the same trace flags as D12's main
  dot.

---------------------------------------------------------------------------------------------------------------------

## 5. The contract and the state machines

### 5.1 Shared block v21 (`src/common/shared_frame.hpp`, `kVersion` 20 -> 21, "21: the off-hand pistol")

Appended after `nadeEvtVel` (sizeof 2120 today, shared_frame.hpp:329). 4-byte fields and Poses (28 bytes) only, so x86 =
x64. The host refuses a version mismatch.

```cpp
    // --- v21: the off-hand pistol ([OffHand] Pistol; src/host/offpistol.cpp, src/mohavr/offpistol.cpp) -----------------
    // game -> host, once per Draw (seqlock pistolSeq: odd while the game writes)
    volatile std::uint32_t pistolSeq;          // 2120
    std::uint32_t          pistolCaps;         // 2124 bit0 installed (the shot's functions resolved on a carried pistol),
                                               //      bit1 a DRAW can happen now, bit2 infinite reserve, bit3 drawn in the
                                               //      hand (carrier ready), bit4 one held may stay, bit5 the only pistol is
                                               //      in the gun hand (a twin only with PistolPair), bit6 the game's switch
                                               //      weapon would take the held pistol (mask Xbox B), bit7 auto fire
    char                   pistolKey[48];      // 2128 the pistol a DRAW gets / the held one: its attachment class (fit key)
    std::int32_t           pistolClip;         // 2176 its rounds (the twin's count when twinned)
    std::int32_t           pistolMax;          // 2180 its magazine
    std::uint32_t          pistolState;        // 2184 bits 0-1: 0 none, 1 held; bit2 empty; bit3 refill pending;
                                               //      bits 8-15 the last refusal (1 unavailable, 2 none carried, 3 in the
                                               //      gun hand, 4 too soon, 5 the shot failed, 6 taken by a switch, 7 gone)
    volatile std::uint32_t pistolShots;        // 2188 +1 per round fired (the recoil pulse; Phase 2: slide, flash, kick)
    volatile std::uint32_t pistolDry;          // 2192 +1 per dry click
    volatile std::uint32_t pistolEvtAck;       // 2196 the last event taken (applied or refused)
    volatile std::uint32_t pistolPawnSeq;      // 2200 +1 on a new local pawn
    volatile float         pistolAimDistance;  // 2204 per player view (as aimDistance): metres along offAimRay; 0 none
    // host -> game, per XR frame INSIDE the view seqlock (viewSeq), read with the hands
    std::uint32_t          pistolFlags;        // 2208 bit0 on, bit1 held, bit2 frozen, bit3 trigger held (past the latch),
                                               //      bit4 a twin (PistolPair)
    float                  pistolTrigger;      // 2212 the off trigger 0..1 (the trigger finger)
    Pose                   offGunPose;         // 2216 the off controller pitched by the pistol fit's angle (as gunPose)
    Pose                   offAimRay;          // 2244 the off pistol's aim line (as aimRay: position = start, -Z = direction)
    float                  offFitGrip[3];      // 2272 the pistol fit's grip point (units, camera frame; the host's fit)
    // host -> game events, ordered (the reloadEvt / nadeEvt pattern)
    volatile std::uint32_t pistolEvtSeq;       // 2284
    std::uint32_t          pistolEvt[8];       // 2288 low byte: 1 DRAW, 2 SHOT, 3 HOLSTER, 4 RELOAD (Phase 2, the pouch)
    Pose                   pistolEvtRay[8];    // 2320 SHOT: the off aim line at the trigger pull (LOCAL)
};                                             // sizeof 2544 (a multiple of 8)
```
Static asserts: `pistolSeq` 2120, `pistolKey` 2128, `pistolClip` 2176, `pistolState` 2184, `pistolEvtAck` 2196,
`pistolAimDistance` 2204, `pistolFlags` 2208, `offGunPose` 2216, `offAimRay` 2244, `offFitGrip` 2272, `pistolEvtSeq` 2284,
`pistolEvt` 2288, `pistolEvtRay` 2320, `sizeof(Header)` 2544.
(2120+4+4+48 = 2176; +4x7 = 2204; +4 = 2208; +4+4 = 2216; +28 = 2244; +28 = 2272; +12 = 2284; +4 = 2288; +32 = 2320;
+224 = 2544.)

Helpers:
- `enum PistolEvent { kPistolDraw = 1, kPistolShot, kPistolHolster, kPistolReload }`;
- `struct PistolStatus` + `ReadPistolStatus(h, s, seq)` (the ReadNadeStatus pattern);
- `struct PistolView {flags; trigger; Pose gun, ray; float grip[3];}` read by `ReadHands(h, hand, valid, rv, nv,
  PistolView* pv = nullptr)` in the same seqlock pass. Its callers are viewmodel.cpp:607, offhand.cpp:846, aim.cpp:546
  and the new module.

Why each part:
- the ring keeps short pulls and their order (a trigger sampled per Draw would miss a 20 ms pull at low game frame rates);
- the ray in the event makes the shot go where the pistol pointed at the pull;
- the view-seqlock fields feed the drawing, the per-view trace and auto fire;
- the grip point is the player's fit, which only the host reads.

### 5.2 Host (`OffHandPistol`, new `src/host/offpistol.hpp/.cpp`; PistolHold=grip)

**Active** = the toggle && caps bit0 && bit1 && the game alive (`pistolSeq` advanced within 250 ms) && the gun hand valid
&& the off hand tracked && its grip action active && `gestures` && `hasView` && no grenade held. **Applies** (the press is
ours) = the toggle && alive && a gun in the other hand.

| From | Input | Condition | To | Sent | Pulse |
|---|---|---|---|---|---|
| None | off grip press in a pistol zone | Active, `pistolKey` set, (caps bit5 -> PistolPair) | Held (grip consumed; latch set) | DRAW | 30 ms 0.5 |
| None | same | Applies, not Active (or bit5 without PistolPair) | None (consumed) | -- | 60 ms 0.2 |
| None | same | not Applies | today's SwitchPistol | -- | today's |
| Held | trigger press edge, past the latch | not frozen | Held | SHOT(offAimRay now) | on the game's `pistolShots` / `pistolDry` |
| Held | trigger above 0.6 / below 0.4 | not frozen | (flag bit3) | -- | -- |
| Held | grip release | not frozen | None | HOLSTER | 20 ms 0.2 |
| Held | a menu / the grip asleep / the off hand untracked | -- | frozen (no shots) | -- | -- |
| frozen | unfrozen | grip held | Held (latch: the trigger must rest) | -- | -- |
| frozen | unfrozen | grip released | None | HOLSTER | 20 ms 0.2 |
| Held | caps bit4 clear for 2 polls | not frozen | None | HOLSTER | 60 ms 0.2 |
| Held | the game's state None for 2 polls, all acked | -- | None | -- | 60 ms 0.2 |
| any | `pistolPawnSeq` changed | -- | None | -- | -- |

- **Masks while Held:** the off grip consumed; `maskTrigger[o]` (kept until let go after the hold); Xbox B when caps
  bit6 (`PistolKeep`, and `TakeNextWeapon` ignored); the gun hand's pistol zone refused.
- **`offBusy`** (grenade or pistol held) hides the foregrip ring and pulse and the reload's rings; the Gun hand change is
  deferred.
- **`toggle` deltas:** DRAW on a press in a pistol zone; HOLSTER on a press in any holster zone; a release does nothing;
  other off-grip presses are consumed.
- **Per frame, in `Hands::Update`** after the main gun's pose and aim line (hands.cpp:383-392): compute `offGun` and
  `offAimRay` from the off aim pose and `Menu::FitFor(pistolKey)`, with `rayRight` negated for a left off hand. Then
  `pistol_->Frame(...)`. Poll before `hands.Update`, Send after (the grenade's order).

### 5.3 Game (`src/mohavr/offpistol.cpp`, per pawn)

| Event / tick | Accepted when | Effect | Log |
|---|---|---|---|
| DRAW | None; available; P found (not `pawn.Weapon` / `PendingWeapon`, unless a twin); P.Instigator == pawn | the refill if due; upgrades if behind (2.5, magnum rule); carrier attach; Held | `offpistol: DRAW MOHAColt45_0 -- clip 7/7, upgrade 2, drawn in the hand` |
| SHOT(ray) | Held; gate (2.2 step 0) | 2.2 | `offpistol: shot 3 -- ... 0 cm from the off dot; clip 4/7; the gun MOHABar_1 unchanged` |
| (tick) auto | Held, auto, flag bit3, rate due | 2.2 along the current off line | (Debug.OffPistolTrace) |
| HOLSTER | Held | detach; refill timer (or instant); None | `offpistol: HOLSTER -- clip 4/7, refilled in 1.5 s` |
| (tick) | Held and (holdOk false 2 Draws, or P gone, or P became pawn.Weapon / PendingWeapon) | detach; None; refusal 1 / 7 / 6 | `offpistol: the hold ended -- <why>` |
| (tick) | not held, refill due | `AmmoCount[0] = MaxAmmoCount[0]` (direct) | `offpistol: refilled 2 -> 7` |
| new pawn | -- | None, carrier dropped, `pistolPawnSeq`++ | |
| anything else | -- | refused, acked | |

- **Availability:** `offhand::Available` factored for both features, with its 4 Hz script part shared. For the pistol a
  grenade main weapon is allowed and the HellBox is not; `holdOk` is false when the pending or active weapon is P.
- **Rule 7:** switched off with nothing held, no script calls.

### 5.4 Ordering, acks, reconcile

The ring rules are the reload's and the grenade's (RELOAD-DESIGN 4; host offhand.cpp:327-350):
- the host never writes past 8 unacknowledged events and drops one held for 250 ms;
- the game acks every event, applied or refused, and skips to the newest on an overrun;
- the host takes the game's state after two disagreeing polls with nothing in flight.

---------------------------------------------------------------------------------------------------------------------

## 6. Ini switches (`[OffHand]`, beside D40's keys; each off / safe until proven, standing rule 7)

| Key | Default | Meaning | Side |
|---|---|---|---|
| `Pistol` | **0** until Phase 1 passes [S]; 1 for its headset round | the off-hand pistol (the menu's Weapons tab "Off-hand pistol", saved in the player's ini) | host + game |
| `PistolHold` | `grip` | `grip` / `toggle` | host |
| `PistolSpot` | `holster` | `holster` (any SwitchPistol zone) / `chest` (Phase 3: `[Holsters] OffPistolSpot`) | host |
| `PistolRefill` | `game` | `game` (the pistol's ReloadInterval in the holster) / `instant` / `auto` (a dry press reloads) / `none` (never needs one) | game |
| `PistolPouch` | 1 (Phase 2) | the pistol brought to the pouch swaps its magazine | host + game |
| `PistolCredit` | `main` until S1b/R8 pass, then `pistol` | who earns the off-hand pistol's hits and kills | game |
| `PistolUpgrades` | 1 | apply the pistol's upgrades at the draw (+ the magnum-mix rule) | game |
| `PistolDot` | 1 | the second dot (with the menu's Red dot) | host |
| `PistolKeep` | 1 | Xbox B (switch weapon) can't take the held pistol | host (+ caps bit6) |
| `PistolSounds` | 1 | the report and the dry click at the off muzzle | game |
| `PistolFlash`, `PistolBrass` | 1 (Phase 2, after S3) | the flash / brass at the off muzzle | game |
| `PistolSlide` | 1 (Phase 2) | the hammer, slide, lock-back and top round drawn | game |
| `PistolKick` | 0.0 (Phase 2; [H] 0..1) | the drawn hand kick replayed from the game's fire animation | game |
| `PistolPair` | 0 (Phase 3) | with the pistol in the gun hand, the off hand takes its twin | host + game |
| `Haptics` | 1 (shared) | 3.7 | host |
| `[Debug] OffPistolTrace` | 0 | per-event / per-shot / availability logs (tests) | game |

Requirements, not switches: the pistol is drawn only with the arm bake (`ArmIK=1`, `ViewModel=2`). Without it, shots
still work but nothing is drawn, and it is logged.

---------------------------------------------------------------------------------------------------------------------

## 7. The minimal spikes (implement first; each time-boxed, rule 9)

Deploy for all: `Render.ResX=0`, `Render.ResY=0`, the x64 simulator json, `Debug.GameCommands=1`. Use a long gun in hand
(the harness checkpoint's loadout, or `GiveWeapon` + `SwitchPrimary` as nade0 did). Wait for the landing to end (the
game side's "available" line, as nade4). Copy `MOHAVR.log` before any relaunch (rule 11).

### 7.1 S1 [S]: the off-hand shot (2 h)

**Build:**
- `src/mohavr/offpistol.cpp/.hpp`, test commands only, in `RunTestCommands`;
- aim.cpp `BeginOffShot()/EndOffShot()` (the stand-down, labelled "off-hand shot N" log lines);
- the call helper used from offhand.cpp (or factored);
- no host change, no shared-block change, **no new address**.

**Commands (`python tools/game_cmd.py mohavr pistol ...`):**

| Command | What it does |
|---|---|
| `mohavr pistol` | dump: P (class, owner, `pawn.Weapon`?); vtable, +0xF0, +0x344; Instigator; CurrentFireMode; AmmoCount[0] / MaxAmmoCount[0]; bInfiniteAmmo; CurrentUpgradeLevel vs `mgr.GetAppliedUpgradeLevel(type)`; RefireCheckTime; FiringStatesArray[0]; InstantHitDamage[0]; WeaponRange; ForceFalloffDistanceStart / Max; ReloadInterval[0]. Also each resolved function's flags, native index, ParmsSize and parameter offsets (CalcWeaponFireNative, ProcessInstantHit, SpawnGunshotStimulus, SpawnImpactStimulus, NoiseRadius, OnWeaponFire, PlaySoundAt, upgrade, GetAppliedUpgradeLevel) and the report cue found. |
| `mohavr pistol fire <eye\|hand> [main]` | the 2.2 sequence from the eye along the view, or from the off controller's aim pose (un-mirrored); `main` = no credit swap |
| `mohavr pistol upgrade` | 2.5 |
| `mohavr pistol enemy` | lists `WorldInfo.PawnList`'s MOHAAIPawns: class, team (controller TeamIndex), distance, Health, line of sight from the eye |
| `mohavr pistol fire enemy [head]` | aims the off-hand test shot at the nearest enemy's Spine (or Neck) bone within 50 m |
| `mohavr pistol loop 100` | 100 eye shots in one Draw |

**Pass:**
1. **Resolve.**
   - P is a MOHAColt45 or MOHAMauser and is not `pawn.Weapon`.
   - Its vtable+0xF0 = 0x10DB1FA0 and +0x344 = 0x10F0DF60; Instigator = the pawn.
   - CalcWeaponFireNative: native index 0, ParmsSize <= 0x80, the offsets logged (expect TraceOwner +0, StartTrace +4,
     EndTrace +0x10, vTraceExtents +0x1C, ImpactList +0x28, ReturnValue +0x34).
   - ProcessInstantHit: 0x44 (FiringMode +0, Impact +4).
   - OnWeaponFire resolved on the stats component through UObject::ProcessEvent.
2. **`fire eye` at a wall.**
   - Trace: `ImpactList` num >= 1 and data != 0; `ReturnValue.HitActor` = the wall, HitLocation within 1 unit of
     `aim::WorldTrace` on the same segment.
   - Stand-down: **no `shot N:` line** from aim.cpp.
   - Impacts: G's attachment's `NextImpactSoundIndex` (+0x228) advanced by 1.
   - Ammo: P.AmmoCount 7 -> 6.
   - Isolation: G's class, state, AmmoCount, `pawn.FlashCount`, `ShotCount`, `PendingFire[0..1]` and `pawn.Weapon` the
     same before and after.
   - Then a main-gun shot: `shot N: from the gun ... 0 cm from the red dot's point`.
3. **`fire hand`** with the off hand 25 deg left of the gun hand: the hit is within 2 cm of the off line's own trace
   point. Then **the capture case**: point both hands at one spot, fire the gun (RT), and within 100 ms fire `fire hand`
   (the off line within 18 deg of the armed aim). The off shot must still land on the off line, and the main shot on the
   main dot.
4. **`loop 100`:** the ImpactList data pointer is reused (Max stays small), with no fault and no stall.
5. **`upgrade`** on a pistol never drawn this level:
   - before: CurrentUpgradeLevel -1 and base values (Colt 50 / 0.25 / 7);
   - after: the applied level's values (the player's save: 2 / 100 / 0.06 / 7; C96 20 rounds / WeaponBurstFire);
   - `MagnumRoundsMix.IsActive` 1 -> 0 after the mod's rule (BAR in hand);
   - G's attachment's `WeaponFireAudio.SoundCue` unchanged.
6. **Sound:** the report cue resolved by name. A WorldInfo AudioComponent with it has wave instances > 0 within 1 s.
   Optional x32dbg: no hit at 0x1119863A.

**S1b (damage and credit; needs a live enemy within 50 m in line of sight):**

7. **Damage:** `fire enemy` drops its Health by InstantHitDamage[0] x the bone factor (Spine 1.0: 50 / 100 magnum).
   `fire enemy head` kills it. The death and ragdoll look normal (a capture) and nothing faults (TakeDamage on an AI from
   the pre-draw: native-fire R5).
8. **Credit:**
   - with the swap: the pistol's `MOHAWeaponExperienceComponent` gains 1 (body) / 2 (head), G's is unchanged, and
     `pawn.Weapon` is G again after the call;
   - with `main`: G's component gains instead.
9. **No enemy reachable in the harness checkpoint:**
   - S1 still passes on 1-6, and S1b moves to Phase 1's tests with a longer wait or to the first headset log;
   - a controller-less `Summon`ed pawn is **not** a valid target (finding 6);
   - a hit on a Trigger in the way (R10: `SeqEvent_TakeDamage`) proves the TakeDamage dispatch alone.

**If it fails:** walk 2.7's ladder (F1, F2, F3). Record the result in a new ENGINE-NOTES section (5be) either way, with
5af un-parked for B4.

### 7.2 S2 [S]: the pistol in the off hand (2 h; parallel to S1)

**Build:**
- `mohavr pistol carrier <on|off>`;
- the bake branch (BakeCarrier generalised over a frame source) with `Gw_off = M_left x F_off x W`, taking `F_off` from
  the raw off frame (no host yet; fit angle 0 = the shipped pistol fits);
- the viewmodel pointer skip; `g_parts` 8, `kBakeSlots` 8;
- the C96 collapse;
- the `offgun` grip rows (reload_grips.py part = identity) as a third grip source;
- the pointer published before AttachComponent.

**Pass:**
1. **The clone.**
   - Class MOHASkeletalMeshComponent, mesh `US_M1911A1_Pistol_Rigged` (or `DE_Mauser_Rigged`), Outer P.
   - PhysicsAsset (+0x210) 0 after the clear, PhysicsAssetInstance (+0x214) 0 after the attach, bAttached 1, FOV 65.
   - Never an `armik: baked the move into <clone>` (gun-path) line.
   - Bakes per Draw equal to the arms' (2 standing).
2. **Placement log:**
   - the mesh origin and `tag_barrell` in the off controller frame about (-4.4, -0.4, -1.6) and (15.2, -0.44, 5.8)
     units with the shipped Colt fit;
   - the drawn barrel <= 0.5 deg off the mirrored aim line;
   - a `sim_shot` of the Colt at `hand=r,...` and the clone at the mirrored `hand=l,...` matches when flipped (the r26
     method).
3. **The hand:**
   - the left hand bone about (-14.94, -2.13, -3.09) in the off controller frame;
   - the fingers closed on the grip (capture);
   - `lastTwist[1]` within 5 deg of the free hand's at the same pose.
4. **CatchUp:** strafing and running (pad stick): the logged hand-to-grip distance < 0.5 cm.
5. **A main weapon switch while attached:** no `weapon in hand: ''` line, no parachute path, the fit and key unchanged.
6. **Left-hand mode:** the clone in the right hand, drawn solid (determinant), captured.
7. **Lifetime:** 20 attach / detach cycles, then `Suicide` (a new pawn): no fault, pointers dropped. The C96: clip never
   visible, stock / box magazine matching the main C96 at `mohavr upgradelevel 19 -1|0|1|2`.

### 7.3 S3 [S] (optional, before Phase 2): the flash component (1 h)

`mohavr pistol flash`:
- `InitHitspangPSC` on the gun's attachment; re-parent to the pawn; the pistol's flash template; DPG 2; `ActivateSystem`
  at the computed off muzzle.
- **Pass:** LocalToWorld at the point; bounds grow and LastRenderTime advances (the `Debug.MuzzleFreeze` trace pattern);
  visible in a SloMo 0.05 capture.
- **Survival:** it survives a main weapon switch and a forced GC (`obj garbage` through Debug.GameCommands): still
  attached, Owner the pawn, still rendering. It is re-made on a new pawn.

---------------------------------------------------------------------------------------------------------------------

## 8. Phased plan and effort (one commit per phase; STATUS / ENGINE-NOTES / DECISIONS in the same commit, rule 10)

| # | Step | Tag | Main files | Proof | Effort |
|---|---|---|---|---|---|
| 0a | Spike S1 (7.1) | [S] | `src/mohavr/offpistol.cpp/.hpp` (new), `aim.cpp/.hpp` (the scope), `vr_view.cpp` | 7.1 pass lines; ENGINE-NOTES 5be (measured), 5af (B4 un-parked) | 2 h |
| 0b | Spike S2 (7.2) | [S] | `offpistol.cpp`, `arms_ik.cpp`, `viewmodel.cpp`, `tools/reload_grips.py` -> `reload_grips.inc` | 7.2 logs and captures | 2 h |
| 1 | **Phase 1**: contract v21, host `OffHandPistol`, the press order and second aim line in `Hands`, the second reticle, the menu toggle, pad masks / targets (`@pistol`, `pistol=draw\|shot\|holster`), the executor (DRAW / SHOT / HOLSTER / refill / auto / upgrades / credit), the report and dry click, haptics, the carrier with the grip and the C96 collapse, `script_call.cpp` factored, `offhand::Available` factored, the grenade carrier's W fix | [S] | `shared_frame.hpp`, `src/host/offpistol.*` (new), `hands.*`, `main.cpp`, `menu.*`, `reticle.*`, `pad.*`, `src/mohavr/offpistol.*`, `script_call.*` (new), `offhand.*`, `aim.*`, `viewmodel.*`, `arms_ik.cpp`, `config.*`, `config/MOHAVR.ini`, `tools/pad_cmd.py` / `game_cmd.py` docstrings | tests P1-P11 (below); `harness.ps1 cycle` OK with Pistol=0 and 1; two adversarial reviews; D41, ENGINE-NOTES 5be, STATUS, HEADSET-TESTS round 44, ROADMAP M8b | 6-8 h |
| 2 | **Phase 2**: hammer / slide / bolt / lock-back / top round, the flash and brass (S3 first), the kick, the pouch reload, the off-hand fit page if asked | [S] then [H] | `offpistol.cpp`, `arms_ik.cpp`, `muzzle.cpp` (untouched, check only), host `reload.cpp`/`hands.cpp` (the pouch), `menu.cpp` | SloMo captures; pouch tests | 4-6 h |
| 3 | Headset round(s) with `Pistol=1` | [H] | `HEADSET-TESTS.md` | the player's verdict (11) | -- |
| 4 | **Phase 3** options: `PistolPair` (the same-object twin), `PistolHold=toggle` polish, `PistolSpot=chest`, the C96's loop / report cues, a host ammo counter if asked | [S] then [H] | per option | per option | 3-4 h |

**[S] tests for Phase 1** (the nade4.ps1 pattern; raw pad states queue, a hand line applies at once):

| Test | Steps | Expected |
|---|---|---|
| P1 draw / holster | `hand=l,@pistol`, grip held, let go | DRAW then HOLSTER; no `SwitchPistol`; no weapon-in-hand change; the gun hand stays right |
| P2 shoot | 3 pulls | three `off-hand shot N ... 0 cm from the off dot` lines; clip 7 -> 4; the pulses |
| P3 two guns, one target | `lt=1 rt=1` together; then the guns 5 deg apart | each shot 0 cm from its own dot (the capture case) |
| P4 rate | a level-0 Colt (the 5au method), pulls 0.12 s apart | every other pull refused "too soon"; the save's level-2 Colt fires all |
| P5 empty and refill | 7 shots, then an 8th pull; let go, re-draw after 0.5 s, then after 1.6 s | the 8th clicks; 0 after 0.5 s, 7 after 1.6 s |
| P6 conflicts | foregrip; B; the pouch; the grenade holster; the gun hand at `@pistol`; X; Y from the secondary | foregrip refused; B drops the main magazine; the pouch refused; the grenade holster refused; the gun hand refused; X switches to a grenade with the pistol kept; Y masked, or with `PistolKeep=0` "taken by a switch" |
| P7 the main pistol | with the pistol in the gun hand | refused without PistolPair; Colt + C96 after Give all: both fire |
| P8 freeze | a menu, `lost=l`, let go during the freeze | no shots; let go during it -> HOLSTER at the unfreeze |
| P9 left-hand mode | the right hand draws at RightHip and shoots | captures |
| P10 death / save | Suicide while held; a checkpoint save / load while held | the hold ends untouched; no stale carrier |
| P11 off | `Pistol=0` | today's holster behaviour; the cycle passes |

**Total:** spikes about 4 h, Phase 1 6-8 h, Phase 2 4-6 h, Phase 3 3-4 h: **about 17-22 h of agent time over 2-3
sessions**. Calibration: the off-hand grenade went from its research workflow (commit c603644, 07:40) to Phase 1 shipped
with two reviews (518f9f3, 11:51) in about 4 h. Phase 1 here adds the grip, a second dot, the aim scope and the refill.

---------------------------------------------------------------------------------------------------------------------

## 9. Risks and mitigations

| Risk | Likelihood / impact | Mitigation |
|---|---|---|
| The out-array call (the helper's first HasOutParms call) misbehaves | low (statically verified, 1.1) / the shot fails | S1 pass 2 and 4; F1 (script `CalcWeaponFire`; ReturnValue only) |
| The engine reallocs / frees the reused ImpactList buffer unexpectedly | low / a fault | only engine-allocated data is ever passed; SEH; on a fault the header is abandoned (bounded leak) |
| The bullet hook captures an off-hand trace (or a stale arming captures it) | certain without the scope / the shot goes to the main dot | the stand-down scope (BeginOffShot/EndOffShot also clears `armed` / `followOn`); P3 |
| The swap leaks: something inside step 8 keeps or changes `pawn.Weapon` | low / the wrong weapon in hand, or credit to the wrong gun | the guard (2.4 step 9); a log on any change; isolation checks per shot under Debug; `PistolCredit=main` |
| The player dies inside step 8 | very rare / the pistol tossed instead of the gun, then a checkpoint reload | the guard; accepted |
| The magnum mix left on with the BAR | certain without the rule / the BAR may sound different | finding 1's rule after every mod `upgrade()` and after the off pistol's level-up sequence; [R] audible |
| A level-up's `UpdateMeshVisibility` on a stale `WeaponSkeletalMesh` | low (the flat game does it for grenades) / a fault | R8; if a fault appears, null P's `WeaponSkeletalMesh` while it is holstered (the next equip sets it again, EALAWeapon.uc:930-940) |
| The pistol's sound bank isn't loaded while holstered (GunVR's lesson) | low-medium / silent shots | R1 in S1; fallback silent + haptics; [H] |
| The report cue can't be found by name | low / silent | finding 3's two lookups; logged |
| The never-drawn pistol at base stats | certain without 2.5 / weak pistol | `upgrade()` at the draw; S1 pass 5 |
| The carrier taken for the gun / "no gun" mid-switch | certain without the fixes / the rig on the parachute path, reload state on the wrong part | the bake branch first, the viewmodel skip, `g_parts` 8, the pointer before attach (S2 pass 1, 5) |
| The grenade carrier's missing W (an existing cosmetic bug) | certain with CatchUp / 3-5 cm trail at a walk | include W in both frames (Phase 1) |
| Two shots in one frame share the spang component | cosmetic | accepted (or effects 2.2's split later) |
| The HUD shows only the main gun's ammo | certain / the player can't see the pistol's count | the slide lock-back (Phase 2), the dry click (Phase 1); a host counter if asked [H] |
| An off-hand-emptied pistol drawn into the gun hand within the refill delay | rare / D21's state vs the game's equip refill | the game's `WeaponEquipping` refill + D21's "rose without the mod" reconcile (reload.cpp:2162-2183); P5 variant |
| Same-class pair: the main pistol mid-melee or mid-reload | rare / wrong damage index | CurrentFireMode saved / restored synchronously; zero extents |
| A checkpoint save while held serialises the clone in `FPArms.Attachments` | unknown (the grenade's open risk) / a stale part after load | P10; detached outside holds |
| No enemy in the harness checkpoint for S1b | possible / credit and damage unproven in [S] | S1b's alternatives (7.1 item 9); `PistolCredit=main` until proven |
| Balance: two guns at once too strong | [H] | physical costs (4.6); `PistolSpread` / a damage scale only if asked |
| Version skew of the shared block | low | the host refuses a mismatch; build.ps1 builds both |

---------------------------------------------------------------------------------------------------------------------

## 10. Load-bearing claims (numbered, each checkable)

1. `UObject::ProcessEvent` (0x109CE980) refuses only: no Native/Defined flag, a probe-masked name, pending kill,
   **iNative != 0**, a remote native. It does not check weapon state. [V decompile; OFFHAND-DESIGN 1.1]
2. Through ProcessEvent, out parameters are written straight into the caller's parms block (PropAddr = Parms +
   Property.Offset, 0x109CEAE3-0x109CEAEE), the return value at Parms + ReturnValueOffset (0x109CEB38-0x109CEB4F), and
   out parameters are neither copied back nor destroyed afterwards (0x109CEB51-0x109CEBB5). [V]
3. `CalcWeaponFireNative`, `ApplyDamage`, `NoiseRadius`, `OnWeaponFire`, `SoundRiotMixSelect.IsActive/DeActivate` and
   `Object.FindObject` have no native index. [V decl: EALAWeapon.uc:130, EALASmallArms.uc:233, MOHAPawn.uc:149,
   MOHAPlayerStatsComponent.uc:92, SoundRiotMixSelect.uc:22-37, Object.uc:1435; V-agent package tails]
4. `AEALASmallArms::ApplyDamage` 0x10F0DF60:
   - reads Instigator +0x98 unchecked and measures the falloff from its Location;
   - does no damage at or past ForceFalloffDistanceMax +0x338;
   - indexes the damage by CurrentFireMode +0x22C and momentum / type by FiringMode;
   - uses Instigator.Controller +0x1EC as the instigator and calls the TakeDamage helper 0x10D30DB0;
   - never reads Pawn.Weapon or the weapon's state. [V disassembly]
5. A Colt / C96 object carries the AEALASmallArms vtable 0x115880C0: +0xF0 = 0x10DB1FA0, +0x16C = 0x10F0CCF0, +0x314 =
   0x10F0CDD0, +0x344 = 0x10F0DF60. [V memory + class headers; the live object R in S1 pass 1]
6. A weapon that isn't `Instigator.Weapon` (+0x3A8) is in stasis (0x10F0CCF0 / 0x10F0CD20): no Tick, no timers. [V]
7. `EALASmallArms.ProcessInstantHit` = `ApplyDamage` + the **current** attachment's impact effects, except on Triggers.
   [V EALASmallArms.uc:167-230, EALAWeapon.uc:214-227]
8. `PerformWeaponTrace` / `InstantFire` reach `Pawn.Weapon.PlayFireEffects` through `SetFlashLocation`, and
   `FireAmmunition` fires the current attachment's `WeaponFired`. So neither may be called on the holstered pistol. [V
   EALAWeapon.uc:357-389; Weapon.uc:594-601; Pawn.uc:447-463, 504-513; V-agent EALASmallArms.uc:296-321]
9. `MOHAPlayerPawn.GetAdjustedAimFor` returns `GetBaseAimRotation()` whatever the weapon. [V MOHAPlayerPawn.uc:3413-3417]
10. The mod's GetBaseAimRotation hook arms `g_shot` on any call for the local pawn. Its bullet hook rewrites any armed
    (<= 100 ms) pawn-sourced, zero-extent trace within 18.2 deg of the armed aim onto the main gun's ray. [V
    aim.cpp:267-296, 339-379]
11. Bullet hit and kill credit read `<killer>.Pawn.Weapon.WeaponType` at the victim, and `GiveWeaponExperience` routes
    points by that type. [V MOHAAIPawn.uc:1272; MOHAAIController.uc:390, 394; MOHAPlayerStatsComponent.uc:178-198]
12. Player -> NPC damage adjustment is weapon-agnostic. [V MOHADamageType.uc:104-145]
13. A pistol's upgrades are applied only at an equip, and `upgrade(type, level, W)` upgrades W from its CurrentUpgradeLevel
    and spares an attachment of another class. [V EALAWeapon.uc:548-594, 1714; MOHAWeaponUpgradeManager.uc:120-160;
    MOHAWeaponUpgrade.uc:35-45]
14. The Colt's magnum upgrade activates the shared Magnum_Fader mix, which the game deactivates only at put-down / death.
    [V NN/MOHAUpgradeColt45_2.uc:15; EALAWeapon.uc:697-726, 1442-1444, 1710]
15. The C96's 712 kit fires at RefireCheckTime 0.06 s in WeaponBurstFire (about 16.7 rounds/s). [V
    NN/MOHAUpgradeMauser_2.uc:16-24; DW:2985-2989; EALASmallArms.uc:282-294]
16. An inventory weapon's Instigator is the pawn. [V Inventory.uc:102-107]
17. A player death inside a call runs `HolderDied` / `TossWeapon` on whatever `Pawn.Weapon` is then. [V
    Pawn.uc:1885-1897]
18. The game supports leveling up a weapon not in hand; the sequence refills it with `SetAmmoCount`. [V
    MOHAWeaponUpgradeManager.uc:208-226, 244-345; EALAWeapon.uc:728-735]
19. The call helper requires iNative 0 and ParmsSize <= 256 and accepts actor or component ProcessEvent. [V
    offhand.cpp:98-144]
20. The stats component dispatches through UObject::ProcessEvent (vtable 0x1158E8F8 +0xF0 = 0x109CE980). [V]
21. The executor runs in Hook_Draw's pre-draw on the game thread, after the world tick in which the gun's own shots
    complete. [V vr_view.cpp:499-511; V-agent native-engine 5]
22. The pistol's `DroppedPickupMesh` is the first-person skeletal mesh, and a clone of a pickup mesh can be attached to
    the arms and baked at the off hand (the grenade's S2). [V-agent draw-hold 2.1; V-agent ENGINE-NOTES 5bb at runtime]
23. viewmodel's part list has 4 slots and skips only the grenade carrier. The arm bake gives any unclaimed FOV part to
    the reload as the gun. [V viewmodel.cpp:81, 377; arms_ik.cpp:756-759, 804-806]
24. The grenade carrier's frame lacks the body carry W that the hand's IK target uses. [V offhand.cpp:903-913;
    arms_ik.cpp:399-401, 797]
25. The mirrored per-gun fit puts the off pistol's bore within 0.3 units of the mirrored aim line (shipped Colt fit).
    [V-agent draw-hold 4.2; R S2 pass 2]
26. The game's pistol grip is rigid in `colt45_idle` / `mauser_idle` (0.00 units, <= 0.06 deg) and mirrors to a left-hand
    grip by reload_grips.py's MIRRORED rule. [V-agent draw-hold 5]
27. The pistol holster is RightHip=SwitchPistol, and today any hand there draws the pistol as the main weapon into that
    hand. [V config/MOHAVR.ini:312, 318; hands.cpp:272-283]
28. The off grip and trigger reach no game control while a gun is held (LB / LT none), and the host already masks a
    hand's trigger and RB. [V MOHAVR.ini:241, 243; main.cpp:791-794]
29. A second reticle fits the composition-layer budget. [V main.cpp:965, 1036-1060]
30. Both pistols have infinite reserves, so only the clip matters. [V-agent: class defaults; the mod's log "reserve 9999"]

---------------------------------------------------------------------------------------------------------------------

## 11. Open questions

**For the player [H]** (round 44; ideally after round 43's grenade answers, which decide the shared defaults):
1. A long gun in the gun hand and the pistol in the off hand: is that what you meant? And do you also want two pistols
   (the same Colt twinned, GunVR's pair grab)?
2. Draw from the right hip across the body (today's pistol holster), or move it: a chest holster, or the left hip with
   the grenades moved to the chest (GunVR's layout)?
3. Keep squeezing to hold it (let go = back in the holster), or click to draw and click at a holster to put it away (your
   GunVR habit)? Did it ever go back when you didn't mean it?
4. Reloading: in the holster it refills after the Colt's 1.5 s. Instant instead? Want the belt pouch too?
5. A second dot for the pistol (white): helpful or clutter?
6. Off-hand kills level up the pistol (default once proven) or the gun in hand (the game's literal rule)? Your save
   already has every upgrade.
7. Firing both guns at once: too strong? A buzz with each shot (both hands)?
8. The long gun one-handed while the pistol is out, and no foregrip, off-hand grenade or reload until it's put back: OK?
9. The switch-weapon button kept from taking the held pistol: right?
10. Left-hand mode: is the mirror-image pistol in the right hand noticeable?
11. Phase 2's look: the hand kick (the game's pistol flips 22 deg), which brass spot, does the flash read at arm's length?

**Runtime [R]** (answered by S1-S3 and the Phase 1 tests):
- **R1** the bank is loaded while holstered (3.5);
- **R2** two voices of one cue (the same-object pair);
- **R3** the swap's credit;
- **R4** isolation per shot;
- **R5** CurrentUpgradeLevel at level start;
- **R6** `ForceFalloffDistanceStart/Max` live (DW has two pairs per pistol);
- **R7** impacts with no attachment mid-switch;
- **R8** a pistol level-up from off-hand kills with the BAR in hand (the sequence, the magnum rule, no fault);
- **R9** whether the harness checkpoint has an enemy in reach;
- **R10** `SeqEvent_TakeDamage` on a trigger crossed by an off-hand shot;
- `GetBaseAimRotation` calls per second outside shots (sizes the stale-arming risk the scope removes);
- the C96 upgrade parts' bone names.

---------------------------------------------------------------------------------------------------------------------

## Appendix A. Addresses used (none new; nothing pinned by this design)

| What | Address | Status |
|---|---|---|
| AActor::ProcessEvent / UObject::ProcessEvent | 0x10DB1FA0 / 0x109CE980 | pinned (addresses.hpp) |
| bullet trace call / after (aim.cpp's MidHooks) | 0x10F0CE93 / 0x10F0CE98 | pinned (the scope is a flag in the existing hooks) |
| AEALASmallArms vtable, its +0x344 | 0x115880C0, 0x10F0DF60 | read for checks only (S1 dump) |
| SoundRiot missing-bank path (R1, debugger only) | 0x1119863A | not used by code |

Everything else is reached by reflection (property and function names) and the two pinned ProcessEvents. The
GMalloc global (0x116CBF6C) is deliberately not needed (finding 4).

## Appendix B. File-by-file change list (all phases)

| File | Change |
|---|---|
| `src/common/shared_frame.hpp` | v21 (5.1), asserts, `kVersion` 21, `PistolEvent`, `PistolStatus`/`ReadPistolStatus`, `PistolView` in `ReadHands` |
| `src/host/offpistol.hpp/.cpp` (new) | `OffHandPistol`: Init, SetOn, Poll (reconcile), TakePress, Frame (5.2: edges, latch, freeze, masks, pulses), Send (ring), Flags, the view-block values |
| `src/host/hands.hpp/.cpp` | `SetOffPistol`; the off gun pose and aim line from the pistol's fit; the press order (a pistol zone after the grenade zone; the gun hand refused while held; `offBusy`); `@pistol` target |
| `src/host/reticle.hpp/.cpp` | a colour on `Init` (a second instance) |
| `src/host/main.cpp` | own `OffHandPistol`; Poll / Send; the view-seqlock fields; the second dot; the trigger and Xbox B masks; `TakeNextWeapon` ignored while held with bit6 |
| `src/host/menu.hpp/.cpp` | Weapons tab "Off-hand pistol"; public `FitFor(key)`; Phase 2 the off-hand fit page if asked |
| `src/host/pad.hpp/.cpp` | `hand=l,@pistol`; `pistol=draw\|shot\|holster` direct events; targets[8] -> [9] |
| `src/mohavr/offpistol.hpp/.cpp` (new) | the executor (2, 5.3), the carrier and its frame / grip / parts, test commands (7) |
| `src/mohavr/script_call.hpp/.cpp` (new) | the call helper factored out of offhand.cpp / reload.cpp |
| `src/mohavr/offhand.hpp/.cpp` | `Available` factored; the carrier frame with W |
| `src/mohavr/aim.hpp/.cpp` | `BeginOffShot/EndOffShot`; the off line's per-view trace (`TraceAimLine` factored); `pistolAimDistance` |
| `src/mohavr/viewmodel.hpp/.cpp` | `PistolView` with the hands; the off gun frame and ray; the clone skip; `g_parts[8]` |
| `src/mohavr/arms_ik.cpp` | the pistol branch (BakeCarrier over a frame source, parts posed); the third grip source with the trigger finger; `kBakeSlots` 8 |
| `src/mohavr/reload.hpp/.cpp` | `PlayCueAt(weapon, cue, location)` (or via script_call); a cue-by-path lookup |
| `src/mohavr/vr_view.cpp` | `offpistol::OnDraw(hdr)` after `offhand::OnDraw`; test commands; Configure |
| `src/mohavr/config.hpp/.cpp`, `config/MOHAVR.ini` | the `[OffHand] Pistol*` keys, `[Debug] OffPistolTrace` |
| `tools/reload_grips.py`, `src/mohavr/reload_grips.inc` | `OFFGUNS`: the Colt's / C96's idle right hand MIRRORED with part = identity (`offgun`, `offgun_pull`), and `O_idle` per key |
| `tools/pad_cmd.py`, `tools/game_cmd.py` | docstrings |
| `work/research/tests/pistol0..N.ps1` | the tests |
| docs | DECISIONS D41; ENGINE-NOTES 5be (+ 5af: B4 un-parked); STATUS; HEADSET-TESTS round 44; ROADMAP M8b; this design promoted to `OFFPISTOL-DESIGN.md` |
