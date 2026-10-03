# Scopes you raise to your eye: design (2026-10-03)

The player's request (2026-10-03): "I don't want scopes to be a button press that brings up an overlay. I'd like scopes to
feel natural, you bring them up to your eye and can see through them." Then, choosing the third-view option: "Make both
the game's value and realistic a toggle in menu. Two hands needed to use scope."

Research: `work/research/scope/` (game-scopes, pipeline, rtt, prior-art, synthesis). D50.

## 1. The game's scopes

| Gun | Scoped | The game's zoom | Real scope | Scope part (bone) |
|---|---|---|---|---|
| Springfield | every level (not in grenade mode) | 10..40 deg (9.6x..2.3x) | M73B1 2.5x, crosshair | `upgrade_02_hide_scope` |
| G43 | from level 1 (not in grenade mode) | 10..40 deg (the ini's 25..50 is dead: measured) | ZF4 4x, post and bars | `upgrade_02_scope` |
| StG44 | from level 2 (mounted ~3 s after the switch) | 35 deg fixed | ZF4 4x, post and bars | `altFire_scope` |
| M18 | always | 40 deg (Enhanced Scope 30..58) | Telescope M86C 2.8x | a piece of the body bone RootOffset: the telescope measured (D53) |

The magnification of a game FOV is tan(40 deg) / tan(FOV / 2) against its 80 deg view. The game's own scope (iron sights
state 3: hide the gun, a full-screen reticle texture, a narrow world FOV, blur, sway) never runs in VR: the aim button is
unmapped and the mod sets every view's projection itself.

## 2. The design, as built

### 2.1 The split
- **The scope view is a third view the game renders**: the stereo Draw's third "player" (the eyes are the first two), into
  a square column at the backbuffer's right, taken from the eyes' width **only while a scope is at an eye** (no cost
  otherwise; the backbuffer, the window and the menus' shape never change).
- **The host decides when, from where and how wide**: the gate, the magnification and the camera, written into the view
  seqlock (`scopeWant`, `scopeTanHalf`, `scopeCamera`).
- **The host shows it on the drawn eyepiece**: a one-eye quad layer (the lens), drawn with a small D3D11 pass.

### 2.2 Where the scope is (game side, `src/mohavr/scope.cpp`)
- `tools/scope_points.py` measures each scope's tube from the gun meshes (umodel psk exports, the melee tool's reader):
  the sections square to the bore, kept to their top ring (the tube, not its mount); the eyepiece and objective centres
  and the eyepiece's radius, in mesh space and in the scope bone's own frame -> `scope_points.inc`.

  | Gun | Eyepiece (mesh) | Objective | Radius | Above the bore |
  |---|---|---|---|---|
  | Springfield | (0, -12.14, 0.76) | (0, -12.14, 33.12) | 1.29 | 4.7 |
  | G43 | (0, -13.79, -2.10) | (0, -13.79, 13.33) | 1.25 | 5.6 |
  | StG44 | (0, -16.57, -3.65) | (0, -16.57, 11.69) | 1.14 | 8.3 |

- Per Draw: the gun in hand's attachment class and upgrade level; `IsScopeEnabled()` (covers grenade mode and the StG44's
  mount); the game's `ScopeParams` (MinFOV, MaxFOV).
- **The tube in the host's gun frame** (x right, y up, z back, metres): the bone-local points through the scope bone's live
  pose (SpaceBases, the game's own pose: the bake puts it back after the renderer's copy) x the mesh's LocalToWorld x the
  bake's move, the mirror and the catch-up undone, into the gun hand's controller frame -- as physical melee takes its
  levers. Only while the weapon is quiet (Active 0.3 s, no shot for 0.5 s). Left-hand mode publishes it mirrored, ready
  for the host's left gun pose.
- Published once per Draw under `scopeSeq`; the real scopes' magnification and reticle are a table.

### 2.3 The gate (host, `src/host/scope.cpp`)
- On when: the switch (the menu's Scopes; shipped `[Scope] Enable=1`), the gun's scope on with its geometry, a head-tracked
  view, a gun pose, **both hands on the gun** (the off hand on the foregrip; `[Scope] TwoHands=1`), no menu.
- An eye **0..12 cm behind the eyepiece** (`EyeDistance`), **within 2.5 cm of its axis** (`EyeOffAxis`), looking within
  35 deg of it, for 2 frames -> that eye looks through (the nearer the axis wins). It stays until past 16 cm, 4 cm off or
  50 deg.

### 2.4 The magnification
- `[Scope] Zoom=real` (the menu's Scope zoom, the player's): the real scope's (Springfield 2.5x, G43 and StG44 4x).
- `Zoom=game`: the game's zoom, from its widest; the turning stick's up / down zooms while looking through
  (`ZoomRate=20` deg/s), kept per gun. The stick is kept from the game meanwhile, and until it is centred again (no
  crouch flick).

### 2.5 The scope view (game side, `src/mohavr/vr_view.cpp`)
- The third player: its own `FSceneViewState`; a square, symmetric frustum of tan(field) / M (the eyepiece's apparent field
  22 deg, `Field`); the camera = `scopeCamera`, mapped into the world exactly as the eyes are.
- **The camera** sits at the objective and looks down the optical axis -- **zeroed to the bore**: along the aim line, not
  the drawn tube (the StG44's mount tilts its tube 3.9 deg nose-up against its barrel). It sees over what the scope sees
  over (a camera on the bore line saw the ridge in front).
- The HUD loop runs over the eyes only (`kHudLoopStart`: Num back to 2). The first-person parts are shrunk to a point in
  that view (the view-model hook, by the view's x).
- The frame record carries the eyes' width, the scope view's rect, camera and FOV, and the gun pose the frame was drawn
  with: `SlotScope`, per slot beside `SlotMeta`.

### 2.6 The lens (host)
- A quad on the drawn eyepiece (the frame's render-time gun pose: it stays on the drawn mesh), 0.9 of the eyepiece's
  radius, facing back along the axis; `eyeVisibility` = the looking eye only.
- **Direction-mapped (parallax-free)**: each lens point shows the scope view in the direction the eye looks through it,
  divided by the magnification -- the picture and the reticle stay on the target as the eye moves.
- **The exit pupil** (`EyeRelief=7` cm behind the eyepiece, `Pupil=1.2` cm forgiving): a ray that misses it darkens --
  the shadow when the eye is off the axis or out of its relief. **The field stop**: a black ring at the field's edge.
- **The reticle** (in the field): the Springfield's fine crosshair; the ZF4's post and bars. It sits where the aim line
  is `Zero=50` m out, seen from the scope camera -- the scope's zero: nearer, the shots land a little low (the scope sits
  above the barrel), as with a real scope.
- The aim dot is hidden while looking through.

### 2.7 Shared block v24 (2544 -> 2936)

| Offset | Field | Direction | Meaning |
|---|---|---|---|
| 2544 | `slotScope[3]` | game -> host, per slot | eyeWidth, the scope rect, camera, tanHalf, the render-time gun pose |
| 2796 | `scopeWant` | host -> game, view seqlock | bit0 render it, bit1 the right eye |
| 2800 | `scopeTanHalf` | host -> game | the scope view's half-FOV tangent |
| 2804 | `scopeCamera` | host -> game | its camera (LOCAL) |
| 2832 | `scopeSeq` .. `scopeReticle` | game -> host, seqlock | caps, key, eyepiece, objective, radius, the game's FOVs, real magnification, reticle |

### 2.8 Switches
- `[Scope] Enable=1` (shipped; the menu's Weapons tab "Scopes"), `Zoom=real` ("Scope zoom"), `TwoHands=1`, `EyeDistance`,
  `EyeOffAxis`, `Field`, `EyeRelief`, `Pupil`, `ZoomRate`, `Zero`; `Column=512` (px; 0 = no scope view at all).
- `[Debug] ScopeView=1` (the spike): the scope view always on, from the right eye, `ScopeViewFov` wide.

## 3. Tests [S] (work/research/tests/scope1-5.ps1)

| Case | Result |
|---|---|
| The spike: the third view always on | renders, the HUD not in it, the gun shrunk in it; the eyes intact; the host's eye rects follow `eyeWidth` |
| Its cost (simulator, paced) | the per-Draw wait unchanged within noise (10.1-10.6 ms); memory unchanged (no new targets) |
| The game's scopes (stage 0) | Springfield and G43 10..40 deg, StG44 35 deg (mounted ~3 s after the switch); the tubes in the gun frame |
| Springfield two-handed, the eyepiece 6 cm before the eye | looking through, 2.5x; the scope view 9.2 deg; the lens 2.3 cm across on the eyepiece |
| One hand (the grip let go) | not looking through ("one hand on the gun") |
| Pitched 12 deg at the gate's wall | the scope view and the lens show the wall behind the eyepiece, magnified; the crosshair centred |
| Zoom=game, the stick up 1.5 s | 40 -> 15 deg and on (2.3x -> 6.4x) |
| StG44 | 4x, the post and bars centred (the drawn tube 3.9 deg off: zeroed to the aim line) |
| A shot through the scope | hit at 25.3 m, 4 cm from the aim point |
| The menu | "Scopes < on >", "Scope zoom < realistic >"; Scopes off -> not looking through |
| Left-hand mode | the tube published mirrored; looking through, the lens on the eyepiece |
| Harness cycle with the shipped defaults | OK |

The simulator reports both eye poses at the head's centre, so the tests put the eyepiece before the head's centre; in the
headset each eye is half the IPD to its side.

## 4. [H] questions for the player
- Does raising the gun to your eye feel like looking through a scope? Is the lens the right size, the eye box too strict
  or too loose?
- Realistic or the game's zoom? Does zooming with the stick work?
- Is two hands right, or should one hand do too (`TwoHands=0`)?
- Does the picture swim or shake at 4x (your hand's own tremor, magnified)?
- Which eye: does it pick the one you aim with?

## 5. The code review (8 of 17 findings confirmed, all fixed)
- The lens was posed one game frame ahead of the drawn gun while it moved: the frame now records the last Draw's gun pose
  (the one the bake drew it with).
- The engine truncates Origin x width in float: at some widths the scope view started a pixel early and the gun wasn't
  shrunk in it: the fractions are a quarter pixel in, and the view-model test takes any view at or past the column.
- Raised again, the lens showed the last scoping's picture for a frame or two: it waits for this scoping's view.
- The render thread could draw the last scope view after the column's x was cleared: it stays set for the width.
- The tube's geometry is taken again after the scope comes back on (the StG44 re-mounts it) and after the hand changes.
- No lens during a recentre (the frame is in the old space).

## 6. Known limits
- The M18 recoilless rifle's sight has no part of its own; since D53 tools/scope_points.py measures it as a piece of its
  body bone (a box, the largest connected piece, the eyepiece's glass), and the lens shows the game's own ring sight.
- The scope view costs a third view while a scope is at an eye: measured within noise in the simulator; the headset's GPU
  time is not measured.
- While looking through, the eyes give the scope column their width (512 px): a little softer then.
