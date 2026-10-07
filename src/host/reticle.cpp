#include "formats.hpp"
#include "reticle.hpp"

#include <algorithm>
#include <cmath>

#include "../mohavr/log.hpp"

namespace mohavr::host {

bool Reticle::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt, float sizeDeg) {
    ctx_ = ctx;
    tanHalf_ = std::tan(sizeDeg * 0.5f * 0.0174533f);
    constexpr int kSize = 64;
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
        MLOG("reticle: xrCreateSwapchain failed -- no reticle");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

    // A red dot with a dark rim (visible on bright and dark walls), antialiased, premultiplied alpha, BGRA.
    std::vector<std::uint32_t> px(kSize * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize - 0.5f, dy = (y + 0.5f) / kSize - 0.5f;
            const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;  // 0 centre .. 1 edge
            const float px1 = 2.0f / kSize;                       // one pixel in r
            const float core = std::clamp((0.55f - r) / px1, 0.0f, 1.0f);
            const float rim = std::clamp((0.85f - r) / px1, 0.0f, 1.0f);
            const float a = std::max(core, rim * 0.7f);
            const float red = core * 1.0f, gb = core * 0.2f;  // rim is black
            px[y * kSize + x] = Pack(fmt, red, gb, gb, a);  // the swapchain's channel order (GOAL B2)
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(fmt);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), kSize * 4, 0};
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex_))) {
        MLOG("reticle: CreateTexture2D failed -- no reticle");
        return false;
    }
    layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer_.subImage.swapchain = swapchain_;
    layer_.subImage.imageRect = {{0, 0}, {kSize, kSize}};
    MLOG("reticle: ready (%.2f deg)", sizeDeg);
    return true;
}

const XrCompositionLayerBaseHeader* Reticle::Layer(XrSpace space, const XrPosef& aim, const XrPosef& head, float distance) {
    if (!swapchain_ || !tex_ || !(distance > 0.05f)) return nullptr;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return nullptr;
    ctx_->CopyResource(images_[idx].texture, tex_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);

    // The aim's forward = its orientation applied to (0,0,-1).
    const auto& q = aim.orientation;
    const float fx = -(2.0f * (q.x * q.z + q.w * q.y));
    const float fy = -(2.0f * (q.y * q.z - q.w * q.x));
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    layer_.space = space;
    layer_.pose.position = {aim.position.x + fx * distance, aim.position.y + fy * distance, aim.position.z + fz * distance};
    layer_.pose.orientation = head.orientation;  // parallel to the view: a round dot
    // Constant angular size seen from the head.
    const float hx = layer_.pose.position.x - head.position.x, hy = layer_.pose.position.y - head.position.y,
                hz = layer_.pose.position.z - head.position.z;
    const float s = 2.0f * tanHalf_ * std::sqrt(hx * hx + hy * hy + hz * hz);
    layer_.size = {s, s};
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        MLOG("reticle: shown %.2f m along the aim at (%.2f %.2f %.2f), %.1f cm", distance, layer_.pose.position.x,
             layer_.pose.position.y, layer_.pose.position.z, s * 100.0f);
    }
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_);
}

}  // namespace mohavr::host
