// The in-headset menu (MOHAVR-host.exe). Drawn with Dear ImGui into its own texture and shown as a
// quad layer in front of the player when open. The game never renders it -- it costs the game no
// address space, and it works in menus, cutscenes and gameplay alike.
//
// Items: World Scale (live, saved), Height (seated/standing offset, live, saved), Turning (smooth /
// snap 30 / snap 45, saved; needs Input.Controllers), Recentre (the host re-creates LOCAL at the
// head's heading), Reset World Scale, Close.
// Settings are the PLAYER's (lessons 1): %LOCALAPPDATA%\MOHAVR\MOHAVR.user.ini, separate from the
// shipped defaults, outside the game folder, never touched by deploy/undeploy.
#pragma once
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>
#include <vector>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

// Digital menu input for one frame (edges; the caller turns stick deflection into repeats).
struct MenuInput {
    bool toggle = false, up = false, down = false, left = false, right = false, select = false, back = false;
};

class Menu {
public:
    // `swapchainFormat` must be a B8G8R8A8 format the runtime offers (sRGB preferred).
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t swapchainFormat, shared::Header* hdr);

    // Applies the player's saved settings to the shared block (call once the game's defaults are there).
    void ApplySavedSettings();

    // Per XR frame. `head` = VIEW pose in LOCAL at the predicted display time (used to place the
    // panel when it opens). Renders when visible.
    void Update(float dt, const MenuInput& in, const XrPosef& head, bool headValid);

    bool Visible() const { return visible_; }
    // The quad layer for this frame (only when Visible()).
    const XrCompositionLayerBaseHeader* Layer(XrSpace local);

    // True once after the player chose Recentre; the host then re-creates its LOCAL space.
    bool TakeRecenterRequest() { const bool r = recenterRequested_; recenterRequested_ = false; return r; }
    // After a recentre the panel's old pose is meaningless: close it.
    void Close();
    // Turning (Comfort): 0 = smooth, else the snap step in degrees. Used by the virtual pad.
    int SnapTurnDegrees() const { return snapDeg_; }

private:
    void Render();
    void SetUnitsPerMeter(float v, bool save);
    void SetHeightOffset(float v, bool save);
    void Save();

    ID3D11Device*           dev_ = nullptr;
    ID3D11DeviceContext*    ctx_ = nullptr;
    shared::Header*         hdr_ = nullptr;
    XrSwapchain             swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11Texture2D*        tex_ = nullptr;   // B8G8R8A8_UNORM, ImGui draws here
    ID3D11RenderTargetView* rtv_ = nullptr;
    int                     width_ = 1024, height_ = 480;
    XrCompositionLayerQuad  layer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrPosef                 panelPose_{};
    bool                    visible_ = false;
    bool                    rendered_ = false;  // at least one image released since opening
    int                     selected_ = 0;
    float                   unitsPerMeter_ = 100.0f;
    float                   heightOffset_ = 0.0f;   // metres
    int                     snapDeg_ = 0;           // 0 = smooth turning
    bool                    recenterRequested_ = false;
    std::wstring            iniPath_;
};

}  // namespace mohavr::host
