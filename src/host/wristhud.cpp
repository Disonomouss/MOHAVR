#include "wristhud.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../mohavr/log.hpp"

#pragma comment(lib, "d3dcompiler")

namespace mohavr::host {
namespace {

struct V3 {
    float x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Unit(V3 a) {
    const float l = Len(a);
    return l > 1e-9f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}
V3 P(const XrVector3f& v) { return {v.x, v.y, v.z}; }
V3 Rot(const XrQuaternionf& q, V3 v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z), tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
// The rotation whose columns are x, y, z (orthonormal, right-handed).
XrQuaternionf FromAxes(V3 x, V3 y, V3 z) {
    const float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
XrPosef ToXr(const shared::Pose& p) { return {{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}}; }
float IniF(const std::wstring& ini, const wchar_t* key, float def, float lo, float hi) {
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"HUD", key, L"", v, 32, ini.c_str());
    const float f = static_cast<float>(_wtof(v));
    return v[0] && f >= lo && f <= hi ? f : def;
}
HANDLE DupFromGame(HANDLE game, std::uint64_t value) {
    HANDLE out = nullptr;
    if (!value || !DuplicateHandle(game, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)), GetCurrentProcess(), &out, 0,
                                   FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return out;
}

constexpr int kCell = 512;      // a wrist panel's cell in the atlas (px; the HUD's groups are ~280-350 px)
constexpr int kMasks = 16;

// Per cell: the crop of the HUD texture copied 1:1 into it, faded, over an optional backing plate; inside the rest's cell
// the wrist elements' rectangles are cleared (they show on the wrist).
const char* kShader = R"(
cbuffer C : register(b0) {
    float4 cell;          // xy the cell's origin in the atlas (px), zw the crop's size (px)
    float4 crop;          // xy the crop's origin in the HUD texture (px), z the fade, w the backing plate's alpha
    float4 info;          // x the masks, y the plate's soft edge (px)
    float4 mask[16];      // x0 y0 x1 y1 (HUD texture px)
}
Texture2D t : register(t0);
float4 vs(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_POSITION) : SV_TARGET {
    float2 q = pos.xy - cell.xy;
    if (q.x >= cell.z || q.y >= cell.w) return float4(0, 0, 0, 0);
    float2 s = crop.xy + q;
    float4 c = t.Load(int3(int2(s), 0));
    int n = (int)info.x;
    for (int i = 0; i < n; ++i)
        if (s.x >= mask[i].x && s.x < mask[i].z && s.y >= mask[i].y && s.y < mask[i].w) c = float4(0, 0, 0, 0);
    float2 d = min(q, cell.zw - q);
    float edge = saturate(min(d.x, d.y) / max(info.y, 1.0));
    float a = c.a + crop.w * edge * (1.0 - c.a);
    return float4(c.rgb, a) * crop.z;
}
)";

struct CB {
    float cell[4], crop[4], info[4], mask[kMasks][4];
};

void Union(float (&u)[4], const float (&r)[4], bool& any) {
    if (!(r[2] > r[0] && r[3] > r[1])) return;
    if (!any) {
        std::memcpy(u, r, sizeof(u));
        any = true;
        return;
    }
    u[0] = std::min(u[0], r[0]);
    u[1] = std::min(u[1], r[1]);
    u[2] = std::max(u[2], r[2]);
    u[3] = std::max(u[3], r[3]);
}

}  // namespace

bool WristHud::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, shared::Header* hdr, HANDLE game,
                    const std::wstring& ini) {
    dev_ = dev;
    ctx_ = ctx;
    hdr_ = hdr;
    mmPerPx_ = IniF(ini, L"WristScale", 0.30f, 0.05f, 2.0f);
    {
        wchar_t v[64] = L"";
        GetPrivateProfileStringW(L"HUD", L"WristCentre", L"", v, 64, ini.c_str());
        float c[3];
        if (v[0] && swscanf_s(v, L"%f %f %f", &c[0], &c[1], &c[2]) == 3)
            for (int i = 0; i < 3; ++i) wristCentre_[i] = std::clamp(c[i], -40.0f, 40.0f);
        GetPrivateProfileStringW(L"HUD", L"WristFollow", L"drawn", v, 64, ini.c_str());
        followDrawn_ = _wcsicmp(v, L"controller") != 0;
    }
    gapCm_ = IniF(ini, L"WristGap", 1.0f, 0.0f, 20.0f);
    angleCos_ = std::cos(IniF(ini, L"WristAngle", 55.0f, 5.0f, 90.0f) * 0.0174533f);
    lookCos_ = std::cos(IniF(ini, L"WristLook", 40.0f, 5.0f, 90.0f) * 0.0174533f);
    if (!hdr_ || !(hdr_->hudCaps & 1u) || !hdr_->hudTexW || !hdr_->hudTexH) {
        MLOG("wristhud: the game has no HUD texture ring (HUD.Redirect off, HUD.Mode 0 or its hooks stood down) -- no wrist HUD");
        return false;
    }
    texW_ = hdr_->hudTexW;
    texH_ = hdr_->hudTexH;
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev_->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        MLOG("wristhud: no ID3D11Device1 -- no wrist HUD");
        return false;
    }
    for (unsigned i = 0; i < shared::kRing; ++i) {
        HANDLE h = DupFromGame(game, hdr_->hudTexHandles[i]);
        ID3D11Texture2D* t = nullptr;
        const bool ok = h && SUCCEEDED(dev1->OpenSharedResource1(h, IID_PPV_ARGS(&t)));
        if (h) CloseHandle(h);
        if (!ok) {
            dev1->Release();
            MLOG("wristhud: opening the game's HUD texture %u failed -- no wrist HUD", i);
            return false;
        }
        shared_.push_back(t);
    }
    dev1->Release();
    D3D11_TEXTURE2D_DESC td{};
    shared_[0]->GetDesc(&td);
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &hud_)) || FAILED(dev_->CreateShaderResourceView(hud_, nullptr, &hudView_))) {
        MLOG("wristhud: the HUD copy texture failed -- no wrist HUD");
        return false;
    }
    atlasW_ = std::max(static_cast<int>(texW_), 2 * kCell);
    atlasH_ = static_cast<int>(texH_) + kCell;
    D3D11_TEXTURE2D_DESC ad{};
    ad.Width = static_cast<UINT>(atlasW_);
    ad.Height = static_cast<UINT>(atlasH_);
    ad.MipLevels = ad.ArraySize = 1;
    ad.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ad.SampleDesc.Count = 1;
    ad.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&ad, nullptr, &atlas_)) || FAILED(dev_->CreateRenderTargetView(atlas_, nullptr, &atlasRtv_))) {
        MLOG("wristhud: the atlas failed -- no wrist HUD");
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(atlasW_);
    ci.height = static_cast<uint32_t>(atlasH_);
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &swapchain_))) {
        MLOG("wristhud: xrCreateSwapchain failed -- no wrist HUD");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    if (!MakeShaders()) return false;
    ready_ = true;
    MLOG("wristhud: ready -- the HUD texture %ux%u, the atlas %dx%d; %.2f mm per HUD px, the wrist %.0f %.0f %.0f cm from the "
         "controller, the gate %.0f deg facing / %.0f deg looked at, following the %s", texW_, texH_, atlasW_, atlasH_, mmPerPx_,
         wristCentre_[0], wristCentre_[1], wristCentre_[2], std::acos(angleCos_) * 57.2958f, std::acos(lookCos_) * 57.2958f,
         followDrawn_ ? "drawn arm's pose" : "controller");
    return true;
}

bool WristHud::MakeShaders() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    const size_t len = std::strlen(kShader);
    if (FAILED(D3DCompile(kShader, len, "wristhud", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, len, "wristhud", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        MLOG("wristhud: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    bool ok = SUCCEEDED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
              SUCCEEDED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_));
    vsb->Release();
    psb->Release();
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(CB);
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev_->CreateBuffer(&cd, nullptr, &cb_));
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev_->CreateBlendState(&bl, &blend_));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ok = ok && SUCCEEDED(dev_->CreateRasterizerState(&rd, &raster_));
    if (!ok) MLOG("wristhud: the D3D11 objects failed -- no wrist HUD");
    return ok;
}

void WristHud::TakeFrame(unsigned slot, const shared::SlotHud& hud) {
    if (!ready_ || !hdr_) return;
    slot %= shared::kRing;
    slot_ = hud;  // (the caller's copy, taken with the frame's meta before it acked the frame: a paced game may reuse the slot after)
    haveSlot_ = true;
    if (slot_.flags & 1u) ctx_->CopyResource(hud_, shared_[slot]);
    ++taken_;
    static std::uint32_t seenFlags = 0xFFFFFFFFu;
    static int logged = 0;
    if ((slot_.flags & 7u) != seenFlags && logged < 40) {
        ++logged;
        seenFlags = slot_.flags & 7u;
        MLOG("wristhud: frames now %s%s (canvas %ux%u, resolutionScale %.3f)", (slot_.flags & 1u) ? "carry the HUD pass" : "carry no HUD pass",
             (slot_.flags & 4u) ? ", the elements' rectangles live" : (slot_.flags & 1u) ? ", the default rectangles" : "",
             slot_.canvasW, slot_.canvasH, slot_.rs);
    }
}

// A panel's crop of the HUD texture (and its core: what places it -- the right panel grows with the level badges, the core
// stays put on the wrist).
bool WristHud::PanelCrop(int p, float (&crop)[4], float (&core)[4]) const {
    const float sx = static_cast<float>(texW_) / 1280.0f, sy = static_cast<float>(texH_) / 720.0f;
    bool any = false;
    const bool live = haveSlot_ && (slot_.flags & 4u) && slot_.canvasW == texW_;
    if (p == 0) {
        if (live)
            for (int i : {shared::kHudHealth, shared::kHudCompass, shared::kHudStance}) Union(core, slot_.rect[i], any);
        if (!any) {  // (the design's rectangles at 1280x720, rs 1.0, until the game's are there)
            const float d[4] = {88.0f * sx, 384.0f * sy, 365.0f * sx, 665.0f * sy};
            std::memcpy(core, d, sizeof(core));
        }
        std::memcpy(crop, core, sizeof(crop));
    } else {
        if (live)
            for (int i : {shared::kHudAmmoBar, shared::kHudAmmoText, shared::kHudNadeText, shared::kHudWeaponIcon, shared::kHudNadeIcon,
                          shared::kHudWeaponMedal, shared::kHudNadeMedal})
                Union(core, slot_.rect[i], any);
        if (!any) {
            const float d[4] = {872.0f * sx, 361.0f * sy, 1170.0f * sx, 665.0f * sy};
            std::memcpy(core, d, sizeof(core));
        }
        std::memcpy(crop, core, sizeof(crop));
        if (live) {
            bool more = true;
            for (int i : {shared::kHudWeaponBadge, shared::kHudNadeBadge})
                if ((slot_.shown >> i) & 1u) Union(crop, slot_.rect[i], more);
        }
    }
    // Inside the texture, at most a cell (cut from the top / the right: the core sits bottom-left).
    auto clampBox = [&](float (&r)[4]) {
        r[0] = std::clamp(std::floor(r[0]), 0.0f, static_cast<float>(texW_));
        r[1] = std::clamp(std::floor(r[1]), 0.0f, static_cast<float>(texH_));
        r[2] = std::clamp(std::ceil(r[2]), r[0], static_cast<float>(texW_));
        r[3] = std::clamp(std::ceil(r[3]), r[1], static_cast<float>(texH_));
        r[2] = std::min(r[2], r[0] + kCell);
        r[1] = std::max(r[1], r[3] - kCell);
    };
    clampBox(core);
    clampBox(crop);
    return crop[2] > crop[0] && crop[3] > crop[1];
}

void WristHud::Update(const In& in) {
    const float dt = lastNow_ > 0.0 ? static_cast<float>(std::clamp(in.now - lastNow_, 0.0, 0.1)) : 0.0f;
    lastNow_ = in.now;
    set_ = in.set;
    const bool wristFrame = ready_ && haveSlot_ && (slot_.flags & 3u) == 3u;
    restOn_ = wristFrame && in.hasView && !in.gameMenu && in.headOk;
    const char* why = !ready_ ? "no HUD texture ring" : !wristFrame ? "no wrist HUD pass in the frame" : !in.hasView ? "no view (a menu or cutscene)"
                      : in.gameMenu ? "a game menu" : (in.modMenu && !in.wristPage) ? "the MOHAVR menu" : !in.headOk ? "no head pose"
                      : !in.offTracked ? "the off hand isn't tracked" : (slot_.flags & 32u) ? "the player is dead" : nullptr;
    // The panels' frame on the wrist: from the pose the frame's arms were drawn with (the panels stay on the drawn sleeve),
    // else the controller's now.
    const int oh = in.offHand ? 1 : 0;
    XrPosef pose = in.offPose;
    if (followDrawn_ && haveSlot_ && (slot_.flags & (8u << oh))) pose = ToXr(slot_.hand[oh]);
    const XrQuaternionf& q = pose.orientation;
    const V3 X = Rot(q, {1, 0, 0}), Y = Rot(q, {0, 1, 0}), f = Rot(q, {0, 0, -1});
    const bool left = oh == 0;
    const V3 u = left ? Mul(X, -1.0f) : X;  // the back of the hand
    const V3 W = Add(P(pose.position), Add(Mul(f, wristCentre_[0] / 100.0f), Add(Mul(Y, wristCentre_[1] / 100.0f), Mul(u, wristCentre_[2] / 100.0f))));
    V3 x, y;
    if (set_.layout == 0) {  // forearm: the forearm across the chest, palm down -- health on the player's left (the left wrist:
                             // toward the elbow; the right wrist, left-hand mode: toward the hand)
        x = left ? f : Mul(f, -1.0f);
        y = Mul(Y, -1.0f);
    } else {  // across: the arm pointing forward, palm down -- either side of the wrist
        x = left ? Y : Mul(Y, -1.0f);
        y = f;
    }
    const V3 z = u;
    V3 centre[2], normal[2];
    for (int p = 0; p < 2; ++p) {
        float crop[4], core[4];
        PanelCrop(p, crop, core);
        std::memcpy(cropNow_[p], crop, sizeof(crop));
        const float* a = set_.panel[p];
        const float m = mmPerPx_ / 1000.0f * std::clamp(a[3], 20.0f, 400.0f) / 100.0f;  // metres per HUD px
        const float coreW = (core[2] - core[0]) * m;
        const float side = p == 0 ? -1.0f : 1.0f;
        // The menu's offsets in the wrist's own frame, whatever the layout: along the arm (+ toward the hand, f), across it
        // (+ the controller's -Y) and out from it (the back of the hand).
        const V3 anchor = Add(W, Add(Mul(x, side * (0.5f * gapCm_ / 100.0f + 0.5f * coreW)),
                                     Add(Mul(f, a[0] / 100.0f), Add(Mul(Y, -a[1] / 100.0f), Mul(z, a[2] / 100.0f)))));
        const float t = std::clamp(a[4], -90.0f, 90.0f) * 0.0174533f;
        const V3 yt = Add(Mul(y, std::cos(t)), Mul(z, std::sin(t)));
        const V3 zt = Sub(Mul(z, std::cos(t)), Mul(y, std::sin(t)));
        // The crop's centre against the core's (image y down -> the quad's y up).
        const float dx = 0.5f * ((crop[0] + crop[2]) - (core[0] + core[2])) * m;
        const float dy = -0.5f * ((crop[1] + crop[3]) - (core[1] + core[3])) * m;
        centre[p] = Add(anchor, Add(Mul(x, dx), Mul(yt, dy)));
        normal[p] = zt;
        panelPose_[p].position = {centre[p].x, centre[p].y, centre[p].z};
        panelPose_[p].orientation = FromAxes(x, yt, zt);
        panelSize_[p][0] = (crop[2] - crop[0]) * m;
        panelSize_[p][1] = (crop[3] - crop[1]) * m;
    }
    // The gate: the panels face the eyes and the head looks at them (with some hysteresis); or always (but not popping up
    // in the face while the off hand holds the foregrip unless looked at); always while the Wrist panels page is open.
    float faceDeg = 180.0f, lookDeg = 180.0f;
    bool want = false, always = false;
    if (!why) {
        const V3 c = Mul(Add(centre[0], centre[1]), 0.5f);
        const V3 n = Unit(Add(normal[0], normal[1]));
        const V3 h = P(in.head.position);
        const V3 hf = Rot(in.head.orientation, {0, 0, -1});
        const float facing = Dot(n, Unit(Sub(h, c))), looking = Dot(hf, Unit(Sub(c, h)));
        faceDeg = std::acos(std::clamp(facing, -1.0f, 1.0f)) * 57.2958f;
        lookDeg = std::acos(std::clamp(looking, -1.0f, 1.0f)) * 57.2958f;
        const float fa = std::acos(angleCos_), lo = std::acos(lookCos_), hyst = want_ ? 10.0f * 0.0174533f : 0.0f;
        const bool lookedAt = facing > std::cos(fa + hyst) && looking > std::cos(lo + hyst);
        // (always: still only while the panels face the head -- palm up or the arm hanging they'd be seen from behind, mirrored)
        const bool facesHead = facing > std::cos(std::min(fa + 30.0f * 0.0174533f + hyst, 1.5708f));  // (never past edge-on)
        always = set_.show == 1 && !in.foregrip && facesHead;
        want = in.wristPage || lookedAt || always;
    }
    if (why) {
        fade_ = 0.0f;  // (at once: a menu, no tracking, no HUD)
    } else {
        const float rate = want ? (fadeIn_ > 0.0f ? dt / fadeIn_ : 1.0f) : -(fadeOut_ > 0.0f ? dt / fadeOut_ : 1.0f);
        fade_ = in.wristPage ? 1.0f : std::clamp(fade_ + rate, 0.0f, 1.0f);
    }
    panelsOn_ = !why && fade_ > 0.001f;
    // Logged as it changes.
    const char* note = why ? why : want ? (in.wristPage ? "the Wrist panels page" : always ? "always, facing you" : "looked at")
                                        : (set_.show == 1 ? (in.foregrip ? "the foregrip held, not looked at" : "always, but not facing you")
                                                          : "not looked at");
    if (note != why_ || want != want_) {
        static int logged = 0;
        if (logged++ < 200)
            MLOG("wristhud: panels %s (%s; facing %.0f deg, looked at %.0f deg off; the %s wrist, layout %s, show %s)",
                 want && !why ? "shown" : "hidden", note, faceDeg, lookDeg, oh ? "right" : "left", set_.layout ? "across" : "forearm",
                 set_.show ? "always" : "when looked at");
        if (want && !why && logged < 200) {
            MLOG("wristhud:   left panel %.1f x %.1f cm at %.3f %.3f %.3f (crop x %.0f..%.0f y %.0f..%.0f); right %.1f x %.1f cm at %.3f "
                 "%.3f %.3f (crop x %.0f..%.0f y %.0f..%.0f); the head at %.3f %.3f %.3f; the arm toward the hand %.2f %.2f %.2f",
                 panelSize_[0][0] * 100.0f, panelSize_[0][1] * 100.0f, centre[0].x, centre[0].y, centre[0].z, cropNow_[0][0],
                 cropNow_[0][2], cropNow_[0][1], cropNow_[0][3], panelSize_[1][0] * 100.0f, panelSize_[1][1] * 100.0f, centre[1].x,
                 centre[1].y, centre[1].z, cropNow_[1][0], cropNow_[1][2], cropNow_[1][1], cropNow_[1][3], in.head.position.x,
                 in.head.position.y, in.head.position.z, f.x, f.y, f.z);
        }
        why_ = note;
        want_ = want;
    }
    // The menu's offsets changed (logged where the panels are now: the menu's tests check the direction they moved).
    if (!why && (std::memcmp(loggedPanel_, set_.panel, sizeof(loggedPanel_)) || loggedLayout_ != set_.layout)) {
        static int logged = 0;
        if (loggedLayout_ >= 0 && logged++ < 200)
            MLOG("wristhud: panels moved (layout %s): left panel %.1f x %.1f cm at %.3f %.3f %.3f; right %.1f x %.1f cm at %.3f %.3f %.3f; "
                 "the arm toward the hand %.2f %.2f %.2f", set_.layout ? "across" : "forearm", panelSize_[0][0] * 100.0f,
                 panelSize_[0][1] * 100.0f, centre[0].x, centre[0].y, centre[0].z, panelSize_[1][0] * 100.0f, panelSize_[1][1] * 100.0f,
                 centre[1].x, centre[1].y, centre[1].z, f.x, f.y, f.z);
        std::memcpy(loggedPanel_, set_.panel, sizeof(loggedPanel_));
        loggedLayout_ = set_.layout;
    }
    if (why) want_ = false;
}

void WristHud::Compose(bool rest, bool panels) {
    const float clear[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(atlasRtv_, clear);
    ctx_->OMSetRenderTargets(1, &atlasRtv_, nullptr);
    ctx_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFFu);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->RSSetState(raster_);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &hudView_);
    auto draw = [&](const CB& cb) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        std::memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(cb_, 0);
        const D3D11_VIEWPORT vp{cb.cell[0], cb.cell[1], cb.cell[2], cb.cell[3], 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->PSSetConstantBuffers(0, 1, &cb_);
        ctx_->Draw(3, 0);
    };
    if (rest) {
        CB cb{};
        cb.cell[2] = static_cast<float>(texW_);
        cb.cell[3] = static_cast<float>(texH_);
        cb.crop[2] = 1.0f;
        int n = 0;
        // What the wrist shows is cleared from the rest (the element rectangles, a little padded; the badges while shown).
        if (haveSlot_ && (slot_.flags & 4u)) {
            for (int i = 0; i < shared::kHudRects && n < kMasks; ++i) {
                if ((i == shared::kHudWeaponBadge || i == shared::kHudNadeBadge) && !((slot_.shown >> i) & 1u)) continue;
                const float* r = slot_.rect[i];
                if (!(r[2] > r[0] && r[3] > r[1])) continue;
                cb.mask[n][0] = r[0] - 2.0f;
                cb.mask[n][1] = r[1] - 2.0f;
                cb.mask[n][2] = r[2] + 2.0f;
                cb.mask[n][3] = r[3] + 2.0f;
                ++n;
            }
        } else {
            for (int p = 0; p < 2 && n < kMasks; ++p, ++n) std::memcpy(cb.mask[n], cropNow_[p], sizeof(cb.mask[n]));
        }
        cb.info[0] = static_cast<float>(n);
        draw(cb);
    }
    if (panels) {
        static const float kPlate[3] = {0.0f, 0.35f, 0.6f};
        for (int p = 0; p < 2; ++p) {
            CB cb{};
            cb.cell[0] = static_cast<float>(p * kCell);
            cb.cell[1] = static_cast<float>(texH_);
            cb.cell[2] = cropNow_[p][2] - cropNow_[p][0];
            cb.cell[3] = cropNow_[p][3] - cropNow_[p][1];
            cb.crop[0] = cropNow_[p][0];
            cb.crop[1] = cropNow_[p][1];
            cb.crop[2] = fade_;
            cb.crop[3] = kPlate[std::clamp(set_.backing, 0, 2)];
            cb.info[1] = 8.0f;
            if (cb.cell[2] > 0.0f && cb.cell[3] > 0.0f) draw(cb);
        }
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

int WristHud::Layers(XrSpace local, XrSpace view, const XrCompositionLayerBaseHeader** out, int max, bool panelsAllowed) {
    // (panelsAllowed false mid-recentre: only the LOCAL panels are dropped; the head-locked rest doesn't depend on LOCAL)
    const bool panelsWanted = panelsOn_ && panelsAllowed;
    if (!ready_ || max <= 0 || !(restOn_ || panelsWanted)) return 0;
    const bool panels = panelsWanted && max >= (restOn_ ? 3 : 2);
    const bool rest = restOn_;
    Compose(rest, panels);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return 0;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return 0;
    ctx_->CopyResource(images_[idx].texture, atlas_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);
    int n = 0;
    if (rest) {
        XrCompositionLayerQuad& l = layers_[n];
        l = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        l.space = view;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        l.subImage.swapchain = swapchain_;
        l.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(texW_), static_cast<int32_t>(texH_)}};
        const float d = std::clamp(set_.screen[0], 0.3f, 20.0f), w = std::clamp(set_.screen[1], 0.1f, 10.0f);
        l.pose.orientation.w = 1.0f;
        l.pose.position = {0.0f, -std::clamp(set_.screen[2], -2.0f, 2.0f), -d};
        l.size = {w, w * static_cast<float>(texH_) / static_cast<float>(texW_)};
        out[n] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l);
        ++n;
    }
    if (panels) {
        for (int p = 0; p < 2; ++p) {
            const int cw = static_cast<int>(cropNow_[p][2] - cropNow_[p][0]), ch = static_cast<int>(cropNow_[p][3] - cropNow_[p][1]);
            if (cw <= 0 || ch <= 0) continue;
            XrCompositionLayerQuad& l = layers_[n];
            l = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            l.space = local;
            l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            l.subImage.swapchain = swapchain_;
            l.subImage.imageRect = {{p * kCell, static_cast<int32_t>(texH_)}, {cw, ch}};
            l.pose = panelPose_[p];
            l.size = {panelSize_[p][0], panelSize_[p][1]};
            out[n] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l);
            ++n;
        }
    }
    static int logged = 0;
    if (logged < 3 && panels) {
        ++logged;
        MLOG("wristhud: the panels' quads -- %.1f x %.1f cm and %.1f x %.1f cm (fade %.2f), the rest quad %s", panelSize_[0][0] * 100.0f,
             panelSize_[0][1] * 100.0f, panelSize_[1][0] * 100.0f, panelSize_[1][1] * 100.0f, fade_, rest ? "on" : "off");
    }
    return n;
}

}  // namespace mohavr::host
