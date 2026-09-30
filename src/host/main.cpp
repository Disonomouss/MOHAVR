// MOHAVR-host.exe -- the 64-bit side of the bridge (D10).
//
// Started by the game-side mod (dinput8.dll) with:  --game-pid <pid> [--runtime-json "<path>"] [--mirror 1|2] [--controllers 1]
// Owns everything OpenXR: loader, runtime, D3D11 device, swapchains. Receives the game's frames
// through shared D3D12 textures + fences (src/common/shared_frame.hpp) and shows them on a
// world-locked quad (M2, mono). Exits when the game exits. Logs to MOHAVR-host.log next to itself.
//
// Nothing here can take the game down: if anything fails the host logs it, marks hostState
// Failed and exits; the game keeps running without VR.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <shellapi.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "../mohavr/log.hpp"
#include "menu.hpp"
#include "mirror.hpp"
#include "pad.hpp"
#include "hands.hpp"
#include "reload.hpp"
#include "markers.hpp"
#include "reticle.hpp"

using mohavr::shared::Header;
using mohavr::shared::HostState;
using mohavr::shared::kRing;

namespace {

Header* g_hdr = nullptr;

void SetState(HostState s, const char* status) {
    if (!g_hdr) return;
    if (status) strncpy_s(g_hdr->hostStatus, status, _TRUNCATE);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_hdr->hostState), static_cast<LONG>(s));
}

int Fail(const char* what) {
    MLOG("host: FAILED -- %s", what);
    SetState(HostState::Failed, what);
    return 1;
}

#define XR_OK(call, what)                                                        \
    do {                                                                         \
        const XrResult r_ = (call);                                              \
        if (XR_FAILED(r_)) {                                                     \
            char m_[160];                                                        \
            snprintf(m_, sizeof(m_), "%s -> XrResult %d", what, static_cast<int>(r_)); \
            return Fail(m_);                                                     \
        }                                                                        \
    } while (0)

const char* StateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

HANDLE DupFromGame(HANDLE game, std::uint64_t value) {
    HANDLE out = nullptr;
    if (!DuplicateHandle(game, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)), GetCurrentProcess(), &out, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return out;
}

bool GameAlive(HANDLE game) { return WaitForSingleObject(game, 0) == WAIT_TIMEOUT; }

// --- diagnostics: what did we actually receive? -------------------------------------------------
// Reads `tex` back through a staging copy. Logs mean luminance (sampled) and, if `bmpPath` is
// given, writes a 32-bit BMP. Stalls the host for a frame -- only for the first few frames and on
// request (event Local\MOHAVR_HostCapture), never per frame.
void Inspect(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* label, const wchar_t* bmpPath) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) { MLOG("host: inspect %s -- staging texture failed", label); return; }
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        double sum = 0;
        long n = 0;
        unsigned maxv = 0;
        for (UINT y = 0; y < d.Height; y += 8) {
            const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
            for (UINT x = 0; x < d.Width; x += 8) {
                const std::uint8_t* p = row + x * 4;  // B G R A
                const unsigned l = (p[2] * 77u + p[1] * 150u + p[0] * 29u) >> 8;
                sum += l;
                maxv = l > maxv ? l : maxv;
                ++n;
            }
        }
        MLOG("host: inspect %s -- %ux%u fmt %d, mean luma %.1f, max %u", label, d.Width, d.Height, d.Format, sum / n, maxv);
        if (bmpPath) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(d.Width);
            ih.biHeight = -static_cast<LONG>(d.Height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biSizeImage = d.Width * 4 * d.Height;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + ih.biSizeImage;
            HANDLE f = CreateFileW(bmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(f, &fh, sizeof(fh), &w, nullptr);
                WriteFile(f, &ih, sizeof(ih), &w, nullptr);
                for (UINT y = 0; y < d.Height; ++y)
                    WriteFile(f, static_cast<const std::uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, d.Width * 4, &w, nullptr);
                CloseHandle(f);
            }
        }
        ctx->Unmap(staging, 0);
    }
    staging->Release();
}

int Run(DWORD gamePid, const std::wstring& runtimeJson, int mirrorMode, bool controllers) {
    // --- the game and the shared block -----------------------------------------------------------
    HANDLE game = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid);
    if (!game) return Fail("OpenProcess(game)");
    const std::wstring name = L"Local\\MOHAVR_" + std::to_wstring(gamePid);
    HANDLE mapping = nullptr;
    for (int i = 0; i < 100 && !mapping; ++i) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!mapping) Sleep(100);
    }
    if (!mapping) return Fail("OpenFileMapping (shared block)");
    g_hdr = static_cast<Header*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Header)));
    if (!g_hdr || g_hdr->magic != mohavr::shared::kMagic || g_hdr->version != mohavr::shared::kVersion)
        return Fail("shared block magic/version mismatch");
    g_hdr->hostPid = GetCurrentProcessId();
    SetState(HostState::Starting, "starting");

    // The game fills in the handles on its first Present after device creation.
    while (g_hdr->gameState != static_cast<std::uint32_t>(mohavr::shared::GameState::Ready)) {
        if (!GameAlive(game)) return Fail("game exited before the bridge was ready");
        if (g_hdr->gameState == static_cast<std::uint32_t>(mohavr::shared::GameState::Failed)) {
            char m[200];
            snprintf(m, sizeof(m), "game-side bridge failed: %s", g_hdr->gameStatus);
            return Fail(m);
        }
        Sleep(20);
    }
    MemoryBarrier();
    const UINT width = g_hdr->width, height = g_hdr->height;
    MLOG("host: game bridge ready -- %ux%u format %u, ring %u", width, height, g_hdr->dxgiFormat, g_hdr->ring);

    // --- OpenXR instance / system ----------------------------------------------------------------
    if (!runtimeJson.empty()) {
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", runtimeJson.c_str());
        MLOG("host: XR_RUNTIME_JSON = %ls", runtimeJson.c_str());
    }
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "MOHAVR");
    strcpy_s(ici.applicationInfo.engineName, "Unreal Engine 3 (MOHA)");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    XrInstance instance = XR_NULL_HANDLE;
    XR_OK(xrCreateInstance(&ici, &instance), "xrCreateInstance");
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &ip);
    MLOG("host: runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_OK(xrGetSystem(instance, &sgi, &system), "xrGetSystem (headset connected?)");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &sp);
    MLOG("host: system \"%s\"", sp.systemName);

    // --- D3D11 on the runtime's adapter, which must be the game's -------------------------------
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    XR_OK(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
          "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_OK(getReqs(instance, system, &reqs), "xrGetD3D11GraphicsRequirementsKHR");
    const std::uint64_t xrLuid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(reqs.adapterLuid.HighPart)) << 32) |
                                 reqs.adapterLuid.LowPart;
    if (xrLuid != g_hdr->adapterLuid) {
        MLOG("host: runtime adapter LUID %016llX != game adapter %016llX", static_cast<unsigned long long>(xrLuid),
             static_cast<unsigned long long>(g_hdr->adapterLuid));
        return Fail("the headset runtime uses a different GPU than the game (cross-adapter sharing unsupported)");
    }

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(reqs.adapterLuid, IID_PPV_ARGS(&adapter))))
        return Fail("adapter lookup by LUID");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    ID3D11Device* dev0 = nullptr;
    ID3D11DeviceContext* ctx0 = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, &dev0, nullptr, &ctx0)))
        return Fail("D3D11CreateDevice");
    adapter->Release();
    factory->Release();
    ID3D11Device5* dev = nullptr;
    ID3D11DeviceContext4* ctx = nullptr;
    if (FAILED(dev0->QueryInterface(IID_PPV_ARGS(&dev))) || FAILED(ctx0->QueryInterface(IID_PPV_ARGS(&ctx))))
        return Fail("ID3D11Device5 / ID3D11DeviceContext4 (shared fences need Windows 10 1703+)");

    // --- the game's shared textures and fences ---------------------------------------------------
    ID3D11Texture2D* shared[kRing] = {};
    for (UINT i = 0; i < kRing; ++i) {
        HANDLE h = DupFromGame(game, g_hdr->textureHandles[i]);
        if (!h || FAILED(dev->OpenSharedResource1(h, IID_PPV_ARGS(&shared[i])))) return Fail("open shared texture");
        CloseHandle(h);
    }
    ID3D11Fence* gameFence = nullptr;
    ID3D11Fence* hostFence = nullptr;
    {
        HANDLE hg = DupFromGame(game, g_hdr->gameFenceHandle);
        HANDLE hh = DupFromGame(game, g_hdr->hostFenceHandle);
        if (!hg || !hh || FAILED(dev->OpenSharedFence(hg, IID_PPV_ARGS(&gameFence))) ||
            FAILED(dev->OpenSharedFence(hh, IID_PPV_ARGS(&hostFence))))
            return Fail("open shared fences");
        CloseHandle(hg);
        CloseHandle(hh);
    }
    D3D11_TEXTURE2D_DESC td{};
    shared[0]->GetDesc(&td);
    td.MiscFlags = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* last = nullptr;  // our own copy: the swapchain is fed from here every XR frame
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &last))) return Fail("CreateTexture2D(last)");
    MLOG("host: opened %u shared textures and 2 fences from the game", kRing);

    // --- session, space, quad swapchain ---------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    XR_OK(xrCreateSession(instance, &sci, &session), "xrCreateSession");
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace local = XR_NULL_HANDLE;
    XR_OK(xrCreateReferenceSpace(session, &rsci, &local), "xrCreateReferenceSpace");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XR_OK(xrCreateReferenceSpace(session, &rsci, &viewSpace), "xrCreateReferenceSpace(VIEW)");

    // The frame is B8G8R8A8_UNORM holding gamma-encoded colour, so an sRGB BGRA swapchain shows it
    // correctly via a plain CopyResource (same typeless family).
    uint32_t fmtCount = 0;
    XR_OK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr), "xrEnumerateSwapchainFormats");
    std::vector<int64_t> fmts(fmtCount);
    XR_OK(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data()), "xrEnumerateSwapchainFormats");
    int64_t fmt = 0;
    for (int64_t f : fmts) if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { fmt = f; break; }
    if (!fmt) for (int64_t f : fmts) if (f == DXGI_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
    if (!fmt) return Fail("runtime offers no B8G8R8A8 swapchain format");
    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swci.format = fmt;
    swci.sampleCount = 1;
    swci.width = width;
    swci.height = height;
    swci.faceCount = 1;
    swci.arraySize = 2;  // one slice per eye for the projection layer (M3); the quad uses slice 0
    swci.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XR_OK(xrCreateSwapchain(session, &swci, &swapchain), "xrCreateSwapchain");
    uint32_t imgCount = 0;
    XR_OK(xrEnumerateSwapchainImages(swapchain, 0, &imgCount, nullptr), "xrEnumerateSwapchainImages");
    std::vector<XrSwapchainImageD3D11KHR> images(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_OK(xrEnumerateSwapchainImages(swapchain, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
          "xrEnumerateSwapchainImages");
    MLOG("host: quad swapchain %ux%u format %lld, %u images", width, height, fmt, imgCount);

    // --- in-headset menu + its controller actions ------------------------------------------------
    mohavr::host::Menu menu;
    const bool menuOk = menu.Init(dev, ctx, session, fmt, g_hdr);
    if (menuOk) menu.ApplySavedSettings();

    // --- desktop mirror (Bridge.Mirror) -----------------------------------------------------------
    mohavr::host::Mirror mirror;
    const bool mirrorOk = mirrorMode && mirror.Init(dev, gamePid, mirrorMode);

    XrActionSet menuSet = XR_NULL_HANDLE;
    mohavr::host::Pad pad;  // Input.Controllers (M6)
    XrAction aToggle = XR_NULL_HANDLE, aStick = XR_NULL_HANDLE, aSelect = XR_NULL_HANDLE, aBack = XR_NULL_HANDLE;
    {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(asci.actionSetName, "mohavr_menu");
        strcpy_s(asci.localizedActionSetName, "MOHAVR menu");
        XR_OK(xrCreateActionSet(instance, &asci, &menuSet), "xrCreateActionSet");
        auto make = [&](XrAction& a, const char* name, const char* loc, XrActionType type) {
            XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(aci.actionName, name);
            strcpy_s(aci.localizedActionName, loc);
            aci.actionType = type;
            return xrCreateAction(menuSet, &aci, &a);
        };
        XR_OK(make(aToggle, "menu_toggle", "Open/close menu", XR_ACTION_TYPE_BOOLEAN_INPUT), "xrCreateAction(toggle)");
        XR_OK(make(aStick, "menu_navigate", "Navigate menu", XR_ACTION_TYPE_VECTOR2F_INPUT), "xrCreateAction(stick)");
        XR_OK(make(aSelect, "menu_select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT), "xrCreateAction(select)");
        XR_OK(make(aBack, "menu_back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT), "xrCreateAction(back)");
        auto path = [&](const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath(instance, s, &p); return p; };
        if (controllers && !pad.Init(instance, ExeDir() + L"\\MOHAVR.ini")) {
            MLOG("host: gameplay actions failed -- no virtual pad");
            controllers = false;
        }
        // One suggestion per profile (a second call replaces the first), so the menu's and the pad's
        // bindings go in together. Quest (Touch): left menu button toggles, left stick navigates,
        // either trigger or A selects, B backs out.
        auto suggest = [&](const char* profile, std::vector<XrActionSuggestedBinding> b) {
            if (controllers) pad.AppendBindings(profile, path, b);
            if (b.empty()) return;
            XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            sb.interactionProfile = path(profile);
            sb.suggestedBindings = b.data();
            sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
            if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &sb))) MLOG("host: %s bindings not accepted", profile);
        };
        suggest("/interaction_profiles/oculus/touch_controller", {
            {aToggle, path("/user/hand/left/input/menu/click")},
            {aStick, path("/user/hand/left/input/thumbstick")},
            {aSelect, path("/user/hand/left/input/trigger/value")},
            {aSelect, path("/user/hand/right/input/trigger/value")},
            {aSelect, path("/user/hand/right/input/a/click")},
            {aBack, path("/user/hand/right/input/b/click")},
        });
        suggest("/interaction_profiles/valve/index_controller", {});
        suggest("/interaction_profiles/khr/simple_controller", {
            {aToggle, path("/user/hand/left/input/menu/click")},
            {aSelect, path("/user/hand/right/input/select/click")},
        });
        const XrActionSet sets[] = {menuSet, pad.Set()};
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = controllers ? 2 : 1;
        attach.actionSets = sets;
        XR_OK(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
        MLOG("host: actions attached (menu%s; Touch, Index, simple controller)", controllers ? " + gameplay pad" : "");
    }
    // M7: the aim poses (for the game's aim) and the reticle (ReticleSize in degrees; shown per the menu's Red dot,
    // whose default is the shipped [Aim] Reticle).
    const bool handsOk = controllers && pad.CreateSpaces(session);
    mohavr::host::Reticle reticle;
    bool reticleOk = false;
    {
        const std::wstring ini = ExeDir() + L"\\MOHAVR.ini";
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Aim", L"ReticleSize", L"0.8", v, 16, ini.c_str());
        const float deg = static_cast<float>(_wtof(v));
        if (handsOk)
            reticleOk = reticle.Init(dev, ctx, session, fmt, deg > 0.1f && deg < 10.0f ? deg : 0.8f);
    }
    XrPosef handPose[2] = {};
    std::uint32_t handBits = 0;
    mohavr::host::Hands hands;  // M8: gun hand, foregrip, holsters, reload gesture
    mohavr::host::ManualReload manualReload;  // D21: the manual reload's toggle, engagement and events
    mohavr::host::Hands::Output handsOut;
    mohavr::host::Markers markers;  // the gesture spots' rings
    bool markersOk = false;
    if (handsOk) {
        hands.Init(ExeDir() + L"\\MOHAVR.ini");
        manualReload.Init(ExeDir() + L"\\MOHAVR.ini");
        hands.SetReload(&manualReload);
        if (menuOk) {
            mohavr::host::HolsterSpot defaults[mohavr::host::kSpots];
            for (int i = 0; i < mohavr::host::kSpots; ++i) defaults[i] = hands.DefaultSpot(i);
            menu.LoadHolsters(defaults);
        }
        markersOk = markers.Init(dev, ctx, session, fmt);
    }
    // Test channel: %TEMP%\MOHAVR\host_cmd.txt, one command per line (toggle/up/down/left/right/select/back),
    // consumed and deleted each frame -- the simulator can't press controller buttons.
    std::wstring cmdPath;
    {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        cmdPath = std::wstring(tmp, n) + L"MOHAVR\\host_cmd.txt";
    }
    LARGE_INTEGER qpf, qpcLast;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpcLast);
    std::deque<std::string> testCmds;  // from host_cmd.txt, applied one per frame
    float stickHeld[4] = {0, 0, 0, 0};  // up, down, left, right: seconds held (auto-repeat)
    // With the virtual pad the left menu button is shared: a tap = the game's Start, holding it
    // (Controls.MenuHoldSeconds) = the MOHAVR menu; a tap also closes the MOHAVR menu when it is open.
    const float menuHoldSec = [&] {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", L"MenuHoldSeconds", L"0.6", v, 16, (ExeDir() + L"\\MOHAVR.ini").c_str());
        const float s = static_cast<float>(_wtof(v));
        return s > 0.1f && s < 5.0f ? s : 0.6f;
    }();
    float menuBtnHeld = 0.0f;
    bool menuBtnFired = false;
    // The flat screen for frames without a view (menus, cutscenes, loading): [Camera] ScreenDistance /
    // ScreenWidth in metres, world-locked in front of LOCAL (follows Recentre).
    auto iniFloat = [&](const wchar_t* key, float def, float lo, float hi) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Camera", key, L"", v, 16, (ExeDir() + L"\\MOHAVR.ini").c_str());
        const float f = static_cast<float>(_wtof(v));
        return v[0] && f >= lo && f <= hi ? f : def;
    };
    const float screenDist = iniFloat(L"ScreenDistance", 2.0f, 0.5f, 20.0f);
    const float screenWidth = iniFloat(L"ScreenWidth", 1.6f, 0.2f, 30.0f);
    MLOG("host: flat screen %.2f m wide at %.2f m", screenWidth, screenDist);
    // The screen's height: the head's when it (re)appears -- LOCAL's origin is at eye level on some
    // runtimes (Virtual Desktop) and on the floor on others (the simulator).
    float screenY = 0.0f;
    bool screenUp = false;
    bool screenYReal = false;  // screenY came from a really tracked head pose
    SetState(HostState::Running, "session created");

    // Frame pacing (the menu's Frame pacing -> hdr->pace; game side vr_view.cpp): the game's frame event, set once per XR
    // frame after this frame's poses are written and the last game frame taken -- a paced game starts its next Draw on
    // it. The game creates it; an older game DLL has none.
    HANDLE frameEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, (L"Local\\MOHAVR_Frame_" + std::to_wstring(gamePid)).c_str());
    MLOG("host: game frame event %s", frameEvent ? "open (set once per XR frame)" : "not found (no pacing)");

    // On-request capture of what the host received (the harness's view of the VR side).
    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\MOHAVR_HostCapture");
    std::wstring capturePath;
    {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        capturePath = std::wstring(tmp, n) + L"MOHAVR";
        CreateDirectoryW(capturePath.c_str(), nullptr);
        capturePath += L"\\host_capture.bmp";
    }

    // --- loop -------------------------------------------------------------------------------------
    bool running = false;
    bool loggedViews = false, loggedProjection = false, loggedStereo = false;
    mohavr::shared::SlotMeta lastMeta{};  // render pose/fov of the frame in `last`
    std::uint64_t shown = 0;  // last game frame copied into `last`
    // Recentre (menu): `local` is re-created at the head's heading and floor position. Frames the game
    // rendered with views from the old space are still submitted in it (`prevLocal`) until they are
    // through; recenterSeq is bumped only once views in the new space are published (shared_frame.hpp).
    XrSpace prevLocal = XR_NULL_HANDLE;
    std::uint64_t prevLocalUntil = 0;  // game frames <= this were rendered in prevLocal
    bool recenterBumpPending = false;
    long xrFrames = 0;
    long newFrames = 0;
    long repeats = 0, repeatRun = 0, longestRepeat = 0;  // XR frames without a new game frame (10 s window)
    std::int64_t lastViewQpc = 0;                         // the game's view time of the frame in `last` (v13)
    double lagSum = 0.0, lagSum2 = 0.0, lagLo = 1e9, lagHi = -1e9;  // XR frame time - that view time (10 s window)
    long lagN = 0;
    while (GameAlive(game)) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrSessionState st = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                MLOG("host: session state -> %s", StateName(st));
                if (st == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_OK(xrBeginSession(session, &bi), "xrBeginSession");
                    running = true;
                } else if (st == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
                    MLOG("host: session ending (%s)", StateName(st));
                    SetState(HostState::Exited, "session ended by runtime");
                    return 0;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { Sleep(20); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_OK(xrWaitFrame(session, nullptr, &fs), "xrWaitFrame");
        XR_OK(xrBeginFrame(session, nullptr), "xrBeginFrame");

        // --- menu input: controllers (when focused) + the test command file ---------------------
        LARGE_INTEGER qpcNow;
        QueryPerformanceCounter(&qpcNow);
        const float dt = static_cast<float>(qpcNow.QuadPart - qpcLast.QuadPart) / static_cast<float>(qpf.QuadPart);
        qpcLast = qpcNow;
        {
            // XR frame pacing, logged every 10 s (the runtime paces xrWaitFrame).
            static double sum = 0.0, worst = 0.0, since = 0.0;
            static long n = 0, late = 0;
            const double ms = 1000.0 * dt;
            sum += ms;
            worst = ms > worst ? ms : worst;
            if (ms > 1.5 * 1000.0 / 90.0) ++late;
            ++n;
            since += dt;
            if (since >= 10.0) {
                MLOG("perf: XR frame %.2f ms avg (%.1f Hz), worst %.1f ms, %ld of %ld late (> 1.5 frames at 90 Hz); %ld showed the "
                     "last game frame again (at most %ld in a row)", sum / n, 1000.0 * n / sum, worst, late, n, repeats, longestRepeat);
                if (lagN > 0) {
                    const double mean = lagSum / lagN, sd = std::sqrt(std::fmax(0.0, lagSum2 / lagN - mean * mean));
                    MLOG("perf: the world shown was %.1f ms behind each XR frame (sd %.2f ms, %.1f..%.1f) -- the sd is the jitter "
                         "while moving (at a run, 1 ms = 0.5 cm)", mean, sd, lagLo, lagHi);
                }
                sum = worst = since = 0.0;
                n = late = 0;
                repeats = longestRepeat = 0;
                lagSum = lagSum2 = 0.0;
                lagLo = 1e9;
                lagHi = -1e9;
                lagN = 0;
            }
        }
        mohavr::host::MenuInput mi;
        {
            const XrActiveActionSet active[] = {{menuSet, XR_NULL_PATH}, {pad.Set(), XR_NULL_PATH}};
            XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
            si.countActiveActionSets = controllers ? 2 : 1;
            si.activeActionSets = active;
            if (xrSyncActions(session, &si) == XR_SUCCESS) {
                auto pressed = [&](XrAction a) {
                    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                    gi.action = a;
                    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
                    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &b)) && b.isActive && b.changedSinceLastSync &&
                           b.currentState;
                };
                if (controllers) {
                    XrActionStateGetInfo tg{XR_TYPE_ACTION_STATE_GET_INFO};
                    tg.action = aToggle;
                    XrActionStateBoolean tb{XR_TYPE_ACTION_STATE_BOOLEAN};
                    const bool held = XR_SUCCEEDED(xrGetActionStateBoolean(session, &tg, &tb)) && tb.isActive && tb.currentState;
                    if (held) {
                        menuBtnHeld += dt;
                        if (!menuBtnFired && menuBtnHeld >= menuHoldSec) {
                            mi.toggle = true;
                            menuBtnFired = true;
                        }
                    } else {
                        if (menuBtnHeld > 0.0f && !menuBtnFired) {
                            if (menu.Visible()) mi.toggle = true;
                            else pad.PulseStart();
                        }
                        menuBtnHeld = 0.0f;
                        menuBtnFired = false;
                    }
                } else {
                    mi.toggle = pressed(aToggle);
                }
                mi.select = pressed(aSelect);
                mi.back = pressed(aBack);
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = aStick;
                XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
                if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive) {
                    // Deflection -> presses: immediately, then repeat after 0.4 s every 0.12 s.
                    const bool dir[4] = {v.currentState.y > 0.6f, v.currentState.y < -0.6f, v.currentState.x < -0.6f,
                                         v.currentState.x > 0.6f};
                    bool* out[4] = {&mi.up, &mi.down, &mi.left, &mi.right};
                    for (int d = 0; d < 4; ++d) {
                        if (!dir[d]) { stickHeld[d] = 0.0f; continue; }
                        const float before = stickHeld[d];
                        stickHeld[d] += dt;
                        if (before == 0.0f) *out[d] = true;
                        else if (stickHeld[d] > 0.4f && std::fmod(stickHeld[d] - 0.4f, 0.12f) < dt) *out[d] = true;
                    }
                }
            }
        }
        if (GetFileAttributesW(cmdPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            FILE* cf = nullptr;
            if (_wfopen_s(&cf, cmdPath.c_str(), L"r") == 0 && cf) {
                char line[64];
                while (fgets(line, sizeof(line), cf)) {
                    const std::string c(line, strcspn(line, "\r\n "));
                    if (!c.empty()) testCmds.push_back(c);
                }
                fclose(cf);
            }
            DeleteFileW(cmdPath.c_str());
        }
        // One command per frame, like one button press each ("down down" moves two rows).
        if (!testCmds.empty()) {
            const std::string c = testCmds.front();
            testCmds.pop_front();
            if (c == "toggle") mi.toggle = true;
            else if (c == "up") mi.up = true;
            else if (c == "down") mi.down = true;
            else if (c == "left") mi.left = true;
            else if (c == "right") mi.right = true;
            else if (c == "select") mi.select = true;
            else if (c == "back") mi.back = true;
            MLOG("host: test command '%s'", c.c_str());
        }

        if (controllers) pad.BeginFrame(static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart));
        for (std::uint32_t e : pad.TakeTestReload())
            manualReload.Queue(e, static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart));

        // Head pose + eye views at the predicted display time -> the game (seqlock, M3).
        XrPosef menuHead{};
        menuHead.orientation.w = 1.0f;
        bool menuHeadOk = false;
        bool headTrackedReal = false;  // position tracked and not a runtime placeholder (the flat screen's height)
        {
            XrSpaceLocation headLoc{XR_TYPE_SPACE_LOCATION};
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = local;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t viewCount = 0;
            const bool headOk = XR_SUCCEEDED(xrLocateSpace(viewSpace, local, fs.predictedDisplayTime, &headLoc)) &&
                                (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            const bool viewsOk = XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &viewCount, views)) && viewCount == 2 &&
                                 (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            if (headOk) {
                menuHead = headLoc.pose;
                menuHeadOk = true;
                // The head's yaw in LOCAL (whose forward is the body's heading): the twist about +Y, left positive.
                const auto& q = headLoc.pose.orientation;
                pad.SetHeadYaw(2.0f * std::atan2(q.y, q.w));
            }
            if (headOk && viewsOk) {
                auto toPose = [](const XrPosef& p) {
                    return mohavr::shared::Pose{p.position.x, p.position.y, p.position.z,
                                                p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
                };
                auto toFov = [](const XrFovf& f) {
                    return mohavr::shared::Fov{std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};
                };
                // Everything is worked out first (OpenXR calls, the hands update); the seqlock only covers the copies, so the
                // game's readers rarely meet it mid-write (round 18: a torn read dropped the shot to the game's own aim).
                std::uint32_t gunFlags = 0;
                // Position TRACKED, not just VALID: before the headset reports real tracking, runtimes
                // hand out a placeholder pose (Virtual Desktop: identity at y = -1.187), which the game
                // must not take as its origin (headset round 2: the camera ended up ~1.2 m too high).
                const bool posTracked = (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
                                        (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
                {
                    // Diagnostics (headset round 8): the runtime's own eye FOVs, whenever they change.
                    static XrFovf seenFov[2] = {};
                    static int loggedFov = 0;
                    for (int e = 0; e < 2 && loggedFov < 20; ++e) {
                        const XrFovf& a = views[e].fov;
                        const XrFovf& b = seenFov[e];
                        if (std::fabs(a.angleLeft - b.angleLeft) > 1e-3f || std::fabs(a.angleRight - b.angleRight) > 1e-3f ||
                            std::fabs(a.angleUp - b.angleUp) > 1e-3f || std::fabs(a.angleDown - b.angleDown) > 1e-3f) {
                            ++loggedFov;
                            MLOG("diag: runtime eye %d fov L%.1f R%.1f U%.1f D%.1f deg", e, a.angleLeft * 57.2958f,
                                 a.angleRight * 57.2958f, a.angleUp * 57.2958f, a.angleDown * 57.2958f);
                            seenFov[e] = a;
                        }
                    }
                }
                const auto& hq = headLoc.pose.orientation;
                headTrackedReal = posTracked && !(hq.x == 0.0f && hq.y == 0.0f && hq.z == 0.0f && hq.w == 1.0f);
                // M7: the aim poses, at the same time and in the same space as the head.
                handBits = handsOk ? pad.LocateHands(local, fs.predictedDisplayTime, headLoc.pose, handPose) : 0u;
                // A hand the runtime stops tracking (round 24: after ~10 s without moving, the Quest drops idle
                // controllers, and the gun fell back to the game's flat-screen placement, seen double) stays put.
                if (handsOk) handBits |= pad.HoldLost(headLoc.pose, handPose, handBits);
                // D21: the game's side of the manual reload (its geometry, ammo, state), before the hands use it.
                const double nowS = static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart);
                if (menuOk) manualReload.SetOn(menu.ManualReloadOn());
                manualReload.Poll(g_hdr, nowS, handsOk && (handBits & 3u) == 3u);
                // M8: the gun from both hands (gun hand, foregrip, holsters, reload gesture), in the same seqlock.
                if (handsOk) {
                    mohavr::host::Hands::Input hin{};
                    hin.aim[0] = handPose[0];
                    hin.aim[1] = handPose[1];
                    hin.valid = handBits;
                    hin.head = headLoc.pose;
                    for (int h = 0; h < 2; ++h) hin.grip[h] = pad.GripValue(session, h);
                    hin.fit = menuOk ? menu.Fit() : hands.DefaultFit();
                    hin.startLeft = menuOk && menu.StartLeft();
                    if (menuOk) {
                        for (int i = 0; i < mohavr::host::kSpots; ++i) hands.SetSpot(i, menu.Spot(i));
                        hands.SetHandPoint(menu.HandPoint());
                        hands.SetForegripRadius(menu.ForegripRadius());
                        hands.SetRingScale(menu.RingScale());
                    }
                    hin.gestures = !(menuOk && menu.Visible()) && !g_hdr->gameUiMenu;
                    for (int h = 0; h < 2; ++h) hin.trigger[h] = pad.TriggerValue(session, h);
                    hin.weaponKind = g_hdr->weaponKind;
                    hin.grenade = hin.weaponKind == 2 || (menuOk && menu.WeaponKey().find("Grenade") != std::string::npos);
                    hin.now = static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart);
                    hin.testThrow = pad.TakeTestThrow(hin.testThrowVel);
                    const int rb = manualReload.ReleaseButton();
                    for (int h = 0; h < 2; ++h) hin.release[h] = rb ? pad.FaceButton(session, h, rb == 1) : 0.0f;
                    hin.hasView = lastMeta.hasView != 0;
                    hin.pouchShown = menuOk && menu.HolsterPageOpen();
                    hin.reloadSpotsShown = menuOk && menu.ReloadSpotsPageOpen();
                    if (menuOk) {
                        float magAdj[4], boltAdj[4];
                        menu.SpotAdjust(0, magAdj);
                        menu.SpotAdjust(1, boltAdj);
                        manualReload.SetSpotAdjust(magAdj, boltAdj);
                    }
                    handsOut = hands.Update(hin);
                    gunFlags = (handsOut.gunValid ? 1u : 0u) | (handsOut.twoHanded ? 2u : 0u) | (handsOut.gunHand == 0 ? 4u : 0u);
                }
                manualReload.Send(g_hdr, nowS);
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));  // odd: writing
                g_hdr->viewDisplayTime = fs.predictedDisplayTime;
                g_hdr->head = toPose(headLoc.pose);
                for (int e = 0; e < 2; ++e) {
                    g_hdr->eye[e] = toPose(views[e].pose);
                    g_hdr->eyeFov[e] = toFov(views[e].fov);
                }
                g_hdr->viewValid = 1u | (posTracked ? 2u : 0u);
                g_hdr->handValid = handBits;
                g_hdr->reloadFlags = manualReload.Flags();
                g_hdr->reloadKeyHash = manualReload.KeyHash();
                g_hdr->magPull = manualReload.MagPull();
                g_hdr->magPose = manualReload.MagPose();
                g_hdr->rack = manualReload.Rack();
                for (int h = 0; h < 2; ++h) g_hdr->hand[h] = toPose(handPose[h]);
                if (handsOk) {
                    g_hdr->gunFlags = gunFlags;
                    g_hdr->gunPose = toPose(handsOut.gun);
                    g_hdr->aimRay = toPose(handsOut.aimRay);
                }
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));  // even: done
                if (handsOk) {
                    if (!handsOut.command.empty()) {
                        const size_t n = handsOut.command.size() < 63 ? handsOut.command.size() : 63;
                        std::memcpy(g_hdr->cmd, handsOut.command.c_str(), n);
                        g_hdr->cmd[n] = 0;
                        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->cmdSeq));
                    }
                    if (handsOut.thrown) {
                        for (int i = 0; i < 3; ++i) g_hdr->throwVel[i] = handsOut.throwVel[i];
                        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->throwSeq));
                    }
                    for (int h = 0; h < 2; ++h) {
                        if (handsOut.pulseAmp[h] > 0.0f) pad.Pulse(session, h, handsOut.pulseAmp[h], handsOut.pulseMs[h]);
                        else if (handsOut.pulse[h]) pad.Pulse(session, h);
                        pad.SetMaskedFace(h, manualReload.ReleaseButton() == 1, handsOut.maskFace[h]);
                        pad.SetMaskedTrigger(h, handsOut.maskTrigger[h]);
                    }
                    pad.SetConsumed(handsOut.consumed[0], handsOut.consumed[1]);
                    pad.SetTestTargets(handsOut.target, handsOut.targetOk);
                    pad.SetLeftHanded(handsOut.gunHand == 0);
                    if (menuOk) pad.SetSwapSticks(menu.SwapSticks());
                    if (menuOk) pad.SetMoveByHead(menu.MoveByHead());
                }
                {
                    static std::uint32_t seenBits = 0xFFFFFFFFu;
                    static int logged = 0;
                    if (handBits != seenBits && logged < 20) {
                        ++logged;
                        MLOG("host: aim poses %s%s", handBits & 1u ? "left " : "", handBits & 2u ? "right" : handBits ? "" : "none");
                        seenBits = handBits;
                    }
                }
                if (recenterBumpPending) {
                    recenterBumpPending = false;
                    // Anything published up to now (plus a frame the game may be rendering with views it
                    // read before this point) used the old space.
                    prevLocalUntil = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                                         reinterpret_cast<volatile LONG64*>(&g_hdr->publishedFrame), 0, 0)) + 2;
                    const LONG seq = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->recenterSeq));
                    MLOG("host: recentre #%ld published (old-space frames up to %llu)", seq,
                        static_cast<unsigned long long>(prevLocalUntil));
                }
                if (!loggedViews) {
                    loggedViews = true;
                    MLOG("host: views located -- eye fov L(%.1f %.1f %.1f %.1f) R(%.1f %.1f %.1f %.1f) deg, IPD %.1f mm",
                         views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f, views[0].fov.angleUp * 57.2958f,
                         views[0].fov.angleDown * 57.2958f, views[1].fov.angleLeft * 57.2958f, views[1].fov.angleRight * 57.2958f,
                         views[1].fov.angleUp * 57.2958f, views[1].fov.angleDown * 57.2958f,
                         1000.0f * std::sqrt(std::pow(views[1].pose.position.x - views[0].pose.position.x, 2.0f) +
                                             std::pow(views[1].pose.position.y - views[0].pose.position.y, 2.0f) +
                                             std::pow(views[1].pose.position.z - views[0].pose.position.z, 2.0f)));
                }
            }
        }

        if (menuOk) menu.Update(dt, mi, menuHead, menuHeadOk);
        if (controllers)
            pad.Update(session, static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart),
                       menuOk && menu.Visible(), menuOk ? menu.SnapTurnDegrees() : 0, g_hdr);
        if (menuOk && menu.TakeRecenterRequest()) {
            if (!menuHeadOk || prevLocal != XR_NULL_HANDLE || recenterBumpPending) {
                MLOG("host: recentre ignored (%s)", !menuHeadOk ? "no head pose" : "previous recentre still in flight");
            } else {
                // New LOCAL = the head's heading (yaw only) at its x/z; y stays (the game re-takes its origin).
                const auto& q = menuHead.orientation;
                const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                const float yaw = std::atan2(-fx, -fz);  // R_y(yaw) * (0,0,-1) = (fx, 0, fz)
                XrReferenceSpaceCreateInfo rc{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                rc.poseInReferenceSpace.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
                rc.poseInReferenceSpace.position = {menuHead.position.x, 0.0f, menuHead.position.z};
                XrSpace fresh = XR_NULL_HANDLE;
                if (XR_SUCCEEDED(xrCreateReferenceSpace(session, &rc, &fresh))) {
                    prevLocal = local;
                    prevLocalUntil = ~0ull;  // set when the bump is published
                    local = fresh;
                    recenterBumpPending = true;
                    MLOG("host: recentre -- new LOCAL at yaw %.1f deg, x %.2f z %.2f", yaw * 57.2958f,
                        menuHead.position.x, menuHead.position.z);
                } else {
                    MLOG("host: recentre failed (xrCreateReferenceSpace)");
                }
            }
            menu.Close();
        }
        if (prevLocal != XR_NULL_HANDLE && shown > prevLocalUntil) {
            xrDestroySpace(prevLocal);
            prevLocal = XR_NULL_HANDLE;
        }

        // Take the newest game frame, if there is one we haven't taken (protocol: shared_frame.hpp).
        const std::uint64_t f = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_hdr->publishedFrame), 0, 0));
        if (f > shown) {
            // Frame f's own slot (not publishedSlot, which a paced game may already have moved on to the next frame).
            const UINT slot = static_cast<UINT>(f % kRing);
            lastMeta = g_hdr->slotMeta[slot];  // written by the game before it published f
            lastViewQpc = g_hdr->slotViewQpc[slot];
            {
                // Diagnostics (headset round 8): log whenever what we submit changes kind or FOV.
                static mohavr::shared::SlotMeta seen{};
                static int logged = 0;
                auto differs = [](const mohavr::shared::Fov& a, const mohavr::shared::Fov& b) {
                    return std::fabs(a.tanLeft - b.tanLeft) > 1e-3f || std::fabs(a.tanRight - b.tanRight) > 1e-3f ||
                           std::fabs(a.tanUp - b.tanUp) > 1e-3f || std::fabs(a.tanDown - b.tanDown) > 1e-3f;
                };
                if (logged < 60 && (lastMeta.hasView != seen.hasView || lastMeta.stereo != seen.stereo ||
                                    differs(lastMeta.fov[0], seen.fov[0]) || differs(lastMeta.fov[1], seen.fov[1]))) {
                    ++logged;
                    MLOG("diag: frame %llu hasView %u stereo %u  eye0 L%.3f R%.3f U%.3f D%.3f  eye1 L%.3f R%.3f U%.3f D%.3f",
                         static_cast<unsigned long long>(f), lastMeta.hasView, lastMeta.stereo, lastMeta.fov[0].tanLeft,
                         lastMeta.fov[0].tanRight, lastMeta.fov[0].tanUp, lastMeta.fov[0].tanDown, lastMeta.fov[1].tanLeft,
                         lastMeta.fov[1].tanRight, lastMeta.fov[1].tanUp, lastMeta.fov[1].tanDown);
                    seen = lastMeta;
                }
            }
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_hdr->ackFrame), static_cast<LONG64>(f));
            ctx->Wait(gameFence, f);
            ctx->CopyResource(last, shared[slot % kRing]);
            ctx->Signal(hostFence, f);
            shown = f;
            if (lastMeta.hasView && !loggedProjection) {
                loggedProjection = true;
                MLOG("host: first head-tracked frame -- projection layer, %s (eye0 fov tan L%.3f R%.3f U%.3f D%.3f)",
                     lastMeta.stereo ? "STEREO side by side" : "mono", lastMeta.fov[0].tanLeft, lastMeta.fov[0].tanRight,
                     lastMeta.fov[0].tanUp, lastMeta.fov[0].tanDown);
            }
            if (lastMeta.hasView && lastMeta.stereo && !loggedStereo) {
                loggedStereo = true;
                const float ipd = std::sqrt(std::pow(lastMeta.pose[1].px - lastMeta.pose[0].px, 2.0f) +
                                            std::pow(lastMeta.pose[1].py - lastMeta.pose[0].py, 2.0f) +
                                            std::pow(lastMeta.pose[1].pz - lastMeta.pose[0].pz, 2.0f));
                MLOG("host: first stereo frame -- eye separation %.1f mm", ipd * 1000.0f);
            }
            if (++newFrames == 1) MLOG("host: first game frame received (frame %llu, slot %u)", static_cast<unsigned long long>(f), slot);
            if (newFrames == 1 || newFrames == 300 || newFrames == 1200) {
                char label[48];
                snprintf(label, sizeof(label), "game frame %llu", static_cast<unsigned long long>(f));
                Inspect(dev, ctx, last, label, nullptr);
            }
            if (captureEvent && WaitForSingleObject(captureEvent, 0) == WAIT_OBJECT_0) {
                Inspect(dev, ctx, last, "on request", capturePath.c_str());
            }
            repeatRun = 0;
        } else if (shown) {
            ++repeats;
            longestRepeat = ++repeatRun > longestRepeat ? repeatRun : longestRepeat;
        }
        if (lastViewQpc && shown) {
            // How far behind this XR frame the world it shows is (the game's view time of the frame in `last`). Motion is
            // smooth when this stays the same from frame to frame: its spread is the jitter (round 25).
            const double lag = 1000.0 * static_cast<double>(qpcNow.QuadPart - lastViewQpc) / static_cast<double>(qpf.QuadPart);
            if (lag > -50.0 && lag < 250.0) {
                lagSum += lag;
                lagSum2 += lag * lag;
                lagLo = lag < lagLo ? lag : lagLo;
                lagHi = lag > lagHi ? lag : lagHi;
                ++lagN;
            }
        }
        // This frame's poses are written and the last game frame is taken: a paced game starts its next Draw now.
        if (frameEvent) SetEvent(frameEvent);

        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        const XrCompositionLayerBaseHeader* layers[16];
        uint32_t layerCount = 0;
        if (fs.shouldRender) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR_OK(xrAcquireSwapchainImage(swapchain, &ai, &idx), "xrAcquireSwapchainImage");
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XR_OK(xrWaitSwapchainImage(swapchain, &wi), "xrWaitSwapchainImage");
            for (UINT slice = 0; slice < 2; ++slice)
                ctx->CopySubresourceRegion(images[idx].texture, D3D11CalcSubresource(0, slice, 1), 0, 0, 0, last, 0, nullptr);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_OK(xrReleaseSwapchainImage(swapchain, &ri), "xrReleaseSwapchainImage");

            const XrRect2Di full{{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
            if (lastMeta.hasView) {
                screenUp = false;
                // Every view is submitted with the exact pose and FOV it was rendered with, so the runtime can
                // reproject it and the world stays locked.
                //   stereo (M4): left half = eye 0, right half = eye 1, each with its own pose/fov;
                //   mono   (M3): the whole image, eye 0's pose/fov, in both eyes (slice e).
                const int32_t half = static_cast<int32_t>(width / 2);
                for (uint32_t e = 0; e < 2; ++e) {
                    const uint32_t src = lastMeta.stereo ? e : 0u;
                    const auto& p = lastMeta.pose[src];
                    const auto& rf = lastMeta.fov[src];
                    pviews[e].pose.position = {p.px, p.py, p.pz};
                    pviews[e].pose.orientation = {p.qx, p.qy, p.qz, p.qw};
                    pviews[e].fov = {std::atan(rf.tanLeft), std::atan(rf.tanRight), std::atan(rf.tanUp), std::atan(rf.tanDown)};
                    pviews[e].subImage.swapchain = swapchain;
                    if (lastMeta.stereo) {
                        const bool leftHalf = (e == 0) != (lastMeta.stereo == 2);  // 2 = halves swapped (experiment)
                        pviews[e].subImage.imageRect = leftHalf ? XrRect2Di{{0, 0}, {half, static_cast<int32_t>(height)}}
                                                              : XrRect2Di{{half, 0}, {static_cast<int32_t>(width) - half,
                                                                                      static_cast<int32_t>(height)}};
                        pviews[e].subImage.imageArrayIndex = 0;
                    } else {
                        pviews[e].subImage.imageRect = full;
                        pviews[e].subImage.imageArrayIndex = e;
                    }
                }
                proj.space = (prevLocal != XR_NULL_HANDLE && shown <= prevLocalUntil) ? prevLocal : local;
                proj.viewCount = 2;
                proj.views = pviews;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            } else {
                // At the head's height when the screen appears -- from a really tracked pose: at start-up Virtual
                // Desktop hands out a placeholder about 1.2 m low (round 5: the title screen was far below). If
                // the screen went up on such a pose, or the head is now 40 cm off, follow the tracked height.
                if (!screenUp) {
                    screenUp = true;
                    screenYReal = false;
                    if (menuHeadOk) screenY = menuHead.position.y;
                }
                if (headTrackedReal && (!screenYReal || std::fabs(menuHead.position.y - screenY) > 0.4f)) {
                    if (screenYReal) MLOG("host: flat screen height follows the head (%.2f -> %.2f m)", screenY, menuHead.position.y);
                    screenY = menuHead.position.y;
                    screenYReal = true;
                }
                layer.space = local;
                layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                layer.subImage.swapchain = swapchain;
                layer.subImage.imageRect = full;
                layer.pose.orientation.w = 1.0f;
                layer.pose.position = {0.0f, screenY, -screenDist};
                layer.size = {screenWidth, screenWidth * static_cast<float>(height) / static_cast<float>(width)};
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            }
            layerCount = 1;
            // M7: the reticle where the shot will land, along the aiming hand's ray (gameplay only).
            // Shown with the menu open too, so the Gun fit page can line the barrel up with it.
            if (reticleOk && lastMeta.hasView && menuHeadOk && (!menuOk || menu.RedDot())) {
                const std::uint32_t src = g_hdr->aimSource;
                const float d = g_hdr->aimDistance;
                if (src == 2 || src == 3) {
                    // With the gun in the hand: the host's aim line (hands.cpp: the gun hand, foregrip, the fit's
                    // angle and offset) -- the one the game traces along. Otherwise the aiming controller itself.
                    XrPosef ray{};
                    bool ok = false;
                    if (menuOk && menu.GunInHand() && handsOk && handsOut.gunValid) {
                        ray = handsOut.aimRay;
                        ok = true;
                    } else if (handBits & (1u << (src - 2))) {
                        ray = handPose[src - 2];
                        ok = true;
                    }
                    if (ok)
                        if (const XrCompositionLayerBaseHeader* rl = reticle.Layer(local, ray, menuHead, d))
                            layers[layerCount++] = rl;
                }
            }            // The gesture spots' rings ([Hands] Rings / the menu; all holsters while its Holsters page is open).
            if (markersOk && lastMeta.hasView && menuHeadOk && handsOk)
                layerCount += static_cast<uint32_t>(markers.Layers(
                    local, menuHead, handsOut, static_cast<mohavr::host::Markers::Mode>(menuOk ? menu.RingsMode() : 1),
                    menuOk && (menu.HolsterPageOpen() || menu.ReloadSpotsPageOpen()), layers + layerCount,
                    15 - static_cast<int>(layerCount)));
            // The menu panel on top of the game when open.
            if (menuOk) {
                if (const XrCompositionLayerBaseHeader* ml = menu.Layer(local)) layers[layerCount++] = ml;
            }
        }
        if (mirrorOk) mirror.Update(ctx, last, lastMeta, shown > 0);
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = layerCount;
        fe.layers = layers;
        XR_OK(xrEndFrame(session, &fe), "xrEndFrame");

        if (++xrFrames == 1) MLOG("host: first XR frame submitted");
        else if (xrFrames % 900 == 0) MLOG("host: %ld XR frames, %ld new game frames", xrFrames, newFrames);
    }

    MLOG("host: the game exited -- shutting down");
    xrRequestExitSession(session);
    SetState(HostState::Exited, "game exited");
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    mohavr::log::Open(ExeDir(), L"MOHAVR-host");
    MLOG("MOHAVR-host (built %s %s) -- pid %lu, 64-bit", __DATE__, __TIME__, GetCurrentProcessId());

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD gamePid = 0;
    std::wstring runtimeJson;
    int mirrorMode = 0;
    bool controllers = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--game-pid")) gamePid = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--runtime-json")) runtimeJson = argv[++i];
        else if (!wcscmp(argv[i], L"--mirror")) mirrorMode = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--controllers")) controllers = _wtoi(argv[++i]) != 0;
    }
    LocalFree(argv);
    if (!gamePid) { MLOG("host: no --game-pid -- nothing to do"); return 2; }
    MLOG("host: game pid %lu", gamePid);
    if (mirrorMode < 0 || mirrorMode > 2) mirrorMode = 0;
    // The mirror overlays the game window in physical pixels; set before the runtime makes any window.
    if (mirrorMode) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MLOG("host: mirror %d, controllers %d", mirrorMode, controllers);
    const int rc = Run(gamePid, runtimeJson, mirrorMode, controllers);
    MLOG("host: exit %d", rc);
    return rc;
}
