#include "markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../mohavr/log.hpp"

namespace mohavr::host {

bool Markers::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t fmt) {
    ctx_ = ctx;
    const int w = 3 * kCell, hgt = kCell;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(hgt);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &swapchain_))) {
        MLOG("markers: xrCreateSwapchain failed -- no rings");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

    // Cells: a pale ring, the same ring lit (green, thicker), a dot -- each with a dark edge for bright scenes.
    // Premultiplied alpha, BGRA.
    std::vector<std::uint32_t> px(static_cast<size_t>(w) * hgt, 0);
    auto b8 = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (int cell = 0; cell < 3; ++cell) {
        for (int y = 0; y < kCell; ++y) {
            for (int x = 0; x < kCell; ++x) {
                const float dx = (x + 0.5f) / kCell - 0.5f, dy = (y + 0.5f) / kCell - 0.5f;
                const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;  // 0 centre .. 1 edge
                const float px1 = 2.0f / kCell;
                float core = 0.0f, edge = 0.0f;
                if (cell < 2) {
                    const float half = cell == 1 ? 0.07f : 0.045f;   // the ring's half width
                    const float d = std::fabs(r - 0.9f);
                    core = std::clamp((half - d) / px1, 0.0f, 1.0f);
                    edge = std::clamp((half + 0.03f - d) / px1, 0.0f, 1.0f);
                } else {
                    core = std::clamp((0.55f - r) / px1, 0.0f, 1.0f);
                    edge = std::clamp((0.8f - r) / px1, 0.0f, 1.0f);
                }
                const float a = std::max(core * 0.9f, edge * 0.55f);
                float red = 1.0f, grn = 0.85f, blu = 0.35f;              // pale amber
                if (cell == 1) { red = 0.35f; grn = 1.0f; blu = 0.4f; }  // lit: green
                if (cell == 2) { red = 0.95f; grn = 0.95f; blu = 0.95f; }
                const float k = core * 0.9f;                             // the edge is dark
                px[static_cast<size_t>(y) * w + cell * kCell + x] =
                    b8(blu * k) | (b8(grn * k) << 8) | (b8(red * k) << 16) | (b8(a) << 24);
            }
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(hgt);
    td.MipLevels = td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(fmt);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), static_cast<UINT>(w * 4), 0};
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex_))) {
        MLOG("markers: CreateTexture2D failed -- no rings");
        return false;
    }
    for (auto& q : quads_) {
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = swapchain_;
    }
    MLOG("markers: ready");
    return true;
}

int Markers::Layers(XrSpace space, const XrPosef& head, const Hands::Output& h, Mode mode, bool showAll,
                    const XrCompositionLayerBaseHeader** out, int max) {
    if (!swapchain_ || !tex_) return 0;
    struct Draw { XrVector3f pos; float size; int cell; };
    Draw draws[kHolsters + 3];
    int n = 0;
    bool anyNear = false;
    for (int i = 0; i < h.spotCount; ++i) {
        const Hands::Spot& s = h.spots[i];
        const bool show = mode == kAlways || (showAll && s.kind == Hands::kHolster) || (mode == kNear && s.close);
        if (!show) continue;
        anyNear = anyNear || s.close;
        draws[n++] = {s.pos, 2.0f * s.radius, s.inside ? 1 : 0};
    }
    // The off hand's dot, with its rings (it's what the foregrip and magazine spots are for).
    if (h.offValid && n > 0 && (mode == kAlways || anyNear || showAll)) draws[n++] = {h.offHand, 0.03f, 2};
    n = std::min(n, max);
    if (n == 0) return 0;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return 0;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wi))) return 0;
    ctx_->CopyResource(images_[idx].texture, tex_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);

    for (int i = 0; i < n; ++i) {
        XrCompositionLayerQuad& q = quads_[i];
        q.space = space;
        q.pose.position = draws[i].pos;
        q.pose.orientation = head.orientation;  // facing the viewer
        q.size = {draws[i].size, draws[i].size};
        q.subImage.imageRect = {{draws[i].cell * kCell, 0}, {kCell, kCell}};
        out[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        MLOG("markers: first rings shown (%d)", n);
    }
    return n;
}

}  // namespace mohavr::host
