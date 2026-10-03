// Scopes you raise to your eye (SCOPE-DESIGN.md) -- host side.
//   * The gate: the gun's scope on (the game publishes it and its tube, shared block v24), both hands on the gun ([Scope]
//     TwoHands), and an eye just behind the eyepiece, near its axis and looking along it -- that eye looks through.
//   * The magnification: the real scope's ([Scope] Zoom=real) or the game's zoom (Zoom=game: from its widest, the
//     turning stick's up / down zooms while looking through); the menu's Scope zoom is the player's.
//   * The scope view: the game renders a third view (the stereo Draw's third player) from the objective down the scope's
//     tube, square, the field of the eyepiece divided by the magnification -- the host asks for it (scopeWant,
//     scopeTanHalf, scopeCamera, inside the view seqlock). The reticle marks the aim line (where the shots go) in it, as a
//     real scope's adjusted reticle does.
//   * The lens: a quad on the drawn eyepiece, for the looking eye only. Each point of it shows the scope view in the
//     direction the eye looks through it, magnified -- a parallax-free scope: the picture and the reticle stay on the
//     target as the eye moves -- with the exit pupil's shadow when the eye is off the axis or out of its relief, the
//     field stop's black ring, and the scope's reticle (a crosshair; the German post and bars).
#pragma once
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class Scope {
public:
    // The shipped ini's [Scope]; `fmt` the swapchain format (B8G8R8A8, as the menu's).
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, const std::wstring& ini);
    bool Ready() const { return ready_; }
    void SetOn(bool on) { on_ = on; }
    void SetZoomGame(bool game) { zoomGame_ = game; }

    struct In {
        bool    gunValid = false, twoHanded = false, gestures = false, hasView = false, eyesOk = false;
        XrPosef gun{}, aimRay{}, eye[2]{};  // LOCAL: the gun pose (the fit's grip), the aim line, the eyes this XR frame
        float   stickY = 0.0f;              // the turning stick's up / down (the game zoom while looking through)
        double  now = 0.0;
    };
    // Per XR frame, before the view seqlock: the gate, the zoom, the camera.
    void Update(const shared::Header* hdr, const In& in);
    bool Active() const { return active_; }
    // The game zoom takes the turning stick's up / down (its crouch flick waits) while looking through.
    bool ZoomStick() const { return active_ && zoomGame_ && info_.gameFov[0] > 0.0f && info_.gameFov[0] < info_.gameFov[1]; }
    std::uint32_t Want() const { return active_ ? (1u | (eye_ == 1 ? 2u : 0u)) : 0u; }
    float TanHalf() const { return tanHalf_; }
    shared::Pose Camera() const { return camera_; }
    // Per XR frame, with the layers: the lens -- the scope view of frame `frameNo` (in `frame` at sc.rect) through the
    // drawn eyepiece (sc.gunPose) from this XR frame's eye. Null when not looking through or no scope view yet.
    const XrCompositionLayerBaseHeader* Layer(XrSpace space, ID3D11Texture2D* frame, std::uint64_t frameNo,
                                              const shared::SlotScope& sc, const XrPosef (&eyeNow)[2], bool eyesOk);

private:
    struct Vtx {
        float x, y, u, v, bright, alpha;
    };
    bool  MakeShaders();
    bool  SourceTexture(UINT w, UINT h, DXGI_FORMAT fmt);
    float Magnification() const;

    ID3D11Device*          dev_ = nullptr;
    ID3D11DeviceContext*   ctx_ = nullptr;
    XrSwapchain            swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    int                    size_ = 512;           // the lens texture (px)
    ID3D11Texture2D*       rt_ = nullptr;         // the lens, drawn here (UNORM: gamma-encoded, copied raw like the menu's)
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D*       src_ = nullptr;        // the scope view, copied out of the game frame
    ID3D11ShaderResourceView* srcView_ = nullptr;
    UINT                   srcW_ = 0, srcH_ = 0;
    std::uint64_t          srcFrame_ = 0;         // the game frame src_ holds
    bool                   srcOk_ = false;
    ID3D11VertexShader*    vs_ = nullptr;
    ID3D11PixelShader*     ps_ = nullptr;
    ID3D11InputLayout*     il_ = nullptr;
    ID3D11Buffer*          vb_ = nullptr;
    ID3D11Buffer*          cb_ = nullptr;
    ID3D11SamplerState*    sampler_ = nullptr;
    ID3D11BlendState*      blend_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    std::vector<Vtx>       verts_;
    XrCompositionLayerQuad layer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool                   ready_ = false;

    // [Scope] settings
    bool  on_ = false, zoomGame_ = false, twoHands_ = true;
    bool  m18_ = true;  // [Scope] M18: the M18's Telescope M86C (its tube in the body mesh, work/research/m18scope)
    float enterDist_ = 0.12f, exitDist_ = 0.16f, enterOff_ = 0.025f, exitOff_ = 0.04f;  // m: behind the eyepiece, off its axis
    float enterCos_ = 0.82f, exitCos_ = 0.64f;     // the eye's look along the axis (35 / 50 deg)
    float field_ = 0.194f;                          // tan of the eyepiece's apparent half field (22 deg across)
    float eyeRelief_ = 0.07f, pupil_ = 0.012f;      // m: where the exit pupil is behind the eyepiece, its forgiving radius
    float glass_ = 0.9f;                            // the glass's share of the eyepiece's outer radius
    float zoomRate_ = 20.0f;                        // deg of the game's FOV a second at full stick

    // The state
    shared::ScopeInfo info_{};
    bool          infoOk_ = false;
    bool          active_ = false;
    int           eye_ = 1;
    int           enterFrames_ = 0;
    float         tanHalf_ = 0.05f;
    shared::Pose  camera_{0, 0, 0, 0, 0, 0, 1};
    float         aimInGun_[3] = {0, 0, -1};  // the aim line's direction in the gun pose's frame (the fit's: constant per gun)
    float         aimFromGun_[3] = {0, 0, 0};  // and its start
    float         zero_ = 50.0f;               // m: the reticle meets the aim line this far out (the scope's zero)
    std::map<std::string, float> gameFov_;  // the game zoom's current FOV per gun (deg)
    double        lastNow_ = 0.0;
    std::string   loggedWhy_;
};

}  // namespace mohavr::host
