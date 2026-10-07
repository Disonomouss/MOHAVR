#include "vignette.hpp"

#include <algorithm>
#include <cmath>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
// The quad: 1 m ahead of the eyes, 4 m square (to +-63 degrees, past any headset's view). The mask's radius r is in
// half-widths (1 = 2 m off centre = 63 degrees); the clear centre ends at `inner`, full at `outer`.
constexpr float kDistance = 1.0f, kWidth = 4.0f;
struct Shape { float inner, outer, alpha; };
constexpr Shape kShapes[3] = {{1.0f, 1.0f, 0.0f}, {0.42f, 0.80f, 0.75f}, {0.28f, 0.62f, 1.0f}};  // none, light, strong
const char* kNames[3] = {"none", "light", "strong"};
}  // namespace

bool Vignette::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, float fadeSeconds) {
    dev_ = dev;
    ctx_ = ctx;
    fmt_ = fmt;
    fadeSec_ = std::clamp(fadeSeconds, 0.0f, 2.0f);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = kSize;
    ci.height = kSize;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &swapchain_))) {
        MLOG("vignette: xrCreateSwapchain failed -- no vignette");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer_.subImage.swapchain = swapchain_;
    layer_.subImage.imageRect = {{0, 0}, {kSize, kSize}};
    layer_.pose.orientation.w = 1.0f;
    layer_.pose.position = {0.0f, 0.0f, -kDistance};
    layer_.size = {kWidth, kWidth};
    MLOG("vignette: ready (fade %.2f s)", fadeSec_);
    return true;
}

void Vignette::Build() {
    for (auto*& t : tex_) {
        if (t) t->Release();
        t = nullptr;
    }
    if (strength_ <= 0) return;
    const Shape& sh = kShapes[strength_];
    std::vector<float> mask(kSize * kSize);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize * 2.0f - 1.0f, dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float t = std::clamp((r - sh.inner) / (sh.outer - sh.inner), 0.0f, 1.0f);
            mask[y * kSize + x] = t * t * (3.0f - 2.0f * t) * sh.alpha;  // smoothstep
        }
    std::vector<std::uint32_t> px(kSize * kSize);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(fmt_);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (int l = 1; l <= kLevels; ++l) {
        const float k = static_cast<float>(l) / kLevels;
        for (int i = 0; i < kSize * kSize; ++i)  // black, premultiplied: only the alpha byte
            px[i] = static_cast<std::uint32_t>(std::clamp(mask[i] * k, 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
        D3D11_SUBRESOURCE_DATA sd{px.data(), kSize * 4, 0};
        if (FAILED(dev_->CreateTexture2D(&td, &sd, &tex_[l]))) {
            MLOG("vignette: CreateTexture2D failed -- no vignette");
            strength_ = 0;
            return;
        }
    }
}

void Vignette::SetStrength(int s) {
    s = std::clamp(s, 0, 2);
    if (s == strength_ && (s == 0 || tex_[kLevels])) return;
    strength_ = s;
    if (swapchain_) Build();
    MLOG("vignette: %s", kNames[strength_]);
}

void Vignette::Update(float dt, float motion) {
    const float target = motion > 0.05f ? 1.0f : 0.0f;
    if (fadeSec_ <= 0.0f) fade_ = target;
    else fade_ = std::clamp(fade_ + (target > fade_ ? 1.0f : -1.0f) * dt / fadeSec_, 0.0f, 1.0f);
}

const XrCompositionLayerBaseHeader* Vignette::Layer(XrSpace view) {
    const int level = static_cast<int>(std::lround(fade_ * kLevels));
    const bool shown = swapchain_ && strength_ > 0 && level > 0 && tex_[level];
    if (shown != shownLast_ && logged_ < 40) {
        ++logged_;
        MLOG("vignette: %s", shown ? "in (moving by stick)" : "out");
    }
    shownLast_ = shown;
    if (!shown) return nullptr;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return nullptr;
    ctx_->CopyResource(images_[idx].texture, tex_[level]);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);
    layer_.space = view;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_);
}

}  // namespace mohavr::host
