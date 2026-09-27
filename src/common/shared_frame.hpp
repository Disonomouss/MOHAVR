// Shared-memory contract between the game-side mod (x86, dinput8.dll) and MOHAVR-host.exe
// (x64) -- D10. Compiled into both, so it uses fixed-size fields only: the layout is identical
// in a 32-bit and a 64-bit process (checked by the static_asserts).
//
// Mapping name: Local\MOHAVR_<gamePid>. The game creates it; the host opens it.
//
// Frame handoff (mono in M2; per-eye slots come with stereo):
//   * the game owns a ring of kRing shared D3D12 textures and two shared fences;
//   * game: publishes frame N (1, 2, 3, ...) into slot N % kRing only when the host has
//     acknowledged frame N-1 (ackFrame == publishedFrame). Before overwriting the slot it
//     GPU-waits hostFence >= N - kRing, copies, signals gameFence = N, then stores
//     publishedSlot and publishedFrame (in that order);
//   * host: sees publishedFrame F > its last, sets ackFrame = F, GPU-waits gameFence >= F,
//     copies slot F % kRing into its own texture, signals hostFence = F.
//   Every published frame is consumed exactly once, so hostFence always catches up and the
//   game's GPU wait never deadlocks. The game never blocks on the CPU: when the host hasn't
//   acknowledged yet, the game simply skips publishing that frame.
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
inline constexpr std::uint32_t kVersion = 9;           // 2: views + render pose (M3); 3: per-eye meta (M4); 4: live settings; 5: recentre + height; 6: virtual pad; 7: aim poses; 8: gun fit; 9: hands
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
static_assert(sizeof(Header) == 1176, "shared::Header layout must match between x86 and x64");

// The gun fit (v8) as one value. foreFwd/foreUp (cm, the gun's frame from the gun hand's controller: where the other
// hand holds the foregrip) are the host's only -- they shape gunPose.
struct GunFit {
    float grip[3];
    float angle;
    float rayUp, rayRight;
    float foreFwd, foreUp;
};

// Seqlock read of the host's gun (v9); false if mid-write or no gun pose this frame.
inline bool ReadGun(const Header* h, Pose& gun, Pose& aim, std::uint32_t& flags) {
    const std::uint32_t s1 = h->viewSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    flags = h->gunFlags;
    gun = h->gunPose;
    aim = h->aimRay;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    return (flags & 1u) && h->viewSeq == s1;
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
    const std::uint32_t s1 = h->viewSeq;
    if ((s1 & 1u) || !h->viewValid) return false;
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
    return h->viewSeq == s1;
}

// Seqlock read of the aim poses (v7); false if the host is mid-write. `valid` = hdr->handValid bits.
inline bool ReadHands(const Header* h, Pose (&hand)[2], std::uint32_t& valid) {
    const std::uint32_t s1 = h->viewSeq;
    if (s1 & 1u) return false;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    valid = h->handValid;
    hand[0] = h->hand[0];
    hand[1] = h->hand[1];
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
    return h->viewSeq == s1;
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
