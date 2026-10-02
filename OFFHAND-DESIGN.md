# Off-hand grenade ("dual wield", step 1): the design

Design 2026-10-02, written by a research workflow (six research agents, a designer, three adversarial verifiers); its two
spikes were then run in the simulator the same day (section 0.1) and the verifiers' corrections are collected in section
0.2. The six research notes are in `work/research/dualwield/` (local, untracked: `script-flow.md`,
`projectile-lifecycle.md`, `native-engine.md`, `mod-integration.md`, `draw-in-hand.md`, `vr-prior-art.md`); this design
was checked against the scripts, the unpacked exe and the mod's source at HEAD e2b709d. The checks I added myself are listed in section 1.1 (capstone on `work/MOHA.exe.unpacked.exe`,
image base 0x10900000; scratch scripts in the session scratchpad `design/`).

The player's request (verbatim): "investigate the possibility of a dual wield mode. Being able to grab a grenade with the
off hand and throw it without unequipping your gun would be very immersive."

Tags: **[V]** verified (script text, bytes, mod source; evidence given), **[V-agent]** verified by a research agent and
not contradicted by anything I read, **[I]** inference, **[R]** needs a runtime check (named), **[S]/[H]** acceptance in
the simulator / in the headset (standing rule 8).

---------------------------------------------------------------------------------------------------------------------

## 0. Verdict and the design in one screen

**Feasible, with no engine patch and no new hook.** The gun stays the pawn's active weapon; the mod makes the *inactive*
grenade weapon (the frag / Gammon / stick object in the pawn's `MOHAInventoryManager`) launch its projectile by calling
its own script function **`EALAWeapon.SpawnProjectile(vStartPos, vForward)`** through the `AActor::ProcessEvent` path the
mod already uses for the reload sounds, then finishes what the game's `EALAGrenade.ProjectileFire` would do (fuse, cooked
damage type, draw scale, light, velocity) with a few more script calls and property writes. Every step is statically
verified; what remains is runtime-only and is exactly what **spike S1** (section 9.1) measures in about two hours.

- **Why not the game's own throw path:** `ProjectileFire` starts with `IncrementFlashCount()`, which plays the *active gun's*
  fire effects; `FireAmmunition`/`OnProjectileToss` fire the gun's attachment and clear the shared `PendingFire`; the
  grenade weapon's own states can't run at all while it is not in hand (it is in stasis: no Tick, no timers). So the mod
  owns the interaction and the fuse clock, and calls only the projectile-level functions.
- **Interaction (defaults, each a switch):** the off hand's **grip at the grenade holster** (today's LeftHip
  `SwitchGrenade` spot) takes a grenade (pin in: safe); a **trigger click pulls the pin**; **letting go of the grip
  throws** with the hand's velocity; letting go before the pin is out **puts it back**; a **second trigger squeeze lets the
  spoon fly** (cooking: the game's 4 s fuse, its countdown sound, haptic ticks; held too long it goes off in the hand, the
  game's own rule); an armed grenade let go with almost no speed is **tossed the game's gentlest way along your view**,
  never dropped at your feet.
- **The hand and the grenade:** the free off hand keeps the pose the player tuned and closes its fingers in the game's own
  grenade grip (the right hand's grenade idle, mirrored with `tools/reload_grips.py`'s MIRRORED rule); the grenade itself
  is drawn by a **clone of the grenade weapon's own first-person pickup mesh**, attached to the arms and baked into the
  hand by the existing bake MidHook (the held-magazine precedent: part = inv(grip) x the free hand). This needs one new
  pinned address (`UObject::ProcessEvent` 0x109CE980, for component calls); spike S2 de-risks it.
- **Contract:** shared block v20 (1784 -> 2120 bytes): a game->host status block, the off hand's hold pose inside the view
  seqlock, and an ordered 8-slot event ring (TAKE, PIN, COOK, THROW with position and velocity, PUT BACK).
- **Switch:** `[OffHand] Grenade=0` until the simulator proves Phase 1 (rule 7), then the headset round decides the
  defaults (section 11.2).
- **Fallback if S1 fails:** `ProjectileFire` wrapped in a one-call `Pawn.Weapon` swap (section 2.2, B), then Mode B
  swap-and-return (the gun leaves the hand for ~1.5-2 s; against the request).

### 0.1 The spikes, run (2026-10-02, the main session) -- both PASS

Code: `src/mohavr/offhand.cpp/.hpp` (test commands only, through `Debug.GameCommands`: `mohavr nade` -- a dump, `mohavr
nade throw <frag|gammon|stick|any> <hand|eye> <vx> <vy> <vz> [fuse]`, `mohavr nade carrier <frag|gammon|stick|off>`), the
carrier's bake branch in `arms_ik.cpp` (`BakeCarrier`, before the move test), `UObject::ProcessEvent` pinned in
`addresses.hpp` (prologue `55 8B EC 6A FF 68 08 94 1B 11 64 A1 00 00 00 00`, re-read in Ghidra; build check 35 signatures).
Tests: `work/research/tests/nade0.ps1`, `nade1.ps1`; logs `logs/modlogs/nade0-*`, `nade1-*`; shots `logs/shots/*nade0-*`,
`*nade1-*`.

**S1, the throw [S] PASS** (`nade0`; the BAR in hand throughout):
- Three throws (two from the off hand, one from the eye): `MOHA_MKIIFragGrenade_1 launched MOHAProj_MKII_0: owner
  MOHA_MKIIFragGrenade_1, instigator the pawn, InstigatorController MOHAPlayerController_1, enabled 1, 0.0 units from the
  start`; `SpawnProjectile EALAWeapon -- flags 0x20102, native index 0, parms 28 bytes; vStartPos +0, vForward +12,
  ReturnValue +24` (the return value read from the parms block works).
- `DrawScale 1.00 -> 1.50` on the fresh spawn: an unnumbered native (`SetDrawScale`) runs through ProcessEvent;
  `CreateLight` called with the weapon's light settings (no error logged; the light itself not checked); damage
  `MOHAMKIICookedGrenadeDamageType`; `SetFuseTime(4.0)`: each `went off 4.00 s after the throw`.
- The weapon in hand `MOHABar_1 (unchanged)`, `FlashCount 0 -> 0` every time; `PendingFire [0 0]` before and after; the
  gun fired normally afterwards (`shot 1/2: from the gun ... 0 cm from the red dot's point`); no `throw:` lines from
  throwing.cpp.
- The reserve 3 -> 2 -> 1 -> 0, and the HUD's grenade count followed (the shot shows 2 after the first throw).
- Throws 2 and 3 reused the recycled `MOHAProj_MKII_0` after it had gone off (the pool path); the stick grenade from the hand
  in `nade1` the same (`MOHAProj_StickGrenade_0 ... went off 4.00 s after the throw`). Two live at once (`nade2`): two frags
  in the same Draw became `MOHAProj_MKII_0` and `MOHAProj_MKII_1` (a fresh spawn, not the live one relaunched), both went
  off at 4.00 s.
- Velocity: LOCAL (0, 3, -8) m/s x 2.2 -> (-1, 1760, 660) units/s, plus the pawn's velocity x
  `ExplosivePawnVelocityScale` 0.25 (the game's own carry, not throwing.cpp's 1.0 -- the verifiers' point).

**S2, the carrier [S] PASS** (`nade1`):
- `carrier MOHASkeletalMeshComponent_201 (MOHASkeletalMeshComponent, mesh US_FragGrenade, outer MOHA_MKIIFragGrenade_1)
  attached to MOHASkeletalMeshComponent_2's Camera bone: attached 1, FOV 65, depth group 2` -- `Object.Clone` on the
  weapon's `DroppedPickupMesh` and `FPArms.AttachComponent(clone, 'Camera', 0, 0, (1,1,1))` both through
  `UObject::ProcessEvent`; `DetachComponent` removes it; the stick (`DE_StickGrenade_Rigged`) the same.
- The shots: the frag drawn in true 3D in both eyes at the off hand in three poses (it follows the controller), gone after
  `carrier off`; the stick at the hand; the gun unaffected.
- Not done (Phase 2): the hand's grip (the fingers don't close yet; the grenade sits 6 units ahead of and 2 below the off
  controller, a spike placement) and the pin/spoon/cap bones.

### 0.2 What the verifiers corrected (fold into the phases)

None of the 23 load-bearing claims (section 13) was refuted; one list was incomplete. To fold into the implementation:
- **Availability:** reuse the game's own gate -- `exec SwitchGrenade` refuses when `WorldInfo.Pauser` is set or
  `!PlayerCanSwitchWeapons()` (the HellBox, a pending weapon drop, the D-pad/wheel locks, a mounted MG) -- and
  `IsWeaponDisabled()` at runtime (it is set in more states than section 3.7 lists: ITPInThePlane, BRFInTheBriefing,
  ITPFalling, the AirDrop* states, ladders, death, cinematics); never hard-code the list.
- **Grenade pointers:** the inventory manager's Frag/Gammon/StickGrenadeWeapon are set only by SetupWeaponPointers (the
  loadout); a type given or picked up later is only in the InventoryChain -- walk it (the spike does).
- **Counting:** take the grenade out of the reserve at the THROW, not at TAKE (a checkpoint save while holding would keep
  the reserve one short; a pickup at the cap would lose the put-back one).
- **Following a thrown grenade:** stop on `bDeleteMe` as well as `bEnabled` 1 -> 0 (RecycleProjectile destroys the
  projectile when the class's 3 pool slots are full); read it under SEH, and never write to it after.
- **BOOM** (cooked too long): call CreateLight there too (a pooled projectile keeps its old light otherwise).
- **The toss** (a slow release with the pin out) along the view bounces back off nearby cover: trace first (raise the arc
  or use the hand's velocity when blocked within ~2 m).
- **Tracking loss:** a fast overhand wind-up can drop the controller's VALID bit; throw with the last good velocity
  instead of treating the release as a put-back.
- **The menu while cooking:** refuse opening it, or toss on open (the fuse keeps burning).
- **Hiding the carrier:** a bake that doesn't run would draw the clone at the arms' Camera bone; keep it detached (or
  HiddenGame) when nothing is held, and log `bAttached` (AttachComponent has silent no-op paths).
- **Bake slots:** arms + gun + a gun mid-switch + the carrier = 4 of 4 (`g_saved`, `g_bakedComp`): raise to 6.
- **The HUD:** `GetHUDGrenade` prefers PendingWeapon / Weapon and a rifle's grenade mode over LastGrenadeWeapon.
- **Settled statically:** the Gammon is fuse-only (`bExplodeOnImpactWithPawn` is set only by the rifle grenades and
  rockets), so cooking applies to every hand grenade; Comp B is a placed charge, not thrown.
- **Deviations to name in the headset round:** the pawn-velocity carry (0.25, the game's); cooking from the spoon, where the
  game cooks every throw from the pin pull (AI get the full 4 s); no rate limit on grab-and-throw.

### 0.3 Phase 1, built (2026-10-02, the main session)

Built as sections 5-8 say, with S2's carrier on, and tested in the simulator (`logs/modlogs/nade3-*` to `nade10-*`;
ENGINE-NOTES 5bc, D40). Where it differs from the text below, D40 is right:
- A press at a grenade holster with the switch on, the game alive and a gun in the other hand is never the holster's own
  draw: refused (a pulse) when a take can't happen now (nade3: the landing's fall-through put the gun in the off hand).
- 0.2's corrections are in: tracking loss doesn't freeze (a release then throws with the last tracked velocity, under
  0.25 s old); the MOHAVR menu tosses a cooking grenade; the toss is traced (and goes through triggers, as bullets do)
  and never pitched below the horizon; the bake has 6 slots; the count is taken at the THROW and re-checked there.
- A switch to another gun keeps a held grenade (`nadeCaps` bit4); the game's pause stops the fuse (measured).
- An adversarial review (25 findings, 23 confirmed): the trigger latch at the take and after a freeze, `Pin=auto` with
  `Cook=pin`, the RB mask held until let go, the 4 Hz gate past 2^31 ms of uptime, a destroyed weapon, the over-cook's
  wall and retry, the carrier left out of the gun / no-gun decision, the game side idle while off.
- A second pass on those fixes (5 confirmed): the trigger must rest (< 0.15) before a squeeze counts after a take or a
  freeze; the MOHAVR menu doesn't toss over the game's pause; the trigger mask held until let go; a slow release's toss
  carries the hand's velocity for the blocked fallback; the carrier only with the arm bake.
- Not yet: `Throw=trigger`, `Estimator=peak`, `Spot`, the swing test (T3), T13 (a wall) as written; T12 passed (nade9).

---------------------------------------------------------------------------------------------------------------------

## 1. What I re-checked, and the research notes' disagreements

### 1.1 Re-verified for this design [V]

| Claim | Evidence (mine) |
|---|---|
| `UObject::ProcessEvent` rejects: no FUNC_Native/Defined, a masked probe name, pending kill, **iNative != 0**, a remote native. No weapon-state check. | Capstone at 0x109CE9A1 `test [edi+0x8C],0x402 / je exit`; 0x109CE9E1 `call 0x10919400` (IsProbing); 0x109CE9F5 `call [vtbl+0x40]`; **0x109CE9FF `cmp word [edi+0x90],ax / jne exit`**; 0x109CEA0C..0x109CEA2D `[vtbl+0xFC]` for FUNC_Native. |
| The return value lands in the caller's parms block at `ReturnValueOffset`. | 0x109CEB38 `movzx edx,word [edi+0xA0]; add edx,[ebp+0xC]` then `push edx` as Result into `call [edi+0xA8]`. |
| `AActor::ProcessEvent` 0x10DB1FA0 only gates (world begun play or CDO; not during GC) and forwards. | 0x10DB1FB9 `test byte [WorldInfo+0x310],4`, 0x10DB1FD0 `cmp [0x116D3FC0],0`, 0x10DB1FE5 `call 0x109CE980`. |
| An inventory weapon not in the hand does not tick (no state code, no timers). | `AActor::Tick` 0x10B38042 `test byte [esi+0x74],0x80` (bStasis) then `call [vtbl+0x16C]`; AEALAGrenade vtable 0x1159B7D8: +0xF0 = 0x10DB1FA0, +0x168 = 0x10B38020, **+0x16C = 0x10F0CCF0**, which returns 1 when `Instigator.Weapon (+0x3A8) != this` (0x10F0CD20); `EALAWeapon.uc:1738 bStasis=true`. |
| Components dispatch through `UObject::ProcessEvent` directly; their remote-function slot returns 0. | Vtables MOHASkeletalMeshComponent 0x11587D38, ActorComponent 0x11477830, StaticMeshComponent 0x114F21F8: +0xF0 = 0x109CE980; MOHA skel +0xFC = 0x109692F0 `xor eax,eax; ret 0xC`; +0x124 = 0x10EEB550 (the arms' hooked UpdateTransform). Prologue `55 8B EC 6A FF 68 08 94 1B 11 64 A1 00 00 00 00`. |
| A projectile ignores its own instigator for blocking only when `bBlockedByInstigator` is clear. | `AProjectile::IgnoreBlockingBy` = vtable +0x148 of AProjectile 0x11478120 and AMOHAProj_Explosive 0x11589010 = 0x10DB44E0: `test byte [ecx+0x1E8],2 / jne no; cmp edx,[ecx+0x98] / je ignore`. |
| `SpawnProjectile` reads only CurrentFireMode, WeaponProjectiles, the pool, Instigator, Instigator.Controller; pooled path = SetOwner/Instigator/InstigatorController/SetLocation, else Spawn(..., bNoCollisionFail=true); then Init. | `work/script/MOHAGame/EALAWeapon.uc:423-449`. |
| `ProjectileFire` calls `IncrementFlashCount()` first. | `EALAWeapon.uc:397`. |
| Init = InitProj (re-enables a pooled one: Mesh shown, PHYS_Falling, Speed = fSpeed, SetEnabled(true)) + `SetRotation(Rotator(dir)); Velocity = Speed*dir`. | `MOHAProj_Explosive.uc:90-122, 150-155`; `Engine/Projectile.uc:52-57`. |
| The fuse is the projectile's 'Timer' -> Explode; Explode needs bEnabled; Explode -> HurtRadius(InstigatorController) -> RecycleProjectile (pool or Destroy). | `MOHAProj_Explosive.uc:57-62, 183-254, 299-314, 385-389`. |
| Every player throw gets `MyCookedDamageType`, and the cooked types differ only for stats. | `EALAGrenade.uc:50-64` (StartCooking sets bIsCooking at the pin pull), `:385-392`; `MOHAMKIICookedGrenadeDamageType.uc` = an empty subclass of `MOHAMKIIGrenadeDamageType` (WeaponType 2); only `MOHAPlayerStatsComponent.IsCookedGrenadeKill` (`:374-377`) tells them apart. |
| An inventory weapon's Instigator is the pawn. | `Engine/Inventory.uc:104` (GivenTo: `Instigator = thisPawn`). |
| `LastGrenadeWeapon` is read only by SwitchGrenade (and natively by GetHUDGrenade). | `MOHAInventoryManager.uc:46, 652, 674-676`; native 0x10F0D830 [V-agent]. |
| The weapon is disabled in: crawl stance changes, ladders, Death_In, ITP jump, the airdrop states. | `SetWeaponDisabled(true)` in states StanceChangeIn (crawl only), LadderClimb, Death_In, ITPJump, AirDropDeployChute, AirDropControlledFall, AirDropControlledFlare, AirDropLanding, AirDropLanded (`MOHAPlayerPawn.uc:4107-5207`). |
| The grenade weapon's pickup mesh is the same skeletal mesh as its first-person attachment. | `NN/MOHA_MKIIFragGrenade.uc` PickupMesh `US_FragGrenade` = `NN/Attachment_MKIIFragGrenade.uc` WeaponMeshComponent `US_FragGrenade`; `EALAWeapon.uc:1723-1736`. |
| `Object.Clone` and `SkeletalMeshComponent.AttachComponent` are natives without an index. | `Core/Object.uc:523` (`native final function Object Clone(optional Object InOuter, optional name InName)`); `Engine/SkeletalMeshComponent.uc:166`; precedent `DroppedPickup.uc:62-72` (`PickupMesh.Clone(self)` + AttachComponent). |
| The mod already places a held part as inv(grip) x the free hand. | `src/mohavr/reload.cpp:1738-1760` (`gripInv x (FreeHandRel x off)`). |
| The bake hook takes any FOV != 0 part whose Outer isn't the pawn for the gun. | `src/mohavr/arms_ik.cpp:712-770` (the `arms` test at :729, `reload::OnGunBake` at :770). |
| The test channel runs on the game thread in Hook_Draw's pre-draw section, before `reload::OnDraw`. | `src/mohavr/vr_view.cpp:383-436, 497-508`. |

### 1.2 Where the notes disagree, and what this design does

| Point | Notes | Decision and why |
|---|---|---|
| Where the drawn grenade sits | draw-in-hand: anchor it to the controller and move the hand onto it; mod-integration: inv(grip) x the free hand | **inv(grip) x the free hand** (the held magazine's rule, reload.cpp:1738-1760): the hand stays where the player tuned it (D25, `FreeHand=180`) and only the fingers close; the grenade sits in the palm exactly as the game's grenade idle has it. draw-in-hand's ~19 deg residual is in the hand-to-controller relation the player already tuned. Its anchoring stays a fallback if [H] finds the grenade misplaced (plus a per-type hold page). |
| First visual | mod-integration: C3 (a closed hand, no mesh) first; draw-in-hand: the clone | Both: Phases 1-2 ship C3 internally; the clone (spike S2, Phase 3) lands **before** the first headset round. |
| When the fuse starts | the game: at the pin pull; mod-integration: at the throw (Cook=0) or the pin (Cook=1); vr-prior-art: a second squeeze lets the spoon go | `Cook=spoon` (default): no second squeeze = the fuse starts at the throw (mod-integration's Cook=0), a second squeeze cooks; `Cook=pin` = the game's rule; `Cook=off`. [H]. |
| A slow release with the pin out | mod-integration: the hand's own velocity (a drop); vr-prior-art: a toss along the aim | **Toss** along the head's view at the game's gentlest speed (`ExplosiveSpeed` 750 units/s, +11.5 deg): the flat game never drops a grenade at the feet (MKII radius 6.5 m), and the gun hand already keeps the game's throw under 1 m/s (`hands.cpp:356-362`). `SlowRelease=drop` is the option. [H]. |
| When a grenade is counted out | script-flow: at the throw (`AddReserveAmmo(-1)`); mod-integration: at the take | **At TAKE**, by the reload's proven direct reserve write (reload.cpp:396-436), given back at PUT BACK. The frag and stick pools are shared with the rifle grenades (script-flow 8): a grenade in the hand must already be out of the pool. |
| InitRotator after SpawnProjectile | projectile-lifecycle: InitRotator then Velocity; script-flow: optional | **Not called.** Init (inside SpawnProjectile) already sets Rotation = Rotator(vForward) with vForward = the throw direction; the mod writes Velocity after it. |
| Default spawn point | mod-integration: the eye; others: the hand | **The hand** (the release position the host sends), after an eye->hand trace (a hand through a wall starts on the player's side) and a location check, with the eye as the retry. `SpawnAt=eye` is the option. |
| Velocity estimator | mod-integration: today's 100 ms mean, factored; vr-prior-art: a backdated peak + a speed curve | Today's mean **at the hand point** by default (a proven pipeline), the peak estimate logged on every throw, `Estimator=peak` as the [H] A/B. The curve is a later option. |
| The 5af notify time | 5af: OnProjectileToss at 0.09 s; script-flow: 0.12 s (0.09 is the throw sound) | script-flow (measured from the package); ENGINE-NOTES 5af gets the correction. Not used by this design (it never waits for an animation). |
| Component calls | the mod's guard accepts only `AActor::ProcessEvent` | Pin `UObject::ProcessEvent` 0x109CE980 (bytes in 1.1) and accept either in one script-call helper (Phase 3; S2). |

---------------------------------------------------------------------------------------------------------------------

## 2. Architecture

### 2.1 The shape

```
host (x64, per XR frame)                              game (x86, Hook_Draw pre-draw, once per Draw)
  Hands::Update                                         offhand::OnDraw(hdr)            [new src/mohavr/offhand.cpp]
    off grip press in a grenade holster  --TAKE-->        reserve -1, LastGrenadeWeapon = G, state Held
  OffHandGrenade::Frame  [new src/host/offhand.cpp]       PIN: Armed (+ the pin cue)
    trigger edge                         --PIN--->        COOK: Cooking, fuse clock = WorldInfo.TimeSeconds
    2nd trigger edge                     --COOK-->        THROW: G.SpawnProjectile(hand, dir) via ProcessEvent,
    grip release (+ pos, velocity)       --THROW->               then fuse, cooked type, scale, light, Velocity
    grip release, pin in                 --PUTBACK>       PUTBACK: reserve +1
  writes in the view seqlock: nadeFlags, nadePose,      fuse out while held: SpawnProjectile + Explode at the hand
    nadeAdj (the hold)                                  publishes nadeSeq block: caps, counts, state, fuse, ticks, acks
  reads the game's block: Active, counts, state,
    fuse ticks (haptics), BOOM, new pawn               arms_ik::OnMeshUpdate (the bake MidHook, every arms update)
                                                          the free hand's fingers = the grenade grip
                                                          the cloned grenade baked at inv(grip) x the free hand
```

The host owns the interaction (as for the manual reload, D21); the game owns the inventory, the fuse clock and every engine
call; the shared block carries ordered events with acknowledgements and a reconcile (the reloadEvt pattern,
`shared_frame.hpp:216-220`, host `reload.cpp:719-738`).

### 2.2 Alternatives rejected

| Option | Why not |
|---|---|
| A. `G.ProjectileFire()` on the inactive grenade | `IncrementFlashCount` -> `Pawn.WeaponFired` -> `Pawn.Weapon.PlayFireEffects` = the **gun's** fire animation, tracer data and whiz (`EALAWeapon.uc:397`, `Weapon.uc:574-582`, `Pawn.uc:423-434, 504-513`, `EALASmallArms.uc:235-254`); an eye trace through `CalcWeaponFire` (the mod's bullet-trace hook could take it, `aim.cpp:314-352`); HideMesh on a stale mesh; yaw replaced by the body's. **Kept as fallback B**: write `Pawn.Weapon (+0x3A8) = G` for that one call and restore, plus `PhysicalStartFireOverride`, `ThrowStrength`, no active 'Timer' on G. |
| `GrenadeThrowing.OnProjectileToss` / `FireAmmunition` on G | `GetWeaponAttachment()` is the gun's: its fire sound, muzzle flash and shell; `ClearPendingFire(0/1)` clears the **shared** `InvManager.PendingFire` (releases the gun's held trigger); `ConsumeAmmo` needs AmmoCount filled; `names::FindFieldProbe` finds the empty global stub (no FUNC_Defined -> a silent no-op) (`EALAGrenade.uc:670-682`, `EALAWeapon.uc:179-227, 832-835`; native-engine 1.4). |
| Driving G's own states (`BeginFire`, `StartCooking`, `Timer`) or clearing its `bStasis` | G doesn't tick while the gun is in hand (1.1); `GotoState` is native 113 (refused); its states raise the pawn's Prime/Throw/Raise activities over the gun, `Active.BeginState` re-fires the shared PendingFire, `Timer` -> `WeaponEmpty` -> **SwitchGrenade** (`EALAGrenade.uc:112-161, 496-506, 748-818`). |
| Mode B: SwitchGrenade -> the game's throw -> SwitchPreviousWeapon | Works today but the gun leaves the hand for ~1.5-2 s (put-down + equip + prime + throw + put-down + equip, 5af) and the rig flips into the mirror world meanwhile: the opposite of "without unequipping your gun". **Last fallback.** |
| Spawning natively (UWorld::SpawnActor asm wrapper, a hand-built FFrame for execSpawn) | New pinned addresses and inline asm for what the weapon's own script `SpawnProjectile` already does, pool included (native-engine 2.2, 3). |
| `MOHAAIPawn.ThrowGrenade` | An AI-pawn function (reads AI-only properties); AI grenades have no Owner, so no pool (`MOHAAIPawn.uc:2002-2040`). |
| Carrier: the real projectile held in the hand | The low-poly thrown mesh at 1.5x (11 x 11 x 22 units in the hand vs the first-person 5.9 x 11.7 x 6.8), the world depth group under the foreground fingers, a blocking cylinder in the hand, the game's next throw may take it from the pool; an enabled one raises the AI flee CSA and the HUD danger ring (`MOHAProj_Explosive.uc:416-498`; draw-in-hand 3). |
| Carrier: a render-time transform for a static-mesh proxy | No per-view transform virtual for static meshes (FStaticMeshSceneProxy 0x114F2758 has 20 slots; the proxy is rebuilt on every transform update) (draw-in-hand 4). |
| Carrier: a second grenade attachment | One attachment slot per pawn (`MOHAPawn.CurrentWeaponAttachment`); `WeaponAttachmentChanged` would re-point the gun's Mesh pointers; Spawn is refused by ProcessEvent (draw-in-hand 5). |
| Carrier: the pawn's `ITPClipMesh` | Used by the ITP plane state; its Outer is the pawn, which the bake takes for the arms. Fallback only if Clone fails. |
| A dedicated chest spot as the default | The left-hip holster has the player's muscle memory and is far from the foregrip (MOH:AB's reviewer kept grabbing the foregrip at a chest spot, vr-prior-art 2.5). Offered as `Spot=chest` [H]. |
| Consuming with `AddReserveAmmo(-1)` through ProcessEvent at the throw | Correct, but the grenade must be out of the shared pool while it is in the hand (1.2); the direct reserve write is the reload's proven path. |

---------------------------------------------------------------------------------------------------------------------

## 3. The game side: the exact call sequences (`src/mohavr/offhand.cpp`)

### 3.1 Objects (all by reflection, `names::PropertyOffset` / `BoolProperty`; offsets in brackets are for log checks only)

- `P` = `aim::LocalPlayerPawn()`; must `IsA "MOHAPlayerPawn"`, not bDeleteMe, `Health > 0`.
- `C` = `P.Controller`; `IM` = `P.InvManager` (+0x3A4); `W` = `P.Weapon` (+0x3A8), the gun.
- `G(type)` = `IM.FragGrenadeWeapon` (+0x208) / `IM.GammonGrenadeWeapon` (+0x210) / `IM.StickGrenadeWeapon` (+0x214); "any" =
  `IM.LastGrenadeWeapon` (+0x218) if it has a grenade, else frag, Gammon, stick (SwitchGrenade's order,
  `MOHAInventoryManager.uc:668-701`). A type whose object is none is not carried.
- Count of a type = `G.bInfiniteAmmo` ? infinite : `IM.AmmoStorage[i].ReserveAmmoAmount` for `AmmoClass == G.AmmoClass[0]`
  (exact match, as `reload.cpp:396-410` `ReserveOf`) + `G.AmmoCount[0]` (normally 0 for an inactive grenade).
- `WI` = `P.WorldInfo`; game time = `WI.TimeSeconds` (stops while the game is paused).
- Weapon properties used: `CurrentFireMode` (byte), `FuseTime` (4.0), `ExplosiveSpeed` (750), `DirectionOffset`
  (Pitch 2100), `ExplosiveDrawScale` (1.5), `bUseExplosionLight`, `LightColor` (Color, 4 bytes), `LightRadius`,
  `LightLifespan`, `LightBrightness`, `LightFalloffSpeed`, `LightFalloffExponent`, `LightCreationDelay`,
  `CookingStartSound` / `CookingCountdownSound` (AudioComponents -> their `SoundCue`), `CookingSoundPlayInterval[0..1]`
  (0.25 / 0.1), `WeaponType`, `WeaponClass`, `DroppedPickupMesh`.
- Projectile properties used: `Location`, `Velocity`, `Owner`, `Instigator`, `InstigatorController`, `bEnabled`,
  `MyDamageType`, `MyCookedDamageType`, `ImpactedActor`, `bBlockedByInstigator`, `DrawScale`.

### 3.2 The script-call helper (new `src/mohavr/script_call.cpp/.hpp`, factored out of reload.cpp)

`reload.cpp:544-551 CallProcessEvent` + the lookups `PlayCue` does (`:590-635`) become one helper used by both modules:
- `Fn Find(std::uintptr_t obj, const char* name)`: `names::FindFieldProbe(class, name)`; require `ClassName(fn) ==
  "Function"`, `FunctionFlags (+0x8C) & 0x402`, `iNative (WORD +0x90) == 0`; keep `ParmsSize (WORD +0x9E)` and
  `ReturnValueOffset (WORD +0xA0)`; cache per (class, name).
- `int Parm(const Fn&, const char* p)`: `FindFieldProbe(fn, p)` -> `UProperty.Offset (+0x64)`.
- `bool Invoke(obj, const Fn&, void* parms, size_t cap)`: refuse if `ParmsSize > cap`; vtable `+0xF0` must be
  `kActorProcessEvent` (0x10DB1FA0) for actors or, from Phase 3, `kObjectProcessEvent` (0x109CE980, new in
  addresses.hpp with its 16 prologue bytes in `kSignatures`) for components; SEH-guarded `__fastcall(obj, 0, fn, parms,
  nullptr)` as today. A fault disables the off-hand grenade for the session (logged), as `g_soundsOff` does.
- Optional parameters have **no defaults** through ProcessEvent (native-engine 1.3): every call below fills every
  parameter.

### 3.3 TAKE(type)

1. Accept only in state None, with the availability conditions of 3.7 true, `G(type)` present and its count >= 1
   (else refuse: ack + `nadeState` reason, log `offhand: TAKE refused -- <reason>`).
2. Count it out: infinite -> nothing; else reserve - 1 (or, if the reserve is 0 and `G.AmmoCount[0] > 0`, AmmoCount[0] -
   1). Direct writes (the reload's path); no `SetAmmoCount` (it would switch off the gun's shared low-ammo sound mix,
   script-flow 8).
3. `IM.LastGrenadeWeapon = G` (`[OffHand] HudType=1`): the HUD's grenade count (`GetHUDGrenade`) shows the held type, and the
   game's own SwitchGrenade later picks the type last used -- what the game's own equip does (`MOHAInventoryManager.uc:652`).
   Optional (Phase 4): `MOHAHUD(C.myHUD).ToggleWeaponInfo(Enabled=false, Duration=3.0, infoType=1, complexity=1)` (an actor
   event, all four parameters passed: `MOHAHUD.uc:1781`), as the game's SwitchGrenade exec does (`MOHAPlayerController.uc:970`).
4. State Held {type, G, fuseLen = G.FuseTime}; log `offhand: TAKE frag -- reserve 4 -> 3 (held; HUD shows frag)`.

### 3.4 PIN, COOK and the fuse clock

- PIN: Held -> Armed. Phase 4: the pin cue at the hand (3.10). Log.
- COOK: Armed -> Cooking; `cookStart = WI.TimeSeconds`; Phase 4: `CookingStartSound.SoundCue` at the hand; the countdown
  as the game's `UpdateCookingSound` (`EALAGrenade.uc:76-110`): first tick 0.5 s after the cook, then every
  `CookingSoundPlayInterval[0]` (0.25 s), every `[1]` (0.1 s) once less than 40 % of the fuse is left; each tick plays
  `CookingCountdownSound.SoundCue` at the hand and bumps `nadeTicks` (the host's haptic tick).
- With `Cook=pin` the host sends PIN and COOK together; with `Pin=auto` it sends TAKE and PIN together.
- Fuse left = `fuseLen - (WI.TimeSeconds - cookStart)` while Cooking; when it reaches 0 -> BOOM (3.6). Game time, so the
  game's pause stops it (the MOHAVR menu does not pause the game; section 5.4).
- The weapon's own `StartCooking`/`Timer` are never used (1.1: G doesn't tick; its Timer would switch weapons).

### 3.5 THROW(pos, vel, flags) -- the core sequence

Accepted in Armed or Cooking. `pos` = the release point (the off hand's hand point, LOCAL metres), `vel` = the release
velocity (LOCAL m/s), flags bit0 = toss.

0. Re-check `P`, `G` (still `IM`'s object for the type, `G.Instigator == P`), G's vtable +0xF0 == `kActorProcessEvent`.
1. **Start:** `start = view::PoseToWorld({pos, identity})` (`SpawnAt=hand`; Phase 3 `SpawnAt=drawn`: the drawn grenade's
   centre from the last bake if <= 100 ms old, un-mirrored). `head` = `view::PoseToWorld(hdr->head)`. If
   `aim::WorldTrace(P, head, start, hit)` hits, `start = hit - 5 units x unit(start - head)` (log `blocked`).
   `SpawnAt=eye`: `view::GameCamera` location (the game's own start, the arms' Camera bone within 4-14 units, 5v).
2. **Velocity:** toss -> `dir = the head's forward pitched up by DirectionOffset.Pitch (2100 = 11.5 deg, capped at 90 deg)`,
   `v = dir x G.ExplosiveSpeed`; else `v = view::VectorToWorld(vel) x [OffHand] ThrowScale` (the hand's speed clamped at
   `MaxHandSpeed` m/s first). Then `v += P.Velocity` (as `throwing.cpp:91-97`). `dir = unit(v)` (the head's forward if
   |v| < 1).
3. Write `G.CurrentFireMode = 0` (after a grenade melee it stays 2, and `WeaponProjectiles` has one entry: `Weapon.uc:278-282`).
4. **Call `G.SpawnProjectile(vStartPos = start, vForward = dir)`** (EALAWeapon script function; AActor::ProcessEvent);
   `proj = *(Projectile**)(parms + ReturnValueOffset)`. 0 -> undo the take (reserve + 1), state None, log, ack refused.
5. **Check:** `IsA(proj, "MOHAProj_Explosive")`, `bEnabled` set, `|proj.Location - start| <= 2 units`. A pooled projectile's
   `SetLocation` can fail and leave it where it last exploded (projectile-lifecycle 3.1, [R]): then call
   `proj.RecycleProjectile()` (back to the pool, disabled) and retry step 4 once with `start` = the eye; a second failure
   undoes the take.
6. Writes: `proj.ImpactedActor = none` (a pooled projectile keeps its last life's; full damage to that actor otherwise,
   projectile-lifecycle 3.6); `[OffHand] PassThrower=1`: clear `proj.bBlockedByInstigator` (so a release behind or beside
   the body can't bounce off the thrower: 1.1, 0x10DB44E0; the flag stays with the pooled actor, harmless).
7. **Call `proj.SetDrawScale(NewScale = G.ExplosiveDrawScale)`** (Actor native without an index; a fresh spawn is 1.0, a
   pooled one keeps 1.5). The first unnumbered native the mod calls this way: S1 logs DrawScale before/after.
8. If `G.bUseExplosionLight`: **call `proj.CreateLight(LightColor, LightSize = LightRadius, LightLife = LightLifespan,
   LightBrightness, FalloffSpeed = LightFalloffSpeed, LightFalloffExponent, LightDelay = LightCreationDelay)`** with G's
   values (`MOHAProj_Explosive.uc` CreateLight; the game's `SetupLight`, `EALAGrenade.uc:297-305`).
9. Write `proj.MyDamageType = proj.MyCookedDamageType` when not none (the game does it for every player throw; stats only).
10. **Call `proj.SetFuseTime(newFuseTime = Cooking ? max(fuseLen - (WI.TimeSeconds - cookStart), 0.05) : fuseLen)`**
    (script: `fFuseTime` + `SetTimer`).
11. Write `proj.Velocity = v` (after Init, which set `Speed x dir`).
12. Phase 4: the throw cue at `start` (`Grenade_PC_Throw`, `_Throw_Alt` for a toss); `C.playerStats.OnWeaponFire(WeaponType =
    G.WeaponType, WeaponClass = G.WeaponClass, NumShots = 1)` (a component: UObject::ProcessEvent; the game's
    `ProjectileFire` does it, `EALAGrenade.uc:393`).
13. State None; follow `proj`: 1 s later log Location/Velocity (`throwing.cpp:56-63`'s pattern); each Draw watch `bEnabled`
    -> 0 = exploded (log the time since the throw and where). Never write to `proj` after its life ends (the pool reuses
    the actor).

Nothing here touches `P.Weapon`, the gun's state, `IM.PendingFire`, `G.SpawnedExplosive` (so `throwing.cpp`, which watches
only the active weapon's `SpawnedExplosive`, is not involved) or `P.FlashCount`.

### 3.6 PUT BACK, BOOM, a new pawn, refusals

- PUT BACK (Held or Armed): reserve + 1 (clamped at `MaxAmmoCount`; infinite: nothing); state None. A PUT BACK that
  arrives while Cooking is executed as THROW with the toss flag (a live grenade is never silently removed).
- BOOM (Cooking and the fuse out): `start` = the hand (`hdr->nadePose` mapped; the last one if stale); steps 3-6 and 9
  above, then **call `proj.Explode(HitLocation = start, HitNormal = (0,0,1))`** (script; needs bEnabled, set by Init) --
  what the game's own over-cook does at the eye (`EALAGrenade.uc:112-161`, standalone branch). `nadeBoom` + 1, state None.
- A new local pawn (death, level): state None (a held grenade is lost, as the game's own would be), `nadePawnSeq` + 1.
- No host for 0.5 s while Cooking (the host died): THROW toss. Held / Armed: PUT BACK.
- Every event is acknowledged (`nadeEvtAck`) whether applied or refused; refusals log the reason.

### 3.7 Availability (`nadeCaps`, published every Draw)

- bit0 *installed*: SpawnProjectile, SetFuseTime, SetDrawScale, Explode, RecycleProjectile resolved for the current classes
  and the actor ProcessEvent slot checked.
- bit1 *available now*: `P` a live MOHAPlayerPawn; `W` IsA EALAWeapon and **not** EALAGrenade (a grenade as the main weapon
  disables the off-hand one: one grenade object can't be in both); a gun drawn (not `viewmodel` `g_noGunDrawn`, not
  `vr_view` `g_landingHeld`: export accessors); not `C.bCinematicMode`; not `P.bNoWeaponFiring`; not
  `G.IsWeaponDisabled()` (EALAWeapon script function, bool ReturnValue; evaluated at 4 Hz and at each TAKE/PIN/THROW --
  covers ladders, crawling, ITP, the airdrop states and dying, 1.1).
- bit2 infinite ammo; bit3 the carrier is ready (Phase 3).
- The game never acts on caps itself while something is held: the host decides (it is frozen during menus; 5.4).

### 3.8 What this keeps from the game's own throw, and what it skips

Kept: the grenade class and its projectile, the global pool, Owner = G / Instigator = P / InstigatorController = C (so
HurtRadius credits the player and `OnExplosiveKill(DamageType.WeaponType)` feeds grenade experience,
`MOHAAIPawn.uc:1241-1275`), per-poly collision (`bCollideComplex`: the Owner is a weapon of a MOHAPlayerPawn,
`MOHAProj_Explosive.uc` InitProj), the fuse and the explosion (emitter, light, sound), the cooked damage type, the AI
stimuli (the 'Grenade' flee stimulus at the first bounce, 'NearbyExplosion' at the explosion), the kick CSA, the HUD
danger indicator, the reserve count. Skipped on purpose: the gun's fire effects, the eye trace, the game's
direction/speed/yaw rules (the hand's velocity replaces them), the arms' throw animations and their sounds (played by
the mod, 3.10), `WeaponEmpty -> SwitchGrenade`, the 0.12 s animation delay (the grenade leaves at the release).

### 3.9 Threading and timing

All engine calls run in `Hook_Draw`'s pre-draw section (after `reload::OnDraw`, `vr_view.cpp:508`): the game thread,
after the world tick, `UWorld::InTick` = 0, no GC (native-engine 5.1-5.3; "Give all weapons" already spawns 13 weapons
there). Gate on the pawn and `WorldInfo.bBegunPlay`. The follow and the carrier's bake run in the existing per-view
(`OnPlayerView`) and bake (`OnMeshUpdate`) paths, with plain memory reads/writes only. A release reaches the game within
one game frame (~11 ms); the projectile's first physics step is the next tick.

### 3.10 Sounds (Phase 4)

Through the weapon actor `G.PlaySoundAt(ASound, SourceLocation)` (Actor script function, `Actor.uc:1454`; the reload's
PlayCue path, generalised to a location), cues found by name in `BuildCues`' list (the arms' AnimSets hold them): PIN
`grenade.grenade_pc_pinpull` then `grenade_pc_pinpull_2` ~0.18 s later (frag), `stickgrenade_pc_capunscrew` (stick,
Gammon); THROW `grenade_pc_throw` / `grenade_pc_throw_alt` (toss); TAKE the type's `*_pc_swapin` (optional). The cooking
cues are G's own AudioComponents' `SoundCue`s. [S]: a log of which cues were found (the stick/Gammon group names are not
yet known).

---------------------------------------------------------------------------------------------------------------------

## 4. Drawing the grenade, and the off hand's pose

### 4.1 The hand: the game's grenade grip, mirrored (Phase 2)

- Source [V-agent, draw-in-hand 8.1]: in `VM_AnimSet_NoBazooka` the right hand holds the grenade (`RightProp` = the grenade
  mesh frame, measured identity in `reload-sockets-MOHAVR.log`) rigidly through `grenademkiia_idle`,
  `stickgrenade_idle`, `gammongrenade_idle` (0.00 units, <= 0.1 deg, fingers constant): frame 0 is the grip; hand bone
  9.9 / 11.9 / 9.0 units from the grenade origin.
- `tools/reload_grips.py`: a `GRENADES` list `(key, arms seq, frame)` = `('Attachment_MKIIFragGrenade', 'grenademkiia_idle',
  0)`, `('Attachment_StickGrenade', 'stickgrenade_idle', 0)`, `('Attachment_GammonGrenade', 'gammongrenade_idle', 0)`, run
  through the MIRRORED code path with `part` = identity (no grenade psk/psa needed): `hand = MIRROR_D x (RightHand x
  inv(RightProp)) x MIRROR_S`, fingers = the right chains with their offsets negated; kind `"hold"`, bone `"RootOffset"`.
  Emitted into `reload_grips.inc` beside the guns (draw-in-hand 8.5 lists the expected rows, e.g. MKII hand translation
  (-2.65, 5.29, 7.97)).
- `arms_ik.cpp:483-488`: `else if (offhand::HoldFingers(g_gripFingers, g_gripNames)) gripOn = true;` -- **the target stays
  the free hand's** (`relM x offCtrl`, :479-482), only the 15 fingers take the grip (:527-536). The reload's GripNow can't
  be on at the same time (the off grip holds one thing).
- Shown only while the host says a grenade is held (`nadeFlags` bit1) and the gun hand holds a gun.

### 4.2 The carrier: a cloned pickup mesh, baked into the hand (Phase 3; spike S2)

Once per pawn and type (on the first TAKE of that type), game thread (Hook_Draw pre-draw):
1. `T` = `G.DroppedPickupMesh` (a MOHASkeletalMeshComponent with the first-person grenade mesh, never attached:
   `EALAWeapon.uc:1723-1736`).
2. **`clone = T.Clone(InOuter = G, InName = None)`** (native without an index; UObject::ProcessEvent on the component). Log
   its class, SkeletalMesh and Outer.
3. Before attaching (plain writes, read when the proxy is created): `DepthPriorityGroup (+0x131) = 2` (the foreground, as the
   arms); `CollideActors`, `BlockActors`, `BlockZeroExtent`, `BlockNonZeroExtent`, `BlockRigidBody`,
   `bNotifyRigidBodyCollision` = false; `LightEnvironment` = FPArms' (the pawn's own, which the gun's mesh gets too:
   `WeaponAttachment.uc:216`, `MOHAPlayerPawn.uc:5759-5760`; the pawn sets it to none in some states, :5431-5432, so the
   picture check covers lighting); `iMinLODLevel = 0`, `ForcedLodModel = 1`;
   `FOV (+0x3D0)` = the gun mesh's FOV (65; makes it a first-person part for the proxy hook: drawn in true 3D, through the
   mirror in left-hand mode, `viewmodel.cpp:150-172`); `fCustomBoundsSize = 200`.
4. **`P.FPArms.AttachComponent(Component = clone, BoneName = <the arms' 'Camera' FName, copied from FPArms.SkeletalMesh
   RefSkeleton>, RelativeLocation = 0, RelativeRotation = 0, RelativeScale = (1,1,1))`** (the scale must be passed: no
   default through ProcessEvent, draw-in-hand 7.1). Every arms update then flags it for a transform update
   (`USkeletalMeshComponent::UpdateTransform` 0x10CFAC10 walks `Attachments`), so it passes the bake MidHook (0x10CFAFAD).
5. Keep the pointer per pawn; drop it on a new pawn. Kept attached (collapsed when not held: no calls per throw).

In `arms_ik.cpp OnMeshUpdate`, a pointer-keyed branch **before** the `arms` test (:729) -- else the clone is taken for the
gun and handed to `reload::OnGunBake`:
- save the bones (raise `g_saved` and `g_bakedComp` from 4 to 6 slots: arms + gun + a gun mid-switch + the clone);
- `R` = `armsik::FreeHandRel` (<= 250 ms), `off` = `viewmodel::HandFrames` off frame, `W` = `BodyMoveSinceView` (CatchUp);
  `Tfree = R x off x W`; `H` = the type's "hold" grip with the player's hold adjustment (`nadeAdj`, the `gripOf` rule of
  `reload.cpp:1675-1700`); **`Gw = inv(H) x Tfree`** (the grenade's mesh frame in the world);
- `bones[i] = saved[i] x Gw x inv(L2W)`; when nothing is held: collapse every bone (zero 3x3, as reload's Collapse); pin out:
  collapse `Pin` + `Ring` (frag) / `Cap` (stick, Gammon); cooking: also `spoon` (frag); thrown: all, from the frame the
  projectile exists;
- `MarkBaked(comp)`; store `Gw`'s origin (un-mirrored through `viewmodel::DrawMirror` when mirrored) with a tick for
  `SpawnAt=drawn`; return before the gun path.
- Skip the clone by pointer in `viewmodel` `NotePart`/`UpdateWeaponKey` (its Outer is G, not a WeaponAttachment: today it
  would only set `notGun`).

### 4.3 Left-hand mode (`Weapon.LeftHandMirror`, the gun in the left hand)

The off hand is the real right hand. HandFrames' off frame and FreeHandRel live in the mirror world (`viewmodel.cpp:548-553,
600-618`), so `Gw` is a mirror-world frame and the proxy hook draws the clone through the mirror like the arms (det sign
handled). The grip needs no left-hand variant. The spawn point is un-mirrored (`SpawnAt=drawn`) or comes from the host's
real-world hand point (`SpawnAt=hand`). The host's velocity is real-world LOCAL (no mirror). Cosmetic: the mirrored frag
keeps its spoon under the palm and has its ring on the other side (draw-in-hand 8.3) [H].

### 4.4 Scale at release

The held clone is the first-person mesh at scale 1; the thrown projectile is the low-poly thrown mesh at
`ExplosiveDrawScale` 1.5 (about 1.9x): a visible "pop" at release. `[OffHand] ThrownScale=1.5` (the game's) by default;
~0.8 (MKII) / 0.72 (stick) / 0.94 (Gammon) match the held size (draw-in-hand 9) -- an [H] choice (visibility in flight vs
continuity).

---------------------------------------------------------------------------------------------------------------------

## 5. Interaction

### 5.1 Where grenades are taken from

- `Spot=holster` (default): every holster zone whose command is a grenade switch (`SwitchGrenade` = any type in the game's
  order; `SwitchFragGrenade` / `SwitchGammon` / `SwitchStick` = that type; `[Holsters]` already allows them). Shipped: only
  LeftHip. **The off hand there takes an off-hand grenade; the gun hand there keeps today's behaviour** (the grenade
  becomes the main weapon) -- refused while the off hand holds one. When the off-hand grenade is not Active (switch off,
  a grenade as the main weapon, the parachute...) the off hand there also keeps today's behaviour (the command, and the
  hand becomes the gun hand, `hands.cpp:219-230`).
- `Spot=chest` (Phase 6 option): a 6th body spot `[Holsters] OffGrenadeSpot=-12 -38 14 9` (cm; x toward the off hand's side,
  mirrored with the gun hand), off hand only; no overlap with the shipped spots (mod-integration 3.3).
- Holsters are absolute: in left-hand mode the right (off) hand reaches the left hip, or the player moves the holster on the
  Holsters page as today [H].
- Nothing to take: the press is consumed (no holster swap), a 60 ms 0.2 pulse (the reload's "pouch empty").

### 5.2 Pin, cook, throw, put back

| Switch | Default | Options |
|---|---|---|
| `Pin` | `trigger`: a trigger click (>= 0.6 after < 0.4 since the take) pulls the pin | `auto` (no pin step: the take arms it); `teeth` (Phase 6: hold the grenade at the mouth -- a head-frame zone ~9 cm below and 6 cm ahead of the eyes, r ~8 cm, under 0.6 m/s for 0.1 s) |
| `Throw` | `grip`: letting go of the grip throws an armed grenade | `trigger`: the trigger's press pulls the pin, its release throws (the gun hand's grenade semantics); a grip release before the trigger puts it back, during it throws |
| `Cook` | `spoon`: a second trigger click (armed) lets the spoon go -- the fuse starts in the hand; otherwise the fuse starts at the throw | `pin` (the game's rule: the fuse starts at the pin pull); `off`. With `Throw=trigger`, `spoon` acts as `off` |
| put back | a grip release before the pin is out | (an armed, not cooking grenade let go during a freeze is also put back: 5.4) |

One grenade at a time; after a throw the hand is empty and the next press at the spot takes another.

### 5.3 The throw velocity (host)

- Sample the off hand's **hand point** (`pt[o]`, `hands.cpp:130-134`: where the grenade is) every XR frame (a 24-sample
  history beside today's aim-pose history, which stays for the gun hand).
- `Estimator=mean` (default): the newest sample against the one ~0.1 s before (today's rule, `hands.cpp:344-354`).
  `Estimator=peak`: the fastest 2-frame difference in the last 150 ms averaged with its neighbour (SteamVR's
  AdvancedEstimation). Both are logged on every off-hand release.
- `MaxHandSpeed=12` m/s clamp; `ThrowScale` (off hand) = 2.2 to start, applied game-side; `MinThrowSpeed=1.0` m/s ->
  `SlowRelease=toss` (5.4).
- `throwvel=x,y,z` (pad_cmd) replaces the next off-hand release's velocity too (whichever hand releases first).

### 5.4 Safety rules

- **Slow release** (armed or cooking, < `MinThrowSpeed`): THROW with the toss flag (the head's view, +11.5 deg, 750
  units/s) -- `SlowRelease=drop` sends the hand's own velocity instead [H].
- **Tracking:** `Hands` gets the real tracking bits (captured before `pad.HoldLost` ORs, `main.cpp:698`) and the grip
  action's `isActive` (a sleeping controller's grip reads 0, `pad.cpp:663-671`). While the off hand is not really tracked
  or its grip action is inactive, the state is **frozen**: grip and trigger changes are ignored.
- **Menus:** the MOHAVR menu (its select includes the left trigger, `main.cpp:363`) or the game's UI menu -> frozen. On
  unfreeze: grip still held -> carry on; released -> Held/Armed: PUT BACK, Cooking: THROW toss. The MOHAVR menu doesn't
  pause the game: a cooking fuse burns on (it can go off in the hand) [H]; the game's pause stops game time and the fuse.
- **Active lost while holding** (not frozen; caps bit1 clear for 2 Draws: a ladder, a cutscene, the main weapon became a
  grenade): Held/Armed -> PUT BACK; Cooking -> THROW toss. A new pawn: None.
- The fuse running out in the hand kills like the game's own over-cook; the countdown sound and haptics warn.

### 5.5 Conflicts with the off hand's other uses

| Existing use | Resolution |
|---|---|
| Manual reload grabs (pouch, magazine, bolt, pump; `hands.cpp:215`) | Press order at an off-grip press: (1) the reload's TakePress (unchanged; the pouch still wins its 1.4 cm overlap with the left hip while the magazine is out), (2) the off-hand grenade at a grenade holster, (3) other holsters, (4) the foregrip, (5) the old gesture. While a grenade is held the reload stays Active (the gun hand's release button still drops the magazine) and gets `offBusy` (rings and press candidates hidden). Bolts, the pump, the M18 breech wait until the grenade is thrown. |
| Foregrip (`hands.cpp:232`, the approach pulse :245-250) | Disjoint by the grip; while held, the foregrip ring and pulse are suppressed. |
| The off trigger's contextual uses (twin flip, MP40 grab, foregrip grab, M12 pump) | All need a magazine in the off hand or the foregrip: impossible while a grenade is held. `maskTrigger[o]` while held (a player who maps LT back doesn't zoom on the pin pull). Their own hysteresis keeps a still-held trigger from re-triggering after a throw. |
| X = Xbox RB = SwitchGrenade (`MOHAVR.ini:242`); NextWeapon after Give all | Xbox RB masked while held (`Pad::SetMaskedButtons(0x0200)` after `Map`); if NextWeapon makes a grenade the main weapon, Active is lost (5.4). |
| Gun hand at a grenade holster while the off hand holds one | Refused (consumed, 60 ms 0.2 pulse). |
| The menu's Gun hand toggle (`hands.cpp:98-103`) | Applied only with nothing held (else after the throw / put back). |
| The free hand (D25) | Unchanged pose; the fingers and the grenade follow it (4.1-4.2). |
| Pistols | The ideal case (the off hand is always free). |
| Parachute / landing / cinema screen / the game's menus | Not Active (caps bit1; `hasView`; `gestures`). |

### 5.6 Feedback

Haptics (off hand unless said; `[OffHand] Haptics=1`): entering the spot -- today's 30 ms; TAKE 30 ms 0.5; empty 60 ms 0.2;
PIN 20 ms 0.6; spoon (COOK) 40 ms 0.8; each countdown tick (`nadeTicks` change) 10 ms 0.2; the last 0.75 s 50 ms 0.7
every 50 ms; THROW 30 ms 0.5; toss 2 x 15 ms; PUT BACK 20 ms 0.2; BOOM both hands 200 ms 1.0. Sounds: 3.10. HUD: the
held type's count (`HudType`), optionally the weapon-info flash at TAKE.

---------------------------------------------------------------------------------------------------------------------

## 6. State machines

### 6.1 Host (`OffHandGrenade`, src/host/offhand.cpp; Pin=trigger, Throw=grip, Cook=spoon)

**Active** = the toggle (`[OffHand] Grenade` / the menu) && `nadeCaps` bit0 && bit1 && the game alive (`nadeSeq` advanced
within 250 ms) && the gun hand valid && the off hand really tracked && its grip action active && `gestures` && `hasView`
&& `weaponKind != 2`. States: **None, Held, Armed, Cooking**, plus **frozen**.

| From | Input | Condition | To | Sent | Pulse |
|---|---|---|---|---|---|
| None | off grip press, hand point in a grenade holster | Active, count of the type (or `nadeNext` for any) > 0 | Held (grip consumed) | TAKE(type) | 30 ms 0.5 |
| None | same | Active, nothing of that type | None (press consumed) | -- | 60 ms 0.2 |
| None | same | not Active | today's holster command | -- | today's |
| Held | off trigger press edge | -- | Armed | PIN | 20 ms 0.6 |
| Held | grip release | not frozen | None | PUTBACK | 20 ms 0.2 |
| Armed | off trigger press edge | Cook=spoon | Cooking | COOK | 40 ms 0.8 |
| Armed / Cooking | grip release, speed >= MinThrowSpeed | not frozen | None | THROW(pt, v) | 30 ms 0.5 |
| Armed / Cooking | grip release, slower | not frozen | None | THROW(pt, v, toss) (drop: no flag) | 2 x 15 ms |
| Cooking | `nadeTicks` changed | -- | Cooking | -- | 10 ms 0.2 (last 0.75 s: 0.7) |
| Held / Armed / Cooking | `nadeBoom` changed | -- | None | -- | both 200 ms 1.0 |
| any held | menu open / not tracked / grip inactive | -- | frozen | -- | -- |
| frozen | unfrozen | grip held | as before | -- | -- |
| frozen | unfrozen | grip released | None | PUTBACK (Held, Armed) / THROW toss (Cooking) | as above |
| any held | caps bit1 clear 2 Draws | not frozen | None | PUTBACK / THROW toss | -- |
| any | `nadePawnSeq` changed | -- | None | -- | -- |
| any | game state differs 2 Draws, all events acked | -- | the game's | -- | (empty pulse if it refused a TAKE) |

Option deltas: `Pin=auto` -> the take sends TAKE+PIN (None -> Armed); `Cook=pin` -> PIN sends PIN+COOK (-> Cooking);
`Cook=off` -> no COOK; `Throw=trigger` -> PIN on the trigger press (Held -> Armed/Cooking), THROW on its release (grip
still held), PUTBACK on a grip release from Held, THROW on a grip release from Armed/Cooking.

Masks while held: the off grip stays consumed; `maskTrigger[o]`; Xbox RB.

### 6.2 Game (per pawn; `src/mohavr/offhand.cpp`)

| Event / tick | Accepted when | Effect | Log |
|---|---|---|---|
| TAKE(t) | None, available, a G of t (or any) with count >= 1 | count out one; LastGrenadeWeapon = G; Held | `offhand: TAKE frag -- reserve 4 -> 3` |
| PIN | Held | Armed (+ pin cue) | `offhand: PIN (frag)` |
| COOK | Armed | Cooking; cookStart = TimeSeconds (+ cook cue) | `offhand: COOK -- fuse 4.0 s` |
| THROW(p, v, f) | Armed / Cooking | 3.5; None | `offhand: THROW frag 6.2 m/s -> MOHAProj_MKII_3 (pooled) from the hand, fuse 2.3 s; the gun stays MOHAStg44_0` |
| PUTBACK | Held / Armed (Cooking -> THROW toss) | count back; None | `offhand: PUT BACK frag -- reserve 3 -> 4` |
| (tick) | Cooking | countdown cues, `nadeTicks`++; fuse out -> BOOM | `offhand: went off in the hand` |
| new pawn | -- | None; `nadePawnSeq`++ | `offhand: a new pawn -- state reset` |
| anything else | -- | refused, acked | `offhand: PIN refused -- nothing held` |

### 6.3 Ordering, acks, overrun

Host: an event is written into `nadeEvt[seq % 8]` (+ its position/velocity) and then `nadeEvtSeq` bumped, never while
`nadeEvtSeq - nadeEvtAck >= 8` (held, dropped after 250 ms with a log). Game: processes in order each Draw; on
`nadeEvtSeq - seen > 8` logs, skips to the newest, acks; the reconcile settles the state (the reload's rules, RELOAD-DESIGN 4).

---------------------------------------------------------------------------------------------------------------------

## 7. Shared block v20 (`src/common/shared_frame.hpp`)

`kVersion` 19 -> **20** ("20: the off-hand grenade"). Appended after `pad18` (sizeof 1784 today, `shared_frame.hpp:289`);
4-byte fields only, so x86 = x64.

```cpp
    // --- v20: the off-hand grenade ([OffHand] Grenade; src/host/offhand.cpp, src/mohavr/offhand.cpp; DESIGN.md) ---
    // game -> host, once per Draw (seqlock nadeSeq: odd while the game writes)
    volatile std::uint32_t nadeSeq;        // 1784
    std::uint32_t          nadeCaps;       // 1788 bit0 installed, bit1 available now, bit2 infinite ammo, bit3 carrier ready
    std::int32_t           nadeCount[3];   // 1792 frag, Gammon, stick: what a TAKE can get (-1 not carried, 99 infinite)
    std::uint32_t          nadeNext;       // 1804 the type a TAKE of "any" gives (0 frag, 1 Gammon, 2 stick, 0xFF none)
    std::uint32_t          nadeState;      // 1808 bits 0-1: 0 none, 1 held, 2 armed, 3 cooking; bits 2-3 the type; bits 8-15
                                           //      the last refusal (0 none, 1 unavailable, 2 empty, 3 no weapon, 4 spawn failed)
    float                  nadeFuse;       // 1812 seconds left while cooking (game time), else 0
    float                  nadeFuseLen;    // 1816 the held type's FuseTime
    volatile std::uint32_t nadeTicks;      // 1820 +1 per countdown tick the game plays (the haptic tick)
    volatile std::uint32_t nadeEvtAck;     // 1824 the last event taken (applied or refused)
    volatile std::uint32_t nadePawnSeq;    // 1828 +1 on a new local pawn
    volatile std::uint32_t nadeBoom;       // 1832 +1 when a grenade went off in the hand
    // host -> game, per XR frame INSIDE the view seqlock (viewSeq), read with the hands (RELOAD-DESIGN X3)
    std::uint32_t          nadeFlags;      // 1836 bit0 on, bit1 held, bit2 pin out, bit3 cooking, bits 4-5 the type, bit6 frozen
    Pose                   nadePose;       // 1840 the off hand's hand point (LOCAL; orientation = its aim pose)
    float                  nadeAdj[6];     // 1868 the held type's hold: forward, up, right (cm), tilt, turn, roll (deg)
    // host -> game events, ordered (the reloadEvt pattern)
    volatile std::uint32_t nadeEvtSeq;     // 1892
    std::uint32_t          nadeEvt[8];     // 1896 low byte: 1 TAKE, 2 PIN, 3 COOK, 4 THROW, 5 PUTBACK; bits 8-15 the type
                                           //      (0 frag, 1 Gammon, 2 stick, 0xFF any); bits 16-23 flags (bit16 toss)
    float                  nadeEvtPos[8][3];  // 1928 THROW: the release point (LOCAL, m)
    float                  nadeEvtVel[8][3];  // 2024 THROW: the release velocity (LOCAL, m/s)
};                                         // sizeof 2120 (a multiple of 8)
```

Static asserts: `nadeSeq` 1784, `nadeCount` 1792, `nadeState` 1808, `nadeEvtAck` 1824, `nadeBoom` 1832, `nadeFlags` 1836,
`nadePose` 1840, `nadeAdj` 1868, `nadeEvtSeq` 1892, `nadeEvt` 1896, `nadeEvtPos` 1928, `nadeEvtVel` 2024, `sizeof(Header)` 2120.
Helpers: `enum NadeEvent { kNadeTake = 1, kNadePin, kNadeCook, kNadeThrow, kNadePutBack }`, `kNadeFrag/Gammon/Stick/Any`;
`struct NadeGeo` + `ReadNadeGeo(h, g, seq)` (the ReadReloadGeo pattern); `struct NadeView {flags; Pose pose; adj[6]}` read by
`ReadHands(h, hand, valid, rv, NadeView* nv = nullptr)` in the same pass. `throwSeq`/`throwVel` (v10) stay the gun hand's.
The host refuses a version mismatch (`main.cpp:167`); both binaries ship together.

---------------------------------------------------------------------------------------------------------------------

## 8. Ini switches

New `[OffHand]` in `config/MOHAVR.ini` (the host reads it like `ManualReload::Init`; the game's `Config` gets the keys it
uses); the menu toggle saves the player's `[OffHand] Grenade` in `MOHAVR.user.ini`.

| Key | Default | Meaning |
|---|---|---|
| `Grenade` | **0** until Phase 1 passes [S]; then 1 for the headset round | the off-hand grenade |
| `Spot` | `holster` | `holster` / `chest` (Phase 6) |
| `Pin` | `trigger` | `trigger` / `auto` / `teeth` (Phase 6) |
| `Throw` | `grip` | `grip` / `trigger` |
| `Cook` | `spoon` | `spoon` / `pin` / `off` |
| `ThrowScale` | 2.2 | the off hand's release speed x |
| `Estimator` | `mean` | `mean` / `peak` |
| `MinThrowSpeed` | 1.0 | m/s; slower = SlowRelease |
| `SlowRelease` | `toss` | `toss` / `drop` |
| `MaxHandSpeed` | 12 | m/s clamp |
| `SpawnAt` | `hand` | `hand` / `eye` / `drawn` (Phase 3) |
| `PassThrower` | 1 | the thrown grenade ignores the thrower's body |
| `HudType` | 1 | LastGrenadeWeapon = the held type |
| `Carrier` | 0 until Phase 3 [S] | draw the grenade in the hand |
| `ThrownScale` | 1.5 | the flying grenade's draw scale (the game's) |
| `Sounds` | 1 (Phase 4) | pin, throw, cooking cues |
| `Haptics` | 1 | the off hand's pulses |
| `[Debug] OffHandTrace` | 0 | per-event and per-frame off-hand logs (tests) |

---------------------------------------------------------------------------------------------------------------------

## 9. The minimal spikes (implement first)

### 9.1 S1 [S]: the inactive grenade throws while the gun stays in hand (time box 2 h, rule 9)

**What to build** (no host change, no shared-block change, no new address):
- `src/mohavr/offhand.cpp/.hpp` with `bool TestCommand(const wchar_t* line)` and `void OnDraw()` (the follow); a local
  SEH wrapper identical to `reload.cpp:544-551` (the shared helper comes in Phase 1); `reload::StateName` exposed (or
  copied: `reload.cpp:1931-1937`).
- `vr_view.cpp RunTestCommands` (:383-436): before `gexec::Run`, `if (!wcsncmp(line, L"mohavr nade", 11)) {
  offhand::TestCommand(line); continue; }` (the `upgradelevel` pattern); `offhand::OnDraw()` after `reload::OnDraw` (:508).
- Gated by `[Debug] GameCommands=1` only (tests).

**Commands** (`python tools/game_cmd.py mohavr nade ...` -> `%TEMP%\MOHAVR\game_cmd.txt`):
- `mohavr nade` -- dump: P, W (class, `StateName`), `P.FlashCount`, `IM.PendingFire[0..1]`; per type: G's name/class,
  `CurrentFireMode`, `AmmoCount[0]`, `bInfiniteAmmo`, reserve/cap, `Instigator`, `StateName(G)`, the bStasis bit (+0x74
  0x80), vtable+0xF0 == 0x10DB1FA0; SpawnProjectile's UFunction: FunctionFlags (+0x8C), iNative (+0x90), ParmsSize
  (+0x9E), ReturnValueOffset (+0xA0), the parm offsets of `vStartPos`, `vForward`, `ReturnValue`; `WI.ProjectilePool`
  (each entry's class name and its 3 slots); `MOHAHUD(C.myHUD).GrenadeAmmoCount`.
- `mohavr nade throw <frag|gammon|stick|any> <hand|eye> <vx> <vy> <vz> [fuse]` -- velocity in LOCAL m/s (as `throwvel`),
  x `[Hands] ThrowScale`; `hand` = the controller that does not hold the gun (`hdr->gunFlags` bit2 = gun left -> the right),
  its aim pose from `shared::ReadHands` mapped with `view::PoseToWorld`; `eye` = `view::GameCamera`. Runs 3.5 steps 0-11
  with these deviations: the grenade is counted out **after** a successful launch (no TAKE in S1); step 1's wall trace is
  logged but S1 also accepts an unclamped start (to see the pooled SetLocation behaviour).
- `mohavr nade boom <type>` -- 3.6 BOOM at the off hand (run `God` first; checks `Explode` through ProcessEvent).

**The log lines that prove it** (`offhand:` prefix; values illustrative):
```
offhand: probe -- pawn MOHASingleplayerPawn_0, weapon MOHAStg44_0 (state Active), FlashCount 7, PendingFire 0 0
offhand: probe -- frag MOHA_MKIIFragGrenade_0: mode 0, clip 0, infinite 0, reserve 4/8, instigator MOHASingleplayerPawn_0,
         state Inactive, stasis 1, ProcessEvent 0x10DB1FA0 ok; SpawnProjectile flags 0x00020102 native 0 parms 28 ret @24
offhand: probe -- pool: MOHAProj_MKII [MOHAProj_MKII_0, -, -]
offhand: SpawnProjectile frag from 1234 -567 890 (the left hand, 38 units from the eye) dir 0.00 0.93 0.35 ->
         MOHAProj_MKII_0 (pooled), owner MOHA_MKIIFragGrenade_0, instigator MOHASingleplayerPawn_0, controller
         MOHAPlayerController_0, enabled 1, 0.0 units from the start
offhand: launched MOHAProj_MKII_0 -- DrawScale 1.0 -> 1.5, MOHAMKIICookedGrenadeDamageType, fuse 4.00 s, light 1,
         velocity 0 1760 660 (pawn 0 0 0)
offhand: after -- weapon MOHAStg44_0 (state Active) unchanged, FlashCount 7 unchanged, PendingFire 0 0 unchanged;
         frag reserve 4 -> 3; HUD GrenadeAmmoCount 4 -> 3
offhand: 1 s later MOHAProj_MKII_0 at ..., velocity 0 1760 -420
offhand: MOHAProj_MKII_0 exploded 4.0 s after the throw at ...
```

**Test script** `work/research/tests/nade0.ps1` (the r35.ps1 pattern):
```powershell
param([string]$Tag = 'nade0')
Set-Location C:\Users\j_tom\Projects\MOHAVR
$set = @('Render.ResX=0','Render.ResY=0',"OpenXR.RuntimeJson=$PWD\tools\OpenXR-Simulator\bin\openxr_simulator.json",
         'Debug.GameCommands=1')
& .\tools\deploy.ps1 deploy -Set $set | Out-Null
& .\tools\harness.ps1 launch 2>&1 | Select-Object -Last 1
& .\tools\harness.ps1 to-gameplay 2>&1 | Select-Object -Last 1
$tmp = Join-Path $env:TEMP 'MOHAVR'
function Cmd([string]$c) { [IO.File]::WriteAllText((Join-Path $tmp 'game_cmd.txt'), "$c`n"); Start-Sleep -Milliseconds 800 }
function Pad([string[]]$s) { python tools\pad_cmd.py --seq @s | Out-Null }
function Shot([string]$n) { & .\tools\harness.ps1 shot "$Tag-$n" 2>&1 | Select-Object -Last 1 }
Start-Sleep -Seconds 12
Pad @('hand=r,0.10,-0.12,0.45,-80,0,0', 'hand=l,-0.25,-0.15,0.35,0,0,0')   # the gun ahead, the off hand to the left
Cmd 'EnableCheats'; Cmd 'GiveWeapon MOHAGameNonNative.MOHAStg44'; Cmd 'GiveWeapon MOHAGameNonNative.MOHA_MKIIFragGrenade'
Cmd 'GiveAmmo'; Cmd 'SwitchPrimary'; Start-Sleep -Seconds 2          # a gun in hand (check the viewmodel line)
Cmd 'mohavr nade'
Cmd 'mohavr nade throw frag hand 0 3 -8'; Start-Sleep -Seconds 1; Shot 'flying'; Start-Sleep -Seconds 5
Cmd 'mohavr nade throw frag hand 0 3 -8'; Start-Sleep -Seconds 6      # the pooled path (the first one exploded)
Cmd 'mohavr nade throw frag eye 0 3 -8';  Start-Sleep -Seconds 6
Pad @('raw=1 rt=1 dur=0.3', 'dur=0.5'); Start-Sleep -Seconds 2         # the gun still fires: "shot N:" lines
Cmd 'mohavr nade'
& .\tools\harness.ps1 quit 2>&1 | Select-Object -Last 1
& .\tools\deploy.ps1 undeploy | Out-Null
```
(Copy `MOHAVR.log` before any relaunch, rule 11. If `GiveWeapon` equips the grenade, `SwitchPrimary` brings the gun back:
the `viewmodel: weapon in hand: 'Attachment_Stg44' -- long gun` line must precede the throws.)

**Pass:** for each of the three throws: a `MOHAProj_MKII` with owner = the frag weapon, instigator = the pawn, controller =
the player controller, enabled, within 2 units of the start; it flies (1 s later: horizontal unchanged, vertical down
~1080 units/s^2) and explodes ~4.0 s after; weapon, gun state, FlashCount and PendingFire unchanged; reserve and HUD count
-1 each; the 2nd throw reports "pooled"; no `throw:` lines for these; `shot N:` lines afterwards; the harness quits cleanly.
**Also record** (answers for the design): pooled vs fresh, DrawScale before/after (proves an unnumbered native through
ProcessEvent), the ReturnValue path, the pool before/after.

**If it fails:** ReturnValue 0 with a projectile visibly spawned -> read the newest pool change / class instance instead and
log ParmsSize/ReturnValueOffset; FlashCount changed -> something reached IncrementFlashCount (stop); no projectile ->
check `CurrentFireMode`, `WeaponProjectiles[0]`, the function flags; SetDrawScale/CreateLight faults -> drop them
(cosmetic). A NO-GO for SpawnProjectile itself -> fallback B (ProjectileFire with a one-call `Pawn.Weapon = G` write,
`PhysicalStartFireOverride = start`, `ThrowStrength = 1`, no active 'Timer' on G; then the same Velocity/fuse writes), and
only then Mode B. Record the result in ENGINE-NOTES 5bb either way.

### 9.2 S2 [S]: the carrier (time box 2 h; can run in parallel after S1)

- `addresses.hpp`: `kObjectProcessEvent = 0x109CE980` + bytes `55 8B EC 6A FF 68 08 94 1B 11 64 A1 00 00 00 00`, in
  `kSignatures` (rule 4).
- `mohavr nade carrier <frag|gammon|stick|off>`: 4.2 steps 1-5 for that type (`off`: collapse); the bake branch with a fixed
  `Gw` = the off controller frame (`HandFrames`) moved 6 cm forward (no grip yet).
- **Pass:** `offhand: carrier -- cloned <name> (MOHASkeletalMeshComponent, US_FragGrenade, outer MOHA_MKIIFragGrenade_0)`;
  `attached to FPArms at Camera (Attachments n -> n+1)`; `armik: baked the move into <clone> (the off-hand grenade, N
  bones)` and its bakes per Draw equal to the arms' (2 standing, ~6.7 moving, 5am); a `sim_shot` with the grenade at the left
  hand, lit, LOD 0, in front of the fingers where it should be; the gun, its key (`weaponKey`) and the aim unchanged; 60 s
  plus `Suicide` (a new pawn) without a fault; left-hand mode drawn mirrored and solid.
- **If Clone or AttachComponent misbehave:** try the weapon's own `DroppedPickupMesh` attached directly (no clone), then the
  pawn's `ITPClipMesh` with `SetSkeletalMesh`; else ship C3 (fingers only) and say so in the round.

---------------------------------------------------------------------------------------------------------------------

## 10. Implementation plan (one commit per phase; STATUS / ENGINE-NOTES / DECISIONS in the same commit, rule 10)

| # | Step | Tag | Files | Proof |
|---|---|---|---|---|
| 0a | Spike S1 (9.1) | [S] | `src/mohavr/offhand.cpp/.hpp` (new), `vr_view.cpp`, `reload.hpp` (StateName) | nade0 logs per 9.1; ENGINE-NOTES 5bb (measured), 5af un-parked + corrected (0.12 s notify; UObject vs AActor ProcessEvent) |
| 0b | Spike S2 (9.2) | [S] | `addresses.hpp`, `offhand.cpp`, `arms_ik.cpp` (branch, 6 slots), `viewmodel.cpp` (skip pointer) | the S2 logs and pictures |
| 1 | Contract + host state machine + executor, no mesh (C3 without fingers) | [S] | `shared_frame.hpp` (v20), `src/host/offhand.cpp/.hpp` (new), `hands.cpp/.hpp` (press order, tracked bits, grip active, hand-point history, offBusy, masks, targets[8]), `reload.hpp/.cpp` host (`In::offBusy`), `pad.cpp/.hpp` (`nade=`, `@grenade`, `swing=`, RB mask, the grip's isActive), `main.cpp` (Poll/Send, the view-seqlock fields, masks, Gun hand toggle), `menu.cpp/.hpp` (Weapons tab item), `src/mohavr/script_call.cpp/.hpp` (from reload.cpp), `offhand.cpp` (3.3-3.9), `config.cpp/.hpp`, `config/MOHAVR.ini` ([OffHand] Grenade=0), `tools/pad_cmd.py` / `game_cmd.py` docstrings | [S] tests T1-T13 (11.1); `tools/harness.ps1 cycle` OK with Grenade=0 and =1; D40, ENGINE-NOTES 5bb, STATUS |
| 2 | The hand: grenade grips, left-hand mode | [S] | `tools/reload_grips.py` (GRENADES), `reload_grips.inc` (regenerated), `offhand.cpp` (`HoldFingers`), `arms_ik.cpp` (:485) | sim_shot of the closed hand in both hand modes; the grips' rows match draw-in-hand 8.5 |
| 3 | The carrier (S2 promoted): per type, collapse rules, `SpawnAt=drawn`, the hold adjustment, `ThrownScale` | [S] | `offhand.cpp`, `arms_ik.cpp`, `viewmodel.cpp`, `script_call.cpp` (component path), host `menu.cpp` (Grenade hold page -> `nadeAdj`, saved `[OffHandGrip] Attachment_*Grenade=`), ini `Carrier` | pictures held / pin out / cooking / released; the log of the drawn centre vs the projectile's first position (<= 2 units); lifetime: death, level load, a checkpoint save/load while held |
| 4 | Feedback: cues, haptics, HUD flash, the throw stat | [S] | `offhand.cpp` (cues via PlaySoundAt, ticks, OnWeaponFire via UObject::ProcessEvent), host `offhand.cpp` (pulses), `reload.cpp` (PlayCue generalised: a location) | `reload: sound ... is playing`-style checks (Debug), the host pulse log, nadeTicks cadence 0.25 / 0.1 s |
| 5 | Headset round 43 with `Grenade=1`, `Carrier=1` and the defaults | [H] | `config/MOHAVR.ini` defaults; HEADSET-TESTS round 43 | the player's verdict (11.2) |
| 6 | Options after the round: `Spot=chest`, `Pin=teeth`, `Estimator=peak` / a speed curve, X cycles the held type, a throw-help cone | [S] then [H] | host `offhand.cpp`, `hands.cpp` (6th spot; RELOAD-DESIGN 3.7 corr X8's list of kSpots sites), `markers.cpp`, `menu.cpp` | per option |
| 7 | Extension: pick up a live (enemy) grenade and throw it back (the player's backlog) | research [S] | `offhand.cpp`: grip near a resting live grenade -> `MOHAGrenadeCSA.UsedBy(P)` (an event: Instigator, InstigatorController, the kicked type, PHYS_Falling) then the hand's Velocity; Held/Armed reused | its own spike |

---------------------------------------------------------------------------------------------------------------------

## 11. Tests

### 11.1 [S] (Phase 1-4; deploy with `Render.ResX=0 Render.ResY=0`, the x64 simulator json, `Debug.GameCommands=1`,
`OffHand.Grenade=1`, `Debug.OffHandTrace=1`; a gun given and drawn first)

- T1 take / put back: `hand=l,@grenade`, `raw=1 press=lgrip dur=1.5`, hand to the view, release -> TAKE then PUT BACK, the
  reserve back, **no SwitchGrenade and no weapon change** in the game log; picture while held.
- T2 pin + throw with a test velocity: `throwvel=0,3,-8`, then the queued raw states `raw=1 press=lgrip dur=1` (take),
  `raw=1 press=lgrip lt=1 dur=0.3` (pin), `raw=1 press=lgrip dur=0.5`, `dur=0.5` (release) -> THROW, the projectile
  velocity (x ThrowScale), count -1, `1 s later`, the explosion ~4 s later, FlashCount/state/PendingFire unchanged.
- T3 a real swing: `swing=l,0,0.3,-0.6,150` then release -> the host's release speed ~4.5 m/s (mean) and the peak logged.
- T4 cooking: as T2 with a second `raw=1 press=lgrip lt=1 dur=0.3` (the spoon) and `raw=1 press=lgrip dur=2` before the
  release -> fuse ~2.0 s left at the throw; over-cook (hold 5 s) with `God` -> BOOM, the host to None.
- T5 slow release with the pin out -> "toss" (750 units/s along the view).
- T6 conflicts: the foregrip then a take (log order); B drops the magazine while holding; the MP40 trigger near its
  magazine while holding (only PIN); the M12 pump gated until thrown; a pistol; the main weapon a grenade -> the holster's
  SwitchGrenade as today; X while holding -> masked (pad log); the gun hand at the grenade holster while holding -> refused.
- T7 left-hand mode: the gun drawn left, the right hand takes and throws; pictures (Phase 2/3).
- T8 menus: holding, `menu_cmd.py toggle`, `lt=1` selects menu items but no PIN; closed -> still held; the game's pause
  (`buttons=start`) -> frozen and the fuse stopped; release while paused -> the rule at close.
- T9 tracking: `lost=l` while armed, release -> nothing; `lost=none` -> carries on.
- T10 off: `OffHand.Grenade=0` -> the left hip behaves exactly as before; `harness.ps1 cycle` OK.
- T11 afterwards: the gun fires (`shot N:`); a gun-hand grenade throw with the game's own path still works on the pooled
  projectile (`throw: grenade ...`).
- T12 the thrower: a release behind the head with forward velocity passes the body (`PassThrower=1`; 1 s later ahead).
- T13 a wall: the off hand's test pose pushed through a wall -> `blocked`, spawned on the player's side.

### 11.2 [H] round 43 (HEADSET-TESTS)

The spot (left hip vs chest); pin by trigger vs auto (vs teeth, later); throw on grip release vs trigger release (both
deployable through the menu in one round); cooking by the 2nd squeeze -- found, used, safe?; the off hand's strength
(logged speeds, mean vs peak) and accuracy; the slow-release toss vs a drop; the grenade's look in the hand (position,
mirrored ring) and the pop to 1.5x when it flies; the countdown haptics; whether firing the gun while holding a grenade
matters; the MOHAVR menu while cooking (fuse keeps burning); X cycling the held type (wanted?); throwing back enemy grenades
next?

---------------------------------------------------------------------------------------------------------------------

## 12. Risks and mitigations

| Risk | Mitigation |
|---|---|
| A script-side precondition of SpawnProjectile on an inactive weapon misbehaves at runtime | Spike S1 first; fallback B, then Mode B (2.2). |
| The return value isn't read correctly through CallProcessEvent (the mod's first non-void call) | 1.1 verified the Result pointer; S1 logs ParmsSize / ReturnValueOffset and cross-checks with the pool before/after. |
| The first unnumbered native through ProcessEvent (SetDrawScale, later Clone/AttachComponent) misbehaves | S1 logs DrawScale before/after; SetDrawScale and CreateLight are cosmetic and can be dropped; S2 covers Clone/Attach with fallbacks (9.2). |
| A pooled projectile's SetLocation fails for a hand start | The eye->hand trace clamp; the location check, RecycleProjectile, one retry at the eye (the game's own start); undo the take on a second failure. |
| The grenade bounces off the thrower | `PassThrower=1` (verified native read, 0x10DB44E0); T12. |
| An accidental throw at the feet (an opened hand, a lost or sleeping controller) | The pin step (Held is safe); the toss rule for slow releases; frozen while untracked or the grip inactive; the 12 m/s clamp. |
| A cooking grenade during the MOHAVR menu goes off | Documented; [H] question; the menu freezes input but not game time. |
| The off hand's existing uses (pouch, foregrip, MP40/M12 triggers, menu select) | Press order, masks, offBusy, frozen in menus (5.5); T6, T8. |
| HUD shows another type's count | `HudType` (LastGrenadeWeapon = G); T1 logs `MOHAHUD.GrenadeAmmoCount`. |
| Kill credit / experience missing | Credit comes from the damage type via InstigatorController (verified script); SpawnProjectile sets it; [R] one kill in T2's level logs the grenade experience. |
| The rifle grenades share the frag/stick pools | Counted out at TAKE. |
| The carrier's lifetime (GC, death, level change, a checkpoint save serialising `Attachments`) | One clone per pawn and type, the pointer dropped on a new pawn; Phase 3 test with a save/load while held; fallback: detach outside holds. |
| Bake bookkeeping capacity (4 slots) | 6 slots for `g_saved` / `g_bakedComp`. |
| The game's own later throw reuses a projectile the mod modified | The game's ProjectileFire rewrites DrawScale, damage type, fuse, velocity; only `bBlockedByInstigator` stays clear (harmless). |
| Left-hand mode | Mirror-world frames drawn through the mirror (as the arms); the spawn point un-mirrored; T7. |
| Stasis assumption wrong (the inactive weapon does tick) | Irrelevant: the design never uses G's Tick, states or timers. |
| Per-frame cost | No per-frame ProcessEvent: IsWeaponDisabled at 4 Hz, calls only at events and countdown ticks. |
| Version skew of the shared block | The host refuses a mismatch; build.ps1 builds both. |

---------------------------------------------------------------------------------------------------------------------

## 13. Load-bearing claims (each checkable)

1. `UObject::ProcessEvent` (0x109CE980) refuses only: no FUNC_Native|Defined, a probe name masked by the state, pending kill,
   **iNative != 0** (0x109CE9FF), a remote native -- nothing about weapon state. [V]
2. It writes a function's return value at `Parms + ReturnValueOffset (UFunction +0xA0)` (0x109CEB38). [V]
3. `EALAWeapon.SpawnProjectile` reads only CurrentFireMode, WeaponProjectiles, the WorldInfo pool, Instigator,
   Instigator.Controller, and spawns with Owner = the weapon, Instigator = its Instigator (`EALAWeapon.uc:423-449`;
   execSpawn 0x10CE8F80 [V-agent]). [V]
4. An inventory weapon's Instigator is the pawn (`Inventory.uc:104`). [V]
5. `EALAWeapon.ProjectileFire` begins with `IncrementFlashCount()`, which plays the *active* weapon's fire effects
   (`EALAWeapon.uc:397`; `Pawn.uc:423-434, 504-513`). [V / V-agent]
6. An inventory weapon not in the hand doesn't tick (0x10F0CCF0, 0x10B38042, `EALAWeapon.uc:1738`). [V]
7. A pooled projectile is re-armed by Init (InitProj: shown, PHYS_Falling, enabled) and launched at `fSpeed` along
   vForward; the mod's Velocity write after it is what flies (`MOHAProj_Explosive.uc:90-155`, `Projectile.uc:52-57`). [V]
8. `SetFuseTime` sets the projectile's 'Timer' -> `Explode` -> `HurtRadius(InstigatorController)` -> RecycleProjectile. [V]
9. The cooked damage types differ from the plain ones only for stats (`IsCookedGrenadeKill`). [V]
10. Grenade kill credit uses `DamageType.WeaponType` with the projectile's InstigatorController
    (`MOHAAIPawn.uc:1241-1275`). [V-agent]
11. The pool only ever holds exploded projectiles (FindProjectileInPool 0x10F0D720 clears the slot; AddProjectileToPool
    only from RecycleProjectile). [V-agent]
12. `AProjectile::IgnoreBlockingBy` (0x10DB44E0) ignores the Instigator only when `bBlockedByInstigator` is clear. [V]
13. Components' `+0xF0` is `UObject::ProcessEvent` (0x109CE980); their remote-function slot returns 0. [V]
14. `Object.Clone` and `SkeletalMeshComponent.AttachComponent` are natives without an index (iNative 0) and
    `execAttachComponent` defaults RelativeScale only when the parameter is skipped. [V decl / V-agent cooked data]
15. The grenade weapon's `DroppedPickupMesh` is a MOHASkeletalMeshComponent with the first-person grenade mesh. [V]
16. The arms' grenade idles hold the grenade rigidly in the right hand: frame 0 is the grip (draw-in-hand 8.1). [V-agent]
17. The held magazine is placed at inv(grip) x FreeHandRel x off (`reload.cpp:1738-1760`). [V]
18. The bake MidHook takes FOV != 0 parts whose Outer isn't the pawn for the gun (`arms_ik.cpp:729, 770`). [V]
19. `IsWeaponDisabled` is true in the crawl stance change, LadderClimb, Death_In, ITPJump and the airdrop states. [V]
20. `LastGrenadeWeapon` is read only by SwitchGrenade and GetHUDGrenade. [V / V-agent]
21. The off grip and trigger reach no game control while a gun is held, in both hand modes (`MOHAVR.ini:241-243` LB/LT none;
    `pad.cpp:268-271`). [V-agent, consistent with the ini]
22. `RunTestCommands` and `Hook_Draw`'s pre-draw run on the game thread after the world tick, outside GC
    (`vr_view.cpp:383-436, 497-508`; native-engine 5.1-5.2). [V / V-agent]

---------------------------------------------------------------------------------------------------------------------

## 14. Open questions

For the player [H] (round 43): see 11.2. In short: spot, pin, throw input, cooking, strength, slow release, the look in
the hand, the release pop, the menu-while-cooking rule, X, throw-backs next, a second gun at all (section 15).

Runtime [R] (answered by S1, S2, Phase 1 tests):
- SpawnProjectile through ProcessEvent on the inactive weapon: owner/instigator/controller, the pool, no gun effects (S1).
- The ReturnValue read; an unnumbered native (SetDrawScale) through ProcessEvent (S1).
- How often a pooled SetLocation fails for hand starts near walls (S1 + T13).
- The HUD grenade count and the type it shows with `HudType` (T1).
- Grenade experience from an off-hand kill (T2's level, if an enemy is near; else a later mission).
- Clone + AttachComponent, the bake count, lifetime, a save/load while held (S2, Phase 3).
- The stick/Gammon pin cue names in `BuildCues` (Phase 4 log).
- The Quest's grip value while tracking is lost (headset log of `pad: the left hand lost tracking` against the grip).
- Whether the Gammon explodes on impact in MOHA (`bExplodeOnImpactWithPawn` false by default; a point-blank throw at a
  wall: explosion time vs FuseTime) -- if it does, `Cook` makes no sense for it.

---------------------------------------------------------------------------------------------------------------------

## 15. Beyond grenades: a second gun ("true" dual wield)

Not designed here. What this design proves applies: an inactive weapon's script functions can be driven through
ProcessEvent, and a cloned first-person mesh can be drawn in the off hand. A second *firing* gun would further need: its
shot (its `InstantFire`/`ProcessInstantHit` path without `IncrementFlashCount`, i.e. the trace and damage done by the mod
or by a one-call `Pawn.Weapon` swap), its own muzzle flash, brass and sounds (today all bound to the active weapon's
attachment), a second aim line and red dot, its ammo (and the two-handed manual reload replaced by a one-handed one), and
both arms posed as gun arms. Engine cost: high (5af B4). Recommendation: ship the off-hand grenade first; revisit pistols
only if the player asks after round 43.

---------------------------------------------------------------------------------------------------------------------

## 16. File-by-file change list (all phases)

| File | Change |
|---|---|
| `src/common/shared_frame.hpp` | v20 (section 7), asserts, kVersion 20, helpers |
| `src/host/offhand.hpp/.cpp` (new) | `OffHandGrenade`: Init ([OffHand]), SetOn, Poll (caps, counts, state, acks, ticks, boom, pawnSeq; reconcile), TakePress(type), Frame (6.1), Send (ring), Queue (tests), Flags, HoldPose, Adj |
| `src/host/hands.hpp/.cpp` | SetOffHand; Input: tracked bits, grip active; the press order (5.5); hand-point history + Estimator; foregrip pulse/ring suppressed while held; refuse the gun hand's grenade holster; targets[8] (`@grenade`) |
| `src/host/reload.hpp/.cpp` | `In::offBusy` (rings and press candidates off) |
| `src/host/pad.hpp/.cpp` | `nade=` with `take[:type]`, `pin`, `cook`, `throw[:vx,vy,vz]`, `toss`, `putback`; `hand=l,@grenade`; `swing=h,dx,dy,dz,ms`; `SetMaskedButtons`; the grip's isActive |
| `src/host/main.cpp` | own OffHandGrenade; Poll before hands.Update, Send after; nadeFlags/nadePose/nadeAdj in the view seqlock; tracked bits before HoldLost; masks; the Gun hand toggle only when nothing is held |
| `src/host/menu.hpp/.cpp` | Weapons tab "Off-hand grenade"; Phase 3 Grenade hold page; Phase 6 options and the chest spot |
| `src/host/markers.hpp/.cpp` | Phase 6: one more spot |
| `src/mohavr/offhand.hpp/.cpp` (new) | the executor (section 3), test commands (9.1-9.2), HoldFingers, the carrier and its bake branch helper |
| `src/mohavr/script_call.hpp/.cpp` (new) | the ProcessEvent helper (3.2), used by reload.cpp too |
| `src/mohavr/reload.hpp/.cpp` | use script_call; expose ReserveOf/StateName/PlayCue-at-a-location for offhand |
| `src/mohavr/arms_ik.cpp` | HoldFingers at :485; the clone branch before :729; 6 slots |
| `src/mohavr/viewmodel.cpp/.hpp` | read NadeView with the hands; skip the clone in NotePart/UpdateWeaponKey; export g_noGunDrawn |
| `src/mohavr/vr_view.cpp/.hpp` | `offhand::OnDraw(hdr)` after reload::OnDraw; `mohavr nade` in RunTestCommands; export g_landingHeld |
| `src/mohavr/addresses.hpp` | Phase 3 (S2): `kObjectProcessEvent` + bytes in kSignatures |
| `src/mohavr/config.hpp/.cpp` | [OffHand] keys the game uses; `[Debug] OffHandTrace` |
| `tools/reload_grips.py`, `src/mohavr/reload_grips.inc` | GRENADES (4.1) |
| `config/MOHAVR.ini` | [OffHand] (section 8) |
| `tools/pad_cmd.py`, `tools/game_cmd.py` | docstrings |
| docs | DECISIONS D40; ENGINE-NOTES 5bb (+ 5af corrected); STATUS (backlog "dual wielding" -> in progress); HEADSET-TESTS round 43; PLAN / ROADMAP item |
