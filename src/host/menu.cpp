#include "formats.hpp"
#include "menu.hpp"

#include <windows.h>
#include <shlobj.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../mohavr/log.hpp"
#include "../common/render_presets.hpp"

namespace mohavr::host {
namespace {

constexpr float kScaleMin = 20.0f, kScaleMax = 200.0f, kScaleStep = 5.0f;
constexpr float kHeightMin = -0.6f, kHeightMax = 0.6f, kHeightStep = 0.05f;
enum Item { kWorldScale, kHeight, kTurn, kSticks, kMove, kGunHand, kRedDot, kPacing, kReload, kGunFit, kHolsterPage, kFreeHandPage,
            kRecenter, kResetScale, kClose, kGripPage, kHandFwd, kHandUp, kHandIn, kForeSize, kRingScale, kSpotPage, kGiveAll,
            kOffNade, kOffPistol, kNadeHold, kGunNade, kPouchReload, kMelee, kScope, kScopeZoom, kOffKnife, kKnifePage, kRackEject,
            kRackKeep, kHudPlace, kHudShow, kHudLayout, kHudBacking, kHudWristPage, kHudScreenPage, kCrouch, kVignette, kSeated, kResolution, kChute, kRecoil, kGrabPickup, kMgHands, kNadeStyle, kDamageTint, kHolsterRings, kReloadRings, kFireShake, kRoomScale, kPouchMag, kItemCount };
// Round 32: the main page in tabs (the player: "the menu is getting cluttered"). The tab row is selected_ -1: left /
// right switch tabs there, down goes into the tab's items (up from the first comes back).
// D81 (the player, 2026-10-08: clean it up again -- General had grown to 15 items, Weapons to 21): six tabs, each item where
// a player would look for it and the ones changed most first. Tests reach an item by its key ("goto=recoil", kItemKeys),
// not by counting steps.
enum Tab { tGeneral, tComfort, tWeapons, tReload, tHands, tHud, kTabCount };
const char* kTabNames[kTabCount] = {"General", "Comfort", "Weapons", "Reload", "Hands", "HUD"};
constexpr int kTabMax = 24;
const int kTabItems[kTabCount][kTabMax] = {
    // General: the view and the setup.
    {kRecenter, kWorldScale, kHeight, kResetScale, kGunHand, kResolution, kPacing, kHolsterRings, kReloadRings, kClose, -1},
    // Comfort: moving, turning, stance.
    {kTurn, kMove, kSticks, kRoomScale, kVignette, kDamageTint, kFireShake, kSeated, kCrouch, kChute, kClose, -1},
    // Weapons: the gun in hand.
    {kGunFit, kRedDot, kRecoil, kScope, kScopeZoom, kMelee, kGunNade, kGrabPickup, kMgHands, kGiveAll, kClose, -1},
    // Reload: the manual reload and its pages.
    {kReload, kPouchReload, kPouchMag, kGripPage, kSpotPage, kRackEject, kRackKeep, kClose, -1},
    // Hands: the holsters, the off hand's items, the hand point and rings.
    {kHolsterPage, kNadeStyle, kOffNade, kNadeHold, kOffPistol, kOffKnife, kKnifePage, kFreeHandPage, kHandFwd, kHandUp, kHandIn, kForeSize,
     kRingScale, kClose, -1},
    {kHudPlace, kHudShow, kHudLayout, kHudBacking, kHudWristPage, kHudScreenPage, kClose, -1},
};
// D81: each item's key for tests (menu_cmd.py "goto=<key>"), in Item order.
const char* kItemKeys[kItemCount] = {
    "worldscale", "height", "turning", "sticks", "movedir", "gunhand", "reddot", "pacing", "manualreload", "gunfit", "holsters",
    "freehand", "recentre", "resetscale", "close", "reloadgrip", "handfwd", "handup", "handin", "foresize", "rings", "reloadspots",
    "giveall", "offnade", "offpistol", "nadehold", "gunnade", "pouchreload", "melee", "scope", "scopezoom", "offknife", "knifegrip",
    "rackeject", "rackkeep", "hudplace", "hudshow", "hudlayout", "hudbacking", "hudwrist", "hudscreen", "crouch", "vignette",
    "seated", "resolution", "chute", "recoil", "grabpickup", "mghands", "nadestyle", "damageflash", "holsterrings", "reloadrings", "fireshake", "roomscale", "pouchmag"};
static_assert(sizeof(kItemKeys) / sizeof(kItemKeys[0]) == kItemCount, "one key per menu item");
// "Give all weapons" (the player's request, 2026-10-01): shown only with the shipped [Weapon] GiveAllMenu=1.
bool g_giveAllMenu = false;
// D83: with the simple grenades, the classic style's own items (the gun hand's grenade, the hold) are hidden.
bool g_nadeSimple = true;
bool Shown(int item) {
    if (item == kGiveAll) return g_giveAllMenu;
    if (item == kGunNade || item == kNadeHold) return !g_nadeSimple;
    return true;
}
// Tab t's i-th shown item (-1 past the end), and how many it shows.
int ItemAt(int t, int i) {
    for (int k = 0; k < kTabMax && kTabItems[t][k] >= 0; ++k)
        if (Shown(kTabItems[t][k]) && i-- == 0) return kTabItems[t][k];
    return -1;
}
int TabCount(int t) {
    int n = 0;
    while (ItemAt(t, n) >= 0) ++n;
    return n;
}
// The Reload grip page (round 32): which grip, the hand moved on the part and turned at the wrist, per weapon.
enum GripItem { gWhich, gHoldLikeGrab, gFwd, gUp, gRight, gTilt, gTurn, gRoll, gReset, gBack, gCount };
const char* kGripNames[3] = {"magazine grab", "held magazine", "handle / bolt"};
const wchar_t* kGripKinds[3] = {L"mag", L"hold", L"bolt"};
// The Reload spots page (round 33): the magazine's and the handle's grab rings, moved and sized, per weapon.
enum SpotItem { pWhich, pFwd, pUp, pRight, pSize, pReset, pBack, pCount };
const char* kSpotNames[2] = {"magazine", "handle / bolt"};
const wchar_t* kSpotKinds[2] = {L"mag", L"bolt"};
constexpr int kSnapSteps[] = {0, 30, 45};  // Turning: smooth, snap 30, snap 45 (degrees)
// The Gun fit page (M8): per weapon, saved in the player's ini [GunFit] <weapon class> = gx gy gz angle rayUp rayRight
// foreFwd foreUp foreRight (older entries have the first six or eight).
enum FitItem { fForward, fRight, fUp, fAngle, fRayUp, fRayRight, fForeFwd, fForeUp, fForeRight, fReset, fBack, fCount };
constexpr float kFitStep = 1.0f, kAngleStep = 2.0f, kRayStep = 0.5f;  // units (cm at scale 100), degrees, cm
// The Holsters page: pick a holster, choose what it holds (the player, 2026-10-02: "add option in menu to decide what is
// in each holster"; saved in the player's ini [Holsters] <Name> = a game command or none), move it and size it (cm; [Holsters]
// <Name>Spot = x y z r); the rings' visibility ([Hands] Rings = never / near / always).
enum HolsterItem { hWhich, hHolds, hRight, hUp, hForward, hSize, hShown, hRings, hReset, hBack, hCount };
// What a holster can hold: the game's commands, in the menu's order, with their names.
const char* const kHoldCommands[] = {"SwitchPrimary", "SwitchSecondary", "SwitchPistol", "SwitchGrenade", "SwitchFragGrenade",
                                     "SwitchGammon", "SwitchStick", "Knife", "Reload", ""};
const char* const kHoldNames[] = {"primary", "secondary", "pistol", "grenade", "frag grenade", "Gammon bomb", "stick grenade",
                                  "knife (off hand)", "reload", "nothing"};
constexpr int kHoldCount = 10;
int HoldIndex(const std::string& c) {
    for (int i = 0; i < kHoldCount; ++i)
        if (!_stricmp(c.c_str(), kHoldCommands[i])) return i;
    return -1;  // a command of the player's own ini, kept as it is
}
// The Free hand page: how the free support hand sits on its controller (pitch, yaw, roll in degrees; forward in cm),
// saved in the player's ini [Hands] FreeHand = p y r f.
enum FreeHandItem { eqPitch, eqYaw, eqRoll, eqForward, eqReset, eqBack, eqCount };
// The Knife grip page (after round 50: "Knife needs hand position adjustment"): the grip, and the knife moved and turned in
// the hand, saved in the player's ini [OffHand] KnifeGrip = forward | icepick, KnifeAdj = fwd right up tilt turn roll.
enum KnifeItem { kgGrip, kgFwd, kgRight, kgUp, kgTilt, kgTurn, kgRoll, kgReset, kgBack, kgCount };
// The Wrist panels page: which panel (left: health and compass; right: weapon and grenades; both), moved along the forearm,
// across it, out from it (cm), sized (%), tilted toward the eyes (deg); [HUD] WristLeftPanel / WristRightPanel = a c o s t.
enum WristItem { wpWhich, wpAlong, wpAcross, wpOut, wpSize, wpTilt, wpReset, wpBack, wpCount };
const char* kWristPanelNames[3] = {"left (health, compass)", "right (weapon, grenades)", "both"};
const wchar_t* kWristPanelKeys[2] = {L"WristLeftPanel", L"WristRightPanel"};
// The Screen HUD page: the head-locked panel's distance, size and height ([HUD] Distance / Width / Down).
enum ScreenItem { shDist, shWidth, shDown, shReset, shBack, shCount };
const wchar_t* kBackings[3] = {L"none", L"dim", L"dark"};
const char* kHolsterLabels[kSpots] = {"right shoulder", "left shoulder", "right hip", "left hip", "chest", "lower back", "magazine pouch"};
const wchar_t* kRingModes[3] = {L"never", L"near", L"always"};
const char*    kRingLabels[3] = {"off", "near", "always"};  // (D86: the menu's words)

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
    td.Format = RtFormat(swapchainFormat);  // the swapchain's family (GOAL B2)
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
    shippedPath_ = shipped;
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
    g_giveAllMenu = GetPrivateProfileIntW(L"Weapon", L"GiveAllMenu", 0, shipped.c_str()) != 0;
    MLOG("menu: \"Give all weapons\" %s ([Weapon] GiveAllMenu)", g_giveAllMenu ? "shown" : "hidden");

    // Controls (the player's): the sticks and the starting gun hand; the shipped [Controls] ones are the defaults.
    const int defSwap = static_cast<int>(GetPrivateProfileIntW(L"Controls", L"SwapSticks", 0, shipped.c_str()));
    swapSticks_ = GetPrivateProfileIntW(L"Controls", L"SwapSticks", defSwap, iniPath_.c_str()) != 0;
    GetPrivateProfileStringW(L"Controls", L"GunHand", L"", buf, 32, shipped.c_str());
    const bool defLeft = !_wcsicmp(buf, L"left");
    GetPrivateProfileStringW(L"Controls", L"GunHand", defLeft ? L"left" : L"right", buf, 32, iniPath_.c_str());
    startLeft_ = !_wcsicmp(buf, L"left");
    const int defDot = static_cast<int>(GetPrivateProfileIntW(L"Aim", L"Reticle", 1, shipped.c_str()));
    redDot_ = GetPrivateProfileIntW(L"Aim", L"Reticle", defDot, iniPath_.c_str()) != 0;
    // Frame pacing (round 25): the shipped [Bridge] Pace is the default; the player's own only once they toggle it (so
    // a later shipped default isn't pinned). Live: the game reads hdr->pace every Draw.
    const int defPace = static_cast<int>(GetPrivateProfileIntW(L"Bridge", L"Pace", 0, shipped.c_str()));
    pacing_ = GetPrivateProfileIntW(L"Bridge", L"Pace", defPace, iniPath_.c_str()) != 0;
    if (hdr_) hdr_->pace = pacing_ ? 1u : 0u;
    // Physical crouch (GOAL A1, D61): likewise the shipped [Controls] PhysicalCrouch until the player toggles it; live to the
    // game through hdr->crouchMode (1 off, 2 on).
    const int defCrouch = static_cast<int>(GetPrivateProfileIntW(L"Controls", L"PhysicalCrouch", 0, shipped.c_str()));
    crouch_ = GetPrivateProfileIntW(L"Controls", L"PhysicalCrouch", defCrouch, iniPath_.c_str()) != 0;
    // Seated (GOAL A3): likewise the shipped [Comfort] Seated; live through crouchMode's bits 2-3.
    const int defSeated = static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"Seated", 0, shipped.c_str()));
    seated_ = GetPrivateProfileIntW(L"Comfort", L"Seated", defSeated, iniPath_.c_str()) != 0;
    // D75: the parachute steered by the hands -- the shipped [Controls] ChuteHands until the player toggles it.
    const int defChute = static_cast<int>(GetPrivateProfileIntW(L"Controls", L"ChuteHands", 0, shipped.c_str()));
    chuteHands_ = GetPrivateProfileIntW(L"Controls", L"ChuteHands", defChute, iniPath_.c_str()) != 0;
    // The render resolution preset (D73): the shipped [Render] Preset until the player picks one; the game applies it at start.
    {
        wchar_t k[32] = L"";
        GetPrivateProfileStringW(L"Render", L"Preset", L"custom", k, 32, shipped.c_str());
        GetPrivateProfileStringW(L"Render", L"Preset", k, k, 32, iniPath_.c_str());
        char k8[32] = "";
        for (int i = 0; i < 31 && k[i]; ++i) k8[i] = static_cast<char>(k[i] < 128 ? k[i] : '?');
        resPreset_ = presets::Find(k8);
        startPreset_ = resPreset_;
        shippedResX_ = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResX", 2880, shipped.c_str()));
        shippedResY_ = static_cast<int>(GetPrivateProfileIntW(L"Render", L"ResY", 1620, shipped.c_str()));
    }
    if (hdr_) hdr_->crouchMode = CrouchWord();
    // D76-D78: the recoil, the grab pickup and the mounted MG42 by hand -- the shipped [Weapon] Kick, [Controls] GrabPickup and
    // [Weapon] MountedHands until the player changes them; live to the game through hdr->kickMode, pickupMode and mgMode.
    {
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"Weapon", L"Kick", L"1", v, 32, shipped.c_str());
        GetPrivateProfileStringW(L"Weapon", L"Kick", v, v, 32, iniPath_.c_str());
        kickPct_ = std::clamp(static_cast<int>(std::lround(_wtof(v) * 100.0)), 0, 200);
        const int defGrab = static_cast<int>(GetPrivateProfileIntW(L"Controls", L"GrabPickup", 1, shipped.c_str()));
        grabPickup_ = GetPrivateProfileIntW(L"Controls", L"GrabPickup", defGrab, iniPath_.c_str()) != 0;
        const int defMg = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"MountedHands", 0, shipped.c_str()));
        mgHands_ = GetPrivateProfileIntW(L"Weapon", L"MountedHands", defMg, iniPath_.c_str()) != 0;
        PublishWeaponModes();
        // D84: the damage flash -- the shipped [Camera] DamageTint until the player toggles it (live: hdr->damageTint).
        const int defTint = static_cast<int>(GetPrivateProfileIntW(L"Camera", L"DamageTint", 1, shipped.c_str()));
        damageTint_ = GetPrivateProfileIntW(L"Camera", L"DamageTint", defTint, iniPath_.c_str()) != 0;
        if (hdr_) hdr_->damageTint = damageTint_ ? 2u : 1u;
        // D87: the firing shake -- the shipped [Camera] FireShake until the player toggles it (live: hdr->fireShake).
        const int defShake = static_cast<int>(GetPrivateProfileIntW(L"Camera", L"FireShake", 1, shipped.c_str()));
        fireShake_ = GetPrivateProfileIntW(L"Camera", L"FireShake", defShake, iniPath_.c_str()) != 0;
        if (hdr_) hdr_->fireShake = fireShake_ ? 2u : 1u;
        // D88: room-scale walking -- the shipped [Comfort] RoomScale until the player toggles it (live: hdr->roomScale).
        const int defRoom = static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"RoomScale", 1, shipped.c_str()));
        roomScale_ = GetPrivateProfileIntW(L"Comfort", L"RoomScale", defRoom, iniPath_.c_str()) != 0;
        if (hdr_) hdr_->roomScale = roomScale_ ? 2u : 1u;
        // D89: the spare magazine in the pouch -- the shipped [ManualReload] PouchMag until the player toggles it.
        const int defPouchMag = static_cast<int>(GetPrivateProfileIntW(L"ManualReload", L"PouchMag", 1, shipped.c_str()));
        pouchMag_ = GetPrivateProfileIntW(L"ManualReload", L"PouchMag", defPouchMag, iniPath_.c_str()) != 0;
        if (hdr_) hdr_->pouchMagMode = pouchMag_ ? 2u : 1u;
    }
    // The comfort vignette (GOAL A2): likewise the shipped [Comfort] Vignette (0 none, 1 light, 2 strong).
    const int defVig = static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"Vignette", 0, shipped.c_str()));
    vignette_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Comfort", L"Vignette", defVig, iniPath_.c_str())), 0, 2);
    // Move direction (round 29): likewise the shipped [Controls] MoveDirection until the player toggles it.
    GetPrivateProfileStringW(L"Controls", L"MoveDirection", L"head", buf, 32, shipped.c_str());
    GetPrivateProfileStringW(L"Controls", L"MoveDirection", _wcsicmp(buf, L"body") ? L"head" : L"body", buf, 32, iniPath_.c_str());
    moveByHead_ = _wcsicmp(buf, L"body") != 0;
    MLOG("menu: move direction %s", moveByHead_ ? "head" : "body");
    // Manual reload (D21): likewise the shipped [Weapon] ManualReload until the player toggles it.
    const int defReload = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"ManualReload", 0, shipped.c_str()));
    manualReload_ = GetPrivateProfileIntW(L"Weapon", L"ManualReload", defReload, iniPath_.c_str()) != 0;
    MLOG("menu: manual reload %s", manualReload_ ? "on" : "off");
    // D54, the rack eject: likewise the shipped [ManualReload] RackEject / RackEjectKeep until the player toggles them.
    const int defRack = static_cast<int>(GetPrivateProfileIntW(L"ManualReload", L"RackEject", 0, shipped.c_str()));
    rackEject_ = GetPrivateProfileIntW(L"ManualReload", L"RackEject", defRack, iniPath_.c_str()) != 0;
    const int defKeep = static_cast<int>(GetPrivateProfileIntW(L"ManualReload", L"RackEjectKeep", 0, shipped.c_str()));
    rackEjectKeep_ = GetPrivateProfileIntW(L"ManualReload", L"RackEjectKeep", defKeep, iniPath_.c_str()) != 0;
    MLOG("menu: rack eject %s, an ejected round %s", rackEject_ ? "on" : "off", rackEjectKeep_ ? "kept" : "lost");
    // The off-hand grenade: likewise the shipped [OffHand] Grenade until the player toggles it.
    const int defNade = static_cast<int>(GetPrivateProfileIntW(L"OffHand", L"Grenade", 0, shipped.c_str()));
    offHandNade_ = GetPrivateProfileIntW(L"OffHand", L"Grenade", defNade, iniPath_.c_str()) != 0;
    MLOG("menu: off-hand grenade %s", offHandNade_ ? "on" : "off");
    {
        wchar_t d[16] = L"", u[16] = L"";
        GetPrivateProfileStringW(L"OffHand", L"GrenadeHold", L"grip", d, 16, shipped.c_str());
        GetPrivateProfileStringW(L"OffHand", L"GrenadeHold", d, u, 16, iniPath_.c_str());
        nadeClick_ = !_wcsicmp(u, L"click");
        MLOG("menu: grenade hold %s", nadeClick_ ? "click" : "grip");
        // D83: the grenade style -- the shipped [OffHand] GrenadeStyle (simple) until the player picks.
        wchar_t st[32] = L"";
        GetPrivateProfileStringW(L"OffHand", L"GrenadeStyle", L"simple", st, 32, shipped.c_str());
        GetPrivateProfileStringW(L"OffHand", L"GrenadeStyle", st, st, 32, iniPath_.c_str());
        nadeSimple_ = _wcsicmp(st, L"classic") != 0;
        g_nadeSimple = nadeSimple_;
        MLOG("menu: grenades %s", nadeSimple_ ? "simple" : "classic");
        const int defPin = static_cast<int>(GetPrivateProfileIntW(L"Weapon", L"GrenadePin", 0, shipped.c_str()));
        gunNadePin_ = GetPrivateProfileIntW(L"Weapon", L"GrenadePin", defPin, iniPath_.c_str()) != 0;
        MLOG("menu: the gun hand's grenade %s", gunNadePin_ ? "by pin, cook and grip" : "the game's own");
        const int defPouch = static_cast<int>(GetPrivateProfileIntW(L"Hands", L"PouchReload", 0, shipped.c_str()));
        pouchReload_ = GetPrivateProfileIntW(L"Hands", L"PouchReload", defPouch, iniPath_.c_str()) != 0;
        MLOG("menu: pouch reload %s", pouchReload_ ? "on" : "off");
        const int defMelee = static_cast<int>(GetPrivateProfileIntW(L"Melee", L"Physical", 0, shipped.c_str()));
        physicalMelee_ = GetPrivateProfileIntW(L"Melee", L"Physical", defMelee, iniPath_.c_str()) != 0;
        MLOG("menu: physical melee %s", physicalMelee_ ? "on" : "off");
        // Scopes (SCOPE-DESIGN): the shipped [Scope] Enable and Zoom until the player toggles them.
        const int defScope = static_cast<int>(GetPrivateProfileIntW(L"Scope", L"Enable", 0, shipped.c_str()));
        scope_ = GetPrivateProfileIntW(L"Scope", L"Enable", defScope, iniPath_.c_str()) != 0;
        wchar_t dz[16] = L"", uz[16] = L"";
        GetPrivateProfileStringW(L"Scope", L"Zoom", L"real", dz, 16, shipped.c_str());
        GetPrivateProfileStringW(L"Scope", L"Zoom", dz, uz, 16, iniPath_.c_str());
        scopeZoomGame_ = !_wcsicmp(uz, L"game");
        MLOG("menu: scopes %s, zoom %s", scope_ ? "on" : "off", scopeZoomGame_ ? "the game's" : "realistic");
    }
    // The off-hand pistol: likewise the shipped [OffHand] Pistol until the player toggles it.
    const int defPistol = static_cast<int>(GetPrivateProfileIntW(L"OffHand", L"Pistol", 0, shipped.c_str()));
    offHandPistol_ = GetPrivateProfileIntW(L"OffHand", L"Pistol", defPistol, iniPath_.c_str()) != 0;
    MLOG("menu: off-hand pistol %s", offHandPistol_ ? "on" : "off");
    // The off-hand knife: likewise the shipped [OffHand] Knife until the player toggles it.
    const int defKnife = static_cast<int>(GetPrivateProfileIntW(L"OffHand", L"Knife", 0, shipped.c_str()));
    offHandKnife_ = GetPrivateProfileIntW(L"OffHand", L"Knife", defKnife, iniPath_.c_str()) != 0;
    MLOG("menu: off-hand knife %s", offHandKnife_ ? "on" : "off");
    LoadHud();
    MLOG("menu: sticks %s, gun hand %s (at start), red dot %s, frame pacing %s", swapSticks_ ? "swapped (right moves)" : "normal",
         startLeft_ ? "left" : "right", redDot_ ? "on" : "off", pacing_ ? "on" : "off");
    MLOG("menu: gun fit defaults grip %.1f %.1f %.1f, aim line %.1f cm up (gun in hand %d)", fitDefault_.grip[0],
         fitDefault_.grip[1], fitDefault_.grip[2], fitDefault_.rayUp, gunInHand_);
}

void Menu::LoadHolsters(const HolsterSpot (&defaults)[kSpots], const std::string (&commands)[kHolsters]) {
    // What each holster holds: the player's ([Holsters] <Name> = a command, or none), else the shipped one.
    for (int i = 0; i < kHolsters; ++i) {
        commandDefaults_[i] = commands_[i] = commands[i];
        wchar_t b[64] = L"";
        GetPrivateProfileStringW(L"Holsters", Hands::SpotName(i), L"", b, 64, iniPath_.c_str());
        if (b[0]) {
            std::string c;
            for (const wchar_t* p = b; *p; ++p) c += static_cast<char>(*p < 128 ? *p : '?');
            commands_[i] = _stricmp(c.c_str(), "none") ? c : std::string();
        }
        MLOG("menu: holster %ls holds '%s' (%s)", Hands::SpotName(i), commands_[i].empty() ? "nothing" : commands_[i].c_str(),
             b[0] ? "player's" : "default");
    }
    for (int i = 0; i < kSpots; ++i) {
        spotDefaults_[i] = spots_[i] = defaults[i];
        const std::wstring key = std::wstring(Hands::SpotName(i)) + L"Spot";
        wchar_t b[64] = L"";
        GetPrivateProfileStringW(L"Holsters", key.c_str(), L"", b, 64, iniPath_.c_str());
        HolsterSpot cm{};
        if (b[0] && swscanf_s(b, L"%f %f %f %f", &cm.x, &cm.y, &cm.z, &cm.r) == 4)
            spots_[i] = {cm.x / 100.0f, cm.y / 100.0f, cm.z / 100.0f, cm.r / 100.0f};
        MLOG("menu: holster %ls at %.0f %.0f %.0f cm, %.0f cm across (%s)", Hands::SpotName(i), spots_[i].x * 100.0f,
             spots_[i].y * 100.0f, spots_[i].z * 100.0f, spots_[i].r * 200.0f, b[0] ? "player's" : "default");
        // Its ring shown: the player's [Holsters] <Name>Ring, else the shipped one (1).
        const std::wstring rkey = std::wstring(Hands::SpotName(i)) + L"Ring";
        wchar_t exe0[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe0, MAX_PATH);
        std::wstring shipped0(exe0);
        shipped0 = shipped0.substr(0, shipped0.find_last_of(L'\\')) + L"\\MOHAVR.ini";
        spotShownDef_[i] = GetPrivateProfileIntW(L"Holsters", rkey.c_str(), 1, shipped0.c_str()) != 0;
        spotShown_[i] = GetPrivateProfileIntW(L"Holsters", rkey.c_str(), spotShownDef_[i] ? 1 : 0, iniPath_.c_str()) != 0;
        if (!spotShown_[i]) MLOG("menu: holster %ls's ring hidden", Hands::SpotName(i));
    }
    // Rings: the player's, else the shipped default.
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring shipped(exe);
    shipped = shipped.substr(0, shipped.find_last_of(L'\\')) + L"\\MOHAVR.ini";
    // D86 (the player: "all visible reload and holster rings off by default, and able to be enabled separately in the
    // first tab"): [Hands] HolsterRings and ReloadRings, the shipped ones (never) until the player picks -- the older
    // single Rings is no longer read.
    auto ringMode = [&](const wchar_t* key) {
        wchar_t def[16] = L"", v[16] = L"";
        GetPrivateProfileStringW(L"Hands", key, L"never", def, 16, shipped.c_str());
        GetPrivateProfileStringW(L"Hands", key, def, v, 16, iniPath_.c_str());
        int mode = 0;
        for (int m = 0; m < 3; ++m)
            if (!_wcsicmp(v, kRingModes[m])) mode = m;
        return mode;
    };
    ringsMode_ = ringMode(L"HolsterRings");
    reloadRings_ = ringMode(L"ReloadRings");
    MLOG("menu: holster rings %ls, reload rings %ls", kRingModes[ringsMode_], kRingModes[reloadRings_]);
    // Round 31: the hand point, the foregrip ring and the reload rings (the player's, else the shipped [Hands] ones).
    {
        wchar_t d[64] = L"", u[64] = L"";
        GetPrivateProfileStringW(L"Hands", L"HandPoint", L"0 0 0", d, 64, shipped.c_str());
        GetPrivateProfileStringW(L"Hands", L"HandPoint", d, u, 64, iniPath_.c_str());
        float cm[3] = {0, 0, 0}, cmDef[3] = {0, 0, 0};
        swscanf_s(d, L"%f %f %f", &cmDef[0], &cmDef[1], &cmDef[2]);
        swscanf_s(u, L"%f %f %f", &cm[0], &cm[1], &cm[2]);
        for (int i = 0; i < 3; ++i) {
            handPointDef_[i] = cmDef[i] / 100.0f;
            handPoint_[i] = cm[i] / 100.0f;
        }
        auto num = [&](const wchar_t* sec, const wchar_t* key, float def, float& shippedOut) {
            wchar_t a[32] = L"", b[32] = L"";
            GetPrivateProfileStringW(sec, key, L"", a, 32, shipped.c_str());
            shippedOut = a[0] ? static_cast<float>(_wtof(a)) : def;
            GetPrivateProfileStringW(sec, key, L"", b, 32, iniPath_.c_str());
            return b[0] ? static_cast<float>(_wtof(b)) : shippedOut;
        };
        float fr = 12.0f, rs = 100.0f;
        foregripR_ = num(L"Hands", L"ForegripRadius", 12.0f, fr) / 100.0f;
        foregripRDef_ = fr / 100.0f;
        ringScale_ = num(L"Hands", L"ReloadRingScale", 100.0f, rs) / 100.0f;
        ringScaleDef_ = rs / 100.0f;
        MLOG("menu: hand point %.0f %.0f %.0f cm (forward, up, in), foregrip ring %.0f cm, reload rings %.0f%%", handPoint_[0] * 100.0f,
             handPoint_[1] * 100.0f, handPoint_[2] * 100.0f, foregripR_ * 100.0f, ringScale_ * 100.0f);
    }
    // The free hand: the player's, else the shipped [Hands] FreeHand.
    wchar_t fh[64] = L"", fhDef[64] = L"";
    GetPrivateProfileStringW(L"Hands", L"FreeHand", L"0 0 0 0", fhDef, 64, shipped.c_str());
    GetPrivateProfileStringW(L"Hands", L"FreeHand", fhDef, fh, 64, iniPath_.c_str());
    swscanf_s(fhDef, L"%f %f %f %f", &freeHandDef_[0], &freeHandDef_[1], &freeHandDef_[2], &freeHandDef_[3]);
    swscanf_s(fh, L"%f %f %f %f", &freeHand_[0], &freeHand_[1], &freeHand_[2], &freeHand_[3]);
    PublishFreeHand(false);
    // The knife's hold: the player's, else the shipped [OffHand] KnifeGrip / KnifeAdj.
    {
        wchar_t d[64] = L"", u[64] = L"";
        GetPrivateProfileStringW(L"OffHand", L"KnifeGrip", L"forward", d, 64, shipped.c_str());
        knifeIcepickDef_ = !_wcsicmp(d, L"icepick") || !_wcsicmp(d, L"reverse");
        GetPrivateProfileStringW(L"OffHand", L"KnifeGrip", d, u, 64, iniPath_.c_str());
        knifeIcepick_ = !_wcsicmp(u, L"icepick") || !_wcsicmp(u, L"reverse");
        GetPrivateProfileStringW(L"OffHand", L"KnifeAdj", L"0 0 0 0 0 0", d, 64, shipped.c_str());
        GetPrivateProfileStringW(L"OffHand", L"KnifeAdj", d, u, 64, iniPath_.c_str());
        float* a = knifeAdjDef_;
        swscanf_s(d, L"%f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
        a = knifeAdj_;
        swscanf_s(u, L"%f %f %f %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
        PublishKnife(false);
        MLOG("menu: knife grip %s, hold %.1f %.1f %.1f cm, tilt %.0f turn %.0f roll %.0f", knifeIcepick_ ? "icepick" : "forward", knifeAdj_[0],
             knifeAdj_[1], knifeAdj_[2], knifeAdj_[3], knifeAdj_[4], knifeAdj_[5]);
    }
    MLOG("menu: free hand pitch %.0f yaw %.0f roll %.0f, forward %.0f cm", freeHand_[0], freeHand_[1], freeHand_[2], freeHand_[3]);
}

void Menu::SaveHands() {
    if (iniPath_.empty()) return;
    wchar_t b[64];
    swprintf_s(b, L"%.0f %.0f %.0f", handPoint_[0] * 100.0f, handPoint_[1] * 100.0f, handPoint_[2] * 100.0f);
    WritePrivateProfileStringW(L"Hands", L"HandPoint", b, iniPath_.c_str());
    swprintf_s(b, L"%.0f", foregripR_ * 100.0f);
    WritePrivateProfileStringW(L"Hands", L"ForegripRadius", b, iniPath_.c_str());
    swprintf_s(b, L"%.0f", ringScale_ * 100.0f);
    WritePrivateProfileStringW(L"Hands", L"ReloadRingScale", b, iniPath_.c_str());
}

void Menu::PublishFreeHand(bool save) {
    if (hdr_)
        for (int i = 0; i < 4; ++i) hdr_->freeHand[i] = freeHand_[i];
    if (save && !iniPath_.empty()) {
        wchar_t b[64];
        swprintf_s(b, L"%.0f %.0f %.0f %.0f", freeHand_[0], freeHand_[1], freeHand_[2], freeHand_[3]);
        WritePrivateProfileStringW(L"Hands", L"FreeHand", b, iniPath_.c_str());
    }
}

void Menu::SaveHolster(int i) {
    if (iniPath_.empty()) return;
    const std::wstring key = std::wstring(Hands::SpotName(i)) + L"Spot";
    wchar_t b[64];
    swprintf_s(b, L"%.0f %.0f %.0f %.0f", spots_[i].x * 100.0f, spots_[i].y * 100.0f, spots_[i].z * 100.0f, spots_[i].r * 100.0f);
    WritePrivateProfileStringW(L"Holsters", key.c_str(), b, iniPath_.c_str());
}

void Menu::SyncWeapon() {
    if (!hdr_ || hdr_->weaponSeq == seenWeaponSeq_) return;
    seenWeaponSeq_ = hdr_->weaponSeq;
    char key[48];
    std::memcpy(key, hdr_->weaponKey, sizeof(key));
    key[47] = 0;
    weaponKey_ = key;
    fit_ = WeaponDefault();
    bool saved = false;
    if (!weaponKey_.empty()) {
        shared::GunFit f = fit_;
        if (ReadFit(iniPath_, f)) {
            fit_ = f;
            saved = true;
        }
    }
    MLOG("menu: weapon '%s' -- fit %s: grip %.1f %.1f %.1f, angle %.0f, aim line up %.1f right %.1f", weaponKey_.c_str(),
         saved ? "saved" : "default", fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle, fit_.rayUp, fit_.rayRight);
    PublishFit();
    LoadGrips();
    PublishGrips();
    LoadSpots();
}

// [GunFit] <weapon> = gx gy gz angle rayUp rayRight foreFwd foreUp foreRight (6 values: saved before the foregrip existed;
// 8: before its right / left).
bool Menu::ReadFit(const std::wstring& ini, shared::GunFit& f) const {
    if (weaponKey_.empty() || ini.empty()) return false;
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    wchar_t b[128] = L"";
    GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", b, 128, ini.c_str());
    shared::GunFit t = f;
    const int n = b[0] ? swscanf_s(b, L"%f %f %f %f %f %f %f %f %f", &t.grip[0], &t.grip[1], &t.grip[2], &t.angle, &t.rayUp,
                                   &t.rayRight, &t.foreFwd, &t.foreUp, &t.foreRight)
                       : 0;
    if (n != 6 && n != 8 && n != 9) return false;
    if (n < 9) t.foreRight = f.foreRight;
    f = t;
    return true;
}

// The weapon's shipped fit (round 30: the player's fits for the loadout weapons), else the global default.
shared::GunFit Menu::WeaponDefault() const {
    shared::GunFit f = fitDefault_;
    ReadFit(shippedPath_, f);
    return f;
}

shared::GunFit Menu::FitFor(const std::string& key) {
    if (key == weaponKey_ && !key.empty()) return fit_;  // the weapon in hand's, as the Gun fit page has it now
    if (key == fitForKey_) return fitForFit_;
    fitForKey_ = key;
    fitForFit_ = fitDefault_;
    if (key.empty()) return fitForFit_;
    const std::string held = weaponKey_;  // (ReadFit reads weaponKey_'s line)
    weaponKey_ = key;
    shared::GunFit f = fitDefault_;
    ReadFit(shippedPath_, f);
    const bool shipped = !(f.grip[0] == fitDefault_.grip[0] && f.grip[1] == fitDefault_.grip[1] && f.grip[2] == fitDefault_.grip[2]);
    const bool saved = ReadFit(iniPath_, f);
    weaponKey_ = held;
    fitForFit_ = f;
    MLOG("menu: %s's fit (the off-hand pistol) %s: grip %.1f %.1f %.1f, angle %.0f, aim line up %.1f right %.1f", key.c_str(),
         saved ? "saved" : shipped ? "shipped" : "default", f.grip[0], f.grip[1], f.grip[2], f.angle, f.rayUp, f.rayRight);
    return fitForFit_;
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
    fitForKey_ = "\x01";  // FitFor reads it again
    if (weaponKey_.empty() || iniPath_.empty()) return;
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    wchar_t b[128];
    swprintf_s(b, L"%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle,
               fit_.rayUp, fit_.rayRight, fit_.foreFwd, fit_.foreUp, fit_.foreRight);
    WritePrivateProfileStringW(L"GunFit", wkey.c_str(), b, iniPath_.c_str());
}

void Menu::LoadGrips() {
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    // Round 35: <weapon>.holdLikeGrab; round 34's <weapon>.likeHeld (set by a player who meant this) counts until changed.
    gripHoldLikeGrab_ = false;
    if (!weaponKey_.empty() && !iniPath_.empty()) {
        const int old = GetPrivateProfileIntW(L"ReloadGrip", (wkey + L".likeHeld").c_str(), 0, iniPath_.c_str());
        gripHoldLikeGrab_ = GetPrivateProfileIntW(L"ReloadGrip", (wkey + L".holdLikeGrab").c_str(), old, iniPath_.c_str()) != 0;
    }
    for (int g = 0; g < 3; ++g) {
        for (float& v : gripAdj_[g]) v = 0.0f;
        if (weaponKey_.empty() || iniPath_.empty()) continue;
        wchar_t b[128] = L"";
        GetPrivateProfileStringW(L"ReloadGrip", (wkey + L"." + kGripKinds[g]).c_str(), L"", b, 128, iniPath_.c_str());
        if (b[0])
            swscanf_s(b, L"%f %f %f %f %f %f", &gripAdj_[g][0], &gripAdj_[g][1], &gripAdj_[g][2], &gripAdj_[g][3], &gripAdj_[g][4],
                      &gripAdj_[g][5]);
    }
}

void Menu::SaveGrip(int which) {
    if (weaponKey_.empty() || iniPath_.empty()) return;
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    const float* a = gripAdj_[which];
    wchar_t b[128];
    swprintf_s(b, L"%.1f %.1f %.1f %.0f %.0f %.0f", a[0], a[1], a[2], a[3], a[4], a[5]);
    WritePrivateProfileStringW(L"ReloadGrip", (wkey + L"." + kGripKinds[which]).c_str(), b, iniPath_.c_str());
}

void Menu::LoadSpots() {
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    for (int k = 0; k < 2; ++k) {
        spotAdj_[k][0] = spotAdj_[k][1] = spotAdj_[k][2] = 0.0f;
        spotAdj_[k][3] = 100.0f;
        if (weaponKey_.empty() || iniPath_.empty()) continue;
        wchar_t b[96] = L"";
        GetPrivateProfileStringW(L"ReloadSpot", (wkey + L"." + kSpotKinds[k]).c_str(), L"", b, 96, iniPath_.c_str());
        if (b[0]) swscanf_s(b, L"%f %f %f %f", &spotAdj_[k][0], &spotAdj_[k][1], &spotAdj_[k][2], &spotAdj_[k][3]);
    }
}

void Menu::SaveSpot(int which) {
    if (weaponKey_.empty() || iniPath_.empty()) return;
    const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
    const float* a = spotAdj_[which];
    wchar_t b[96];
    swprintf_s(b, L"%.0f %.0f %.0f %.0f", a[0], a[1], a[2], a[3]);
    WritePrivateProfileStringW(L"ReloadSpot", (wkey + L"." + kSpotKinds[which]).c_str(), b, iniPath_.c_str());
}

void Menu::PublishGrips() {
    if (!hdr_) return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr_->gripSeq));  // odd: writing
    const size_t n = weaponKey_.size() < 47 ? weaponKey_.size() : 47;
    std::memcpy(hdr_->gripKey, weaponKey_.c_str(), n);
    hdr_->gripKey[n] = 0;
    for (int g = 0; g < 3; ++g)
        for (int k = 0; k < 6; ++k) hdr_->gripAdj[g][k] = gripAdj_[g][k];
    hdr_->gripFlags = gripHoldLikeGrab_ ? 1u : 0u;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr_->gripSeq));  // even: done
}

// Stick right = the gun forward / right / up, muzzle up, the aim line up / right. (The grip is the gun's point put
// on the controller, so moving the gun forward moves that point back.)
// D81, tests: the item with this key (kItemKeys) selected in its tab, or a tab's row by its name; the menu must be open.
bool Menu::Goto(const std::string& key) {
    if (!visible_) {
        MLOG("menu: test goto '%s' -- the menu isn't open", key.c_str());
        return false;
    }
    for (int t = 0; t < kTabCount; ++t) {
        if (!_stricmp(key.c_str(), kTabNames[t])) {
            page_ = 0;
            tab_ = t;
            selected_ = -1;
            MLOG("menu: test goto -- the %s tab", kTabNames[t]);
            return true;
        }
        for (int i = 0; i < TabCount(t); ++i) {
            const int it = ItemAt(t, i);
            if (it >= 0 && it != kClose && key == kItemKeys[it]) {
                page_ = 0;
                tab_ = t;
                selected_ = i;
                MLOG("menu: test goto '%s' -- the %s tab, row %d", key.c_str(), kTabNames[t], i);
                return true;
            }
        }
    }
    MLOG("menu: test goto '%s' -- no such item", key.c_str());
    return false;
}

void Menu::AdjustFit(int item, float dir) {
    if (weaponKey_.empty()) return;
    switch (item) {
        case fForward: fit_.grip[0] -= dir * kFitStep; break;
        case fRight: fit_.grip[1] -= dir * kFitStep; break;
        case fUp: fit_.grip[2] -= dir * kFitStep; break;
        case fAngle: fit_.angle = std::fmax(-180.0f, std::fmin(180.0f, fit_.angle + dir * kAngleStep)); break;  // round 17: grenades want more than 45
        // (The player, 2026-10-01: the Panzerschreck's aim needed more than 30 cm.)
        case fRayUp: fit_.rayUp = std::fmax(-200.0f, std::fmin(200.0f, fit_.rayUp + dir * (std::fabs(fit_.rayUp) >= 30.0f ? 2.0f : kRayStep))); break;
        case fRayRight: fit_.rayRight = std::fmax(-200.0f, std::fmin(200.0f, fit_.rayRight + dir * (std::fabs(fit_.rayRight) >= 30.0f ? 2.0f : kRayStep))); break;
        case fForeFwd: fit_.foreFwd = std::fmax(0.0f, std::fmin(80.0f, fit_.foreFwd + dir * kFitStep)); break;
        case fForeUp: fit_.foreUp = std::fmax(-30.0f, std::fmin(30.0f, fit_.foreUp + dir * kFitStep)); break;
        case fForeRight: fit_.foreRight = std::fmax(-40.0f, std::fmin(40.0f, fit_.foreRight + dir * kFitStep)); break;
        default: return;
    }
    for (float& g : fit_.grip) g = std::fmax(-200.0f, std::fmin(200.0f, g));
    PublishFit();
    SaveFit();
    MLOG("menu: %s fit -> grip %.1f %.1f %.1f, angle %.0f, aim line up %.1f right %.1f, foregrip %.0f / %.0f / %.0f cm "
         "(forward, up, right)", weaponKey_.c_str(), fit_.grip[0], fit_.grip[1], fit_.grip[2], fit_.angle, fit_.rayUp,
         fit_.rayRight, fit_.foreFwd, fit_.foreUp, fit_.foreRight);
}

// D73: what the headset's runtime recommended last session (the host saves it; %LOCALAPPDATA%\MOHAVR\MOHAVR.headset.ini).
void Menu::AutoEye(int& w, int& h) const {
    wchar_t local[MAX_PATH] = L"";
    const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (ln == 0 || ln >= MAX_PATH) return;
    const std::wstring hs = std::wstring(local) + L"\\MOHAVR\\MOHAVR.headset.ini";
    w = static_cast<int>(GetPrivateProfileIntW(L"Headset", L"EyeWidth", 0, hs.c_str()));
    h = static_cast<int>(GetPrivateProfileIntW(L"Headset", L"EyeHeight", 0, hs.c_str()));
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
    WritePrivateProfileStringW(L"Aim", L"Reticle", redDot_ ? L"1" : L"0", iniPath_.c_str());
    WritePrivateProfileStringW(L"Hands", L"HolsterRings", kRingModes[ringsMode_], iniPath_.c_str());
    WritePrivateProfileStringW(L"Hands", L"ReloadRings", kRingModes[reloadRings_], iniPath_.c_str());
}

void Menu::Update(float dt, const MenuInput& in, const XrPosef& head, bool headValid) {
    SyncWeapon();  // every frame: the fit follows the weapon in hand, open or not
    const bool backFromPage = visible_ && ((page_ == 1 && (in.back || (in.select && selected_ == fBack))) ||
                                           (page_ == 2 && (in.back || (in.select && selected_ == hBack))) ||
                                           (page_ == 3 && (in.back || (in.select && selected_ == eqBack))) ||
                                           (page_ == 4 && (in.back || (in.select && selected_ == gBack))) ||
                                           (page_ == 5 && (in.back || (in.select && selected_ == pBack))) ||
                                           (page_ == 6 && (in.back || (in.select && selected_ == kgBack))) ||
                                           (page_ == 7 && (in.back || (in.select && selected_ == wpBack))) ||
                                           (page_ == 8 && (in.back || (in.select && selected_ == shBack))));
    if (backFromPage) {
        // Back on the item that opened it, in its tab.
        const int opener = page_ == 1 ? kGunFit : page_ == 2 ? kHolsterPage : page_ == 3 ? kFreeHandPage : page_ == 4 ? kGripPage
                         : page_ == 5 ? kSpotPage : page_ == 7 ? kHudWristPage : page_ == 8 ? kHudScreenPage : kKnifePage;
        for (int t = 0; t < kTabCount; ++t)
            for (int i = 0; i < TabCount(t); ++i)
                if (ItemAt(t, i) == opener) {
                    tab_ = t;
                    selected_ = i;
                }
        page_ = 0;
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
            fit_ = WeaponDefault();
            PublishFit();
            const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
            WritePrivateProfileStringW(L"GunFit", wkey.c_str(), nullptr, iniPath_.c_str());
            MLOG("menu: %s fit reset to the defaults", weaponKey_.c_str());
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 5) {
        if (in.up) selected_ = (selected_ + pCount - 1) % pCount;
        if (in.down) selected_ = (selected_ + 1) % pCount;
        if ((in.left || in.right) && selected_ == pWhich) spotSel_ = 1 - spotSel_;
        if ((in.left || in.right) && selected_ >= pFwd && selected_ <= pSize && !weaponKey_.empty()) {
            const float dir = in.right ? 1.0f : -1.0f;
            float& v = spotAdj_[spotSel_][selected_ - pFwd];
            v = selected_ == pSize ? std::fmax(40.0f, std::fmin(250.0f, v + dir * 10.0f)) : std::fmax(-20.0f, std::fmin(20.0f, v + dir));
            SaveSpot(spotSel_);
            const float* a = spotAdj_[spotSel_];
            MLOG("menu: %s %s ring -> %.0f %.0f %.0f cm (forward, up, right), %.0f%%", weaponKey_.c_str(), kSpotNames[spotSel_],
                 a[0], a[1], a[2], a[3]);
        }
        if (in.select && selected_ == pReset && !weaponKey_.empty()) {
            spotAdj_[spotSel_][0] = spotAdj_[spotSel_][1] = spotAdj_[spotSel_][2] = 0.0f;
            spotAdj_[spotSel_][3] = 100.0f;
            const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
            WritePrivateProfileStringW(L"ReloadSpot", (wkey + L"." + kSpotKinds[spotSel_]).c_str(), nullptr, iniPath_.c_str());
            MLOG("menu: %s %s ring reset", weaponKey_.c_str(), kSpotNames[spotSel_]);
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 4) {
        if (in.up) selected_ = (selected_ + gCount - 1) % gCount;
        if (in.down) selected_ = (selected_ + 1) % gCount;
        if ((in.left || in.right) && selected_ == gWhich) gripSel_ = (gripSel_ + (in.right ? 1 : 2)) % 3;
        if ((in.left || in.right) && selected_ == gHoldLikeGrab && !weaponKey_.empty()) {
            gripHoldLikeGrab_ = !gripHoldLikeGrab_;
            const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
            WritePrivateProfileStringW(L"ReloadGrip", (wkey + L".holdLikeGrab").c_str(), gripHoldLikeGrab_ ? L"1" : nullptr,
                                       iniPath_.c_str());
            WritePrivateProfileStringW(L"ReloadGrip", (wkey + L".likeHeld").c_str(), nullptr, iniPath_.c_str());  // round 34's
            PublishGrips();
            MLOG("menu: %s magazine in the hand %s", weaponKey_.c_str(), gripHoldLikeGrab_ ? "held like the grab" : "its own grip");
        }
        if ((in.left || in.right) && selected_ >= gFwd && selected_ <= gRoll && !weaponKey_.empty()) {
            const float dir = in.right ? 1.0f : -1.0f;
            float& v = gripAdj_[gripSel_][selected_ - gFwd];
            v = selected_ >= gTilt ? std::fmax(-90.0f, std::fmin(90.0f, v + dir * 5.0f)) : std::fmax(-15.0f, std::fmin(15.0f, v + dir * 0.5f));
            SaveGrip(gripSel_);
            PublishGrips();
            const float* a = gripAdj_[gripSel_];
            MLOG("menu: %s %s grip -> %.1f %.1f %.1f cm, %.0f %.0f %.0f deg", weaponKey_.c_str(), kGripNames[gripSel_], a[0], a[1], a[2],
                 a[3], a[4], a[5]);
        }
        if (in.select && selected_ == gReset && !weaponKey_.empty()) {
            for (float& v : gripAdj_[gripSel_]) v = 0.0f;
            const std::wstring wkey(weaponKey_.begin(), weaponKey_.end());
            WritePrivateProfileStringW(L"ReloadGrip", (wkey + L"." + kGripKinds[gripSel_]).c_str(), nullptr, iniPath_.c_str());
            PublishGrips();
            MLOG("menu: %s %s grip reset", weaponKey_.c_str(), kGripNames[gripSel_]);
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 7) {
        if (in.up) selected_ = (selected_ + wpCount - 1) % wpCount;
        if (in.down) selected_ = (selected_ + 1) % wpCount;
        if ((in.left || in.right) && selected_ == wpWhich) wristSel_ = (wristSel_ + (in.right ? 1 : 2)) % 3;
        if ((in.left || in.right) && selected_ >= wpAlong && selected_ <= wpTilt) {
            const float dir = in.right ? 1.0f : -1.0f;
            for (int p = 0; p < 2; ++p) {
                if (wristSel_ != 2 && wristSel_ != p) continue;
                float& v = hud_.panel[p][selected_ - wpAlong];
                switch (selected_) {
                    case wpSize: v = std::fmax(40.0f, std::fmin(300.0f, v + dir * 10.0f)); break;
                    case wpTilt: v = std::fmax(-60.0f, std::fmin(80.0f, v + dir * 5.0f)); break;
                    default: v = std::fmax(-20.0f, std::fmin(20.0f, v + dir * 0.5f)); break;
                }
            }
            SaveHud(false, true, false);
            for (int p = 0; p < 2; ++p)
                if (wristSel_ == 2 || wristSel_ == p)
                    MLOG("menu: wrist panel %s -> along %+.1f across %+.1f out %+.1f cm, size %.0f%%, tilt %.0f deg", p ? "right" : "left",
                         hud_.panel[p][0], hud_.panel[p][1], hud_.panel[p][2], hud_.panel[p][3], hud_.panel[p][4]);
        }
        if (in.select && selected_ == wpReset) {
            for (int p = 0; p < 2; ++p) {
                if (wristSel_ != 2 && wristSel_ != p) continue;
                for (int k = 0; k < 5; ++k) hud_.panel[p][k] = hudDef_.panel[p][k];
                if (!iniPath_.empty()) WritePrivateProfileStringW(L"HUD", kWristPanelKeys[p], nullptr, iniPath_.c_str());
            }
            MLOG("menu: wrist panel %s reset", kWristPanelNames[wristSel_]);
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 8) {
        if (in.up) selected_ = (selected_ + shCount - 1) % shCount;
        if (in.down) selected_ = (selected_ + 1) % shCount;
        if ((in.left || in.right) && selected_ <= shDown) {
            const float dir = in.right ? 1.0f : -1.0f;
            float& v = hud_.screen[selected_];
            if (selected_ == shDist) v = std::fmax(0.5f, std::fmin(5.0f, v + dir * 0.1f));
            else if (selected_ == shWidth) v = std::fmax(0.5f, std::fmin(5.0f, v + dir * 0.1f));
            else v = std::fmax(-1.0f, std::fmin(1.0f, v - dir * 0.05f));  // (right = up)
            v = std::round(v * 100.0f) / 100.0f;
            PublishHud();
            static const wchar_t* kScreenKeys[3] = {L"Distance", L"Width", L"Down"};
            SaveHud(false, false, true, kScreenKeys[selected_]);  // (only the key changed)
            MLOG("menu: screen HUD -> %.2f m away, %.2f m wide, %.2f m down", hud_.screen[0], hud_.screen[1], hud_.screen[2]);
        }
        if (in.select && selected_ == shReset) {
            for (int k = 0; k < 3; ++k) hud_.screen[k] = hudDef_.screen[k];
            PublishHud();
            if (!iniPath_.empty())
                for (const wchar_t* k : {L"Distance", L"Width", L"Down"}) WritePrivateProfileStringW(L"HUD", k, nullptr, iniPath_.c_str());
            MLOG("menu: screen HUD reset (%.2f m away, %.2f m wide, %.2f m down)", hud_.screen[0], hud_.screen[1], hud_.screen[2]);
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 6) {
        if (in.up) selected_ = (selected_ + kgCount - 1) % kgCount;
        if (in.down) selected_ = (selected_ + 1) % kgCount;
        if ((in.left || in.right) && selected_ == kgGrip) {
            knifeIcepick_ = !knifeIcepick_;
            PublishKnife(true);
            MLOG("menu: knife grip -> %s", knifeIcepick_ ? "icepick" : "forward");
        } else if ((in.left || in.right) && selected_ >= kgFwd && selected_ <= kgRoll) {
            const float dir = in.right ? 1.0f : -1.0f;
            float& v = knifeAdj_[selected_ - kgFwd];
            v = selected_ <= kgUp ? std::fmax(-20.0f, std::fmin(20.0f, v + dir * 0.5f)) : std::fmax(-180.0f, std::fmin(180.0f, v + dir * 5.0f));
            PublishKnife(true);
            MLOG("menu: knife hold -> %.1f %.1f %.1f cm, tilt %.0f turn %.0f roll %.0f", knifeAdj_[0], knifeAdj_[1], knifeAdj_[2],
                 knifeAdj_[3], knifeAdj_[4], knifeAdj_[5]);
        }
        if (in.select && selected_ == kgReset) {
            for (int i = 0; i < 6; ++i) knifeAdj_[i] = knifeAdjDef_[i];  // the shipped defaults
            knifeIcepick_ = knifeIcepickDef_;
            PublishKnife(false);
            WritePrivateProfileStringW(L"OffHand", L"KnifeAdj", nullptr, iniPath_.c_str());
            WritePrivateProfileStringW(L"OffHand", L"KnifeGrip", nullptr, iniPath_.c_str());
            MLOG("menu: knife hold reset");
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 3) {
        if (in.up) selected_ = (selected_ + eqCount - 1) % eqCount;
        if (in.down) selected_ = (selected_ + 1) % eqCount;
        if ((in.left || in.right) && selected_ <= eqForward) {
            const float dir = in.right ? 1.0f : -1.0f;
            float& v = freeHand_[selected_];
            v = selected_ == eqForward ? std::fmax(-30.0f, std::fmin(30.0f, v + dir)) : std::fmax(-180.0f, std::fmin(180.0f, v + dir * 5.0f));
            PublishFreeHand(true);
            MLOG("menu: free hand -> pitch %.0f yaw %.0f roll %.0f, forward %.0f cm", freeHand_[0], freeHand_[1], freeHand_[2],
                 freeHand_[3]);
        }
        if (in.select && selected_ == eqReset) {
            for (int i = 0; i < 4; ++i) freeHand_[i] = freeHandDef_[i];  // the shipped defaults
            PublishFreeHand(false);
            WritePrivateProfileStringW(L"Hands", L"FreeHand", nullptr, iniPath_.c_str());
            MLOG("menu: free hand reset");
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }
    if (page_ == 2) {
        if (in.up) selected_ = (selected_ + hCount - 1) % hCount;
        if (in.down) selected_ = (selected_ + 1) % hCount;
        if (in.left || in.right) {
            const float dir = in.right ? 1.0f : -1.0f;
            HolsterSpot& s = spots_[holsterSel_];
            bool moved = true;
            switch (selected_) {
                case hWhich: holsterSel_ = (holsterSel_ + (in.right ? 1 : kSpots - 1)) % kSpots; moved = false; break;
                case hHolds:
                    moved = false;
                    if (holsterSel_ < kHolsters) {
                        const int at = HoldIndex(commands_[holsterSel_]);
                        const int next = at < 0 ? (in.right ? 0 : kHoldCount - 1) : (at + (in.right ? 1 : kHoldCount - 1)) % kHoldCount;
                        commands_[holsterSel_] = kHoldCommands[next];
                        const std::wstring w(commands_[holsterSel_].begin(), commands_[holsterSel_].end());
                        if (!iniPath_.empty())
                            WritePrivateProfileStringW(L"Holsters", Hands::SpotName(holsterSel_), w.empty() ? L"none" : w.c_str(),
                                                       iniPath_.c_str());
                        MLOG("menu: holster %ls -> holds %s", Hands::SpotName(holsterSel_), kHoldNames[next]);
                    }
                    break;
                case hRight: s.x = std::fmax(-0.8f, std::fmin(0.8f, s.x + dir * 0.01f)); break;
                case hUp: s.y = std::fmax(-1.2f, std::fmin(0.4f, s.y + dir * 0.01f)); break;
                case hForward: s.z = std::fmax(-0.6f, std::fmin(0.6f, s.z + dir * 0.01f)); break;
                case hSize: s.r = std::fmax(0.05f, std::fmin(0.40f, s.r + dir * 0.005f)); break;
                case hShown: {
                    moved = false;
                    spotShown_[holsterSel_] = !spotShown_[holsterSel_];
                    const std::wstring key = std::wstring(Hands::SpotName(holsterSel_)) + L"Ring";
                    if (!iniPath_.empty())
                        WritePrivateProfileStringW(L"Holsters", key.c_str(), spotShown_[holsterSel_] ? L"1" : L"0", iniPath_.c_str());
                    MLOG("menu: holster %ls's ring -> %s", Hands::SpotName(holsterSel_), spotShown_[holsterSel_] ? "shown" : "hidden");
                    break;
                }
                case hRings:
                    ringsMode_ = (ringsMode_ + (in.right ? 1 : 2)) % 3;
                    Save();
                    moved = false;
                    MLOG("menu: holster rings -> %ls", kRingModes[ringsMode_]);
                    break;
                default: moved = false; break;
            }
            if (moved) {
                SaveHolster(holsterSel_);
                MLOG("menu: holster %ls -> %.0f %.0f %.0f cm, %.0f cm across", Hands::SpotName(holsterSel_), s.x * 100.0f,
                     s.y * 100.0f, s.z * 100.0f, s.r * 200.0f);
            }
        }
        if (in.select && selected_ == hReset) {
            spots_[holsterSel_] = spotDefaults_[holsterSel_];
            const std::wstring key = std::wstring(Hands::SpotName(holsterSel_)) + L"Spot";
            WritePrivateProfileStringW(L"Holsters", key.c_str(), nullptr, iniPath_.c_str());
            spotShown_[holsterSel_] = spotShownDef_[holsterSel_];
            const std::wstring rkey = std::wstring(Hands::SpotName(holsterSel_)) + L"Ring";
            WritePrivateProfileStringW(L"Holsters", rkey.c_str(), nullptr, iniPath_.c_str());
            if (holsterSel_ < kHolsters) {  // and what it holds
                commands_[holsterSel_] = commandDefaults_[holsterSel_];
                WritePrivateProfileStringW(L"Holsters", Hands::SpotName(holsterSel_), nullptr, iniPath_.c_str());
            }
            MLOG("menu: holster %ls reset", Hands::SpotName(holsterSel_));
        }
        ImGui::GetIO().DeltaTime = dt > 0.0f ? dt : 1.0f / 90.0f;
        Render();
        return;
    }

    // The main page, in tabs: up / down through the tab row (-1) and the tab's items; left / right switch tabs on the row
    // and adjust an item.
    const int count = TabCount(tab_);
    if (selected_ >= count) selected_ = count - 1;
    if (in.up) selected_ = selected_ < 0 ? count - 1 : selected_ - 1;
    if (in.down) selected_ = selected_ + 1 >= count ? -1 : selected_ + 1;
    const int item = selected_ >= 0 ? ItemAt(tab_, selected_) : -1;
    if ((in.left || in.right) && selected_ < 0) {
        tab_ = (tab_ + (in.right ? 1 : kTabCount - 1)) % kTabCount;
        MLOG("menu: tab %s", kTabNames[tab_]);
    } else if (in.left || in.right) {
        const float dir = in.right ? 1.0f : -1.0f;
        if (item == kWorldScale) {
            SetUnitsPerMeter(unitsPerMeter_ + dir * kScaleStep, true);
            MLOG("menu: world scale -> %.1f", unitsPerMeter_);
        } else if (item == kHeight) {
            SetHeightOffset(heightOffset_ + dir * kHeightStep, true);
            MLOG("menu: height offset -> %+.2f m", heightOffset_);
        } else if (item == kTurn) {
            int i = 0;
            while (i < 2 && kSnapSteps[i] != snapDeg_) ++i;
            i = (i + (in.right ? 1 : 2)) % 3;
            snapDeg_ = kSnapSteps[i];
            Save();
            MLOG("menu: turning -> %s %d", snapDeg_ ? "snap" : "smooth", snapDeg_);
        } else if (item == kSticks) {
            swapSticks_ = !swapSticks_;
            Save();
            MLOG("menu: sticks -> %s", swapSticks_ ? "swapped (right moves, left turns)" : "normal (left moves, right turns)");
        } else if (item == kMove) {
            moveByHead_ = !moveByHead_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"Controls", L"MoveDirection", moveByHead_ ? L"head" : L"body", iniPath_.c_str());
            MLOG("menu: move direction -> %s", moveByHead_ ? "head (forward is where you look)" : "body (the game's own)");
        } else if (item == kGunHand) {
            startLeft_ = !startLeft_;
            Save();
            MLOG("menu: gun hand -> %s", startLeft_ ? "left" : "right");
        } else if (item == kRedDot) {
            redDot_ = !redDot_;
            Save();
            MLOG("menu: red dot -> %s", redDot_ ? "on" : "off");
        } else if (item == kPacing) {
            pacing_ = !pacing_;
            if (hdr_) hdr_->pace = pacing_ ? 1u : 0u;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Bridge", L"Pace", pacing_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: frame pacing -> %s", pacing_ ? "on (one game frame per headset frame)" : "off (the game runs uncapped)");
        } else if (item == kResolution) {
            resPreset_ = (resPreset_ + (in.right ? 1 : presets::kPresetCount - 1)) % presets::kPresetCount;
            if (!iniPath_.empty()) {
                const char* k = presets::kPresets[resPreset_].key;
                wchar_t w[32] = L"";
                for (int i = 0; i < 31 && k[i]; ++i) w[i] = static_cast<wchar_t>(k[i]);
                WritePrivateProfileStringW(L"Render", L"Preset", w, iniPath_.c_str());
            }
            MLOG("menu: resolution -> %s (at the next start)", presets::kPresets[resPreset_].label);
        } else if (item == kHolsterRings || item == kReloadRings) {
            int& m = item == kHolsterRings ? ringsMode_ : reloadRings_;
            m = (m + (in.right ? 1 : 2)) % 3;
            Save();
            MLOG("menu: %s rings -> %ls", item == kHolsterRings ? "holster" : "reload", kRingModes[m]);
        } else if (item == kPouchMag) {
            pouchMag_ = !pouchMag_;
            if (hdr_) hdr_->pouchMagMode = pouchMag_ ? 2u : 1u;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"ManualReload", L"PouchMag", pouchMag_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: the spare magazine in the pouch -> %s", pouchMag_ ? "shown" : "hidden");
        } else if (item == kRoomScale) {
            roomScale_ = !roomScale_;
            if (hdr_) hdr_->roomScale = roomScale_ ? 2u : 1u;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Comfort", L"RoomScale", roomScale_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: room-scale walking -> %s", roomScale_ ? "on" : "off");
        } else if (item == kFireShake) {
            fireShake_ = !fireShake_;
            if (hdr_) hdr_->fireShake = fireShake_ ? 2u : 1u;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Camera", L"FireShake", fireShake_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: firing shake -> %s", fireShake_ ? "on" : "off");
        } else if (item == kDamageTint) {
            damageTint_ = !damageTint_;
            if (hdr_) hdr_->damageTint = damageTint_ ? 2u : 1u;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Camera", L"DamageTint", damageTint_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: damage flash -> %s", damageTint_ ? "on" : "off");
        } else if (item == kRecoil) {
            if (in.left) kickPct_ = std::max(0, kickPct_ - 50);
            else if (in.right) kickPct_ = std::min(200, kickPct_ + 50);
            else kickPct_ = kickPct_ >= 200 ? 0 : kickPct_ + 50;  // a select steps up, and from 200 % wraps to off
            if (!iniPath_.empty()) {
                wchar_t v[16];
                swprintf_s(v, L"%.1f", kickPct_ / 100.0);
                WritePrivateProfileStringW(L"Weapon", L"Kick", v, iniPath_.c_str());
            }
            PublishWeaponModes();
            MLOG("menu: recoil -> %d%% of the game's kick", kickPct_);
        } else if (item == kGrabPickup) {
            grabPickup_ = !grabPickup_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Controls", L"GrabPickup", grabPickup_ ? L"1" : L"0", iniPath_.c_str());
            PublishWeaponModes();
            MLOG("menu: grab pickup -> %s", grabPickup_ ? "on (close a free grip on a weapon to take it)" : "off");
        } else if (item == kMgHands) {
            mgHands_ = !mgHands_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Weapon", L"MountedHands", mgHands_ ? L"1" : L"0", iniPath_.c_str());
            PublishWeaponModes();
            MLOG("menu: mounted MG42 -> %s", mgHands_ ? "the gun hand aims it" : "the head aims it (the game's way)");
        } else if (item == kChute) {
            chuteHands_ = !chuteHands_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Controls", L"ChuteHands", chuteHands_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: parachute -> %s", chuteHands_ ? "hands (hold both grips: the risers)" : "stick");
        } else if (item == kSeated) {
            seated_ = !seated_;
            if (hdr_) hdr_->crouchMode = CrouchWord();
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Comfort", L"Seated", seated_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: seated -> %s", seated_ ? "on (a shallower crouch line; recentre seated)" : "off");
        } else if (item == kVignette) {
            vignette_ = (vignette_ + (in.right ? 1 : 2)) % 3;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Comfort", L"Vignette", std::to_wstring(vignette_).c_str(), iniPath_.c_str());
            MLOG("menu: vignette -> %s", vignette_ == 0 ? "none" : vignette_ == 1 ? "light" : "strong");
        } else if (item == kCrouch) {
            crouch_ = !crouch_;
            if (hdr_) hdr_->crouchMode = CrouchWord();
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Controls", L"PhysicalCrouch", crouch_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: physical crouch -> %s", crouch_ ? "on (the game crouches when you do)" : "off (the stick crouches)");
        } else if (item == kReload) {
            manualReload_ = !manualReload_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"Weapon", L"ManualReload", manualReload_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: manual reload -> %s", manualReload_ ? "on" : "off (the game's own reload)");
        } else if (item == kRackEject) {
            rackEject_ = !rackEject_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"ManualReload", L"RackEject", rackEject_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: rack eject -> %s", rackEject_ ? "on (a full stroke of a loaded action throws its round out)" : "off");
        } else if (item == kRackKeep) {
            rackEjectKeep_ = !rackEjectKeep_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"ManualReload", L"RackEjectKeep", rackEjectKeep_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: an ejected round -> %s", rackEjectKeep_ ? "kept (back to the reserve)" : "lost (spent)");
        } else if (item == kOffNade) {
            offHandNade_ = !offHandNade_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"OffHand", L"Grenade", offHandNade_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: off-hand grenade -> %s", offHandNade_ ? "on" : "off (the grenade holster draws the game's grenade)");
        } else if (item == kPouchReload) {
            pouchReload_ = !pouchReload_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Hands", L"PouchReload", pouchReload_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: pouch reload -> %s", pouchReload_ ? "on (a hand with a gun grips the pouch: reloaded at once)" : "off");
        } else if (item == kMelee) {
            physicalMelee_ = !physicalMelee_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Melee", L"Physical", physicalMelee_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: physical melee -> %s", physicalMelee_ ? "on (a swing of the gun's butt strikes)" : "off (the right stick's melee only)");
        } else if (item == kScope) {
            scope_ = !scope_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Scope", L"Enable", scope_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: scopes -> %s", scope_ ? "on (raise the scope to your eye, two hands on the gun)" : "off");
        } else if (item == kScopeZoom) {
            scopeZoomGame_ = !scopeZoomGame_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Scope", L"Zoom", scopeZoomGame_ ? L"game" : L"real", iniPath_.c_str());
            MLOG("menu: scope zoom -> %s", scopeZoomGame_ ? "the game's (the turning stick up / down zooms)" : "realistic");
        } else if (item == kGunNade) {
            gunNadePin_ = !gunNadePin_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"Weapon", L"GrenadePin", gunNadePin_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: the gun hand's grenade -> %s", gunNadePin_ ? "pin, cook and grip" : "the game's own");
        } else if (item == kNadeStyle) {
            nadeSimple_ = !nadeSimple_;
            g_nadeSimple = nadeSimple_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"OffHand", L"GrenadeStyle", nadeSimple_ ? L"simple" : L"classic", iniPath_.c_str());
            MLOG("menu: grenades -> %s", nadeSimple_ ? "simple" : "classic");
        } else if (item == kNadeHold) {
            nadeClick_ = !nadeClick_;
            if (!iniPath_.empty()) WritePrivateProfileStringW(L"OffHand", L"GrenadeHold", nadeClick_ ? L"click" : L"grip", iniPath_.c_str());
            MLOG("menu: grenade hold -> %s", nadeClick_ ? "click" : "grip");
        } else if (item == kOffPistol) {
            offHandPistol_ = !offHandPistol_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"OffHand", L"Pistol", offHandPistol_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: off-hand pistol -> %s", offHandPistol_ ? "on" : "off (the pistol holster draws the game's pistol)");
        } else if (item == kOffKnife) {
            offHandKnife_ = !offHandKnife_;
            if (!iniPath_.empty())
                WritePrivateProfileStringW(L"OffHand", L"Knife", offHandKnife_ ? L"1" : L"0", iniPath_.c_str());
            MLOG("menu: off-hand knife -> %s", offHandKnife_ ? "on" : "off");
        } else if (item == kHudPlace) {
            hud_.place = 1 - hud_.place;
            PublishHud();
            SaveHud(true, false, false, L"Place");
            MLOG("menu: HUD -> %s", hud_.place ? "the wrist (health and compass left, weapon and grenades right)" : "the screen");
        } else if (item == kHudShow) {
            hud_.show = 1 - hud_.show;
            SaveHud(true, false, false, L"WristShow");
            MLOG("menu: wrist HUD shows -> %s", hud_.show ? "always" : "when looked at");
        } else if (item == kHudLayout) {
            hud_.layout = 1 - hud_.layout;
            SaveHud(true, false, false, L"WristLayout");
            MLOG("menu: wrist HUD layout -> %s", hud_.layout ? "across (the arm pointing forward)" : "forearm (the forearm across the chest)");
        } else if (item == kHudBacking) {
            hud_.backing = (hud_.backing + (in.right ? 1 : 2)) % 3;
            SaveHud(true, false, false, L"WristBacking");
            MLOG("menu: wrist HUD backing -> %ls", kBackings[hud_.backing]);
        } else if (item == kHandFwd || item == kHandUp || item == kHandIn) {
            float& v = handPoint_[item - kHandFwd];
            v = std::fmax(-0.15f, std::fmin(0.15f, v + dir * 0.01f));
            SaveHands();
            MLOG("menu: hand point -> %.0f %.0f %.0f cm (forward, up, in)", handPoint_[0] * 100.0f, handPoint_[1] * 100.0f,
                 handPoint_[2] * 100.0f);
        } else if (item == kForeSize) {
            foregripR_ = std::fmax(0.04f, std::fmin(0.25f, foregripR_ + dir * 0.01f));
            SaveHands();
            MLOG("menu: foregrip ring -> %.0f cm", foregripR_ * 100.0f);
        } else if (item == kRingScale) {
            ringScale_ = std::fmax(0.3f, std::fmin(2.0f, ringScale_ + dir * 0.1f));
            SaveHands();
            MLOG("menu: reload rings -> %.0f%%", ringScale_ * 100.0f);
        }
    }
    if (in.select) {
        if (item == kGunFit) {
            page_ = 1;
            selected_ = 0;
            MLOG("menu: gun fit page (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
        } else if (item == kHolsterPage) {
            page_ = 2;
            selected_ = 0;
            MLOG("menu: holsters page");
        } else if (item == kFreeHandPage) {
            page_ = 3;
            selected_ = 0;
            MLOG("menu: free hand page");
        } else if (item == kHudWristPage) {
            page_ = 7;
            selected_ = 0;
            MLOG("menu: wrist panels page (the panels show while it is open)");
        } else if (item == kHudScreenPage) {
            page_ = 8;
            selected_ = 0;
            MLOG("menu: screen HUD page");
        } else if (item == kKnifePage) {
            page_ = 6;
            selected_ = 0;
            MLOG("menu: knife grip page");
        } else if (item == kGripPage) {
            page_ = 4;
            selected_ = 0;
            MLOG("menu: reload grip page (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
        } else if (item == kSpotPage) {
            page_ = 5;
            selected_ = 0;
            MLOG("menu: reload spots page (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
        } else if (item == kGiveAll) {
            giveAllRequested_ = true;  // the host sends the game "mohavr giveall"; the menu closes so the guns can be seen
            MLOG("menu: give all weapons requested");
            Close();
            return;
        } else if (item == kRecenter) {
            recenterRequested_ = true;  // the host re-creates LOCAL at the current head pose, then closes us
            MLOG("menu: recentre requested");
        } else if (item == kResetScale) {
            const float def = (hdr_ && hdr_->defaultUnitsPerMeter > 1.0f) ? hdr_->defaultUnitsPerMeter : 100.0f;
            SetUnitsPerMeter(def, true);
            MLOG("menu: world scale reset to %.1f", def);
        } else if (item == kClose) {
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
    } else if (page_ == 2) {
        RenderHolsterPage();
    } else if (page_ == 3) {
        RenderFreeHandPage();
    } else if (page_ == 4) {
        RenderGripPage();
    } else if (page_ == 5) {
        RenderSpotPage();
    } else if (page_ == 6) {
        RenderKnifePage();
    } else if (page_ == 7) {
        RenderWristPage();
    } else if (page_ == 8) {
        RenderScreenHudPage();
    } else {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "MOHAVR");
    // The tab row: the current tab lit; framed while the row itself is selected (left / right switch). D81: its own line,
    // smaller, for six tabs.
    ImGui::PushFont(nullptr, 31.0f);
    for (int t = 0; t < kTabCount; ++t) {
        if (t > 0) ImGui::SameLine(0.0f, 10.0f);
        const bool cur = t == tab_;
        const ImVec4 col = cur ? (selected_ < 0 ? ImVec4(0.95f, 0.8f, 0.45f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f))
                               : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        char tl[48];
        snprintf(tl, sizeof(tl), cur && selected_ < 0 ? "<%s>" : cur ? "[%s]" : " %s ", kTabNames[t]);
        ImGui::TextColored(col, "%s", tl);
    }
    ImGui::PopFont();
    ImGui::Separator();

    const float ipdMm = hdr_ ? 1000.0f * std::sqrt(std::pow(hdr_->eye[1].px - hdr_->eye[0].px, 2.0f) +
                                                   std::pow(hdr_->eye[1].py - hdr_->eye[0].py, 2.0f) +
                                                   std::pow(hdr_->eye[1].pz - hdr_->eye[0].pz, 2.0f))
                              : 64.0f;
    char label[128];
    auto note = [&](const char* text) {
        ImGui::PushFont(nullptr, 28.0f);
        ImGui::TextDisabled("   %s", text);
        ImGui::PopFont();
    };
    for (int i = 0; i < TabCount(tab_); ++i) {
        const int it = ItemAt(tab_, i);
        const bool sel = selected_ == i;
        switch (it) {
            case kWorldScale:
                snprintf(label, sizeof(label), "World scale      <  %.0f  >", unitsPerMeter_);
                ImGui::Selectable(label, sel);
                snprintf(label, sizeof(label), "higher = smaller world   (eyes %.1f units apart)", ipdMm * unitsPerMeter_ / 1000.0f);
                note(label);
                break;
            case kHeight:
                snprintf(label, sizeof(label), "Height           <  %+.0f cm  >", heightOffset_ * 100.0f);
                ImGui::Selectable(label, sel);
                break;
            case kTurn:
                if (snapDeg_) snprintf(label, sizeof(label), "Turning          <  snap %d\xC2\xB0  >", snapDeg_);
                else snprintf(label, sizeof(label), "Turning          <  smooth  >");
                ImGui::Selectable(label, sel);
                break;
            case kSticks:
                snprintf(label, sizeof(label), "Sticks           <  %s  >", swapSticks_ ? "move right, turn left" : "move left, turn right");
                ImGui::Selectable(label, sel);
                break;
            case kMove:
                snprintf(label, sizeof(label), "Move direction   <  %s  >", moveByHead_ ? "where you look" : "body");
                ImGui::Selectable(label, sel);
                break;
            case kGunHand:
                snprintf(label, sizeof(label), "Gun hand         <  %s  >", startLeft_ ? "left" : "right");
                ImGui::Selectable(label, sel);
                break;
            case kRedDot:
                snprintf(label, sizeof(label), "Red dot          <  %s  >", redDot_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                break;
            case kPacing:
                snprintf(label, sizeof(label), "Frame pacing     <  %s  >", pacing_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("on = one game frame per headset frame");
                break;
            case kRecenter: ImGui::Selectable("Recentre (face forward, here)", sel); break;
            case kResolution: {
                const presets::Preset& p = presets::kPresets[resPreset_];
                int w = p.eyeW, h = p.eyeH;
                if (!std::strcmp(p.key, "custom")) w = shippedResX_ / 2, h = shippedResY_;
                if (!std::strcmp(p.key, "auto")) AutoEye(w, h);
                char size[32] = "headset not seen yet";
                if (w > 0 && h > 0) snprintf(size, sizeof(size), "%dx%d per eye", w, h);
                snprintf(label, sizeof(label), "Resolution       <  %s  >", p.label);
                ImGui::Selectable(label, sel);
                char n2[160];
                snprintf(n2, sizeof(n2), "%s; %s", size,
                         resPreset_ == startPreset_ ? (hdr_ && hdr_->width ? "as now" : "") : "applies at the next start");
                note(n2);
                break;
            }
            case kHolsterRings:
                snprintf(label, sizeof(label), "Holster rings    <  %s  >", kRingLabels[ringsMode_]);
                ImGui::Selectable(label, sel);
                note("rings at the holsters and the belt pouch (near = when a hand comes close)");
                break;
            case kReloadRings:
                snprintf(label, sizeof(label), "Reload rings     <  %s  >", kRingLabels[reloadRings_]);
                ImGui::Selectable(label, sel);
                note("rings at the gun's magazine, bolt, pump and foregrip");
                break;
            case kPouchMag:
                snprintf(label, sizeof(label), "Magazine in pouch <  %s  >", pouchMag_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("with your gun's magazine out, a fresh one shows in the belt pouch");
                break;
            case kRoomScale:
                snprintf(label, sizeof(label), "Room-scale walk  <  %s  >", roomScale_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("walk around your room and your soldier walks with you (walls stop him)");
                break;
            case kFireShake:
                snprintf(label, sizeof(label), "Firing shake     <  %s  >", fireShake_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("the view jolting with each shot (off: steady; the gun still kicks in your hand)");
                break;
            case kDamageTint:
                snprintf(label, sizeof(label), "Damage flash     <  %s  >", damageTint_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("the screen goes red when you're hit (and the game's other screen tints)");
                break;
            case kRecoil:
                if (kickPct_ == 0) snprintf(label, sizeof(label), "Recoil           <  off  >");
                else snprintf(label, sizeof(label), "Recoil           <  %d%%  >", kickPct_);
                ImGui::Selectable(label, sel);
                note("the muzzle rises in your hand per shot (100% = the game's own kick)");
                break;
            case kGrabPickup:
                snprintf(label, sizeof(label), "Grab pickup      <  %s  >", grabPickup_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("reach for a weapon or a crate and close a free grip on it to take it");
                break;
            case kMgHands:
                snprintf(label, sizeof(label), "Mounted MG42     <  %s  >", mgHands_ ? "hands" : "head");
                ImGui::Selectable(label, sel);
                note("hands: grip the handle and push it (B gets off); head: it follows your view");
                break;
            case kChute:
                snprintf(label, sizeof(label), "Parachute        <  %s  >", chuteHands_ ? "hands" : "stick");
                ImGui::Selectable(label, sel);
                note("hands: hold both grips, pull one down to turn, both to slow, a hard pull flares");
                break;
            case kSeated:
                snprintf(label, sizeof(label), "Seated           <  %s  >", seated_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("sit, then Recentre: your seated head becomes standing height");
                break;
            case kVignette:
                snprintf(label, sizeof(label), "Vignette         <  %s  >", vignette_ == 0 ? "none" : vignette_ == 1 ? "light" : "strong");
                ImGui::Selectable(label, sel);
                note("darkens the edges while the stick moves or turns you");
                break;
            case kCrouch:
                snprintf(label, sizeof(label), "Physical crouch  <  %s  >", crouch_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("on = crouch for real and the game crouches (recentre standing)");
                break;
            case kGiveAll:
                ImGui::Selectable("Give all weapons", sel);
                note("the game's cheat: every gun, full ammo (switch with next weapon)");
                break;
            case kResetScale:
                snprintf(label, sizeof(label), "Reset world scale (%.0f)", hdr_ ? hdr_->defaultUnitsPerMeter : 100.0f);
                ImGui::Selectable(label, sel);
                break;
            case kGunFit:
                snprintf(label, sizeof(label), "Gun fit  (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
                ImGui::Selectable(label, sel);
                break;
            case kReload:
                snprintf(label, sizeof(label), "Manual reload    <  %s  >", manualReload_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                break;
            case kRackEject:
                snprintf(label, sizeof(label), "Rack ejects a round <  %s  >", rackEject_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("racking a loaded slide, bolt or pump throws the chambered round out");
                break;
            case kRackKeep:
                snprintf(label, sizeof(label), "  Ejected round   <  %s  >", rackEjectKeep_ ? "kept" : "lost");
                ImGui::Selectable(label, sel);
                note(rackEjectKeep_ ? "kept: it goes back to your reserve" : "lost: it counts as a round spent");
                break;
            case kNadeStyle:
                snprintf(label, sizeof(label), "Grenades         <  %s  >", nadeSimple_ ? "simple" : "classic");
                ImGui::Selectable(label, sel);
                note(nadeSimple_ ? "hold the grip to take one, trigger once to cook, let go to throw"
                                 : "trigger pulls the pin, a 2nd pull cooks; hold as set below");
                break;
            case kOffNade:
                snprintf(label, sizeof(label), "Off-hand grenade <  %s  >", offHandNade_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("the free hand takes a grenade at the grenade holster: the gun stays in hand");
                break;
            case kPouchReload:
                snprintf(label, sizeof(label), "Pouch reload     <  %s  >", pouchReload_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("a hand holding a gun grips the ammo pouch on your belt: reloaded at once");
                break;
            case kMelee:
                snprintf(label, sizeof(label), "Physical melee   <  %s  >", physicalMelee_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("swing the butt of your gun into an enemy (a bayonet: thrust or slash): the game's melee");
                break;
            case kScope:
                snprintf(label, sizeof(label), "Scopes           <  %s  >", scope_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("raise a scoped gun to your eye, both hands on it: you look through the scope");
                break;
            case kScopeZoom:
                snprintf(label, sizeof(label), "Scope zoom       <  %s  >", scopeZoomGame_ ? "game" : "realistic");
                ImGui::Selectable(label, sel);
                note(scopeZoomGame_ ? "the game's zoom: the turning stick up / down zooms while you look through"
                                    : "each scope's real magnification (Springfield 2.5x, G43 and StG44 4x, M18 2.8x)");
                break;
            case kGunNade:
                snprintf(label, sizeof(label), "Hand grenades    <  %s  >", gunNadePin_ ? "pin & grip" : "game");
                ImGui::Selectable(label, sel);
                note("in the gun hand: trigger pulls the pin, again cooks; squeeze, swing and let go to throw");
                break;
            case kNadeHold:
                snprintf(label, sizeof(label), "  Grenade hold   <  %s  >", nadeClick_ ? "click" : "grip");
                ImGui::Selectable(label, sel);
                note("grip: hold the grip, let go to throw.  click: click to take, squeeze and let go to throw");
                break;
            case kOffPistol:
                snprintf(label, sizeof(label), "Off-hand pistol  <  %s  >", offHandPistol_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("the free hand draws the pistol at the pistol holster: the gun stays in hand");
                break;
            case kOffKnife:
                snprintf(label, sizeof(label), "Off-hand knife   <  %s  >", offHandKnife_ ? "on" : "off");
                ImGui::Selectable(label, sel);
                note("the MP40's dagger: the free hand draws it at the lower back; stab or slash");
                break;
            case kKnifePage:
                ImGui::Selectable("Knife grip  (how the knife sits in your hand)", sel);
                break;
            case kGripPage:
                snprintf(label, sizeof(label), "Reload grip  (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
                ImGui::Selectable(label, sel);
                note("where your hand sits on the magazine / handle");
                break;
            case kSpotPage:
                snprintf(label, sizeof(label), "Reload spots  (%s)", weaponKey_.empty() ? "no gun in hand" : weaponKey_.c_str());
                ImGui::Selectable(label, sel);
                note("where you grab the magazine / handle: move and size the rings");
                break;
            case kHolsterPage:
                snprintf(label, sizeof(label), "Holsters and pouch  (rings: %s)", kRingLabels[ringsMode_]);
                ImGui::Selectable(label, sel);
                break;
            case kHandFwd:
                snprintf(label, sizeof(label), "Hand point fwd/back   <  %+.0f  >", handPoint_[0] * 100.0f);
                ImGui::Selectable(label, sel);
                break;
            case kHandUp:
                snprintf(label, sizeof(label), "Hand point up/down    <  %+.0f  >", handPoint_[1] * 100.0f);
                ImGui::Selectable(label, sel);
                break;
            case kHandIn:
                snprintf(label, sizeof(label), "Hand point in/out     <  %+.0f  >", handPoint_[2] * 100.0f);
                ImGui::Selectable(label, sel);
                note("the white dot: put it in your hand (in = toward the palm)");
                break;
            case kForeSize:
                snprintf(label, sizeof(label), "Foregrip ring         <  %.0f across  >", foregripR_ * 200.0f);
                ImGui::Selectable(label, sel);
                break;
            case kRingScale:
                snprintf(label, sizeof(label), "Reload rings          <  %.0f%%  >", ringScale_ * 100.0f);
                ImGui::Selectable(label, sel);
                break;
            case kFreeHandPage: ImGui::Selectable("Free hand  (how your other hand sits)", sel); break;
            case kHudPlace: {
                const bool can = wristAvail_;  // (the game has the HUD ring and the host opened it)
                snprintf(label, sizeof(label), "HUD              <  %s  >", hud_.place ? (can ? "wrist" : "wrist (not available)") : "screen");
                ImGui::Selectable(label, sel);
                note(hud_.place ? "your other wrist, palm down: health and the compass (the minimap) left, weapon and grenades right"
                                : "one panel in front of you");
                break;
            }
            case kHudShow:
                snprintf(label, sizeof(label), "  Wrist shows    <  %s  >", hud_.show ? "always" : "when looked at");
                ImGui::Selectable(label, sel);
                note("when looked at: turn the wrist toward you and look at it");
                break;
            case kHudLayout:
                snprintf(label, sizeof(label), "  Wrist layout   <  %s  >", hud_.layout ? "arm forward" : "forearm across chest");
                ImGui::Selectable(label, sel);
                note(hud_.layout ? "the arm pointing forward: the panels either side of the wrist"
                                 : startLeft_ ? "the forearm across the chest: health on your left (toward the hand), weapon toward the elbow"
                                              : "the forearm across the chest: health on your left (toward the elbow), weapon toward the hand");
                break;
            case kHudBacking:
                snprintf(label, sizeof(label), "  Wrist backing  <  %ls  >", kBackings[hud_.backing]);
                ImGui::Selectable(label, sel);
                break;
            case kHudWristPage: ImGui::Selectable("Wrist panels  (move and size them)", sel); break;
            case kHudScreenPage:
                snprintf(label, sizeof(label), "Screen HUD  (%.1f m away, %.1f m wide)", hud_.screen[0], hud_.screen[1]);
                ImGui::Selectable(label, sel);
                note("the panel in front of you; on the wrist, what stays in view (hits, objectives, prompts)");
                break;
            case kClose: ImGui::Selectable("Close", sel); break;
            default: break;
        }
        if (sel) ImGui::SetScrollHereY(0.5f);  // (a tab longer than the panel scrolls with the selection)
    }
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Stick: choose / adjust (up: the tabs)   Trigger: select   Menu: close");
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
    snprintf(label, sizeof(label), "Foregrip right / left <  %+.0f cm  >", fit_.foreRight);
    ImGui::Selectable(label, selected_ == fForeRight);
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

// Positions from the head (in its heading), in cm; the rings of all holsters show while this page is open.
void Menu::RenderHolsterPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Holsters");
    ImGui::SameLine();
    ImGui::TextDisabled("  from your head, in cm");
    ImGui::Separator();
    const HolsterSpot& s = spots_[holsterSel_];
    char label[128];
    snprintf(label, sizeof(label), "Spot                  <  %s  >", kHolsterLabels[holsterSel_]);
    ImGui::Selectable(label, selected_ == hWhich);
    if (holsterSel_ < kHolsters) {
        const int at = HoldIndex(commands_[holsterSel_]);
        snprintf(label, sizeof(label), "Holds                 <  %s  >", at >= 0 ? kHoldNames[at] : commands_[holsterSel_].c_str());
        ImGui::Selectable(label, selected_ == hHolds);
    } else {
        ImGui::Selectable("Holds                    a new magazine (the manual reload)", selected_ == hHolds);
    }
    snprintf(label, sizeof(label), "Right / left          <  %+.0f  >", s.x * 100.0f);
    ImGui::Selectable(label, selected_ == hRight);
    snprintf(label, sizeof(label), "Up / down             <  %+.0f  >", s.y * 100.0f);
    ImGui::Selectable(label, selected_ == hUp);
    snprintf(label, sizeof(label), "Forward / back        <  %+.0f  >", s.z * 100.0f);
    ImGui::Selectable(label, selected_ == hForward);
    snprintf(label, sizeof(label), "Size                  <  %.0f across  >", s.r * 200.0f);
    ImGui::Selectable(label, selected_ == hSize);
    snprintf(label, sizeof(label), "Ring shown            <  %s  >", spotShown_[holsterSel_] ? "yes" : "no");
    ImGui::Selectable(label, selected_ == hShown);
    snprintf(label, sizeof(label), "Holster rings         <  %s  >", kRingLabels[ringsMode_]);
    ImGui::Selectable(label, selected_ == hRings);
    ImGui::Selectable("Reset this spot", selected_ == hReset);
    ImGui::Selectable("Back", selected_ == hBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Stick right = right / up / forward / bigger. Every ring shows while this page is open.");
    ImGui::TextDisabled("Ring shown: this spot's ring, on its own. Holster rings: near = when a hand comes close.");
    ImGui::TextDisabled("Saved for you.   B: back");
    ImGui::PopFont();
}

// Round 33: where the hand grabs the magazine / handle -- the grab rings moved along the gun and sized.
void Menu::RenderSpotPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Reload spots");
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", weaponKey_.empty() ? "(no gun in hand)" : weaponKey_.c_str());
    ImGui::Separator();
    const bool on = !weaponKey_.empty();
    const float* a = spotAdj_[spotSel_];
    char label[128];
    snprintf(label, sizeof(label), "Ring                  <  %s  >", kSpotNames[spotSel_]);
    ImGui::Selectable(label, selected_ == pWhich);
    if (!on) ImGui::BeginDisabled();
    snprintf(label, sizeof(label), "Forward / back        <  %+.0f  >", a[0]);
    ImGui::Selectable(label, selected_ == pFwd);
    snprintf(label, sizeof(label), "Up / down             <  %+.0f  >", a[1]);
    ImGui::Selectable(label, selected_ == pUp);
    snprintf(label, sizeof(label), "Right / left          <  %+.0f  >", a[2]);
    ImGui::Selectable(label, selected_ == pRight);
    snprintf(label, sizeof(label), "Size                  <  %.0f%%  >", a[3]);
    ImGui::Selectable(label, selected_ == pSize);
    ImGui::Selectable("Reset this ring", selected_ == pReset);
    if (!on) ImGui::EndDisabled();
    ImGui::Selectable("Back", selected_ == pBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Moves where you grab (cm along the gun); both rings show.");
    ImGui::TextDisabled("The magazine still goes in at the well. Saved for this gun.   B: back");
    ImGui::PopFont();
}

// Round 32: where the hand sits on the gun's magazine / handle -- the game's own reload grip, moved and turned.
void Menu::RenderGripPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Reload grip");
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", weaponKey_.empty() ? "(no gun in hand)" : weaponKey_.c_str());
    ImGui::Separator();
    const bool on = !weaponKey_.empty();
    const float* a = gripAdj_[gripSel_];
    char label[128];
    snprintf(label, sizeof(label), "Grip                  <  %s  >", kGripNames[gripSel_]);
    ImGui::Selectable(label, selected_ == gWhich);
    if (!on) ImGui::BeginDisabled();
    snprintf(label, sizeof(label), "Hold like grab        <  %s  >", gripHoldLikeGrab_ ? "yes" : "no");
    ImGui::Selectable(label, selected_ == gHoldLikeGrab);
    if (gripSel_ == 1 && gripHoldLikeGrab_) {
        ImGui::PushFont(nullptr, 28.0f);
        ImGui::TextDisabled("   the magazine in your hand takes the grab's grip (adjust that one)");
        ImGui::PopFont();
    }
    snprintf(label, sizeof(label), "Forward / back        <  %+.1f  >", a[0]);
    ImGui::Selectable(label, selected_ == gFwd);
    snprintf(label, sizeof(label), "Up / down             <  %+.1f  >", a[1]);
    ImGui::Selectable(label, selected_ == gUp);
    snprintf(label, sizeof(label), "Right / left          <  %+.1f  >", a[2]);
    ImGui::Selectable(label, selected_ == gRight);
    snprintf(label, sizeof(label), "Tilt (muzzle up)      <  %+.0f\xC2\xB0  >", a[3]);
    ImGui::Selectable(label, selected_ == gTilt);
    snprintf(label, sizeof(label), "Turn                  <  %+.0f\xC2\xB0  >", a[4]);
    ImGui::Selectable(label, selected_ == gTurn);
    snprintf(label, sizeof(label), "Roll                  <  %+.0f\xC2\xB0  >", a[5]);
    ImGui::Selectable(label, selected_ == gRoll);
    ImGui::Selectable("Reset this grip", selected_ == gReset);
    if (!on) ImGui::EndDisabled();
    ImGui::Selectable("Back", selected_ == gBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Moves your hand on the part (cm) and turns it at the wrist;");
    ImGui::TextDisabled("a held magazine moves the other way in your hand.");
    ImGui::TextDisabled("Hold the part to see it. Saved for this gun.   B: back");
    ImGui::PopFont();
}

// The off-hand knife in the hand: the grip, moved and turned (about its handle) in the controller's frame.
void Menu::RenderKnifePage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Knife grip");
    ImGui::SameLine();
    ImGui::TextDisabled("  the knife in your other hand");
    ImGui::Separator();
    char label[128];
    snprintf(label, sizeof(label), "Grip                  <  %s  >", knifeIcepick_ ? "icepick (blade down)" : "forward");
    ImGui::Selectable(label, selected_ == kgGrip);
    const char* names[6] = {"Forward / back", "Right / left", "Up / down", "Tilt (up / down)", "Turn (left / right)", "Roll"};
    for (int i = 0; i < 6; ++i) {
        if (i < 3) snprintf(label, sizeof(label), "%-22s<  %+.1f cm  >", names[i], knifeAdj_[i]);
        else snprintf(label, sizeof(label), "%-22s<  %+.0f\xC2\xB0  >", names[i], knifeAdj_[i]);
        ImGui::Selectable(label, selected_ == kgFwd + i);
    }
    ImGui::Selectable("Reset", selected_ == kgReset);
    ImGui::Selectable("Back", selected_ == kgBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Draw the knife and hold it in view, then adjust. Saved for you.");
    ImGui::TextDisabled("B: back");
    ImGui::PopFont();
}

void Menu::PublishKnife(bool save) {
    if (hdr_)
        for (int i = 0; i < 6; ++i) hdr_->knifeAdj[i] = knifeAdj_[i];
    if (save && !iniPath_.empty()) {
        wchar_t b[96];
        swprintf_s(b, L"%.1f %.1f %.1f %.0f %.0f %.0f", knifeAdj_[0], knifeAdj_[1], knifeAdj_[2], knifeAdj_[3], knifeAdj_[4], knifeAdj_[5]);
        WritePrivateProfileStringW(L"OffHand", L"KnifeAdj", b, iniPath_.c_str());
        WritePrivateProfileStringW(L"OffHand", L"KnifeGrip", knifeIcepick_ ? L"icepick" : L"forward", iniPath_.c_str());
    }
}

// The free support hand (off the foregrip) on its controller: turned about the wrist, moved forward/back.
void Menu::RenderFreeHandPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Free hand");
    ImGui::SameLine();
    ImGui::TextDisabled("  your other hand, off the gun");
    ImGui::Separator();
    char label[128];
    snprintf(label, sizeof(label), "Tilt (up / down)      <  %+.0f\xC2\xB0  >", freeHand_[0]);
    ImGui::Selectable(label, selected_ == eqPitch);
    snprintf(label, sizeof(label), "Turn (left / right)   <  %+.0f\xC2\xB0  >", freeHand_[1]);
    ImGui::Selectable(label, selected_ == eqYaw);
    snprintf(label, sizeof(label), "Roll                  <  %+.0f\xC2\xB0  >", freeHand_[2]);
    ImGui::Selectable(label, selected_ == eqRoll);
    snprintf(label, sizeof(label), "Forward / back        <  %+.0f cm  >", freeHand_[3]);
    ImGui::Selectable(label, selected_ == eqForward);
    ImGui::Selectable("Reset", selected_ == eqReset);
    ImGui::Selectable("Back", selected_ == eqBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Hold your other hand in view and adjust until it sits like your real hand. Saved for you.");
    ImGui::TextDisabled("B: back");
    ImGui::PopFont();
}

// The wrist HUD's panels (WRISTHUD-DESIGN): moved and sized; they show while this page is open.
void Menu::RenderWristPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Wrist panels");
    ImGui::SameLine();
    ImGui::TextDisabled("  on your other wrist");
    ImGui::Separator();
    const int p = wristSel_ == 2 ? 0 : wristSel_;
    const float* a = hud_.panel[p];
    char label[128];
    snprintf(label, sizeof(label), "Panel                 <  %s  >", kWristPanelNames[wristSel_]);
    ImGui::Selectable(label, selected_ == wpWhich);
    snprintf(label, sizeof(label), "Along the arm         <  %+.1f cm  >", a[0]);
    ImGui::Selectable(label, selected_ == wpAlong);
    snprintf(label, sizeof(label), "Across the arm        <  %+.1f cm  >", a[1]);
    ImGui::Selectable(label, selected_ == wpAcross);
    snprintf(label, sizeof(label), "Out from the arm      <  %+.1f cm  >", a[2]);
    ImGui::Selectable(label, selected_ == wpOut);
    snprintf(label, sizeof(label), "Size                  <  %.0f%%  >", a[3]);
    ImGui::Selectable(label, selected_ == wpSize);
    snprintf(label, sizeof(label), "Tilt toward you       <  %+.0f\xC2\xB0  >", a[4]);
    ImGui::Selectable(label, selected_ == wpTilt);
    ImGui::Selectable("Reset", selected_ == wpReset);
    ImGui::Selectable("Back", selected_ == wpBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Hold your other wrist palm down in view: the panels show while this page is open.");
    ImGui::TextDisabled("Along the arm: + toward the hand (either layout).");
    ImGui::TextDisabled(hud_.place ? "Saved for you.   B: back" : "(The HUD is on the screen: choose wrist on the HUD tab.)   B: back");
    ImGui::PopFont();
}

void Menu::RenderScreenHudPage() {
    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "Screen HUD");
    ImGui::SameLine();
    ImGui::TextDisabled("  the panel in front of you");
    ImGui::Separator();
    char label[128];
    snprintf(label, sizeof(label), "Distance              <  %.1f m  >", hud_.screen[0]);
    ImGui::Selectable(label, selected_ == shDist);
    snprintf(label, sizeof(label), "Size (width)          <  %.1f m  >", hud_.screen[1]);
    ImGui::Selectable(label, selected_ == shWidth);
    snprintf(label, sizeof(label), "Height                <  %+.0f cm  >", -hud_.screen[2] * 100.0f);
    ImGui::Selectable(label, selected_ == shDown);
    ImGui::Selectable("Reset", selected_ == shReset);
    ImGui::Selectable("Back", selected_ == shBack);
    ImGui::Separator();
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::TextDisabled("Live. With the HUD on the wrist this places what stays in view (hits, objectives, prompts).");
    ImGui::TextDisabled("Saved for you.   B: back");
    ImGui::PopFont();
}

void Menu::LoadHud() {
    // The shipped [HUD] keys are the defaults; the player's own only once changed in the menu.
    auto str = [&](const wchar_t* key, const wchar_t* def, wchar_t (&out)[64]) {
        wchar_t d[64] = L"";
        GetPrivateProfileStringW(L"HUD", key, def, d, 64, shippedPath_.c_str());
        GetPrivateProfileStringW(L"HUD", key, d, out, 64, iniPath_.c_str());
        return std::wstring(d);
    };
    auto parse = [](const wchar_t* v, float* f, int n, const float* lo, const float* hi) {
        float t[5] = {};
        const int got = swscanf_s(v, L"%f %f %f %f %f", &t[0], &t[1], &t[2], &t[3], &t[4]);
        if (got != n) return;
        for (int i = 0; i < n; ++i) f[i] = std::fmax(lo[i], std::fmin(hi[i], t[i]));
    };
    wchar_t v[64];
    const std::wstring dPlace = str(L"Place", L"screen", v);
    hudDef_.place = !_wcsicmp(dPlace.c_str(), L"wrist") ? 1 : 0;
    hud_.place = !_wcsicmp(v, L"wrist") ? 1 : 0;
    const std::wstring dShow = str(L"WristShow", L"look", v);
    hudDef_.show = !_wcsicmp(dShow.c_str(), L"always") ? 1 : 0;
    hud_.show = !_wcsicmp(v, L"always") ? 1 : 0;
    const std::wstring dLayout = str(L"WristLayout", L"forearm", v);
    hudDef_.layout = !_wcsicmp(dLayout.c_str(), L"across") ? 1 : 0;
    hud_.layout = !_wcsicmp(v, L"across") ? 1 : 0;
    const std::wstring dBack = str(L"WristBacking", L"dim", v);
    hudDef_.backing = hud_.backing = 1;
    for (int i = 0; i < 3; ++i) {
        if (!_wcsicmp(dBack.c_str(), kBackings[i])) hudDef_.backing = i;
        if (!_wcsicmp(v, kBackings[i])) hud_.backing = i;
    }
    const float plo[5] = {-20, -20, -20, 40, -60}, phi[5] = {20, 20, 20, 300, 80};
    for (int p = 0; p < 2; ++p) {
        const std::wstring d = str(kWristPanelKeys[p], L"", v);
        parse(d.c_str(), hudDef_.panel[p], 5, plo, phi);
        for (int k = 0; k < 5; ++k) hud_.panel[p][k] = hudDef_.panel[p][k];
        parse(v, hud_.panel[p], 5, plo, phi);
    }
    const float slo[3] = {0.3f, 0.1f, -2.0f}, shi[3] = {20.0f, 10.0f, 2.0f};
    const wchar_t* sk[3] = {L"Distance", L"Width", L"Down"};
    for (int k = 0; k < 3; ++k) {
        const std::wstring d = str(sk[k], L"", v);
        float f = hudDef_.screen[k];
        parse(d.c_str(), &f, 1, &slo[k], &shi[k]);
        hudDef_.screen[k] = f;
        parse(v, &f, 1, &slo[k], &shi[k]);
        hud_.screen[k] = f;
    }
    PublishHud();
    MLOG("menu: HUD %s (shows %s, layout %s, backing %ls); wrist panels L %.1f %.1f %.1f %.0f%% %.0f deg, R %.1f %.1f %.1f %.0f%% %.0f "
         "deg; screen panel %.2f m away, %.2f m wide, %.2f m down", hud_.place ? "wrist" : "screen", hud_.show ? "always" : "when looked at",
         hud_.layout ? "across" : "forearm", kBackings[hud_.backing], hud_.panel[0][0], hud_.panel[0][1], hud_.panel[0][2],
         hud_.panel[0][3], hud_.panel[0][4], hud_.panel[1][0], hud_.panel[1][1], hud_.panel[1][2], hud_.panel[1][3], hud_.panel[1][4],
         hud_.screen[0], hud_.screen[1], hud_.screen[2]);
}

void Menu::PublishHud() {
    if (!hdr_) return;
    hdr_->hudScreen[1] = hud_.screen[1];
    hdr_->hudScreen[2] = hud_.screen[2];
    hdr_->hudScreen[0] = hud_.screen[0];  // (the game takes the down with the distance)
    hdr_->hudPlace = hud_.place && wristAvail_ ? 2u : 1u;
}

void Menu::SaveHud(bool place, bool panels, bool screen, const wchar_t* only) {
    if (iniPath_.empty()) return;
    const wchar_t* ini = iniPath_.c_str();
    // (only the key the player changed: the shipped defaults of the others stay the defaults)
    auto want = [&](const wchar_t* k) { return place && (!only || !wcscmp(only, k)); };
    if (want(L"Place")) WritePrivateProfileStringW(L"HUD", L"Place", hud_.place ? L"wrist" : L"screen", ini);
    if (want(L"WristShow")) WritePrivateProfileStringW(L"HUD", L"WristShow", hud_.show ? L"always" : L"look", ini);
    if (want(L"WristLayout")) WritePrivateProfileStringW(L"HUD", L"WristLayout", hud_.layout ? L"across" : L"forearm", ini);
    if (want(L"WristBacking")) WritePrivateProfileStringW(L"HUD", L"WristBacking", kBackings[hud_.backing], ini);
    wchar_t b[96];
    if (panels)
        for (int p = 0; p < 2; ++p) {
            if (wristSel_ != 2 && wristSel_ != p) continue;  // (the panel the page changed)
            const float* a = hud_.panel[p];
            swprintf_s(b, L"%.1f %.1f %.1f %.0f %.0f", a[0], a[1], a[2], a[3], a[4]);
            WritePrivateProfileStringW(L"HUD", kWristPanelKeys[p], b, ini);
        }
    if (screen) {
        const wchar_t* sk[3] = {L"Distance", L"Width", L"Down"};
        for (int k = 0; k < 3; ++k) {
            if (only && wcscmp(only, sk[k])) continue;
            swprintf_s(b, L"%.2f", hud_.screen[k]);
            WritePrivateProfileStringW(L"HUD", sk[k], b, ini);
        }
    }
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
