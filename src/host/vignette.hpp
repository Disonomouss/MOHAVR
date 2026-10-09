// The comfort vignette (GOAL A2, D62, [Comfort] Vignette / the menu's General tab): while the player moves or turns by
// stick, the edge of the view darkens -- a head-locked quad, black with alpha rising from a clear centre to the edge, faded
// in and out over FadeSeconds. Not for head motion (that is the player's own). Layered right after the game's image, so
// the reticle, the scope, the wrist HUD, the rings and the menu stay above it.
#pragma once
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

namespace mohavr::host {

class Vignette {
public:
    // `swapchainFormat` must be a B8G8R8A8 format the runtime offers.
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t swapchainFormat, float fadeSeconds);
    // 0 none, 1 light, 2 strong (the menu; rebuilds the masks); 3 solid black (D88: the wall fade).
    void SetStrength(int s);
    void SetName(const char* name, const char* why) { name_ = name; why_ = why; }
    int  Strength() const { return strength_; }
    // Per XR frame: `motion` 0..1, how much the stick moves or turns the player (Pad::Motion).
    void Update(float dt, float motion);
    // The quad for this frame in `view` (head-locked), or null while clear.
    const XrCompositionLayerBaseHeader* Layer(XrSpace view);

private:
    static constexpr int kSize = 256, kLevels = 16;
    void Build();
    ID3D11Device*          dev_ = nullptr;
    ID3D11DeviceContext*   ctx_ = nullptr;
    XrSwapchain            swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    int64_t                fmt_ = 0;
    ID3D11Texture2D*       tex_[kLevels + 1] = {};  // level 0 clear .. kLevels full (premultiplied BGRA)
    int                    strength_ = 0;
    float                  fade_ = 0.0f, fadeSec_ = 0.2f;
    int                    logged_ = 0;
    bool                   shownLast_ = false;
    const char*            name_ = "vignette";
    const char*            why_ = "in (moving by stick)";
    XrCompositionLayerQuad layer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
};

}  // namespace mohavr::host
