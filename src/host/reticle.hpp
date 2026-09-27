// The aim reticle (M7, Aim.Reticle): a small dot where the shot will land. The game traces the aim ray
// and publishes how far along it the hit is (hdr->aimDistance); the host draws the dot that far along the
// SAME controller's aim ray, located this XR frame, so it moves with the hand without waiting for a game
// frame. A quad layer at the hit point: both eyes see it at the right depth, and at a fixed angular size.
#pragma once
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

namespace mohavr::host {

class Reticle {
public:
    // `swapchainFormat` must be a B8G8R8A8 format the runtime offers; `sizeDeg` = the dot's angular size.
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t swapchainFormat, float sizeDeg);
    // The quad for this frame: `distance` metres along `aim`'s forward (-Z), facing like `head`.
    const XrCompositionLayerBaseHeader* Layer(XrSpace space, const XrPosef& aim, const XrPosef& head, float distance);

private:
    ID3D11DeviceContext*   ctx_ = nullptr;
    XrSwapchain            swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11Texture2D*       tex_ = nullptr;  // the dot, premultiplied alpha
    float                  tanHalf_ = 0.007f;
    XrCompositionLayerQuad layer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
};

}  // namespace mohavr::host
