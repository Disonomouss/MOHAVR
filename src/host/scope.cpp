#include "formats.hpp"
#include "scope.hpp"

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
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Unit(V3 a) {
    const float l = Len(a);
    return l > 1e-9f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}
V3 P(const XrVector3f& v) { return {v.x, v.y, v.z}; }
// v rotated by the unit quaternion q.
V3 Rot(const XrQuaternionf& q, V3 v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z), tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
XrQuaternionf Conj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
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
float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float IniF(const std::wstring& ini, const wchar_t* key, float def, float lo, float hi) {
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"Scope", key, L"", v, 32, ini.c_str());
    const float f = static_cast<float>(_wtof(v));
    return v[0] && f >= lo && f <= hi ? f : def;
}

// The scope in LOCAL from a gun pose: the drawn eyepiece, and the optical axis from it -- zeroed to the bore: along the aim
// line (`aimInGun`, the aim's direction in the gun pose's frame), not the drawn tube, which the StG44's mount tilts ~4 deg
// nose-up against its barrel; the objective as far along it as the tube is long.
struct Tube {
    V3 eye, obj, axis;
};
Tube TubeOf(const XrPosef& gun, const shared::ScopeInfo& s, const float (&aimInGun)[3]) {
    const V3 o = P(gun.position);
    Tube t;
    t.eye = Add(o, Rot(gun.orientation, {s.ocular[0], s.ocular[1], s.ocular[2]}));
    const V3 drawn = Sub(V3{s.objective[0], s.objective[1], s.objective[2]}, V3{s.ocular[0], s.ocular[1], s.ocular[2]});
    t.axis = Unit(Rot(gun.orientation, {aimInGun[0], aimInGun[1], aimInGun[2]}));
    t.obj = Add(t.eye, Mul(t.axis, Len(drawn)));
    return t;
}
// The drawn tube's own axis (the log: how far the mount tilts it off the aim line).
V3 DrawnAxis(const XrPosef& gun, const shared::ScopeInfo& s) {
    return Unit(Rot(gun.orientation, Sub(V3{s.objective[0], s.objective[1], s.objective[2]}, V3{s.ocular[0], s.ocular[1], s.ocular[2]})));
}

const char* kShader = R"(
cbuffer C : register(b0) { float4 ret; float4 aim; }  // ret x: the reticle (0 a crosshair, 1 a post and bars, 2 the M18's
                                                      // ring sight), y: line,
                                                      // z: post, w: bars' gap; aim xy: the aim line in the field (its centre)
struct VI { float2 pos : POSITION; float2 uv : TEXCOORD0; float2 ba : TEXCOORD1; };
struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float2 ba : TEXCOORD1; };
VO vs(VI i) { VO o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.ba = i.ba; return o; }
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 ps(VO i) : SV_TARGET {
    float2 f = (i.uv - 0.5) * 2.0;  // the field, -1..1 (y down)
    float r = length(f);
    float3 c = t.Sample(s, i.uv).rgb * saturate(i.ba.x);
    c *= saturate((1.0 - r) / 0.025);  // the field stop: a black ring, antialiased
    f -= aim.xy;  // the reticle on the aim line
    float k = 0.0;
    if (ret.x < 0.5) {
        k = max(1.0 - abs(f.x) / ret.y, 1.0 - abs(f.y) / ret.y);
    } else if (ret.x < 1.5) {
        float post = ret.z * saturate(f.y / (ret.z * 4.0));  // from the bottom up to the centre, pointed
        k = (f.y > 0.0) ? 1.0 - abs(f.x) / max(post, 1e-4) : 0.0;
        k = max(k, (abs(f.x) > ret.w) ? 1.0 - abs(f.y) / (ret.y * 2.0) : 0.0);
    } else {  // the M18's sight (the game's US_M18_Scope_HUD, measured): fine and heavy cross lines, four rings
        float ax = abs(f.x), rr = length(f), w = 0.0045, h = 0.013;
        k = (ax < 0.252) ? 1.0 - abs(f.y) / w : 0.0;
        k = max(k, (ax >= 0.252 && ax < 0.755) ? 1.0 - abs(f.y) / h : 0.0);
        k = max(k, (f.y > -0.252 && f.y < 0.576) ? 1.0 - ax / w : 0.0);
        k = max(k, (f.y <= -0.252 || f.y >= 0.576) ? 1.0 - ax / h : 0.0);
        k = max(k, 0.6 * (1.0 - abs(rr - 0.066) / w));
        k = max(k, 1.0 - abs(rr - 0.143) / w);
        k = max(k, 1.0 - abs(rr - 0.253) / (w * 1.4));
        k = max(k, 1.0 - abs(rr - 0.382) / (w * 1.4));
    }
    c *= 1.0 - saturate(k * 3.0);
    float a = saturate(i.ba.y);
    return float4(c * a, a);
}
)";

constexpr int kRings = 8, kSegs = 72;
constexpr float kMargin = 1.06f;  // the quad beyond the glass (its antialiased edge)
constexpr int kVerts = kSegs * (3 + 6 * kRings);  // the centre's triangle and kRings quads per segment

}  // namespace

bool Scope::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, const std::wstring& ini) {
    dev_ = dev;
    ctx_ = ctx;
    on_ = GetPrivateProfileIntW(L"Scope", L"Enable", 0, ini.c_str()) != 0;
    wchar_t z[16] = L"";
    GetPrivateProfileStringW(L"Scope", L"Zoom", L"real", z, 16, ini.c_str());
    zoomGame_ = !_wcsicmp(z, L"game");
    twoHands_ = GetPrivateProfileIntW(L"Scope", L"TwoHands", 1, ini.c_str()) != 0;
    m18_ = GetPrivateProfileIntW(L"Scope", L"M18", 1, ini.c_str()) != 0;
    enterDist_ = IniF(ini, L"EyeDistance", 12.0f, 2.0f, 30.0f) / 100.0f;
    exitDist_ = enterDist_ + 0.04f;
    enterOff_ = IniF(ini, L"EyeOffAxis", 2.5f, 0.5f, 10.0f) / 100.0f;
    exitOff_ = enterOff_ * 1.6f;
    field_ = std::tan(0.5f * IniF(ini, L"Field", 22.0f, 5.0f, 60.0f) * 0.0174533f);
    eyeRelief_ = IniF(ini, L"EyeRelief", 7.0f, 1.0f, 20.0f) / 100.0f;
    pupil_ = IniF(ini, L"Pupil", 1.2f, 0.2f, 5.0f) / 100.0f;
    zoomRate_ = IniF(ini, L"ZoomRate", 20.0f, 1.0f, 200.0f);
    zero_ = IniF(ini, L"Zero", 50.0f, 1.0f, 1000.0f);

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = ci.height = static_cast<uint32_t>(size_);
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &swapchain_))) {
        MLOG("scope: xrCreateSwapchain failed -- no lens");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = static_cast<UINT>(size_);
    td.MipLevels = td.ArraySize = 1;
    td.Format = RtFormat(fmt);  // the swapchain's family (GOAL B2)
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &rt_)) || FAILED(dev_->CreateRenderTargetView(rt_, nullptr, &rtv_))) {
        MLOG("scope: the lens's render target failed -- no lens");
        return false;
    }
    if (!MakeShaders()) return false;
    layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer_.subImage.swapchain = swapchain_;
    layer_.subImage.imageRect = {{0, 0}, {size_, size_}};
    ready_ = true;
    MLOG("scope: ready -- %s, zoom %s, %s; the eye within %.0f cm behind the eyepiece and %.1f cm of its axis; field %.0f deg, "
         "exit pupil %.1f cm at %.0f cm", on_ ? "on" : "off (the shipped default)", zoomGame_ ? "the game's" : "the real scope's",
         twoHands_ ? "two hands" : "one hand will do", enterDist_ * 100.0f, enterOff_ * 100.0f, 2.0f * std::atan(field_) * 57.2958f,
         pupil_ * 100.0f, eyeRelief_ * 100.0f);
    return true;
}

bool Scope::MakeShaders() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    const size_t len = std::strlen(kShader);
    if (FAILED(D3DCompile(kShader, len, "scope", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, len, "scope", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        MLOG("scope: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    bool ok = SUCCEEDED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
              SUCCEEDED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_));
    const D3D11_INPUT_ELEMENT_DESC ie[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = ok && SUCCEEDED(dev_->CreateInputLayout(ie, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il_));
    vsb->Release();
    psb->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(sizeof(Vtx) * kVerts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev_->CreateBuffer(&bd, nullptr, &vb_));
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 32;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev_->CreateBuffer(&cd, nullptr, &cb_));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev_->CreateSamplerState(&sd, &sampler_));
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev_->CreateBlendState(&bl, &blend_));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ok = ok && SUCCEEDED(dev_->CreateRasterizerState(&rd, &raster_));
    if (!ok) MLOG("scope: the lens's D3D11 objects failed -- no lens");
    return ok;
}

bool Scope::SourceTexture(UINT w, UINT h, DXGI_FORMAT fmt) {
    if (src_ && w == srcW_ && h == srcH_) return true;
    if (srcView_) srcView_->Release(), srcView_ = nullptr;
    if (src_) src_->Release(), src_ = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &src_)) || FAILED(dev_->CreateShaderResourceView(src_, nullptr, &srcView_))) {
        MLOG("scope: the scope view's texture (%ux%u) failed", w, h);
        return false;
    }
    srcW_ = w;
    srcH_ = h;
    return true;
}

float Scope::Magnification() const {
    // The game's FOV against its own 80 deg view (MOHAPlayerCamera.DefaultFOV): tan 40 / tan (FOV / 2).
    auto fromFov = [](float fov) { return std::tan(40.0f * 0.0174533f) / std::tan(0.5f * fov * 0.0174533f); };
    float m = 0.0f;
    if (zoomGame_ && info_.gameFov[1] > 1.0f) {
        const auto it = gameFov_.find(info_.key);
        m = fromFov(it != gameFov_.end() ? it->second : info_.gameFov[1]);
    } else if (info_.realMag > 0.5f) {
        m = info_.realMag;
    } else if (info_.gameFov[1] > 1.0f) {
        m = fromFov(info_.gameFov[1]);
    }
    return std::clamp(m > 0.0f ? m : 2.5f, 1.0f, 20.0f);
}

void Scope::Update(const shared::Header* hdr, const In& in) {
    const double dt = lastNow_ > 0.0 ? std::clamp(in.now - lastNow_, 0.0, 0.1) : 0.0;
    lastNow_ = in.now;
    shared::ScopeInfo si{};
    std::uint32_t seq = 0;
    if (hdr && shared::ReadScopeInfo(hdr, si, seq)) {
        info_ = si;
        infoOk_ = true;
    }
    const bool geo = infoOk_ && (info_.caps & 7u) == 7u && info_.radius > 0.001f;
    if (in.gunValid) {  // the aim line in the gun pose's frame (the fit's: a constant of the held gun)
        const XrQuaternionf inv = Conj(in.gun.orientation);
        const V3 aim = Rot(inv, Rot(in.aimRay.orientation, {0, 0, -1}));
        const V3 from = Rot(inv, Sub(P(in.aimRay.position), P(in.gun.position)));
        aimInGun_[0] = aim.x;
        aimInGun_[1] = aim.y;
        aimInGun_[2] = aim.z;
        aimFromGun_[0] = from.x;
        aimFromGun_[1] = from.y;
        aimFromGun_[2] = from.z;
    }
    const char* why = !ready_ ? "no lens" : !on_ ? "switched off" : !geo ? "no scope on the gun in hand" :
                      (!m18_ && !std::strcmp(info_.key, "Attachment_M18RecoillessRifle")) ? "the M18's scope switched off" : !in.hasView ? "no view" :
                      !in.gunValid ? "no gun pose" : (twoHands_ && !in.twoHanded) ? "one hand on the gun" :
                      !in.gestures ? "a menu" : !in.eyesOk ? "no eyes" : nullptr;
    float d[2] = {0, 0}, off[2] = {0, 0}, look[2] = {0, 0};
    if (!why) {
        const Tube t = TubeOf(in.gun, info_, aimInGun_);
        for (int e = 0; e < 2; ++e) {
            const V3 v = Sub(P(in.eye[e].position), t.eye);
            const float along = Dot(v, t.axis);
            d[e] = -along;  // behind the eyepiece
            off[e] = Len(Sub(v, Mul(t.axis, along)));
            look[e] = Dot(Rot(in.eye[e].orientation, {0, 0, -1}), t.axis);
        }
    }
    auto enter = [&](int e) { return d[e] > 0.0f && d[e] < enterDist_ && off[e] < enterOff_ && look[e] > enterCos_; };
    auto stay = [&](int e) { return d[e] > -0.02f && d[e] < exitDist_ && off[e] < exitOff_ && look[e] > exitCos_; };
    const bool was = active_;
    if (why) {
        active_ = false;
        enterFrames_ = 0;
    } else if (active_) {
        if (!stay(eye_)) active_ = false;
    } else {
        int pick = -1;
        for (int e = 0; e < 2; ++e)
            if (enter(e) && (pick < 0 || off[e] < off[pick])) pick = e;
        if (pick >= 0 && (enterFrames_ == 0 || pick == eye_)) {
            eye_ = pick;
            if (++enterFrames_ >= 2) active_ = true;
        } else {
            enterFrames_ = 0;
        }
    }
    if (active_ != was) {
        const float aimOff =
            std::acos(std::clamp(Dot(DrawnAxis(in.gun, info_), Rot(in.aimRay.orientation, {0, 0, -1})), -1.0f, 1.0f)) * 57.2958f;
        if (active_)
            MLOG("scope: looking through %s with the %s eye -- %.1f cm behind the eyepiece, %.1f cm off its axis, %.0f deg off it; "
                 "%.1fx (%s); the drawn tube %.2f deg off the aim line (the scope zeroed to it)", info_.key, eye_ ? "right" : "left", d[eye_] * 100.0f, off[eye_] * 100.0f,
                 std::acos(std::clamp(look[eye_], -1.0f, 1.0f)) * 57.2958f, Magnification(), zoomGame_ ? "the game's zoom" : "the real scope",
                 aimOff);
        else
            MLOG("scope: away from the eye (%s)", why ? why : "the eye moved off");
        enterFrames_ = 0;
        srcOk_ = false;  // (the lens waits for a frame with this scoping's view)
        srcFrame_ = 0;
    }
    // The game zoom: the turning stick's up / down while looking through, between the game's widest and narrowest FOV.
    if (active_ && zoomGame_ && info_.gameFov[0] > 0.0f && info_.gameFov[1] > info_.gameFov[0]) {
        auto it = gameFov_.find(info_.key);
        float fov = it != gameFov_.end() ? it->second : info_.gameFov[1];
        const float stick = std::fabs(in.stickY) > 0.2f ? in.stickY : 0.0f;
        if (stick != 0.0f) {
            const float before = fov;
            fov = std::clamp(fov - stick * zoomRate_ * static_cast<float>(dt), info_.gameFov[0], info_.gameFov[1]);
            static int logged = 0;
            if (std::floor(fov / 5.0f) != std::floor(before / 5.0f) && logged++ < 60)
                MLOG("scope: the game zoom -> %.0f deg (%.1fx)", fov, std::tan(40.0f * 0.0174533f) / std::tan(0.5f * fov * 0.0174533f));
        }
        gameFov_[info_.key] = fov;
    }
    tanHalf_ = field_ / Magnification();
    // The camera: at the objective, looking down the tube (the gun's up as its up) -- it sees over what the scope sees
    // over. The aim line's direction in the gun's frame (the reticle goes there).
    if (geo && in.gunValid) {
        const Tube t = TubeOf(in.gun, info_, aimInGun_);
        const V3 up = Rot(in.gun.orientation, {0, 1, 0});
        const V3 back = Mul(t.axis, -1.0f);
        const V3 right = Unit(Cross(up, back));
        const V3 camUp = Cross(back, right);
        const XrQuaternionf q = FromAxes(right, camUp, back);
        camera_ = {t.obj.x, t.obj.y, t.obj.z, q.x, q.y, q.z, q.w};
    }
    if (!why && !active_) {  // (near misses, every 2 s: how far each eye is from looking through)
        static double next = 0.0;
        static int logged = 0;
        if (in.now >= next && logged < 40 && (std::min(d[0], d[1]) < 0.3f)) {
            next = in.now + 2.0;
            ++logged;
            const Tube t = TubeOf(in.gun, info_, aimInGun_);
            MLOG("scope: the eyes against the eyepiece -- left %.1f cm behind, %.1f off the axis, %.0f deg; right %.1f, %.1f, %.0f deg "
                 "(eyepiece %.3f %.3f %.3f, eyes %.3f %.3f %.3f / %.3f %.3f %.3f, gun %.3f %.3f %.3f)",
                 d[0] * 100.0f, off[0] * 100.0f, std::acos(std::clamp(look[0], -1.0f, 1.0f)) * 57.2958f, d[1] * 100.0f,
                 off[1] * 100.0f, std::acos(std::clamp(look[1], -1.0f, 1.0f)) * 57.2958f, t.eye.x, t.eye.y, t.eye.z,
                 in.eye[0].position.x, in.eye[0].position.y, in.eye[0].position.z, in.eye[1].position.x, in.eye[1].position.y,
                 in.eye[1].position.z, in.gun.position.x, in.gun.position.y, in.gun.position.z);
        }
    }
    const char* note = why ? why : active_ ? "" : "the eye not at the eyepiece";
    if (note != loggedWhy_ && geo && on_) {
        static int logged = 0;
        if (logged++ < 60 && !active_) MLOG("scope: not looking through -- %s", note);
        loggedWhy_ = note;
    }
}

const XrCompositionLayerBaseHeader* Scope::Layer(XrSpace space, ID3D11Texture2D* frame, std::uint64_t frameNo,
                                                 const shared::SlotScope& sc, const XrPosef (&eyeNow)[2], bool eyesOk) {
    if (!ready_ || !active_ || !eyesOk || !frame) return nullptr;
    if (frameNo != srcFrame_ && sc.rect[2] > 0 && sc.rect[3] > 0) {
        D3D11_TEXTURE2D_DESC fd{};
        frame->GetDesc(&fd);
        if (sc.rect[0] + sc.rect[2] <= fd.Width && sc.rect[1] + sc.rect[3] <= fd.Height && SourceTexture(sc.rect[2], sc.rect[3], fd.Format)) {
            const D3D11_BOX box{sc.rect[0], sc.rect[1], 0, sc.rect[0] + sc.rect[2], sc.rect[1] + sc.rect[3], 1};
            ctx_->CopySubresourceRegion(src_, 0, 0, 0, 0, frame, 0, &box);
            srcFrame_ = frameNo;
            srcOk_ = true;
        }
    }
    if (!srcOk_ || !(sc.flags & 1u)) return nullptr;
    // The drawn eyepiece (the gun pose the frame was drawn with); the quad's axes: z back toward the eye, x the gun's right.
    const XrPosef gun = ToXr(sc.gunPose);
    const Tube t = TubeOf(gun, info_, aimInGun_);
    const V3 zq = Mul(t.axis, -1.0f);
    const V3 xg = Rot(gun.orientation, {1, 0, 0});
    const V3 xq = Unit(Sub(xg, Mul(zq, Dot(xg, zq))));
    const V3 yq = Cross(zq, xq);
    const float rg = glass_ * info_.radius;
    const V3 eye = P(eyeNow[eye_].position);
    const XrQuaternionf camInv = Conj(XrQuaternionf{sc.camera.qx, sc.camera.qy, sc.camera.qz, sc.camera.qw});
    const V3 pupil = Sub(t.eye, Mul(t.axis, eyeRelief_));  // the exit pupil's centre
    // The lens's vertices: rings out to the glass's edge (and an outer ring at the quad's margin, transparent).
    auto vertex = [&](float u, float v, float alpha) {
        Vtx o{u / kMargin, v / kMargin, 2.0f, 2.0f, 0.0f, alpha};
        const V3 p = Add(t.eye, Add(Mul(xq, u * rg), Mul(yq, v * rg)));
        const V3 w = Sub(p, eye);
        const V3 wc = Rot(camInv, w);
        if (wc.z < -1e-5f) {
            o.u = 0.5f + 0.5f * (wc.x / -wc.z) / field_;
            o.v = 0.5f - 0.5f * (wc.y / -wc.z) / field_;
        }
        // The exit pupil: where this point's ray to the eye crosses the pupil's plane, against the pupil's radius.
        const V3 pe = Sub(eye, p);
        const float den = Dot(pe, t.axis);
        if (std::fabs(den) > 1e-6f) {
            const float s = Dot(Sub(pupil, p), t.axis) / den;
            const V3 x = Add(p, Mul(pe, s));
            o.bright = 1.0f - Smooth(pupil_, pupil_ * 1.6f, Len(Sub(x, pupil)));
        }
        return o;
    };
    verts_.clear();
    auto ring = [&](int k, int s) {
        const float r = k <= kRings ? static_cast<float>(k) / kRings : kMargin;
        const float a = 6.2831853f * static_cast<float>(s % kSegs) / kSegs;
        return vertex(r * std::cos(a), r * std::sin(a), k <= kRings ? 1.0f : 0.0f);
    };
    const Vtx centre = vertex(0.0f, 0.0f, 1.0f);
    for (int s = 0; s < kSegs; ++s) {
        verts_.push_back(centre);
        verts_.push_back(ring(1, s));
        verts_.push_back(ring(1, s + 1));
        for (int k = 1; k <= kRings; ++k) {
            const Vtx a = ring(k, s), b = ring(k, s + 1), c = ring(k + 1, s), d = ring(k + 1, s + 1);
            verts_.insert(verts_.end(), {a, c, b, b, c, d});
        }
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (verts_.size() > static_cast<size_t>(kVerts) || FAILED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return nullptr;
    std::memcpy(m.pData, verts_.data(), sizeof(Vtx) * verts_.size());
    ctx_->Unmap(vb_, 0);
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        // The reticle in the field's units (1 = the field stop's radius): a fine crosshair; the German post and bars -- at the
        // scope's zero: where the aim line (the shots', as drawn) is Zero metres out, seen from the scope view's camera,
        // tangent / the view's half tangent (y down). Nearer the shots land a little low, as with a real scope.
        const V3 from = Add(P(gun.position), Rot(gun.orientation, {aimFromGun_[0], aimFromGun_[1], aimFromGun_[2]}));
        const V3 zero = Add(from, Mul(Rot(gun.orientation, {aimInGun_[0], aimInGun_[1], aimInGun_[2]}), zero_));
        const V3 aim = Rot(camInv, Sub(zero, V3{sc.camera.px, sc.camera.py, sc.camera.pz}));
        float ax = 0.0f, ay = 0.0f;
        if (aim.z < -1e-4f && sc.tanHalf > 1e-4f) {
            ax = std::clamp((aim.x / -aim.z) / sc.tanHalf, -0.9f, 0.9f);
            ay = std::clamp(-(aim.y / -aim.z) / sc.tanHalf, -0.9f, 0.9f);
        }
        const float r[8] = {static_cast<float>(info_.reticle < 2u ? info_.reticle : 2u), 0.006f, 0.035f, 0.30f, ax, ay, 0.0f, 0.0f};
        std::memcpy(m.pData, r, sizeof(r));
        ctx_->Unmap(cb_, 0);
    }
    const float clear[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rtv_, clear);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(size_), static_cast<float>(size_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_);
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFFu);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    const UINT stride = sizeof(Vtx), offset = 0;
    ctx_->IASetInputLayout(il_);
    ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetShaderResources(0, 1, &srcView_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->Draw(static_cast<UINT>(verts_.size()), 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return nullptr;
    ctx_->CopyResource(images_[idx].texture, rt_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);

    layer_.space = space;
    layer_.eyeVisibility = eye_ ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
    const V3 at = Add(t.eye, Mul(zq, 0.001f));  // (a millimetre behind the drawn eyepiece)
    layer_.pose.position = {at.x, at.y, at.z};
    layer_.pose.orientation = FromAxes(xq, yq, zq);
    layer_.size = {2.0f * rg * kMargin, 2.0f * rg * kMargin};
    static int logged = 0;
    if (logged++ == 0)
        MLOG("scope: the lens shown -- %.1f cm across at (%.3f %.3f %.3f), the scope view %ux%u, the %s eye %.1f cm from it", 200.0f * rg,
             at.x, at.y, at.z, srcW_, srcH_, eye_ ? "right" : "left", Len(Sub(eye, at)) * 100.0f);
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_);
}

}  // namespace mohavr::host
