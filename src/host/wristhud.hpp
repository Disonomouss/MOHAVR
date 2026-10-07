// The wrist HUD's host side (WRISTHUD-DESIGN.md, D55). The game draws its HUD pass once into a texture of the mod's own and
// publishes it with each frame (shared block v28: a second texture ring, premultiplied BGRA, and per slot the HUD elements'
// rectangles, read live from the game's HUD). Here:
//   * two panels on the off hand's wrist (the opposite of the hand holding the gun): health + the compass ("the minimap": the
//     radar compass with its beads -- the game's MiniMap is never created in single player) + the stance icon on the
//     player's left, the weapon and grenade info (ammo, the icons, the exp bars, the level badges and kill medals while they
//     show) on the right; facing up with the palm flat and face down; placed every XR frame from the pose the frame's arms
//     were drawn with (else the controller's); shown when the wrist faces the eyes and the player looks at it (or always),
//     fading in and out;
//   * the rest of the HUD texture (hit indicators, the grenade warning, objectives and notifications, prompts, the
//     stopwatch, the letterbox / fade) on one head-locked quad where the screen panel would be.
// One swapchain (an atlas: the rest's cell and the two panels' cells), composed by a small shader (crop, fade, backing).
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

// The player's HUD settings (the menu's HUD tab; [HUD] in the player's ini, the shipped ones the defaults).
struct HudSettings {
    int   place = 0;     // 0 screen (the head-locked panel in the eyes), 1 wrist
    int   show = 0;      // 0 when looked at, 1 always
    int   layout = 0;    // 0 forearm (the forearm across the chest), 1 across (the arm pointing forward)
    int   backing = 1;   // 0 none, 1 dim, 2 dark (a plate behind the wrist panels)
    // Per wrist panel (0 the left: health and compass; 1 the right: weapon and grenades): along, across, out (cm, moved
    // from where it sits by default), size (%), tilt (deg, toward the eyes).
    float panel[2][5] = {{0, 0, 0, 100, 15}, {0, 0, 0, 100, 15}};
    float screen[3] = {2.0f, 2.4f, 0.1f};  // the screen panel (and the rest quad on the wrist): distance, width, down (m)
};

class WristHud {
public:
    // `game` = the game process (to duplicate the texture handles). False: no wrist HUD (the game has no HUD ring).
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, shared::Header* hdr, HANDLE game,
              const std::wstring& ini);
    bool Ready() const { return ready_; }

    // At the host's frame copy, between its GPU wait for the game's fence and its signal: the slot's HUD texture copied
    // and what it holds kept (shared::SlotHud).
    void TakeFrame(unsigned slot, const shared::SlotHud& hud);

    struct In {
        bool        hasView = false;     // a head-tracked frame (not a menu / cutscene screen)
        bool        gameMenu = false;    // one of the game's menus is open
        bool        modMenu = false;     // the MOHAVR menu is open
        bool        wristPage = false;   // its Wrist panels page (the panels always show)
        int         offHand = 0;         // 0 left, 1 right (the opposite of the hand holding the gun)
        XrPosef     offPose{};           // the off hand's aim pose now (LOCAL)
        bool        offTracked = false;  // really tracked
        bool        foregrip = false;    // the off hand holds the gun's foregrip (no pop-up then unless looked at)
        XrPosef     gunPose{};           // GOAL C1: the gun hand's aim pose (LOCAL) -- the gun's line, the forearm behind it
        bool        gunOk = false;       //   a gun in that hand, tracked
        XrPosef     head{};
        bool        headOk = false;
        double      now = 0.0;
        HudSettings set;
    };
    void Update(const In& in);
    // D60: the gate is open this frame (the off hand palm down and looked at, or always; whichever HUD place): the off hand's
    // lower face button is then the MOHAVR menu's (main.cpp).
    bool PanelsUp() const { return want_ && panelsOn_; }
    // This frame's layers (the rest quad, then the two panels), at most `max`; returns how many were written.
    int Layers(XrSpace local, XrSpace view, const XrCompositionLayerBaseHeader** out, int max, bool panelsAllowed = true);
    // Diagnostics: the last HUD texture taken and the composed atlas.
    ID3D11Texture2D* HudTexture() const { return hud_; }
    ID3D11Texture2D* Atlas() const { return atlas_; }

private:
    bool MakeShaders();
    void Compose(bool rest, bool panels);
    bool PanelCrop(int p, float (&crop)[4], float (&core)[4]) const;

    ID3D11Device*             dev_ = nullptr;
    ID3D11DeviceContext*      ctx_ = nullptr;
    shared::Header*           hdr_ = nullptr;
    bool                      ready_ = false;
    std::vector<ID3D11Texture2D*> shared_;      // the game's ring
    ID3D11Texture2D*          hud_ = nullptr;   // the last HUD texture taken
    ID3D11ShaderResourceView* hudView_ = nullptr;
    unsigned                  texW_ = 0, texH_ = 0;
    shared::SlotHud           slot_{};          // what hud_ holds
    bool                      haveSlot_ = false;
    std::uint64_t             taken_ = 0;
    // the atlas: the rest's cell (texW x texH at 0,0), the panels' cells (kCell square, below it)
    ID3D11Texture2D*          atlas_ = nullptr;
    ID3D11RenderTargetView*   atlasRtv_ = nullptr;
    int                       atlasW_ = 0, atlasH_ = 0;
    XrSwapchain               swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    ID3D11VertexShader*       vs_ = nullptr;
    ID3D11PixelShader*        ps_ = nullptr;
    ID3D11Buffer*             cb_ = nullptr;
    ID3D11BlendState*         blend_ = nullptr;
    ID3D11RasterizerState*    raster_ = nullptr;
    XrCompositionLayerQuad    layers_[3]{};
    // the gate and the panels
    float                     fade_ = 0.0f;
    bool                      want_ = false;    // the gate's verdict (with its hysteresis)
    bool                      restOn_ = false, panelsOn_ = false;
    float                     cropNow_[2][4] = {};
    XrPosef                   panelPose_[2]{};
    float                     panelSize_[2][2] = {};
    HudSettings               set_{};
    double                    lastNow_ = 0.0;
    const char*               why_ = "";
    float                     loggedPanel_[2][5] = {};  // the offsets last logged ("panels moved": the menu's tests)
    int                       loggedLayout_ = -1;
    // ini ([HUD], the shipped)
    float                     mmPerPx_ = 0.30f;   // WristScale: mm per HUD px at size 100 %
    float                     wristCentre_[3] = {-13.0f, 0.0f, 3.0f};  // WristCentre: cm along the forearm, across, out
    float                     gapCm_ = 1.0f;      // WristGap: between the two panels
    float                     angleCos_ = 0.0f, lookCos_ = 0.0f;  // WristAngle, WristLook (deg -> cos)
    bool                      followDrawn_ = true;  // WristFollow=drawn (the arms' pose) | controller
    float                     fadeIn_ = 0.12f, fadeOut_ = 0.25f;
    bool                      occlusion_ = true;          // [HUD] WristOcclusion (GOAL C1)
    float                     occ_[2] = {1.0f, 1.0f};     // each panel's dimming behind the gun (1 clear)
    bool                      occBehind_[2] = {};
};

}  // namespace mohavr::host
