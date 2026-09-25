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
inline constexpr std::uint32_t kVersion = 4;           // 2: views + render pose (M3); 3: per-eye meta (M4); 4: live settings
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
    float                  reservedSettings[6];
};
#pragma pack(pop)

static_assert(sizeof(Pose) == 28 && sizeof(Fov) == 16 && sizeof(SlotMeta) == 96, "shared structs must be packed identically");
static_assert(offsetof(Header, publishedFrame) == 80, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, ackFrame) == 96, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, viewSeq) == 368, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, viewDisplayTime) == 376, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, slotMeta) == 500, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, defaultUnitsPerMeter) == 788, "shared::Header layout must match between x86 and x64");
static_assert(sizeof(Header) == 824, "shared::Header layout must match between x86 and x64");  // 788 + 4 + 4 + 24 = 820, padded to 8

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

}  // namespace mohavr::shared
