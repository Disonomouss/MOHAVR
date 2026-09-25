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

namespace mohavr::shared {

inline constexpr std::uint32_t kMagic   = 0x3152564D;  // "MVR1"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kRing    = 3;

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
};
#pragma pack(pop)

static_assert(sizeof(Header) == 368, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, publishedFrame) == 80, "shared::Header layout must match between x86 and x64");
static_assert(offsetof(Header, ackFrame) == 96, "shared::Header layout must match between x86 and x64");

}  // namespace mohavr::shared
