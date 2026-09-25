#include "menu.hpp"

#include <windows.h>
#include <shlobj.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <cmath>
#include <cstdio>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

constexpr float kScaleMin = 20.0f, kScaleMax = 200.0f, kScaleStep = 5.0f;
constexpr float kHeightMin = -0.6f, kHeightMax = 0.6f, kHeightStep = 0.05f;
enum Item { kWorldScale, kHeight, kTurn, kRecenter, kResetScale, kClose, kItemCount };
constexpr int kSnapSteps[] = {0, 30, 45};  // Turning: smooth, snap 30, snap 45 (degrees)

std::wstring UserIniPath() {
    wchar_t base[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) return L"";
    std::wstring dir = std::wstring(base) + L"\\MOHAVR";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\MOHAVR.user.ini";
}

}  // namespace

bool Menu::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, XrSession session, int64_t swapchainFormat, shared::Header* hdr) {
    dev_ = dev;
    ctx_ = ctx;
    hdr_ = hdr;
    iniPath_ = UserIniPath();

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = swapchainFormat;
    sci.sampleCount = 1;
    sci.width = static_cast<uint32_t>(width_);
    sci.height = static_cast<uint32_t>(height_);
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &sci, &swapchain_))) { MLOG("menu: xrCreateSwapchain failed"); return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr);
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

    // ImGui draws into our own UNORM texture; its bits are gamma-encoded colour, so a raw copy into
    // the sRGB swapchain shows them correctly (same trick as the game frames).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width_);
    td.Height = static_cast<UINT>(height_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex_)) || FAILED(dev_->CreateRenderTargetView(tex_, nullptr, &rtv_))) {
        MLOG("menu: render target creation failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(width_), static_cast<float>(height_));
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = 34.0f;
    st.WindowRounding = 0.0f;
    st.WindowPadding = ImVec2(28, 24);
    st.ItemSpacing = ImVec2(12, 14);
    if (!ImGui_ImplDX11_Init(dev_, ctx_)) { MLOG("menu: ImGui DX11 init failed"); return false; }

    MLOG("menu: ready (%dx%d, %u images, settings file %ls)", width_, height_, n, iniPath_.c_str());
    return true;
}

void Menu::ApplySavedSettings() {
    const float def = (hdr_ && hdr_->defaultUnitsPerMeter > 1.0f) ? hdr_->defaultUnitsPerMeter : 50.0f;
    float v = def;
    wchar_t buf[32] = L"";
    GetPrivateProfileStringW(L"Camera", L"UnitsPerMeter", L"", buf, 32, iniPath_.c_str());
    if (buf[0]) {
        const float s = static_cast<float>(_wtof(buf));
        if (s >= kScaleMin && s <= kScaleMax) v = s;
    }
    MLOG("menu: world scale %.1f (%s; default %.1f)", v, buf[0] ? "player's saved setting" : "default", def);
    SetUnitsPerMeter(v, false);

    float h = 0.0f;
    GetPrivateProfileStringW(L"Camera", L"HeightOffset", L"", buf, 32, iniPath_.c_str());
    if (buf[0]) {
        const float s = static_cast<float>(_wtof(buf));
        if (s >= kHeightMin && s <= kHeightMax) h = s;
    }
    MLOG("menu: height offset %+.2f m (%s)", h, buf[0] ? "player's saved setting" : "default");
    SetHeightOffset(h, false);

    // Turning: the shipped default is [Comfort] SnapTurn in MOHAVR.ini next to the host.
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring shipped(exe);
    shipped = shipped.substr(0, shipped.find_last_of(L'\\')) + L"\\MOHAVR.ini";
    const int defSnap = static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"SnapTurn", 0, shipped.c_str()));
    const int saved = static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"SnapTurn", -1, iniPath_.c_str()));
    snapDeg_ = 0;
    for (int s : kSnapSteps)
        if (s == (saved >= 0 ? saved : defSnap)) snapDeg_ = s;
    MLOG("menu: turning %s%d (%s)", snapDeg_ ? "snap " : "smooth ", snapDeg_, saved >= 0 ? "player's saved setting" : "default");
}

void Menu::SetHeightOffset(float v, bool save) {
    v = v < kHeightMin ? kHeightMin : (v > kHeightMax ? kHeightMax : v);
    v = std::round(v / kHeightStep) * kHeightStep;  // no float drift from repeated steps
    heightOffset_ = v;
    if (hdr_) hdr_->heightOffset = v;
    if (save) Save();
}

void Menu::Close() {
    if (!visible_) return;
    visible_ = false;
    Save();
    MLOG("menu: closed (world scale %.1f, height %+.2f m, turning %d saved)", unitsPerMeter_, heightOffset_, snapDeg_);
}

void Menu::SetUnitsPerMeter(float v, bool save) {
    v = v < kScaleMin ? kScaleMin : (v > kScaleMax ? kScaleMax : v);
    unitsPerMeter_ = v;
    if (hdr_) hdr_->unitsPerMeter = v;  // the game reads it on its next view
    if (save) Save();
}

void Menu::Save() {
    if (iniPath_.empty()) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", unitsPerMeter_);
    WritePrivateProfileStringW(L"Camera", L"UnitsPerMeter", buf, iniPath_.c_str());
    swprintf_s(buf, L"%.2f", heightOffset_);
    WritePrivateProfileStringW(L"Camera", L"HeightOffset", buf, iniPath_.c_str());
    swprintf_s(buf, L"%d", snapDeg_);
    WritePrivateProfileStringW(L"Comfort", L"SnapTurn", buf, iniPath_.c_str());
}

void Menu::Update(float dt, const MenuInput& in, const XrPosef& head, bool headValid) {
    if (in.toggle || (visible_ && in.back)) {
        visible_ = !visible_;
        if (visible_) {
            // Open 1 m in front of the head, level and facing the player (heading only).
            const auto& q = head.orientation;
            float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));  // forward = R*(0,0,-1)
            const float len = std::sqrt(fx * fx + fz * fz);
            if (!headValid || len < 1e-3f) { fx = 0.0f; fz = -1.0f; } else { fx /= len; fz /= len; }
            const float yaw = std::atan2(-fx, -fz);  // rotation about +Y so the panel faces the head
            panelPose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            panelPose_.position = {head.position.x + fx * 1.0f, head.position.y - 0.15f, head.position.z + fz * 1.0f};
            selected_ = 0;
            rendered_ = false;
            MLOG("menu: opened");
        } else {
            visible_ = true;  // Close() only acts on a visible menu
            Close();
        }
    }
    if (!visible_) return;

    if (in.up) selected_ = (selected_ + kItemCount - 1) % kItemCount;
    if (in.down) selected_ = (selected_ + 1) % kItemCount;
    if (in.left || in.right) {
        const float dir = in.right ? 1.0f : -1.0f;
        if (selected_ == kWorldScale) {
            SetUnitsPerMeter(unitsPerMeter_ + dir * kScaleStep, true);
            MLOG("menu: world scale -> %.1f", unitsPerMeter_);
        } else if (selected_ == kHeight) {
            SetHeightOffset(heightOffset_ + dir * kHeightStep, true);
            MLOG("menu: height offset -> %+.2f m", heightOffset_);
        } else if (selected_ == kTurn) {
            int i = 0;
            while (i < 2 && kSnapSteps[i] != snapDeg_) ++i;
            i = (i + (in.right ? 1 : 2)) % 3;
            snapDeg_ = kSnapSteps[i];
            Save();
            MLOG("menu: turning -> %s %d", snapDeg_ ? "snap" : "smooth", snapDeg_);
        }
    }
    if (in.select) {
        if (selected_ == kRecenter) {
            recenterRequested_ = true;  // the host re-creates LOCAL at the current head pose, then closes us
            MLOG("menu: recentre requested");
        } else if (selected_ == kResetScale) {
            const float def = (hdr_ && hdr_->defaultUnitsPerMeter > 1.0f) ? hdr_->defaultUnitsPerMeter : 100.0f;
            SetUnitsPerMeter(def, true);
            MLOG("menu: world scale reset to %.1f", def);
        } else if (selected_ == kClose) {
            Close();
            return;
        }
    }

    ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
    Render();
}

void Menu::Render() {
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("MOHAVR", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    // 1.92: the size is set per push (the style's FontSizeBase alone left it at the 13 px default).
    ImGui::PushFont(nullptr, 40.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "MOHAVR");
    ImGui::Separator();

    const float ipdMm = hdr_ ? 1000.0f * std::sqrt(std::pow(hdr_->eye[1].px - hdr_->eye[0].px, 2.0f) +
                                                   std::pow(hdr_->eye[1].py - hdr_->eye[0].py, 2.0f) +
                                                   std::pow(hdr_->eye[1].pz - hdr_->eye[0].pz, 2.0f))
                              : 64.0f;
    char label[128];
    snprintf(label, sizeof(label), "World scale      <  %.0f  >", unitsPerMeter_);
    ImGui::Selectable(label, selected_ == kWorldScale);
    ImGui::PushFont(nullptr, 28.0f);
    ImGui::TextDisabled("   higher = smaller world   (eyes %.1f units apart)", ipdMm * unitsPerMeter_ / 1000.0f);
    ImGui::PopFont();
    snprintf(label, sizeof(label), "Height           <  %+.0f cm  >", heightOffset_ * 100.0f);
    ImGui::Selectable(label, selected_ == kHeight);
    if (snapDeg_) snprintf(label, sizeof(label), "Turning          <  snap %d\xC2\xB0  >", snapDeg_);
    else snprintf(label, sizeof(label), "Turning          <  smooth  >");
    ImGui::Selectable(label, selected_ == kTurn);
    ImGui::Selectable("Recentre (face forward, here)", selected_ == kRecenter);
    snprintf(label, sizeof(label), "Reset world scale (%.0f)", hdr_ ? hdr_->defaultUnitsPerMeter : 100.0f);
    ImGui::Selectable(label, selected_ == kResetScale);
    ImGui::Selectable("Close", selected_ == kClose);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Left stick: choose / adjust    Trigger: select    Menu: close");
    ImGui::PopFont();
    ImGui::PopFont();
    ImGui::End();
    ImGui::Render();

    const float clear[4] = {0.08f, 0.08f, 0.09f, 0.92f};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(swapchain_, &wi);
    ctx_->CopyResource(images_[idx].texture, tex_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);
    rendered_ = true;
}

const XrCompositionLayerBaseHeader* Menu::Layer(XrSpace local) {
    if (!visible_ || !rendered_) return nullptr;
    layer_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer_.space = local;
    layer_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer_.subImage.swapchain = swapchain_;
    layer_.subImage.imageRect = {{0, 0}, {width_, height_}};
    layer_.pose = panelPose_;
    layer_.size = {0.8f, 0.8f * static_cast<float>(height_) / static_cast<float>(width_)};
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_);
}

}  // namespace mohavr::host
