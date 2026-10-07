// Swapchain format helpers (GOAL B2, D65): the host prefers a B8G8R8A8 swapchain (the game's frames are BGRA and copy
// straight in) but takes R8G8B8A8 when the runtime offers nothing else ([Debug] ForceRgbaSwapchain forces it for tests).
// Raw copies between the two families are invalid, so every texture copied into a swapchain image is made in the
// swapchain's family: render targets via RtFormat (their shaders write the right colours either way), CPU-packed pixels
// via Pack, and the game's frame through blit.cpp.
#pragma once
#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>

namespace mohavr::host {

inline bool IsRgba(std::int64_t f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }

// A render target to be copied into a swapchain of format `f` (UNORM: the frames hold gamma-encoded colour, so an sRGB
// swapchain of the same family shows them right through a raw copy).
inline DXGI_FORMAT RtFormat(std::int64_t f) { return IsRgba(f) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM; }

// One pixel for a texture of the swapchain's family, from 0..1 channels (already premultiplied where that matters).
inline std::uint32_t Pack(std::int64_t f, float r, float g, float b, float a) {
    auto b8 = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IsRgba(f) ? (b8(r) | (b8(g) << 8) | (b8(b) << 16) | (b8(a) << 24))
                     : (b8(b) | (b8(g) << 8) | (b8(r) << 16) | (b8(a) << 24));
}

}  // namespace mohavr::host
