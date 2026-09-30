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
inline constexpr std::uint32_t kVersion = 16;          // 2: views + render pose (M3); 3: per-eye meta (M4); 4: live settings; 5: recentre + height; 6: virtual pad; 7: aim poses; 8: gun fit; 9: hands; 10: throwing; 11: weapon kind; 12: free hand; 13: view times; 14: manual reload; 15: the reload grips' held magazine; 16: grip adjustments
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
                                           // trigger releases a locked-back action (TriggerRack)
    char                   reloadKey[48];  // the attachment class the geometry is for
    float                  magGrab[3];     // the in-gun magazine's grab point = the insert target
    float                  magOut[3];      // unit: the way the magazine leaves the well
    float                  magGrabR;       // metres: its grab radius
    float                  boltGrab[3];    // the handle / slide grip point at its held position (without the pull)
    float                  boltBack[3];    // unit: the pull direction
    float                  boltTravel;     // metres from the held position to full back
    std::int32_t           ammoClip, ammoMax, ammoReserve;  // ammoReserve = the reserve + the rounds owed (RELOAD-DESIGN 2.2)
    std::uint32_t          reloadState;    // bit0 magIn, bit1 pending, bit2 ready (closed: clip >= 1; open: cocked), bit3
                                           // action held back, bit4 rack needed, bit5 open-bolt gun, bit6 infinite ammo
    volatile std::uint32_t reloadPawnSeq;  // bumped on a new local pawn: the host resets every gun to "in the gun"
    volatile std::uint32_t reloadEvtAck;   // the last event the game processed (applied or rejected)
    // host -> game, per XR frame inside the view seqlock (viewSeq), next to gunPose
    std::uint32_t          reloadFlags;    // bit0 manual reload on (the player's toggle); bits 1-2 the magazine (0 in gun,
                                           // 1 grabbed, 2 in the off hand, 3 out); bit3 action held; bit4 engaged (the
                                           // pipeline is alive: the game blocks its own reload only then); bit5 the held
                                           // taped pair flipped (its other half toward the well)
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
    std::uint32_t          gripFlags;      // 1620 bit0: the magazine is grabbed with the held grip (round 34)
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
static_assert(sizeof(Header) == 1624, "shared::Header layout must match between x86 and x64");

// Manual reload events (reloadEvt low byte) and the key hash both sides use.
// kReloadInsertOther: a taped pair inserted flipped -- its other half goes in (twin magazines).
enum ReloadEvent : std::uint32_t { kReloadEject = 1, kReloadInsert = 2, kReloadRack = 3, kReloadTake = 4, kReloadDrop = 5,
                                   kReloadInsertOther = 6 };
inline std::uint32_t KeyHash(const char* s) {  // FNV-1a 32
    std::uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<std::uint8_t>(*s)) * 16777619u;
    return h;
}

// What the game publishes for the host's manual reload (v14).
struct ReloadGeo {
    std::uint32_t caps, state;
    char          key[48];
    float         magGrab[3], magOut[3], magGrabR, boltGrab[3], boltBack[3], boltTravel;
    std::int32_t  clip, max, reserve;
    Pose          magHeld;
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
};

// Seqlock readers spin a little when they meet the host mid-write: the host holds the lock only for the copies
// (round 19), so a retry nearly always gets the frame, where giving up had dropped the shot to the game's own aim.
inline constexpr int kSeqTries = 64;

// Seqlock read of the host's gun (v9); false if no gun pose this frame, or (flags = 0xFFFFFFFF) still mid-write.
inline bool ReadGun(const Header* h, Pose& gun, Pose& aim, std::uint32_t& flags) {
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
#if defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        if (h->viewSeq != s1) continue;
        flags = f;
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

// Seqlock read of the aim poses (v7); false if the host is mid-write. `valid` = hdr->handValid bits. `rv` (optional):
// the manual reload's flags, pull, held magazine and rack in the same pass (RELOAD-DESIGN X3).
inline bool ReadHands(const Header* h, Pose (&hand)[2], std::uint32_t& valid, ReloadView* rv = nullptr) {
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
