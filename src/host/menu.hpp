// The in-headset menu (MOHAVR-host.exe). Drawn with Dear ImGui into its own texture and shown as a
// quad layer in front of the player when open. The game never renders it -- it costs the game no
// address space, and it works in menus, cutscenes and gameplay alike.
//
// Items: World Scale (live, saved), Height (seated/standing offset, live, saved), Turning (smooth /
// snap 30 / snap 45, saved; needs Input.Controllers), Gun fit (a page: the gun in hand's position, angle and aim
// line, live and saved per weapon -- the game names the weapon, hdr->weaponKey), Recentre (the host re-creates
// LOCAL at the head's heading), Reset World Scale, Close.
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
#include "hands.hpp"

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
    // The gun fit in use (the weapon in hand's saved fit, or the defaults): the reticle follows the same aim line.
    const shared::GunFit& Fit() const { return fit_; }
    bool GunInHand() const { return gunInHand_; }
    // Controls (the player's, saved): right stick moves / left turns; the gun hand at start (a draw changes it).
    bool SwapSticks() const { return swapSticks_; }
    bool MoveByHead() const { return moveByHead_; }
    bool RedDot() const { return redDot_; }  // the reticle shown (the player's; default the shipped [Aim] Reticle)
    bool Pacing() const { return pacing_; }  // frame pacing (the player's once toggled; default the shipped [Bridge] Pace)
    const std::string& WeaponKey() const { return weaponKey_; }  // the weapon in hand's class ("" none)
    // Holsters (the Holsters page; the player's, saved): load with the shipped spots (metres), then the current ones.
    void LoadHolsters(const HolsterSpot (&defaults)[kHolsters]);
    const HolsterSpot& Spot(int i) const { return spots_[i]; }
    int  RingsMode() const { return ringsMode_; }                 // 0 never, 1 near, 2 always
    bool HolsterPageOpen() const { return visible_ && page_ == 2; }
    bool StartLeft() const { return startLeft_; }

private:
    void Render();
    void RenderFitPage();
    void RenderHolsterPage();
    void SaveHolster(int i);
    void RenderFreeHandPage();
    void PublishFreeHand(bool save);
    void SetUnitsPerMeter(float v, bool save);
    void SetHeightOffset(float v, bool save);
    void Save();
    // Gun fit (M8): follows hdr->weaponKey, loads that weapon's fit from the player's ini, publishes it.
    void SyncWeapon();
    bool ReadFit(const std::wstring& ini, shared::GunFit& f) const;
    shared::GunFit WeaponDefault() const;
    void PublishFit();
    void SaveFit();
    void AdjustFit(int item, float dir);

    ID3D11Device*           dev_ = nullptr;
    ID3D11DeviceContext*    ctx_ = nullptr;
    shared::Header*         hdr_ = nullptr;
    XrSwapchain             swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11Texture2D*        tex_ = nullptr;   // B8G8R8A8_UNORM, ImGui draws here
    ID3D11RenderTargetView* rtv_ = nullptr;
    int                     width_ = 1024, height_ = 990;
    std::wstring            shippedPath_;  // MOHAVR.ini next to the host: the shipped [GunFit] per weapon
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
    int                     page_ = 0;                // 0 main, 1 gun fit, 2 holsters, 3 free hand
    float                   freeHand_[4] = {0, 0, 0, 0};  // pitch, yaw, roll (degrees), forward (cm)
    float                   freeHandDef_[4] = {0, 0, 0, 0};  // the shipped [Hands] FreeHand
    HolsterSpot             spots_[kHolsters]{}, spotDefaults_[kHolsters]{};  // metres
    int                     holsterSel_ = 0;
    int                     ringsMode_ = 1;
    std::uint32_t           seenWeaponSeq_ = 0xFFFFFFFFu;
    std::string             weaponKey_;               // the weapon in hand ("" none)
    shared::GunFit          fit_{}, fitDefault_{};    // current; the shipped defaults (MOHAVR.ini)
    bool                    gunInHand_ = false;       // Weapon.ViewModel=2 (the fit applies)
    bool                    swapSticks_ = false, startLeft_ = false, redDot_ = true, pacing_ = false, moveByHead_ = true;
};

}  // namespace mohavr::host
