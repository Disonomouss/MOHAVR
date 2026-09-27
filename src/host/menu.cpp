#include "menu.hpp"

#include <windows.h>
#include <shlobj.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

constexpr float kScaleMin = 20.0f, kScaleMax = 200.0f, kScaleStep = 5.0f;
constexpr float kHeightMin = -0.6f, kHeightMax = 0.6f, kHeightStep = 0.05f;
enum Item { kWorldScale, kHeight, kTurn, kSticks, kGunHand, kGunFit, kRecenter, kResetScale, kClose, kItemCount };
constexpr int kSnapSteps[] = {0, 30, 45};  // Turning: smooth, snap 30, snap 45 (degrees)
// The Gun fit page (M8): per weapon, saved in the player's ini [GunFit] <weapon class> = gx gy gz angle rayUp rayRight
// foreFwd foreUp (older entries have the first six).
enum FitItem { fForward, fRight, fUp, fAngle, fRayUp, fRayRight, fForeFwd, fForeUp, fReset, fBack, fCount };
constexpr float kFitStep = 1.0f, kAngleStep = 2.0f, kRayStep = 0.5f;  // units (cm at scale 100), degrees, cm

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

    // Gun fit defaults: the shipped [Weapon] GripX/Y/Z and [Aim] RayUp (the game uses the same without a fit).
    auto shippedFloat = [&](const wchar_t* sec, const wchar_t* key, float def) {
        wchar_t b[32] = L"";
        GetPrivateProfileStringW(sec, key, L"", b, 32, shipped.c_str());
        return b[0] ? static_cast<float>(_wtof(b)) : def;
    };
    fitDefault_ = {{shippedFloat(L"Weapon", L"GripX", 34.0f), shippedFloat(L"Weapon", L"GripY", 11.0f),
                    shippedFloat(L"Weapon", L"GripZ", -17.0f)},
                   0.0f, shippedFloat(L"Aim", L"RayUp", 8.0f), 0.0f, shippedFloat(L"Hands", L"ForeFwd", 30.0f),
                   shippedFloat(L"Hands", L"ForeUp", 0.0f)};
    fit_ = fitDefault_;
    gunInHand_ = GetPrivateProfileIntW(L"Weapon", L"ViewModel", 2, shipped.c_str()) == 2;

    // Controls (the player's): the sticks and the starting gun hand; the shipped [Controls] ones are the defaults.
    const int defSwap = static_cast<int>(GetPrivateProfileIntW(L"Controls", L"SwapSticks", 0, shipped.c_str()));
    swapSticks_ = GetPrivateProfileIntW(L"Controls", L"SwapSticks", defSwap, iniPath_.c_str()) != 0;
    GetPrivateProfileStringW(L"Controls", L"GunHand", L"", buf, 32, shipped.c_str());
    const bool defLeft = !_wcsicmp(buf, L"left");
    GetPrivateProfileStringW(L"Controls", L"GunHand", defLeft ? L"left" : L"right", buf, 32, iniPath_.c_str());
    startLeft_ = !_wcsicmp(buf, L"left");
    MLOG("menu: sticks %s, gun hand %s (at start)", swapSticks_ ? "swapped (right moves)" : "normal", startLeft_ ? "left" : "right");
    MLOG("menu: gun fit defaults grip %.1f %.1f %.1f, aim line %.1f cm up (gun in hand %d)", fitDefault_.grip[0],
         fitDefault_.grip[1], fitDefault_.grip[2], fitDefault_.rayUp, gunInHand_);
}

void Menu::SyncWeapon() {
    if (!hdr_ || hdr_->weaponSeq == seenWeaponSeq_) return;
    seenWeaponSeq_ = hdr_->weaponSeq;
    char key[48];
    std::memcpy(key, hdr_->weaponKey, sizeof(key));
    key[47] = 0;
    weaponKey_ = key;
    fit_ = fitDefault_;
    bool saved = false;
    if (!weaponKey_.empty()) {
        const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
        wchar_t b[128] = L"";
        GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", b, 128, iniPath_.c_str());
        shared::GunFit f = fitDefault_;
        const int n = b[0] ? swscanf_s(b, L"%f %f %f %f %f %f %f %f", &f.grip[0], &f.grip[1], &f.grip[2], &f.angle, &f.rayUp,
                                       &f.rayRight, &f.foreFwd, &f.foreUp)
                           : 0;
        if (n == 6 || n == 8) {  // 6: saved before the foregrip existed (it keeps the default)
            fit_ = f;
            saved = true;
        }
    }
    MLOG("menu: weapon '%s' -- fit %s: grip %.1f %.1f %.1f, angle %.0f, aim line up %.1f right %.1f", weaponKey_.c_str(),
         saved ? "saved" : "default", fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle, fit_.rayUp, fit_.rayRight);
    PublishFit();
}

void Menu::PublishFit() {
    if (!hdr_) return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr_->fitSeq));  // odd: writing
    const size_t n = weaponKey_.size() < 47 ? weaponKey_.size() : 47;
    std::memcpy(hdr_->fitKey, weaponKey_.c_str(), n);
    hdr_->fitKey[n] = 0;
    for (int i = 0; i < 3; ++i) hdr_->fitGrip[i] = fit_.grip[i];
    hdr_->fitAngle = fit_.angle;
    hdr_->fitRayUp = fit_.rayUp;
    hdr_->fitRayRight = fit_.rayRight;
    hdr_->fitValid = weaponKey_.empty() ? 0u : 1u;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr_->fitSeq));  // even: done
}

void Menu::SaveFit() {
    if (weaponKey_.empty() || iniPath_.empty()) return;
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    wchar_t b[128];
    swprintf_s(b, L"%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle, fit_.rayUp,
               fit_.rayRight, fit_.foreFwd, fit_.foreUp);
    WritePrivateProfileStringW(L"GunFit", wkey.c_str(), b, iniPath_.c_str());
}

// Stick right = the gun forward / right / up, muzzle up, the aim line up / right. (The grip is the gun's point put
// on the controller, so moving the gun forward moves that point back.)
void Menu::AdjustFit(int item, float dir) {
    if (weaponKey_.empty()) return;
    switch (item) {
        case fForward: fit_.grip[0] -= dir * kFitStep; break;
        case fRight: fit_.grip[1] -= dir * kFitStep; break;
        case fUp: fit_.grip[2] -= dir * kFitStep; break;
        case fAngle: fit_.angle = std::fmax(-45.0f, std::fmin(45.0f, fit_.angle + dir * kAngleStep)); break;
        case fRayUp: fit_.rayUp = std::fmax(-30.0f, std::fmin(30.0f, fit_.rayUp + dir * kRayStep)); break;
        case fRayRight: fit_.rayRight = std::fmax(-30.0f, std::fmin(30.0f, fit_.rayRight + dir * kRayStep)); break;
        case fForeFwd: fit_.foreFwd = std::fmax(0.0f, std::fmin(80.0f, fit_.foreFwd + dir * kFitStep)); break;
        case fForeUp: fit_.foreUp = std::fmax(-30.0f, std::fmin(30.0f, fit_.foreUp + dir * kFitStep)); break;
        default: return;
    }
    for (float& g : fit_.grip) g = std::fmax(-200.0f, std::fmin(200.0f, g));
    PublishFit();
    SaveFit();
    MLOG("menu: %s fit -> grip %.1f %.1f %.1f, angle %.0f, aim line up %.1f right %.1f, foregrip %.0f / %.0f cm",
         weaponKey_.c_str(), fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle, fit_.rayUp, fit_.rayRight, fit_.foreFwd,
         fit_.foreUp);
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
    WritePrivateProfileStringW(L"Controls", L"SwapSticks", swapSticks_ ? L"1" : L"0", iniPath_.c_str());
    WritePrivateProfileStringW(L"Controls", L"GunHand", startLeft_ ? L"left" : L"right", iniPath_.c_str());
}

void Menu::Update(float dt, const MenuInput& in, const XrPosef& head, bool headValid) {
    SyncWeapon();  // every frame: the fit follows the weapon in hand, open or not
    if (visible_ && page_ == 1 && (in.back || (in.select && selected_ == fBack))) {
        page_ = 0;  // back from the Gun fit page to the main page
        selected_ = kGunFit;
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
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
            page_ = 0;
            rendered_ = false;
            MLOG("menu: opened");
        } else {
            visible_ = true;  // Close() only acts on a visible menu
            Close();
        }
    }
    if (!visible_) return;

    if (page_ == 1) {
        if (in.up) selected_ = (selected_ + fCount - 1) % fCount;
        if (in.down) selected_ = (selected_ + 1) % fCount;
        if (in.left || in.right) AdjustFit(selected_, in.right ? 1.0f : -1.0f);
        if (in.select && selected_ == fReset && !weaponKey_.empty()) {
            fit_ = fitDefault_;
            PublishFit();
            const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
            WritePrivateProfileStringW(L"GunFit", wkey.c_str(), nullptr, iniPath_.c_str());
            MLOG("menu: %s fit reset to the defaults", weaponKey_.c_str());
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }

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
        } else if (selected_ == kSticks) {
            swapSticks_ = !swapSticks_;
            Save();
            MLOG("menu: sticks -> %s", swapSticks_ ? "swapped (right moves, left turns)" : "normal (left moves, right turns)");
        } else if (selected_ == kGunHand) {
            startLeft_ = !startLeft_;
            Save();
            MLOG("menu: gun hand -> %s", startLeft_ ? "left" : "right");
        }
    }
    if (in.select) {
        if (selected_ == kGunFit) {
            page_ = 1;
            selected_ = 0;
            MLOG("menu: gun fit page (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
        } else if (selected_ == kRecenter) {
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
    if (page_ == 1) {
        RenderFitPage();
    } else {
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
    snprintf(label, sizeof(label), "Sticks           <  %s  >", swapSticks_ ? "move right, turn left" : "move left, turn right");
    ImGui::Selectable(label, selected_ == kSticks);
    snprintf(label, sizeof(label), "Gun hand         <  %s  >", startLeft_ ? "left" : "right");
    ImGui::Selectable(label, selected_ == kGunHand);
    snprintf(label, sizeof(label), "Gun fit  (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
    ImGui::Selectable(label, selected_ == kGunFit);
    ImGui::Selectable("Recentre (face forward, here)", selected_ == kRecenter);
    snprintf(label, sizeof(label), "Reset world scale (%.0f)", hdr_ ? hdr_->defaultUnitsPerMeter : 100.0f);
    ImGui::Selectable(label, selected_ == kResetScale);
    ImGui::Selectable("Close", selected_ == kClose);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Left stick: choose / adjust    Trigger: select    Menu: close");
    ImGui::PopFont();
    }
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

// Values shown relative to the defaults for the gun's position (0 = as shipped), absolute for the rest.
void Menu::RenderFitPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Gun fit");
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", weaponKey_.empty() ? "(no gun in hand)" : weaponKey_.c_str());
    ImGui::Separator();
    const bool on = !weaponKey_.empty();
    if (!on) ImGui::BeginDisabled();
    char label[128];
    auto moved = [&](int i) { return std::round(fitDefault_.grip[i] - fit_.grip[i]) + 0.0f; };  // +0.0f: no "-0"
    snprintf(label, sizeof(label), "Gun forward / back    <  %+.0f  >", moved(0));
    ImGui::Selectable(label, selected_ == fForward);
    snprintf(label, sizeof(label), "Gun right / left      <  %+.0f  >", moved(1));
    ImGui::Selectable(label, selected_ == fRight);
    snprintf(label, sizeof(label), "Gun up / down         <  %+.0f  >", moved(2));
    ImGui::Selectable(label, selected_ == fUp);
    snprintf(label, sizeof(label), "Gun angle             <  %+.0f\xC2\xB0  >", fit_.angle);
    ImGui::Selectable(label, selected_ == fAngle);
    snprintf(label, sizeof(label), "Aim line up / down    <  %+.1f cm  >", fit_.rayUp);
    ImGui::Selectable(label, selected_ == fRayUp);
    snprintf(label, sizeof(label), "Aim line right / left <  %+.1f cm  >", fit_.rayRight);
    ImGui::Selectable(label, selected_ == fRayRight);
    snprintf(label, sizeof(label), "Foregrip forward      <  %.0f cm  >", fit_.foreFwd);
    ImGui::Selectable(label, selected_ == fForeFwd);
    snprintf(label, sizeof(label), "Foregrip up / down    <  %+.0f cm  >", fit_.foreUp);
    ImGui::Selectable(label, selected_ == fForeUp);
    ImGui::Selectable("Reset this gun", selected_ == fReset);
    if (!on) ImGui::EndDisabled();
    ImGui::Selectable("Back", selected_ == fBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled(gunInHand_ ? "Stick right = forward / right / up / muzzle up. Saved for this gun."
                                   : "The gun isn't drawn in your hand (Weapon.ViewModel=2 in MOHAVR.ini): no effect.");
    ImGui::TextDisabled("Line the barrel up with the red dot using the aim line.   B: back");
    ImGui::PopFont();
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
