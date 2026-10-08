// Shared-memory contract between the game-side mod (x86, dinput8.dll) and MOHAVR-host.exe
// (x64) -- D10. Compiled into both, so it uses fixed-size fields only: the layout is identical
// in a 32-bit and a 64-bit process (checked by the static_asserts).
//
// Mapping name: Local\MOHAVR_<gamePid>. The game creates it; the host opens it.
//
// Frame handoff (mono in M2; per-eye slots come with stereo):
//   * the game owns a ring of kRing shared D3D12 textures and two shared fences;
//   * game: publishes frame N (1, 2, 3, ...) into slot N % kRing only when the host has
//     acknowledged frame N-1 (ackFrame == publishedFrame) -- paced (`pace`, v13), when it has
//     acknowledged frame N-kRing (N <= ackFrame + kRing). Before overwriting the slot it
//     GPU-waits hostFence >= N - kRing, copies, signals gameFence = N, then stores
//     slotMeta/slotViewQpc, publishedSlot and publishedFrame (in that order);
//   * host: sees publishedFrame F > its last, reads slot F % kRing's meta, sets ackFrame = F,
//     GPU-waits gameFence >= F, copies slot F % kRing into its own texture, signals hostFence = F.
//   The host always takes the newest frame; one it skipped needs no signal of its own (hostFence
//   = F covers it), so hostFence always catches up and the game's GPU wait never deadlocks. The
//   game never blocks on the CPU: when the host is behind, it simply skips publishing that frame.
//
// Handles are NT handles valid in the GAME process; the host DuplicateHandle()s them in.
#pragma once
#include <cstddef>
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mohavr::shared {

inline constexpr std::uint32_t kMagic   = 0x3152564D;  // "MVR1"
inline constexpr std::uint32_t kVersion = 31;          // 2: views + render pose (M3); 3: per-eye meta (M4); 4: live settings; 5: recentre + height; 6: virtual pad; 7: aim poses; 8: gun fit; 9: hands; 10: throwing; 11: weapon kind; 12: free hand; 13: view times; 14: manual reload; 15: the reload grips' held magazine; 16: grip adjustments; 17: the slide insert; 18: the two-stage action; 19: the pump (no layout change); 20: the off-hand grenade; 21: the off-hand pistol; 22: the gun hand's grenade by pin, cook and grip (no layout change); 23: physical melee; 24: scopes; 25: the off-hand knife; 26: the knife's hold adjusted; 27: the rack eject (no layout change); 28: the wrist HUD; 29: physical crouch (no layout change); 30: the main gun's shots (D74); 31: the airdrop phase (D75)
inline constexpr std::uint32_t kRing    = 3;

// OpenXR conventions throughout (right-handed, +Y up, -Z forward, metres), in the host's LOCAL
// reference space. The game converts to Unreal units/axes itself.
struct Pose {
    float px, py, pz;
    float qx, qy, qz, qw;
};
// Tangents of the view frustum edges: left and down are negative.
struct Fov {
    float tanLeft, tanRight, tanUp, tanDown;
};
// What a published frame was rendered with -- the host submits it with exactly these poses/fovs.
//   hasView 0: rendered without head tracking (show on the quad)
//   stereo  0: mono -- the whole image, eye[0] pose/fov, shown to both eyes
//   stereo  1: side by side -- left half = eye 0, right half = eye 1 (M4)
struct SlotMeta {
    Pose          pose[2];
    Fov           fov[2];
    std::uint32_t hasView;
    std::uint32_t stereo;
};
// v24 (SCOPE-DESIGN.md), per slot beside slotMeta: where the eyes and the scope view are in the image, and what the scope
// view was rendered with. With a scope column the eyes are eyeWidth wide each from the left and the column follows.
struct SlotScope {
    std::uint32_t eyeWidth;   // px of each eye (0: half the width -- no scope column)
    std::uint32_t rect[4];    // the scope view: x, y, w, h px (w 0: not rendered this frame)
    Pose          camera;     // its camera (LOCAL; -Z the view direction, +Y up)
    float         tanHalf;    // its half-FOV tangent (square, symmetric)
    Pose          gunPose;    // the gun pose the frame was drawn with (the lens stays on the drawn scope)
    std::uint32_t flags;      // bit0 gunPose valid
};

// v28 (WRISTHUD-DESIGN.md): the HUD's element rectangles in the HUD texture (canvas px, x0 y0 x1 y1), read live from the
// game's HUD objects each pass. "The minimap" is the radar compass (the game's MiniMap is never created in single player).
enum HudRect : std::uint32_t {
    kHudHealth = 0, kHudCompass = 1, kHudStance = 2,                     // the left wrist panel
    kHudAmmoBar = 3, kHudAmmoText = 4, kHudNadeText = 5, kHudWeaponIcon = 6, kHudNadeIcon = 7,  // the right panel's core
    kHudWeaponBadge = 8, kHudNadeBadge = 9,                              // the level badges (right panel, while shown)
    kHudWeaponMedal = 10, kHudNadeMedal = 11,                            // the kill medals (right panel)
    kHudRects = 12
};
// Per slot beside slotMeta: what the HUD texture copied with that frame holds.
struct SlotHud {
    std::uint32_t flags;          // bit0 the texture holds this frame's HUD pass (else empty: hide the quads), bit1 the wrist
                                  // pass (the eyes are HUD-free), bit2 rect valid, bit3 / bit4 hand[0] / hand[1] valid,
                                  // bit5 the player is dead or has no pawn (the wrist panels hide)
    std::uint32_t canvasW, canvasH;
    float         rs;             // the HUD's live resolutionScale (MOHAHUD.resolutionScale)
    std::uint32_t shown;          // bit i: element rect i is drawn now (its bRender)
    float         rect[kHudRects][4];
    Pose          hand[2];        // the controller aim poses this frame's arms were drawn with (LOCAL)
};

enum class GameState : std::uint32_t { None = 0, Starting = 1, Ready = 2, Failed = 3 };
enum class HostState : std::uint32_t { None = 0, Starting = 1, Running = 2, Failed = 3, Exited = 4 };

#pragma pack(push, 8)
struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t gamePid;
    std::uint32_t hostPid;

    // Written once by the game before gameState = Ready.
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t dxgiFormat;          // DXGI_FORMAT of the shared textures
    std::uint32_t ring;                // == kRing
    std::uint64_t adapterLuid;         // LUID of the game's D3D12 adapter (LowPart | HighPart << 32)
    std::uint64_t textureHandles[kRing];
    std::uint64_t gameFenceHandle;     // game signals N when frame N is in its slot
    std::uint64_t hostFenceHandle;     // host signals N when it has finished reading frame N

    // Live counters (accessed with interlocked operations).
    volatile std::uint64_t publishedFrame;
    volatile std::uint32_t publishedSlot;
    volatile std::uint32_t pad0;
    volatile std::uint64_t ackFrame;

    volatile std::uint32_t gameState;  // GameState
    volatile std::uint32_t hostState;  // HostState
    char gameStatus[128];              // last notable message, for logs
    char hostStatus[128];

    // --- v2: views, host -> game (seqlock: viewSeq odd while the host writes) -----------------
    volatile std::uint32_t viewSeq;
    std::uint32_t          viewValid;  // bit 0: orientation valid (views usable); bit 1: head position TRACKED
                                       // (not a placeholder -- safe to take as the translation origin)
    std::int64_t           viewDisplayTime;
    Pose                   head;       // VIEW space located in LOCAL
    Pose                   eye[2];     // xrLocateViews, left/right
    Fov                    eyeFov[2];

    // --- v2: per-slot render metadata, game -> host (written before publishedFrame) ------------
    SlotMeta               slotMeta[kRing];

    // --- v4: live settings (the host's in-headset menu -> game) --------------------------------
    // The game writes its ini defaults once at start; the host overwrites the live values from
    // the player's saved settings and the menu. 0 = not set (the game keeps its own value).
    float                  defaultUnitsPerMeter;   // game -> host: the shipped/ini default
    volatile float         unitsPerMeter;          // host -> game: live world scale (Unreal units per metre)
    volatile float         heightOffset;           // host -> game: v5, metres added to the camera height (seated/standing)
    volatile std::uint32_t recenterSeq;            // host -> game: v5, bumped when the host recentres LOCAL space
    float                  reservedSettings[4];

    // --- v6: the virtual Xbox pad, host -> game (M6, Input.Controllers; seqlock: padSeq odd while the
    // host writes). The game's XInputGetState(0) returns this while padActive (XINPUT_GAMEPAD layout).
    volatile std::uint32_t padSeq;
    volatile std::uint32_t padActive;      // 1 = the host drives pad 0; 0 = the real pad 0 passes through
    std::uint16_t          padButtons;     // XINPUT_GAMEPAD_* bits
    std::uint8_t           padLeftTrigger, padRightTrigger;
    std::int16_t           padThumbLX, padThumbLY, padThumbRX, padThumbRY;
    // Snap turn (Comfort, host -> game): the running sum of snap steps in rotator units (65536 = 360
    // degrees). The game adds each change to the PlayerController's yaw, once.
    volatile std::int32_t  snapYawTotal;
    // Game -> host: 1 while one of the game's UI menus is open (the cursor test, ENGINE-NOTES 5m). The
    // virtual pad then uses its menu layout (A selects, B backs out).
    volatile std::uint32_t gameUiMenu;

    // --- v7: the controllers' aim poses, host -> game (M7), written inside the view seqlock (viewSeq) ---------
    std::uint32_t          handValid;  // bit 0: left aim pose valid (orientation + position), bit 1: right
    Pose                   hand[2];    // /input/aim/pose located in LOCAL, left/right
    // Game -> host: where the shot will land, for the reticle -- metres along the aim pose's forward from
    // its position (0 = nothing to show), and which pose: 0 none, 1 head, 2 left hand, 3 right hand.
    volatile float         aimDistance;
    volatile std::uint32_t aimSource;

    // --- v8: the gun fit (M8, the host menu's Gun fit page) ----------------------------------------------------
    // Game -> host: the weapon in the player's hands, by class name ("" = none); weaponSeq is bumped on a change.
    char                   weaponKey[48];
    volatile std::uint32_t weaponSeq;
    // Host -> game (seqlock fitSeq): the fit for fitKey. Grip: the point of the gun, in the game camera's frame, that
    // is put on the controller (Unreal units: forward, right, up). Angle: the gun's pitch against the controller
    // (degrees, + = muzzle up). Ray: the aim line's offset from the controller along the gun's up / right (cm).
    volatile std::uint32_t fitSeq;
    std::uint32_t          fitValid;
    char                   fitKey[48];
    float                  fitGrip[3];
    float                  fitAngle;
    float                  fitRayUp, fitRayRight;

    // --- v9: hands, host -> game, written inside the view seqlock (viewSeq) --------------------------------------
    // The host works out the gun from both controllers (the gun hand, the foregrip, the fit's angle and aim line):
    // gunPose is the controller pose the gun is fitted to (the fit's grip goes on it), aimRay the aim line (its
    // position = the start, its -Z = the direction). gunFlags: bit 0 valid, bit 1 two-handed, bit 2 left-handed.
    std::uint32_t          gunFlags;
    Pose                   gunPose;
    Pose                   aimRay;
    // Host -> game: a console command to run once on the game thread (holsters, the reload gesture); cmdSeq is
    // bumped after cmd is written.
    volatile std::uint32_t cmdSeq;
    char                   cmd[64];

    // --- v10: throwing, host -> game: the gun hand's velocity (LOCAL, m/s) when its trigger let go of a grenade;
    // throwSeq is bumped after throwVel is written. The game gives it to the grenade that the throw spawns.
    volatile std::uint32_t throwSeq;
    float                  throwVel[3];

    // --- v11: game -> host: what the weapon in hand is (0 a long gun, 1 a pistol, 2 a grenade); bumps weaponSeq.
    // The foregrip is for long guns only, the reload gesture not for grenades.
    volatile std::uint32_t weaponKind;
    std::uint32_t          pad11;

    // --- v12: host -> game (the menu's Free hand): how the free support hand sits on its controller, on top of the
    // mirrored gun-hand grip -- pitch, yaw, roll (degrees, about the wrist) and forward (cm).
    volatile float         freeHand[4];

    // --- v13: host -> game: frame pacing (the menu's Frame pacing; default the shipped [Bridge] Pace): 1 = the game
    // starts each Draw on the host's frame event (Local\MOHAVR_Frame_<gamePid>), one game frame per XR frame.
    volatile std::uint32_t pace;
    std::uint32_t          pad13;
    // game -> host, per slot, written before publishedFrame: when that frame's view was computed (QPC; one clock for
    // both processes). The host logs how far behind each XR frame the world it shows is (round 25).
    std::int64_t           slotViewQpc[kRing];

    // --- v14: manual reload ([Weapon] ManualReload; src/mohavr/reload.cpp, src/host/reload.cpp; RELOAD-DESIGN.md 4) ---
    // game -> host, once per Draw (seqlock reloadGeoSeq: odd while the game writes). Geometry in the host's gun frame
    // (x right, y up, z back, metres), already un-mirrored for a left gun hand.
    volatile std::uint32_t reloadGeoSeq;
    std::uint32_t          reloadCaps;     // bit0 converted gun in hand, bit1 action bone(s) found, bit2 hook installed,
                                           // bit3 alt-fire mode, bit4 the game's block filter passes for this gun now,
                                           // bit5 a taped (twin) magazine pair: the off hand's trigger flips it,
                                           // bit6 magHeld valid (the reload grips), bit7 the magazine is grabbed
                                           // only with the off hand's trigger held (GrabTrigger), bit8 the gun hand's
                                           // trigger releases a locked-back action (TriggerRack), bit9 the seated
                                           // magazine can't be grabbed (NoGrab: the Garand's clip), bit10 the release
                                           // button does nothing on this gun (Latch=0), bit11 a pump gun (v19: the
                                           // pump is the foregrip; BOLT BACK / FORWARD are its strokes; reloadState
                                           // bits 7 spent, 8 the pump back, 9 room, 11 trigger held, 13 the chamber
                                           // empty), bit12 a two-stage action
                                           // (a bolt: actPath; reloadState bits 7 spent, 8 open, 9 room, 10 held
                                           // open, 11 trigger held, 12 a clip in the guides, 14 lifted, 15 forward)
    char                   reloadKey[48];  // the attachment class the geometry is for
    float                  magGrab[3];     // the in-gun magazine's grab point = the insert target
    float                  magOut[3];      // unit: the way the magazine leaves the well
    float                  magGrabR;       // metres: its grab radius
    float                  boltGrab[3];    // the handle / slide grip point at its held position (without the pull)
    float                  boltBack[3];    // unit: the pull direction
    float                  boltTravel;     // metres from the held position to full back
    std::int32_t           ammoClip, ammoMax, ammoReserve;  // ammoReserve = the reserve + the rounds owed (RELOAD-DESIGN 2.2)
    std::uint32_t          reloadState;    // bit0 magIn, bit1 pending, bit2 ready (closed: clip >= 1; open: cocked), bit3
                                           // action held back, bit4 rack needed, bit5 open-bolt gun, bit6 infinite ammo;
                                           // v27: bit16 a live round chambered that a full stroke of the action would
                                           // eject now (the rack eject on, the gun throws one: RackRound)
    volatile std::uint32_t reloadPawnSeq;  // bumped on a new local pawn: the host resets every gun to "in the gun"
    volatile std::uint32_t reloadEvtAck;   // the last event the game processed (applied or rejected)
    // host -> game, per XR frame inside the view seqlock (viewSeq), next to gunPose
    std::uint32_t          reloadFlags;    // bit0 manual reload on (the player's toggle); bits 1-2 the magazine (0 in gun,
                                           // 1 grabbed, 2 in the off hand, 3 out); bit3 action held; bit4 engaged (the
                                           // pipeline is alive: the game blocks its own reload only then); bit5 the held
                                           // taped pair flipped (its other half toward the well); bit6 the off hand holds
                                           // the action (a bolt's knob, a handle, the pump) -- its grip applies;
                                           // v27 (D54): bit7 the rack eject on (a full stroke of a loaded action throws
                                           // the chambered round out, spent), bit8 an ejected round goes back to the
                                           // reserve (RackEjectKeep)
    std::uint32_t          reloadKeyHash;  // FNV-1a 32 of the attachment class these flags are for
    float                  magPull;        // metres the grabbed magazine is drawn out along magOut
    Pose                   magPose;        // the held magazine's grab-point frame in LOCAL (the gun frame's axes)
    float                  rack;           // 0..1 of boltTravel pulled
    // host -> game events, ordered: reloadEvt[seq % 8] written, then reloadEvtSeq bumped (the cmdSeq pattern); the host
    // never writes while reloadEvtSeq - reloadEvtAck >= 8
    volatile std::uint32_t reloadEvtSeq;
    std::uint32_t          reloadEvt[8];   // low byte: 1 EJECT, 2 INSERT, 3 RACK, 4 TAKE, 5 DROP; high 24 bits: the low 24
                                           // bits of the key hash
    // --- v15 (round 31): where a held magazine sits in the drawn hand (the reload animation's grip), for the host's
    // insert test and drawing -- the magazine's grab-point frame in the off hand's aim frame (host convention, metres;
    // un-mirrored). Written with the geometry (reloadGeoSeq); valid when reloadCaps bit6.
    Pose                   magHeld;        // 1464
    std::uint32_t          pad15;          // 1492
    // --- v16 (round 32): the player's reload-grip adjustments for the weapon in hand (the menu's Reload grip page; host
    // writes, seqlock gripSeq). Per grip (0 the magazine grabbed, 1 held, 2 the handle / bolt): the hand moved on the
    // part -- forward, up, right (units ~ cm) -- and turned at the wrist about the gun's axes -- tilt, turn, roll (deg).
    volatile std::uint32_t gripSeq;        // 1496
    char                   gripKey[48];    // 1500
    float                  gripAdj[3][6];  // 1548
    std::uint32_t          gripFlags;      // 1620 bit0: the magazine in the hand is held with the grab grip (round 35)
    // v17 (GOAL A5, the Panzerschreck's rocket): a slide insert (with reloadGeoSeq). A long magazine's front meets the
    // well's mouth, then it slides in along the way out: magLen = from its grab point to its front, magSeat = how far
    // inside the mouth the seated grab point sits (metres, host gun frame; magLen 0 = the snap insert at the well).
    float                  magLen;         // 1624
    float                  magSeat;        // 1628
    // v18 (GOAL A2, the bolt actions; later the M18's breech): a two-stage action the off hand works by its knob, along
    // the knob's path from closed (s 0) through stage 1's end (s 1: the bolt lifted) to fully open (s 2: drawn back);
    // game->host with reloadGeoSeq (host gun frame, metres). The host poses it with reloadFlags bit3 and rack = s / 2.
    float                  actPath[9][3];  // 1632
    float                  actPathS[9];    // 1740
    std::uint32_t          actPathN;       // 1776  0 = no two-stage action
    std::uint32_t          pad18;          // 1780
    // --- v20: the off-hand grenade ([OffHand] Grenade; src/host/offhand.cpp, src/mohavr/offhand.cpp; OFFHAND-DESIGN.md 7).
    // game -> host, once per Draw (seqlock nadeSeq: odd while the game writes)
    volatile std::uint32_t nadeSeq;        // 1784
    std::uint32_t          nadeCaps;       // 1788 bit0 installed (the script functions resolved), bit1 a TAKE can happen
                                           //      now (a gun in hand, nothing in the way), bit2 infinite ammo, bit3 the
                                           //      grenade can be drawn in the hand, bit4 one held may stay (bit1, or only a
                                           //      switch to another gun under way); 0 while switched off with none held
    std::int32_t           nadeCount[3];   // 1792 frag, Gammon, stick: what a TAKE can get (-1 not carried, 99 infinite)
    std::uint32_t          nadeNext;       // 1804 the type a TAKE of "any" gives (0 frag, 1 Gammon, 2 stick, 0xFF none)
    std::uint32_t          nadeState;      // 1808 bits 0-1: 0 none, 1 held, 2 armed, 3 cooking; bits 2-3 the type; bits 8-15
                                           //      the last refusal (0 none, 1 unavailable, 2 empty, 3 no weapon, 4 spawn failed)
    float                  nadeFuse;       // 1812 seconds left while cooking (game time), else 0
    float                  nadeFuseLen;    // 1816 the held type's FuseTime
    volatile std::uint32_t nadeTicks;      // 1820 +1 per countdown tick while cooking (the host's haptic tick)
    volatile std::uint32_t nadeEvtAck;     // 1824 the last event taken (applied or refused)
    volatile std::uint32_t nadePawnSeq;    // 1828 +1 on a new local pawn
    volatile std::uint32_t nadeBoom;       // 1832 +1 when a grenade went off in the hand
    // host -> game, per XR frame INSIDE the view seqlock (viewSeq), read with the hands (RELOAD-DESIGN X3)
    std::uint32_t          nadeFlags;      // 1836 bit0 on, bit1 held, bit2 pin out, bit3 cooking, bits 4-5 the type, bit6 frozen
    Pose                   nadePose;       // 1840 the off hand's hand point (LOCAL; orientation = its aim pose)
    float                  nadeAdj[6];     // 1868 the held type's hold: forward, up, right (cm), tilt, turn, roll (deg)
    // host -> game events, ordered: nadeEvt[seq % 8] (and its position / velocity) written, then nadeEvtSeq bumped; the
    // host never writes while nadeEvtSeq - nadeEvtAck >= 8
    volatile std::uint32_t nadeEvtSeq;     // 1892
    std::uint32_t          nadeEvt[8];     // 1896 low byte: 1 TAKE, 2 PIN, 3 COOK, 4 THROW, 5 PUTBACK; bits 8-15 the type
                                           //      (0 frag, 1 Gammon, 2 stick, 0xFF any); bits 16-23 flags (bit16 toss)
    float                  nadeEvtPos[8][3];  // 1928 THROW: the release point (LOCAL, m)
    float                  nadeEvtVel[8][3];  // 2024 THROW: the release velocity (LOCAL, m/s)
    // --- v21: the off-hand pistol ([OffHand] Pistol; src/host/offpistol.cpp, src/mohavr/offpistol.cpp; OFFPISTOL-DESIGN.md 5).
    // game -> host, once per Draw (seqlock pistolSeq: odd while the game writes)
    volatile std::uint32_t pistolSeq;         // 2120
    std::uint32_t          pistolCaps;        // 2124 bit0 installed (the shot's functions resolved on the pistol a DRAW gets),
                                              //      bit1 a DRAW can happen now, bit2 infinite reserve, bit3 drawn in the hand
                                              //      (the arm bake), bit4 one held may stay, bit5 the only pistol is in the gun
                                              //      hand, bit6 the game's switch weapon would take the held pistol (Xbox B is
                                              //      kept back: PistolKeep), bit7 the held pistol fires while held (the C96's 712)
    char                   pistolKey[48];     // 2128 the pistol a DRAW gets / the held one: its attachment class (the fit's key)
    std::int32_t           pistolClip;        // 2176 its rounds
    std::int32_t           pistolMax;         // 2180 its magazine
    std::uint32_t          pistolState;       // 2184 bits 0-1: 0 none, 1 held; bit2 empty; bit3 a refill pending; bits 8-15 the
                                              //      last refusal (1 unavailable, 2 none carried, 3 in the gun hand, 4 too soon,
                                              //      5 the shot failed, 6 taken by a switch, 7 gone)
    volatile std::uint32_t pistolShots;       // 2188 +1 per round fired (the host's recoil pulse)
    volatile std::uint32_t pistolDry;         // 2192 +1 per dry click
    volatile std::uint32_t pistolEvtAck;      // 2196 the last event taken (applied or refused)
    volatile std::uint32_t pistolPawnSeq;     // 2200 +1 on a new local pawn
    volatile float         pistolAimDistance; // 2204 per Draw while held: metres along offAimRay to what it hits; 0 none
    volatile std::uint32_t pistolRefills;     // 2208 +1 per refill in the holster (the host's click)
    std::uint32_t          pistolSpare;       // 2212
    // host -> game, per XR frame INSIDE the view seqlock (viewSeq), read with the hands
    std::uint32_t          pistolFlags;       // 2216 bit0 on, bit1 held, bit2 frozen, bit3 the trigger held (past the latch)
    float                  pistolTrigger;     // 2220 the off trigger 0..1
    Pose                   offAimRay;         // 2224 the off pistol's aim line (LOCAL: position = start, -Z = direction)
    float                  offFit[4];         // 2252 the pistol's fit (the host's, for pistolKey): grip forward, right, up
                                              //      (units, camera frame), angle (deg, + = muzzle up)
    // host -> game events, ordered: pistolEvt[seq % 8] (and its ray) written, then pistolEvtSeq bumped; the host never
    // writes while pistolEvtSeq - pistolEvtAck >= 8
    volatile std::uint32_t pistolEvtSeq;      // 2268
    std::uint32_t          pistolEvt[8];      // 2272 low byte: 1 DRAW, 2 SHOT, 3 HOLSTER
    Pose                   pistolEvtRay[8];   // 2304 SHOT: the off aim line at the trigger pull (LOCAL)
    // v23: physical melee (MELEE-DESIGN 2.10).
    std::uint32_t          meleeOn;           // 2528 host -> game, per XR frame INSIDE the view seqlock: bit0 the switch, bit1 the
                                              //      gun hand (and the off hand on the foregrip) really tracked (POSITION_ and
                                              //      ORIENTATION_TRACKED: not HoldLost, not inferred), bit2 the gun hand busy (a
                                              //      holster or the pouch just pressed, the magazine out, a menu), bits 8-15 the gun
                                              //      pose's epoch (+1 on a jump: the foregrip's turn on / off, a hand held / back,
                                              //      the gun hand changed, a recentre); v25, the off-hand knife: bit3 held,
                                              //      bit4 the off hand really tracked, bit5 the off hand busy (its grip just
                                              //      pressed or let go, a menu), bits 16-23 the off hand's pose epoch
    volatile std::uint32_t meleeHits;         // 2532 game -> host: +1 per strike that hit (the host's pulse)
    volatile float         meleePower;        // 2536 game -> host: that strike's speed over its gate, 0..1 (written before meleeHits)
    volatile std::uint32_t meleeKind;         // 2540 game -> host: 1 a soldier, 2 an actor, 3 the world (written before meleeHits);
                                              //      (v25: the knife's strikes are counted in knifeHits)
    // --- v24: scopes (SCOPE-DESIGN.md) ---
    SlotScope              slotScope[kRing];  // 2544 game -> host, per slot, written with slotMeta (before publishedFrame)
    // host -> game, per XR frame INSIDE the view seqlock
    std::uint32_t          scopeWant;         // 2796 bit0 render the scope view, bit1 the right eye looks through it (else left)
    float                  scopeTanHalf;      // 2800 the scope view's half-FOV tangent (the magnification chosen)
    Pose                   scopeCamera;       // 2804 its camera (LOCAL; -Z the view direction): on the aim line, at the objective
    // game -> host, once per Draw (seqlock scopeSeq: odd while the game writes)
    volatile std::uint32_t scopeSeq;          // 2832
    std::uint32_t          scopeCaps;         // 2836 bit0 the column is there (a scope view can render), bit1 a scope on the gun
                                              //      in hand (the game's IsScopeEnabled), bit2 the scope's geometry is known
    char                   scopeKey[48];      // 2840 the attachment class
    float                  scopeOcular[3];    // 2888 the eyepiece's centre, host gun frame (x right, y up, z back, m), un-mirrored
    float                  scopeObjective[3]; // 2900 the objective's centre (the axis runs ocular -> objective)
    float                  scopeRadius;       // 2912 the eyepiece's radius (m)
    float                  scopeGameFov[2];   // 2916 the game's zoom: its narrowest and widest FOV (deg; its 80 deg view = 1x)
    float                  scopeRealMag;      // 2924 the real scope's magnification (x; 0 unknown)
    std::uint32_t          scopeReticle;      // 2928 0 a crosshair, 1 a post and bars (the German scopes), 2 the M18's ring sight
    std::uint32_t          pad24;             // 2932
    // --- v25: the off-hand knife (OFFKNIFE-DESIGN.md): the host holds whether it is held, the game draws it to match ---
    // game -> host, once per Draw (seqlock knifeSeq: odd while the game writes)
    volatile std::uint32_t knifeSeq;          // 2936
    std::uint32_t          knifeCaps;         // 2940 bit0 installed (the template found, the arm bake on), bit1 a draw can happen
                                              //      now, bit2 the Dagger earned ([Knife] Require met), bit3 drawn in the hand,
                                              //      bit6 the game's button melee would hang its own knife (an MP40 at level 2)
    std::uint32_t          knifeState;        // 2944 bits 8-15 the last refusal: 1 unavailable, 2 not earned, 3 no template,
                                              //      4 the draw failed
    float                  knifeDamage;       // 2948 what a hit does
    volatile std::uint32_t knifePawnSeq;      // 2952 +1 on a new local pawn (the host lets go of one held)
    // host -> game, per XR frame INSIDE the view seqlock
    std::uint32_t          knifeFlags;        // 2956 bit0 on, bit1 held, bit2 the icepick grip (else forward)
    // game -> host: the knife's strikes, as meleeHits / meleeKind / meleePower for the gun (its own: a gun hit in the same
    // window can't take its pulse); kind and power written before the count
    volatile std::uint32_t knifeHits;         // 2960
    volatile std::uint32_t knifeKind;         // 2964 1 a soldier, 2 an actor, 3 the world
    volatile float         knifePower;        // 2968
    std::uint32_t          pad25;             // 2972
    // --- v26: the knife's hold, adjusted (the menu's Knife grip page; host -> game, written as it changes): forward, right,
    // up (cm, the off controller's frame), tilt, turn, roll (deg, about the handle's middle)
    float                  knifeAdj[6];       // 2976
    // --- v28: the wrist HUD (WRISTHUD-DESIGN.md; src/mohavr/hudtex.cpp, src/host/wristhud.cpp) ---
    // game -> host, written once before gameState = Ready: a second ring of shared textures, hudTexW x hudTexH B8G8R8A8 with
    // premultiplied alpha, holding the HUD pass the game drew into the mod's own render target (handles 0: none)
    std::uint64_t          hudTexHandles[kRing];  // 3000
    std::uint32_t          hudTexW, hudTexH;      // 3024
    std::uint32_t          hudCaps;               // 3032 bit0 the HUD redirect is installed (the wrist HUD can be chosen)
    // host -> game, live (the menu; written as they change): 0 = not set, the game's own ini
    volatile std::uint32_t hudPlace;              // 3036 1 screen (the head-locked per-eye panel), 2 wrist
    volatile float         hudScreen[3];          // 3040 the screen panel: distance, width, down (m)
    // 29, physical crouch (GOAL A1, D61): host -> game, live (the menu): 0 = not set (the game's [Controls] PhysicalCrouch),
    // 1 off, 2 on; bits 2-3 seated (GOAL A3: the same values); game -> host: bumped once per crouch toggle the game side wants (the host pulses Xbox X, the game's crouch)
    volatile std::uint16_t crouchMode;            // 3052
    volatile std::uint16_t crouchReqSeq;          // 3054
    SlotHud                slotHud[kRing];        // 3056 game -> host, per slot, written with slotMeta (before publishedFrame)
    // 30 (D74): game -> host, +1 per muzzle flash of the player's weapon (a shot): the host's haptic pulse
    volatile std::uint32_t gunShots;              // 3860
    // 31 (D75): game -> host, the airdrop: 0 none, 1 freefall, 2 the chute open (steered by the move stick), 3 flaring
    volatile std::uint32_t airdrop;               // 3864
};
#pragma pack(pop)

static_assert(sizeof(Pose) == 28 && sizeof(Fov) == 16 && sizeof(SlotMeta) == 96, "shared structs must be packed identically");
static_assert(offsetof(Header, publishedFrame) == 80, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, ackFrame) == 96, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, viewSeq) == 368, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, viewDisplayTime) == 376, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, slotMeta) == 500, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, defaultUnitsPerMeter) == 788, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, padSeq) == 820, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, snapYawTotal) == 840, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, gameUiMenu) == 844, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, hand) == 852, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, aimSource) == 912, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, weaponKey) == 916, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, fitKey) == 976, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, fitRayRight) == 1044, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, gunPose) == 1052, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, cmd) == 1112, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, throwVel) == 1180, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, weaponKind) == 1192, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, freeHand) == 1200, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pace) == 1216, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, slotViewQpc) == 1224, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadGeoSeq) == 1248, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadKey) == 1256, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, magGrab) == 1304, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, magGrabR) == 1328, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, boltTravel) == 1356, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, ammoClip) == 1360, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadState) == 1372, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadEvtAck) == 1380, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadFlags) == 1384, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, magPose) == 1396, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, rack) == 1424, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadEvtSeq) == 1428, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, reloadEvt) == 1432, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, magHeld) == 1464, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, gripSeq) == 1496, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, gripAdj) == 1548, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, magLen) == 1624, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, actPath) == 1632, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, actPathN) == 1776, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeSeq) == 1784, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeCount) == 1792, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeState) == 1808, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeEvtAck) == 1824, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeBoom) == 1832, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeFlags) == 1836, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadePose) == 1840, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeAdj) == 1868, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeEvtSeq) == 1892, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeEvt) == 1896, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeEvtPos) == 1928, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, nadeEvtVel) == 2024, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolSeq) == 2120, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolKey) == 2128, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolClip) == 2176, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolState) == 2184, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolEvtAck) == 2196, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, crouchReqSeq) == 3054, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolAimDistance) == 2204, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolRefills) == 2208, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolFlags) == 2216, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, offAimRay) == 2224, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, offFit) == 2252, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolEvtSeq) == 2268, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolEvt) == 2272, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, pistolEvtRay) == 2304, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, meleeOn) == 2528, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, meleeHits) == 2532, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, meleePower) == 2536, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, meleeKind) == 2540, "shared::Header layout must match between x86 and x64");
static_assert(sizeof(SlotScope) == 84, "shared structs must be packed identically");
static_assert(offsetof(Header, slotScope) == 2544, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, scopeWant) == 2796, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, scopeCamera) == 2804, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, scopeSeq) == 2832, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, scopeKey) == 2840, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, scopeReticle) == 2928, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, knifeSeq) == 2936, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, knifePawnSeq) == 2952, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, knifeFlags) == 2956, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, knifeHits) == 2960, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, knifeAdj) == 2976, "shared::Header layout must match between x86 and x64");
static_assert(sizeof(SlotHud) == 268, "shared structs must be packed identically");
static_assert(offsetof(Header, hudTexHandles) == 3000, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, hudCaps) == 3032, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, hudPlace) == 3036, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, hudScreen) == 3040, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, slotHud) == 3056, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, gunShots) == 3860, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, airdrop) == 3864, "shared::Header layout must match between x86 and x64");
static_assert(sizeof(Header) == 3872, "shared::Header layout must match between x86 and x64");

// Manual reload events (reloadEvt low byte) and the key hash both sides use.
// kReloadInsertOther: a taped pair inserted flipped -- its other half goes in (twin magazines).
// v18 (GOAL A2): the two-stage action's steps -- a bolt lifted (7), drawn back (8), pushed forward (9), turned down (10).
// v27 (D54): RACK BACK (11) -- a Step 1 action (slide, bolt handle, op-rod) drawn back to its arm point (not a tug): a
// chambered live round leaves the port.
enum ReloadEvent : std::uint32_t { kReloadEject = 1, kReloadInsert = 2, kReloadRack = 3, kReloadTake = 4, kReloadDrop = 5,
                                   kReloadInsertOther = 6, kReloadBoltUp = 7, kReloadBoltBack = 8, kReloadBoltForward = 9,
                                   kReloadBoltDown = 10, kReloadRackBack = 11 };
inline std::uint32_t KeyHash(const char* s) {  // FNV-1a 32
    std::uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<std::uint8_t>(*s)) * 16777619u;
    return h;
}

// What the game publishes about the scope on the gun in hand (v24).
struct ScopeInfo {
    std::uint32_t caps;
    char          key[48];
    float         ocular[3], objective[3], radius, gameFov[2], realMag;
    std::uint32_t reticle;
};
// Seqlock read of it; false while the game is mid-write (try next frame).
inline bool ReadScopeInfo(const Header* h, ScopeInfo& s, std::uint32_t& seq) {
    const std::uint32_t s1 = h->scopeSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    s.caps = h->scopeCaps;
    for (int i = 0; i < 48; ++i) s.key[i] = h->scopeKey[i];
    s.key[47] = 0;
    for (int i = 0; i < 3; ++i) {
        s.ocular[i] = h->scopeOcular[i];
        s.objective[i] = h->scopeObjective[i];
    }
    s.radius = h->scopeRadius;
    s.gameFov[0] = h->scopeGameFov[0];
    s.gameFov[1] = h->scopeGameFov[1];
    s.realMag = h->scopeRealMag;
    s.reticle = h->scopeReticle;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    seq = s1;
    return h->scopeSeq == s1;
}
// Seqlock read of what the host wants rendered (v24, inside the view seqlock).
inline bool ReadScopeWant(const Header* h, std::uint32_t& want, float& tanHalf, Pose& camera) {
    for (int t = 0; t < 64; ++t) {
        const std::uint32_t s1 = h->viewSeq;
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        want = h->scopeWant;
        tanHalf = h->scopeTanHalf;
        camera = h->scopeCamera;
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq == s1) return true;
    }
    return false;
}

// What the game publishes about the off-hand knife (v25).
struct KnifeInfo {
    std::uint32_t caps, state, pawnSeq;
    float         damage;
};
// Seqlock read of it; false while the game is mid-write (try next frame).
inline bool ReadKnifeInfo(const Header* h, KnifeInfo& k) {
    const std::uint32_t s1 = h->knifeSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    k.caps = h->knifeCaps;
    k.state = h->knifeState;
    k.pawnSeq = h->knifePawnSeq;
    k.damage = h->knifeDamage;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    return h->knifeSeq == s1;
}
// The host's knife flags (v25, inside the view seqlock).
inline bool ReadKnifeFlags(const Header* h, std::uint32_t& flags) {
    for (int t = 0; t < 64; ++t) {
        const std::uint32_t s1 = h->viewSeq;
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        flags = h->knifeFlags;
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq == s1) return true;
    }
    return false;
}

// The off-hand grenade's events (nadeEvt low byte) and types (bits 8-15); flags (bits 16-23): bit16 a toss (v20).
enum NadeEvent : std::uint32_t { kNadeTake = 1, kNadePin = 2, kNadeCook = 3, kNadeThrow = 4, kNadePutBack = 5 };
inline constexpr std::uint32_t kNadeFrag = 0, kNadeGammon = 1, kNadeStick = 2, kNadeAny = 0xFF;
inline constexpr std::uint32_t kNadeToss = 1u << 16;
// v22: the gun hand's grenade ([Weapon] GrenadePin): its events carry kNadeMain (the grenade in the gun hand, not one taken
// from a holster: a PIN starts the hold); nadeState bit4 says the hold is the gun hand's; nadeFlags bit7 likewise.
inline constexpr std::uint32_t kNadeMain = 1u << 17;

// What the game publishes for the off-hand grenade (v20).
struct NadeStatus {
    std::uint32_t caps;
    std::int32_t  count[3];
    std::uint32_t next, state;
    float         fuse, fuseLen;
    std::uint32_t ticks, evtAck, pawnSeq, boom;
};
// Seqlock read of it; false while the game is mid-write (try next frame).
inline bool ReadNadeStatus(const Header* h, NadeStatus& s, std::uint32_t& seq) {
    const std::uint32_t s1 = h->nadeSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    s.caps = h->nadeCaps;
    for (int i = 0; i < 3; ++i) s.count[i] = h->nadeCount[i];
    s.next = h->nadeNext;
    s.state = h->nadeState;
    s.fuse = h->nadeFuse;
    s.fuseLen = h->nadeFuseLen;
    s.ticks = h->nadeTicks;
    s.evtAck = h->nadeEvtAck;
    s.pawnSeq = h->nadePawnSeq;
    s.boom = h->nadeBoom;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    seq = s1;
    return h->nadeSeq == s1;
}

// The off-hand pistol's events (pistolEvt low byte, v21).
enum PistolEvent : std::uint32_t { kPistolDraw = 1, kPistolShot = 2, kPistolHolster = 3 };

// What the game publishes for the off-hand pistol (v21).
struct PistolStatus {
    std::uint32_t caps;
    char          key[48];
    std::int32_t  clip, max;
    std::uint32_t state, shots, dry, evtAck, pawnSeq, refills;
    float         aimDistance;
};
// Seqlock read of it; false while the game is mid-write (try next frame).
inline bool ReadPistolStatus(const Header* h, PistolStatus& s, std::uint32_t& seq) {
    const std::uint32_t s1 = h->pistolSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    s.caps = h->pistolCaps;
    for (int i = 0; i < 48; ++i) s.key[i] = h->pistolKey[i];
    s.key[47] = 0;
    s.clip = h->pistolClip;
    s.max = h->pistolMax;
    s.state = h->pistolState;
    s.shots = h->pistolShots;
    s.dry = h->pistolDry;
    s.evtAck = h->pistolEvtAck;
    s.pawnSeq = h->pistolPawnSeq;
    s.refills = h->pistolRefills;
    s.aimDistance = h->pistolAimDistance;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    seq = s1;
    return h->pistolSeq == s1;
}

// What the game publishes for the host's manual reload (v14).
struct ReloadGeo {
    std::uint32_t caps, state;
    char          key[48];
    float         magGrab[3], magOut[3], magGrabR, boltGrab[3], boltBack[3], boltTravel;
    std::int32_t  clip, max, reserve;
    Pose          magHeld;
    float         magLen, magSeat;  // v17: the slide insert (0 = snap)
    float         actPath[9][3], actPathS[9];  // v18: the two-stage action's knob path
    std::uint32_t actPathN;
};
// Seqlock read of it; false while the game is mid-write (try next frame).
inline bool ReadReloadGeo(const Header* h, ReloadGeo& g, std::uint32_t& seq) {
    const std::uint32_t s1 = h->reloadGeoSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    g.caps = h->reloadCaps;
    g.state = h->reloadState;
    for (int i = 0; i < 48; ++i) g.key[i] = h->reloadKey[i];
    g.key[47] = 0;
    for (int i = 0; i < 3; ++i) {
        g.magGrab[i] = h->magGrab[i];
        g.magOut[i] = h->magOut[i];
        g.boltGrab[i] = h->boltGrab[i];
        g.boltBack[i] = h->boltBack[i];
    }
    g.magGrabR = h->magGrabR;
    g.boltTravel = h->boltTravel;
    g.clip = h->ammoClip;
    g.max = h->ammoMax;
    g.reserve = h->ammoReserve;
    g.magHeld = h->magHeld;
    g.magLen = h->magLen;
    g.magSeat = h->magSeat;
    g.actPathN = h->actPathN < 9u ? h->actPathN : 9u;
    for (int i = 0; i < 9; ++i) {
        g.actPathS[i] = h->actPathS[i];
        for (int k = 0; k < 3; ++k) g.actPath[i][k] = h->actPath[i][k];
    }
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    seq = s1;
    return h->reloadGeoSeq == s1;
}

// Seqlock read of the grip adjustments (v16) for `key`; false if mid-write or for another weapon.
inline bool ReadGripAdj(const Header* h, const char* key, float (&adj)[3][6], std::uint32_t& flags) {
    const std::uint32_t s1 = h->gripSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    bool same = true;
    for (int i = 0; i < 48; ++i) {
        if (h->gripKey[i] != key[i]) {
            same = false;
            break;
        }
        if (!key[i]) break;
    }
    for (int g = 0; g < 3; ++g)
        for (int k = 0; k < 6; ++k) adj[g][k] = h->gripAdj[g][k];
    flags = h->gripFlags;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    return same && h->gripSeq == s1;
}

// The gun fit (v8) as one value. foreFwd/foreUp (cm, the gun's frame from the gun hand's controller: where the other
// hand holds the foregrip) are the host's only -- they shape gunPose.
struct GunFit {
    float grip[3];
    float angle;
    float rayUp, rayRight;
    float foreFwd, foreUp;
    float foreRight = 0.0f;  // the player (2026-10-01: the M18's foregrip to the left): + = right, cm
};

// Seqlock readers spin a little when they meet the host mid-write: the host holds the lock only for the copies
// (round 19), so a retry nearly always gets the frame, where giving up had dropped the shot to the game's own aim.
inline constexpr int kSeqTries = 64;

// Seqlock read of the host's gun (v9); false if no gun pose this frame, or (flags = 0xFFFFFFFF) still mid-write.
// v23: optionally the frame's display time (XrTime, ns), the melee bits, the head and both hands, from the same frame.
inline bool ReadGun(const Header* h, Pose& gun, Pose& aim, std::uint32_t& flags, std::int64_t* displayTime = nullptr,
                    std::uint32_t* meleeOn = nullptr, Pose* head = nullptr, Pose* hands = nullptr) {
    for (int t = 0; t < kSeqTries; ++t) {
        const std::uint32_t s1 = h->viewSeq;
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        const std::uint32_t f = h->gunFlags;
        gun = h->gunPose;
        aim = h->aimRay;
        const std::int64_t dt = h->viewDisplayTime;
        const std::uint32_t mo = h->meleeOn;
        if (head) *head = h->head;
        if (hands) {
            hands[0] = h->hand[0];
            hands[1] = h->hand[1];
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq != s1) continue;
        flags = f;
        if (displayTime) *displayTime = dt;
        if (meleeOn) *meleeOn = mo;
        return (f & 1u) != 0;
    }
    flags = 0xFFFFFFFFu;
    return false;
}

// Seqlock read of the host's fit; false if mid-write or none. `key` gets fitKey (NUL-terminated).
inline bool ReadFit(const Header* h, GunFit& fit, char (&key)[48]) {
    const std::uint32_t s1 = h->fitSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    const bool valid = h->fitValid != 0;
    for (int i = 0; i < 48; ++i) key[i] = h->fitKey[i];
    key[47] = 0;
    for (int i = 0; i < 3; ++i) fit.grip[i] = h->fitGrip[i];
    fit.angle = h->fitAngle;
    fit.rayUp = h->fitRayUp;
    fit.rayRight = h->fitRayRight;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    return valid && h->fitSeq == s1;
}

// Seqlock read of the views; false if the host is mid-write (just try again next frame).
inline bool ReadViews(const Header* h, Pose& head, Pose (&eye)[2], Fov (&fov)[2]) {
    for (int t = 0; t < kSeqTries; ++t) {
        const std::uint32_t s1 = h->viewSeq;
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
        if (!h->viewValid) return false;
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        head = h->head;
        eye[0] = h->eye[0];
        eye[1] = h->eye[1];
        fov[0] = h->eyeFov[0];
        fov[1] = h->eyeFov[1];
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq == s1) return true;
    }
    return false;
}

// The host's manual-reload inputs to the game's bake (v14), from the same host frame as the hands.
struct ReloadView {
    std::uint32_t flags, keyHash;
    float         magPull;
    Pose          magPose;
    float         rack;
};
// The host's off-hand grenade inputs (v20), from the same host frame as the hands.
struct NadeView {
    std::uint32_t flags;
    Pose          pose;
    float         adj[6];
};
// The host's off-hand pistol inputs (v21), likewise.
struct PistolView {
    std::uint32_t flags;
    float         trigger;
    Pose          ray;
    float         fit[4];
};

// Seqlock read of the aim poses (v7); false if the host is mid-write. `valid` = hdr->handValid bits. `rv` (optional):
// the manual reload's flags, pull, held magazine and rack in the same pass (RELOAD-DESIGN X3); `nv` (optional) the
// off-hand grenade's (v20); `pv` (optional) the off-hand pistol's (v21).
inline bool ReadHands(const Header* h, Pose (&hand)[2], std::uint32_t& valid, ReloadView* rv = nullptr, NadeView* nv = nullptr,
                      PistolView* pv = nullptr) {
    for (int t = 0; t < kSeqTries; ++t) {
        const std::uint32_t s1 = h->viewSeq;
        if (s1 & 1u) {
            _mm_pause();
            continue;
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        valid = h->handValid;
        hand[0] = h->hand[0];
        hand[1] = h->hand[1];
        if (rv) {
            rv->flags = h->reloadFlags;
            rv->keyHash = h->reloadKeyHash;
            rv->magPull = h->magPull;
            rv->magPose = h->magPose;
            rv->rack = h->rack;
        }
        if (nv) {
            nv->flags = h->nadeFlags;
            nv->pose = h->nadePose;
            for (int i = 0; i < 6; ++i) nv->adj[i] = h->nadeAdj[i];
        }
        if (pv) {
            pv->flags = h->pistolFlags;
            pv->trigger = h->pistolTrigger;
            pv->ray = h->offAimRay;
            for (int i = 0; i < 4; ++i) pv->fit[i] = h->offFit[i];
        }
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq == s1) return true;
    }
    return false;
}

// The virtual pad as XInput lays it out (XINPUT_GAMEPAD, 12 bytes).
struct PadState {
    std::uint16_t buttons;
    std::uint8_t  leftTrigger, rightTrigger;
    std::int16_t  thumbLX, thumbLY, thumbRX, thumbRY;
};
static_assert(sizeof(PadState) == 12, "PadState must match XINPUT_GAMEPAD");

// Seqlock read of the pad; false if the host is mid-write or not driving it. `seq` doubles as the
// XInput packet number (it changes whenever the state does).
inline bool ReadPad(const Header* h, PadState& out, std::uint32_t& seq) {
    const std::uint32_t s1 = h->padSeq;
    if ((s1 & 1u) || !h->padActive) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    out.buttons = h->padButtons;
    out.leftTrigger = h->padLeftTrigger;
    out.rightTrigger = h->padRightTrigger;
    out.thumbLX = h->padThumbLX;
    out.thumbLY = h->padThumbLY;
    out.thumbRX = h->padThumbRX;
    out.thumbRY = h->padThumbRY;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    seq = s1;
    return h->padSeq == s1;
}

}  // namespace mohavr::shared
