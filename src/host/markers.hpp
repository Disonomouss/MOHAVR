// Gesture spot markers (M8, [Hands] Rings; the menu): a ring around each holster spot and the off hand's foregrip and
// magazine spots, lit while a hand is inside, and a dot on the off hand (the game shows nothing there). Billboarded
// quad layers in LOCAL, all from one small texture atlas (ring | lit ring | dot).
#pragma once
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

#include "hands.hpp"

namespace mohavr::host {

class Markers {
public:
    enum Mode { kNever = 0, kNear = 1, kAlways = 2 };
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t swapchainFormat);
    // Appends this frame's marker layers to `out` (at most `max`); returns how many. `showAll`: every holster ring
    // whatever the mode (the menu's Holsters page).
    int Layers(XrSpace space, const XrPosef& head, const Hands::Output& h, Mode mode, bool showAll,
               const XrCompositionLayerBaseHeader** out, int max);

private:
    ID3D11DeviceContext*   ctx_ = nullptr;
    XrSwapchain            swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11Texture2D*       tex_ = nullptr;
    XrCompositionLayerQuad quads_[kHolsters + 3]{};  // the spots and the off-hand dot
    static constexpr int   kCell = 64;
};

}  // namespace mohavr::host
