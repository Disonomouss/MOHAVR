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
#include <algorithm>
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "../mohavr/log.hpp"
#include "menu.hpp"
#include "mirror.hpp"
#include "pad.hpp"
#include "hands.hpp"
#include "reload.hpp"
#include "offhand.hpp"
#include "offknife.hpp"
#include "offpistol.hpp"
#include "markers.hpp"
#include "reticle.hpp"
#include "vignette.hpp"
#include "blit.hpp"
#include "formats.hpp"
#include "scope.hpp"
#include "wristhud.hpp"

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
    // D59: the HP Reverb G2's controllers have their own profile (XR_EXT_hp_mixed_reality_controller: SteamVR offers it,
    // e.g. with the Oasis driver). Enabled only when the runtime has it, so the controllers bind directly instead of
    // through the runtime's remapping of the Touch bindings.
    // GOAL B1: everything the runtime offers, logged once -- what a remote tester's log must answer.
    std::vector<std::string> offered;
    {
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n) {
            std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
            if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
                for (const auto& p : props) offered.emplace_back(p.extensionName);
        }
        std::string all;
        for (const auto& e : offered) all += (all.empty() ? "" : " ") + e;
        MLOG("host: runtime extensions (%zu): %s", offered.size(), all.c_str());
    }
    auto has = [&](const char* name) { return std::find(offered.begin(), offered.end(), name) != offered.end(); };
    const bool hpControllers = has(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
    const bool fbRefresh = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);  // B1: the refresh rate, logged
    // GOAL B3: more controllers bound directly where the runtime offers their profiles (the WMR and Vive wand ones are core).
    const bool cosmosControllers = has("XR_HTC_vive_cosmos_controller_interaction");
    const bool picoControllers = has("XR_BD_controller_interaction");
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (hpControllers) exts.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
    if (fbRefresh) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (cosmosControllers) exts.push_back("XR_HTC_vive_cosmos_controller_interaction");
    if (picoControllers) exts.push_back("XR_BD_controller_interaction");
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "MOHAVR");
    strcpy_s(ici.applicationInfo.engineName, "Unreal Engine 3 (MOHA)");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.enabledExtensionNames = exts.data();
    XrInstance instance = XR_NULL_HANDLE;
    XR_OK(xrCreateInstance(&ici, &instance), "xrCreateInstance");
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &ip);
    MLOG("host: runtime \"%s\" %u.%u.%u (HP Reverb G2 controller profile: %s)", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), hpControllers ? "yes" : "not offered");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_OK(xrGetSystem(instance, &sgi, &system), "xrGetSystem (headset connected?)");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &sp);
    MLOG("host: system \"%s\" (vendor %u): orientation tracking %s, position tracking %s; swapchains up to %ux%u, %u layers",
         sp.systemName, sp.vendorId, sp.trackingProperties.orientationTracking ? "yes" : "NO",
         sp.trackingProperties.positionTracking ? "yes" : "NO", sp.graphicsProperties.maxSwapchainImageWidth,
         sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);
    {
        uint32_t n = 0;
        std::vector<XrViewConfigurationView> vv;
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr)) && n) {
            vv.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vv.data());
        }
        if (!vv.empty()) {
            MLOG("host: the runtime recommends %ux%u per eye (max %ux%u, %u samples)", vv[0].recommendedImageRectWidth,
                 vv[0].recommendedImageRectHeight, vv[0].maxImageRectWidth, vv[0].maxImageRectHeight,
                 vv[0].recommendedSwapchainSampleCount);
            // D73: kept for the next start's "Auto" resolution (the game picks its size before the host runs).
            wchar_t local[MAX_PATH] = L"";
            const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (ln > 0 && ln < MAX_PATH) {
                const std::wstring dir = std::wstring(local) + L"\\MOHAVR";
                CreateDirectoryW(dir.c_str(), nullptr);
                const std::wstring hs = dir + L"\\MOHAVR.headset.ini";
                wchar_t sysName[XR_MAX_SYSTEM_NAME_SIZE] = L"";
                MultiByteToWideChar(CP_UTF8, 0, sp.systemName, -1, sysName, XR_MAX_SYSTEM_NAME_SIZE);
                WritePrivateProfileStringW(L"Headset", L"System", sysName, hs.c_str());
                WritePrivateProfileStringW(L"Headset", L"EyeWidth", std::to_wstring(vv[0].recommendedImageRectWidth).c_str(), hs.c_str());
                WritePrivateProfileStringW(L"Headset", L"EyeHeight", std::to_wstring(vv[0].recommendedImageRectHeight).c_str(), hs.c_str());
            }
        }
    }

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
    {
        // GOAL B1: the formats (DXGI numbers: 87 B8G8R8A8_UNORM, 91 its sRGB, 28 R8G8B8A8_UNORM, 29 its sRGB), the reference
        // spaces (STAGE: a floor), the refresh rate (XR_FB_display_refresh_rate, when offered).
        std::string f;
        for (int64_t v : fmts) f += (f.empty() ? "" : " ") + std::to_string(v);
        uint32_t ns = 0;
        std::vector<XrReferenceSpaceType> spaces;
        if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(session, 0, &ns, nullptr)) && ns) {
            spaces.resize(ns);
            xrEnumerateReferenceSpaces(session, ns, &ns, spaces.data());
        }
        std::string sn;
        for (auto t : spaces)
            sn += std::string(sn.empty() ? "" : " ") + (t == XR_REFERENCE_SPACE_TYPE_VIEW ? "VIEW" : t == XR_REFERENCE_SPACE_TYPE_LOCAL ? "LOCAL"
                                                       : t == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : std::to_string(t));
        float hz = 0.0f;
        PFN_xrGetDisplayRefreshRateFB getHz = nullptr;
        if (fbRefresh && XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetDisplayRefreshRateFB",
                                                            reinterpret_cast<PFN_xrVoidFunction*>(&getHz))) && getHz)
            getHz(session, &hz);
        MLOG("host: swapchain formats %s; reference spaces %s; refresh rate %s", f.c_str(), sn.c_str(),
             hz > 0.0f ? (std::to_string(static_cast<int>(hz + 0.5f)) + " Hz").c_str() : "not exposed");
    }
    // GOAL B2 (D65): B8G8R8A8 first (the game's frames copy straight in); else R8G8B8A8, with the game's frame blitted
    // (blit.cpp) and every texture copied into a swapchain made in its family (formats.hpp). [Debug] ForceRgbaSwapchain=1
    // takes the RGBA path where BGRA is offered too (the simulator's tests).
    const bool forceRgba = GetPrivateProfileIntW(L"Debug", L"ForceRgbaSwapchain", 0, (ExeDir() + L"\\MOHAVR.ini").c_str()) != 0;
    int64_t fmt = 0;
    if (!forceRgba) {
        for (int64_t f : fmts) if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { fmt = f; break; }
        if (!fmt) for (int64_t f : fmts) if (f == DXGI_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
    }
    if (!fmt) for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { fmt = f; break; }
    if (!fmt) for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM) { fmt = f; break; }
    if (!fmt) return Fail("runtime offers no B8G8R8A8 or R8G8B8A8 swapchain format");
    const bool rgba = mohavr::host::IsRgba(fmt);
    mohavr::host::Blit blit;
    ID3D11Texture2D* lastRgba = nullptr;  // the game's frame in the swapchain's family (rgba only)
    if (rgba) {
        D3D11_TEXTURE2D_DESC rd{};
        last->GetDesc(&rd);
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (!blit.Init(dev) || FAILED(dev->CreateTexture2D(&rd, nullptr, &lastRgba)))
            return Fail("the RGBA swapchain path (blit shader or texture)");
        MLOG("host: swapchain format %lld (R8G8B8A8%s) -- the game's frames are blitted into it%s", fmt,
             fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? ", sRGB" : "", forceRgba ? " (Debug.ForceRgbaSwapchain)" : "");
    }
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
    // GOAL C2: the HUD over the mirror in wrist mode ([Bridge] MirrorHud).
    mirror.SetHud(GetPrivateProfileIntW(L"Bridge", L"MirrorHud", 1, (ExeDir() + L"\\MOHAVR.ini").c_str()) != 0);

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
        // D59: the G2's controllers: Touch's layout (X/Y, A/B, a menu button on each), bound the same.
        if (hpControllers)
            suggest("/interaction_profiles/hp/mixed_reality_controller", {
                {aToggle, path("/user/hand/left/input/menu/click")},
                {aStick, path("/user/hand/left/input/thumbstick")},
                {aSelect, path("/user/hand/left/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/a/click")},
                {aBack, path("/user/hand/right/input/b/click")},
            });
        // GOAL B3: the Vive Cosmos and Pico 4 (Touch's layout), first-generation WMR and the Vive wands (no face buttons: back
        // is the right menu button; the wands navigate with the left trackpad).
        if (cosmosControllers)
            suggest("/interaction_profiles/htc/vive_cosmos_controller", {
                {aToggle, path("/user/hand/left/input/menu/click")},
                {aStick, path("/user/hand/left/input/thumbstick")},
                {aSelect, path("/user/hand/left/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/a/click")},
                {aBack, path("/user/hand/right/input/b/click")},
            });
        if (picoControllers)
            suggest("/interaction_profiles/bytedance/pico4_controller", {
                {aToggle, path("/user/hand/left/input/menu/click")},
                {aStick, path("/user/hand/left/input/thumbstick")},
                {aSelect, path("/user/hand/left/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/trigger/value")},
                {aSelect, path("/user/hand/right/input/a/click")},
                {aBack, path("/user/hand/right/input/b/click")},
            });
        suggest("/interaction_profiles/microsoft/motion_controller", {
            {aToggle, path("/user/hand/left/input/menu/click")},
            {aStick, path("/user/hand/left/input/thumbstick")},
            {aSelect, path("/user/hand/left/input/trigger/value")},
            {aSelect, path("/user/hand/right/input/trigger/value")},
            {aBack, path("/user/hand/right/input/menu/click")},
        });
        suggest("/interaction_profiles/htc/vive_controller", {
            {aToggle, path("/user/hand/left/input/menu/click")},
            {aStick, path("/user/hand/left/input/trackpad")},
            {aSelect, path("/user/hand/left/input/trigger/value")},
            {aSelect, path("/user/hand/right/input/trigger/value")},
            {aBack, path("/user/hand/right/input/menu/click")},
        });
        suggest("/interaction_profiles/khr/simple_controller", {
            {aToggle, path("/user/hand/left/input/menu/click")},
            {aSelect, path("/user/hand/right/input/select/click")},
        });
        const XrActionSet sets[] = {menuSet, pad.Set()};
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = controllers ? 2 : 1;
        attach.actionSets = sets;
        XR_OK(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
        MLOG("host: actions attached (menu%s; Touch, Index%s%s%s, WMR, Vive wands, simple controller)",
             controllers ? " + gameplay pad" : "", hpControllers ? ", HP Reverb G2" : "", cosmosControllers ? ", Vive Cosmos" : "",
             picoControllers ? ", Pico 4" : "");
    }
    // M7: the aim poses (for the game's aim) and the reticle (ReticleSize in degrees; shown per the menu's Red dot,
    // whose default is the shipped [Aim] Reticle).
    const bool handsOk = controllers && pad.CreateSpaces(session);
    mohavr::host::Reticle reticle, reticleOff;  // the main dot; the off-hand pistol's ([OffHand] PistolDot)
    bool reticleOk = false, reticleOffOk = false;
    {
        const std::wstring ini = ExeDir() + L"\\MOHAVR.ini";
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Aim", L"ReticleSize", L"0.8", v, 16, ini.c_str());
        const float deg = static_cast<float>(_wtof(v));
        if (handsOk) {
            reticleOk = reticle.Init(dev, ctx, session, fmt, deg > 0.1f && deg < 10.0f ? deg : 0.8f);
            reticleOffOk = reticleOff.Init(dev, ctx, session, fmt, deg > 0.1f && deg < 10.0f ? deg : 0.8f);
        }
    }
    // The comfort vignette (GOAL A2): the strength is the menu's ([Comfort] Vignette by default), the fade the ini's.
    mohavr::host::Vignette vignette;
    bool vignetteOk = false;
    if (controllers) {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Comfort", L"VignetteFade", L"0.2", v, 16, (ExeDir() + L"\\MOHAVR.ini").c_str());
        vignetteOk = vignette.Init(dev, ctx, session, fmt, static_cast<float>(_wtof(v)));
    }
    // Scopes (SCOPE-DESIGN): the scope raised to an eye -- the lens and the scope view the game renders for it.
    mohavr::host::Scope scope;
    const bool scopeOk = handsOk && scope.Init(dev, ctx, session, fmt, ExeDir() + L"\\MOHAVR.ini");
    // The wrist HUD (WRISTHUD-DESIGN): the game's HUD pass on the off hand's wrist, the rest head-locked.
    mohavr::host::WristHud wrist;
    const bool wristOk = wrist.Init(dev, ctx, session, fmt, g_hdr, game, ExeDir() + L"\\MOHAVR.ini");
    if (menuOk) menu.SetWristAvailable(wristOk);  // (the wrist only once the host can show it)
    XrPosef eyeNow[2] = {};  // this XR frame's eye poses (the lens is drawn for the looking eye's)
    bool eyesNowOk = false;
    XrPosef handPose[2] = {};
    std::uint32_t handBits = 0;
    mohavr::host::Hands hands;  // M8: gun hand, foregrip, holsters, reload gesture
    mohavr::host::ManualReload manualReload;  // D21: the manual reload's toggle, engagement and events
    mohavr::host::OffHandGrenade offhandNade;  // the off-hand grenade (OFFHAND-DESIGN): its toggle, state and events
    mohavr::host::OffHandPistol offhandPistol;  // the off-hand pistol (OFFPISTOL-DESIGN): likewise
    mohavr::host::OffHandKnife offhandKnife;    // the off-hand knife (OFFKNIFE-DESIGN): likewise
    mohavr::host::OffHandGrenade gunNade;       // the gun hand's grenade by pin, cook and grip ([Weapon] GrenadePin)
    mohavr::host::Hands::Output handsOut;
    mohavr::host::Markers markers;  // the gesture spots' rings
    bool markersOk = false;
    if (handsOk) {
        hands.Init(ExeDir() + L"\\MOHAVR.ini");
        manualReload.Init(ExeDir() + L"\\MOHAVR.ini");
        hands.SetReload(&manualReload);
        offhandNade.Init(ExeDir() + L"\\MOHAVR.ini");
        hands.SetOffHand(&offhandNade);
        offhandPistol.Init(ExeDir() + L"\\MOHAVR.ini");
        hands.SetOffPistol(&offhandPistol);
        offhandKnife.Init(ExeDir() + L"\\MOHAVR.ini");
        hands.SetOffKnife(&offhandKnife);
        gunNade.InitMain(ExeDir() + L"\\MOHAVR.ini");
        hands.SetGunNade(&gunNade);
        if (menuOk) {
            mohavr::host::HolsterSpot defaults[mohavr::host::kSpots];
            for (int i = 0; i < mohavr::host::kSpots; ++i) defaults[i] = hands.DefaultSpot(i);
            std::string commands[mohavr::host::kHolsters];
            for (int i = 0; i < mohavr::host::kHolsters; ++i) commands[i] = hands.DefaultCommand(i);
            menu.LoadHolsters(defaults, commands);
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
    // D60: a second way to the MOHAVR menu, for controllers whose menu button the runtime keeps (SteamVR's dashboard on the
    // Reverb G2): with the wrist HUD up (the off hand palm down, looked at), the off hand's lower face button (X; A in
    // left-handed mode) works as the menu button does: held for MenuHoldSeconds = the MOHAVR menu, a tap = the game's Start;
    // while the MOHAVR menu is open, either closes it. A press that starts with the gate open (or the menu open) is kept
    // from the game (X's grenade) until let go. [Controls] WristMenu=0 turns it off.
    const bool wristMenuOn = GetPrivateProfileIntW(L"Controls", L"WristMenu", 1, (ExeDir() + L"\\MOHAVR.ini").c_str()) != 0;
    float wristBtnHeld = 0.0f;
    bool wristBtnOwned = false, wristBtnFired = false, wristBtnWas = false;
    int wristHand = 0;      // the off hand, from the last frame's wrist HUD input
    bool wristGate = false;  // the last frame's wrist gate
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
    // D74: the headset taken off or the runtime's dashboard opened (the session leaves FOCUSED) mid-gameplay -- the game's
    // pause menu (its Start's "showmenu", through the console-command channel: the game runs it even while the host isn't
    // drawing). [Bridge] PauseOnFocusLoss.
    const bool pauseOnFocusLoss =
        GetPrivateProfileIntW(L"Bridge", L"PauseOnFocusLoss", 1, (ExeDir() + L"\\MOHAVR.ini").c_str()) != 0;
    bool wasFocused = false;
    auto pauseForFocusLoss = [&](const char* why) {
        if (!pauseOnFocusLoss || !g_hdr) return;
        if (!lastMeta.hasView || g_hdr->gameUiMenu) {
            MLOG("host: focus lost (%s) -- not in gameplay, no pause", why);
            return;
        }
        static const char kPause[] = "showmenu";
        std::memcpy(g_hdr->cmd, kPause, sizeof(kPause));
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->cmdSeq));
        MLOG("host: focus lost (%s) -- the game paused (its pause menu)", why);
    };
    // D74: the main gun's shots (hdr->gunShots, the player's muzzle flashes) -- a pulse in the gun hand, and the off hand
    // when it holds the foregrip. [Controls] ShotHaptics: the strength, 0 = off.
    const float shotHaptics = [&] {
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"Controls", L"ShotHaptics", L"0.6", v, 16, (ExeDir() + L"\\MOHAVR.ini").c_str());
        const float f = static_cast<float>(_wtof(v));
        return f >= 0.0f && f <= 1.0f ? f : 0.6f;
    }();
    std::uint32_t seenShots = g_hdr ? g_hdr->gunShots : 0;
    mohavr::shared::SlotScope lastScope{};  // v24: where its eyes and scope view are
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
                if (st == XR_SESSION_STATE_FOCUSED) wasFocused = true;
                else if (wasFocused) {
                    wasFocused = false;
                    pauseForFocusLoss(StateName(st));
                }
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
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                // Which profile the runtime bound each hand to: the first thing to read when a controller misbehaves.
                for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
                    XrPath hp = XR_NULL_PATH;
                    XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
                    char prof[XR_MAX_PATH_LENGTH] = "none";
                    uint32_t len = 0;
                    if (XR_SUCCEEDED(xrStringToPath(instance, hand, &hp)) &&
                        XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, hp, &ps)) && ps.interactionProfile != XR_NULL_PATH)
                        xrPathToString(instance, ps.interactionProfile, sizeof(prof), &len, prof);
                    MLOG("host: %s bound as %s", hand, prof);
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
        // The frame's test state first: the wrist's menu button below reads the controllers (D60) as the pad's mapping does.
        if (controllers) pad.BeginFrame(static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart));
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
                    if (wristMenuOn) {
                        const bool down = pad.FaceButton(session, wristHand, false) > 0.5f;
                        const bool menuOpen = menuOk && menu.Visible();
                        if (down && !wristBtnWas && (wristGate || menuOpen)) {
                            wristBtnOwned = true;
                            MLOG("host: %s pressed with %s -- the wrist's menu button (held %.1f s toggles the MOHAVR menu)",
                                 wristHand ? "A" : "X", menuOpen ? "the MOHAVR menu open" : "the wrist HUD up", menuHoldSec);
                        }
                        if (down && wristBtnOwned) {
                            wristBtnHeld += dt;
                            if (!wristBtnFired && wristBtnHeld >= menuHoldSec) {
                                mi.toggle = true;
                                wristBtnFired = true;
                                MLOG("host: the wrist's menu button held -- MOHAVR menu %s", menuOpen ? "closed" : "opened");
                            }
                        }
                        if (!down) {
                            // A tap, like the menu button's: the game's Start (its pause menu); the MOHAVR menu closed if open.
                            if (wristBtnOwned && !wristBtnFired && wristBtnHeld > 0.0f) {
                                if (menuOpen) mi.toggle = true;
                                else pad.PulseStart();
                                MLOG("host: the wrist's menu button tapped -- %s", menuOpen ? "MOHAVR menu closed" : "the game's Start");
                            }
                            wristBtnOwned = wristBtnFired = false;
                            wristBtnHeld = 0.0f;
                        }
                        wristBtnWas = down;
                        pad.SetWristMasked(wristHand, wristBtnOwned);
                        pad.SetWristMasked(1 - wristHand, false);
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
        // (Taken by a rename first, then read: a command written meanwhile is never deleted unread -- see Pad::ReadTests.)
        const std::wstring cmdTaken = cmdPath + L".taken";
        if (GetFileAttributesW(cmdPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            MoveFileExW(cmdPath.c_str(), cmdTaken.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            FILE* cf = nullptr;
            if (_wfopen_s(&cf, cmdTaken.c_str(), L"r") == 0 && cf) {
                char line[64];
                while (fgets(line, sizeof(line), cf)) {
                    const std::string c(line, strcspn(line, "\r\n "));
                    if (!c.empty()) testCmds.push_back(c);
                }
                fclose(cf);
            }
            DeleteFileW(cmdTaken.c_str());
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
            else if (c == "unfocus") pauseForFocusLoss("test command");  // D74 (the simulator keeps its focus)
            else if (c.rfind("goto=", 0) == 0 && menuOk) menu.Goto(c.substr(5));  // D81: an item by its key
            MLOG("host: test command '%s'", c.c_str());
        }

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
                const std::uint32_t trackedBits = handBits;  // really tracked (the off-hand grenade freezes without)
                // A hand the runtime stops tracking (round 24: after ~10 s without moving, the Quest drops idle
                // controllers, and the gun fell back to the game's flat-screen placement, seen double) stays put.
                if (handsOk) handBits |= pad.HoldLost(headLoc.pose, handPose, handBits);
                // D21: the game's side of the manual reload (its geometry, ammo, state), before the hands use it.
                const double nowS = static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart);
                if (menuOk) manualReload.SetOn(menu.ManualReloadOn());
                if (menuOk) manualReload.SetRackEject(menu.RackEjectOn(), menu.RackEjectKeep());  // (D54)
                manualReload.Poll(g_hdr, nowS, handsOk && (handBits & 3u) == 3u);
                // The off-hand grenade's game side (counts, availability, its state, the fuse's ticks), likewise.
                if (menuOk) offhandNade.SetOn(menu.OffHandGrenadeOn());
                if (menuOk) offhandNade.SetClick(menu.GrenadeClick());
                if (menuOk) offhandNade.SetSimple(menu.GrenadeSimple());  // D83
                if (menuOk) gunNade.SetSimple(menu.GrenadeSimple());
                if (menuOk) gunNade.SetOn(menu.GunGrenadePin() || menu.GrenadeSimple());  // (D83: simple covers the gun hand's too)
                if (handsOk) gunNade.Poll(g_hdr, nowS);
                if (handsOk) offhandNade.Poll(g_hdr, nowS);
                // The off-hand pistol's game side (the pistol a draw gets, availability, its state, shots and refills).
                if (menuOk) offhandPistol.SetOn(menu.OffHandPistolOn());
                if (handsOk) offhandPistol.Poll(g_hdr, nowS);
                // The off-hand knife's game side (whether a draw can happen, a held one may stay).
                if (menuOk) offhandKnife.SetOn(menu.OffHandKnifeOn());
                if (menuOk) offhandKnife.SetIcepick(menu.KnifeIcepick());
                if (handsOk) offhandKnife.Poll(g_hdr, nowS);
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
                        for (int i = 0; i < mohavr::host::kSpots; ++i) {
                            hands.SetSpot(i, menu.Spot(i));
                            hands.SetSpotShown(i, menu.SpotShown(i));
                        }
                        for (int i = 0; i < mohavr::host::kHolsters; ++i) hands.SetCommand(i, menu.HolsterCommand(i));
                        hands.SetHandPoint(menu.HandPoint());
                        hands.SetForegripRadius(menu.ForegripRadius());
                        hands.SetRingScale(menu.RingScale());
                        hands.SetPouchReload(menu.PouchReload());
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
                    hin.tracked = trackedBits;
                    for (int h = 0; h < 2; ++h) hin.gripActive[h] = pad.GripActive(session, h);
                    hin.modMenu = menuOk && menu.Visible();
                    hin.gameMenu = g_hdr->gameUiMenu != 0;
                    hin.pistolFit = menuOk ? menu.FitFor(offhandPistol.Key()) : hands.DefaultFit();
                    if (hin.weaponKind == 2 && menuOk) {
                        const std::string& wk = menu.WeaponKey();
                        hin.grenadeType = wk.find("MKIIFrag") != std::string::npos ? 0 : wk.find("Gammon") != std::string::npos ? 1 :
                                          wk.find("Stick") != std::string::npos ? 2 : -1;
                    }
                    if (menuOk) {
                        float magAdj[4], boltAdj[4];
                        menu.SpotAdjust(0, magAdj);
                        menu.SpotAdjust(1, boltAdj);
                        manualReload.SetSpotAdjust(magAdj, boltAdj);
                    }
                    handsOut = hands.Update(hin);
                    gunFlags = (handsOut.gunValid ? 1u : 0u) | (handsOut.twoHanded ? 2u : 0u) | (handsOut.gunHand == 0 ? 4u : 0u);
                }
                // Scopes: the gate (an eye at the eyepiece, two hands on the gun), the zoom and the scope view's camera.
                eyeNow[0] = views[0].pose;
                eyeNow[1] = views[1].pose;
                eyesNowOk = true;
                if (scopeOk) {
                    if (menuOk) {
                        scope.SetOn(menu.ScopeOn());
                        scope.SetZoomGame(menu.ScopeZoomGame());
                    }
                    mohavr::host::Scope::In sin{};
                    sin.gunValid = handsOut.gunValid;
                    sin.twoHanded = handsOut.twoHanded;
                    sin.gestures = !(menuOk && menu.Visible()) && !g_hdr->gameUiMenu;
                    sin.hasView = lastMeta.hasView != 0;
                    sin.eyesOk = true;
                    sin.gun = handsOut.gun;
                    sin.aimRay = handsOut.aimRay;
                    sin.eye[0] = views[0].pose;
                    sin.eye[1] = views[1].pose;
                    sin.stickY = pad.ScopeStickY();
                    sin.now = nowS;
                    scope.Update(g_hdr, sin);
                    pad.SetScopeZoom(scope.ZoomStick());
                }
                manualReload.Send(g_hdr, nowS);
                if (handsOk) offhandNade.Send(g_hdr, nowS);
                if (handsOk) gunNade.Send(g_hdr, nowS);
                if (handsOk) offhandPistol.Send(g_hdr, nowS);
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
                {
                    // One grenade hold at a time: the gun hand's (bit7) or the off hand's; bit0 = either switch on.
                    const bool gunHeld = gunNade.Holding();
                    g_hdr->nadeFlags = (gunHeld ? gunNade.Flags() : offhandNade.Flags()) | (offhandNade.On() || gunNade.On() ? 1u : 0u);
                    g_hdr->nadePose = gunHeld ? gunNade.HandPose() : offhandNade.HandPose();
                }
                g_hdr->pistolFlags = offhandPistol.Flags();
                g_hdr->knifeFlags = handsOk ? offhandKnife.Flags() : 0u;
                g_hdr->pistolTrigger = offhandPistol.Trigger();
                g_hdr->offAimRay = offhandPistol.AimRay();
                {
                    float fit[4];
                    offhandPistol.Fit(fit);
                    for (int i = 0; i < 4; ++i) g_hdr->offFit[i] = fit[i];
                }
                for (int h = 0; h < 2; ++h) g_hdr->hand[h] = toPose(handPose[h]);
                // Physical melee (MELEE-DESIGN): the switch; the gun hand -- and the off hand on the foregrip -- really tracked
                // (not held by HoldLost, not inferred); busy; the gun pose's epoch (+1 on a jump: no speed across it).
                {
                    static std::uint32_t epoch = 0, seenHeld = 0xFFFFFFFFu, seenRecenter = 0;
                    static int seenHand = -1;
                    static bool seenTurned = false;
                    const std::uint32_t real = pad.TrackedBits(), held = handBits & ~trackedBits;
                    if (handsOk && (held != seenHeld || handsOut.gunHand != seenHand || handsOut.turned != seenTurned ||
                                    g_hdr->recenterSeq != seenRecenter)) {
                        seenHeld = held;
                        seenHand = handsOut.gunHand;
                        seenTurned = handsOut.turned;
                        seenRecenter = g_hdr->recenterSeq;
                        ++epoch;
                    }
                    const int gh = handsOut.gunHand;
                    const bool tracked = handsOk && (real & (1u << gh)) && (!handsOut.twoHanded || (real & (1u << (1 - gh))));
                    g_hdr->meleeOn = (menuOk && menu.PhysicalMelee() ? 1u : 0u) | (tracked ? 2u : 0u) |
                                     (handsOk && handsOut.meleeBusy ? 4u : 0u) | ((epoch & 0xFFu) << 8);
                    // The off-hand knife's (OFFKNIFE-DESIGN A4): held; the off hand really tracked; busy; the off hand's pose
                    // epoch (+1 on a draw or a put back, the off hand held by HoldLost or back, the gun hand changed, a recentre).
                    static std::uint32_t offEpoch = 0, seenKnife = 0xFFFFFFFFu, seenOffHeld = 0xFFFFFFFFu, seenOffRecenter = 0;
                    static int seenOffHand = -1;
                    const int oh = 1 - gh;
                    const std::uint32_t offHeld = (held >> oh) & 1u;
                    if (handsOk && (offhandKnife.Epoch() != seenKnife || offHeld != seenOffHeld || gh != seenOffHand ||
                                    g_hdr->recenterSeq != seenOffRecenter)) {
                        seenKnife = offhandKnife.Epoch();
                        seenOffHeld = offHeld;
                        seenOffHand = gh;
                        seenOffRecenter = g_hdr->recenterSeq;
                        ++offEpoch;
                    }
                    if (handsOk && offhandKnife.Holding())
                        g_hdr->meleeOn |= 8u | ((real >> oh) & 1u ? 16u : 0u) | (handsOut.knifeBusy ? 32u : 0u) | ((offEpoch & 0xFFu) << 16);
                }
                // D82: the MG42 by hand as a lever (the player: "hand aim is inverted" -- the gun had pointed where the
                // controller pointed, so turning the wrist right swung the handle away to the left). The gun hand's grip
                // takes the handle: the pivot is then put 40 cm along the gun's line from the hand, and while held the gun
                // points from the hand through it (the handle pushed left swings the muzzle right, pushed down raises it);
                // let go and it stays. While manned that grip is the handle's, not the game's use (which got the player
                // off the gun); B still is.
                static bool mgWas = false, mgHeld = false;
                static XrVector3f mgDir{0.0f, 0.0f, -1.0f}, mgPivot{};
                if (handsOk && (g_hdr->mgState & 2u)) {
                    bool& wasMg = mgWas;
                    bool& held = mgHeld;
                    XrVector3f& dir = mgDir;
                    XrVector3f& pivot = mgPivot;
                    const int gh = handsOut.gunHand;
                    const XrVector3f hp = handPose[gh].position;
                    if (!wasMg) {
                        // Start level, the way the head faces.
                        dir = {0.0f, 0.0f, -1.0f};
                        if (menuHeadOk) {
                            const auto& q = menuHead.orientation;
                            float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                            const float l = std::sqrt(fx * fx + fz * fz);
                            if (l > 1e-3f) dir = {fx / l, 0.0f, fz / l};
                        }
                        held = false;
                        MLOG("host: mg -- the gun hand's grip takes the MG42's handle (a lever about its mount)");
                    }
                    wasMg = true;
                    const float gv = pad.GripValue(session, gh);
                    const bool grip = held ? gv > 0.4f : gv > 0.6f;
                    if (grip && !held) {
                        constexpr float kLever = 0.40f;
                        pivot = {hp.x + dir.x * kLever, hp.y + dir.y * kLever, hp.z + dir.z * kLever};
                        MLOG("host: mg -- the handle taken");
                    } else if (!grip && held) {
                        MLOG("host: mg -- the handle let go");
                    }
                    held = grip;
                    if (held) {
                        const XrVector3f d{pivot.x - hp.x, pivot.y - hp.y, pivot.z - hp.z};
                        const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                        if (l > 0.05f) dir = {d.x / l, d.y / l, d.z / l};
                    }
                    // The aim line along dir (-Z forward): yaw about +Y, then pitch about +X.
                    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
                    const float yaw = std::atan2(-dir.x, -dir.z);
                    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
                    handsOut.aimRay.position = hp;
                    handsOut.aimRay.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
                    handsOut.consumed[gh] = true;
                } else if (!(g_hdr->mgState & 2u)) {
                    mgWas = mgHeld = false;
                }
                if (handsOk) {
                    g_hdr->gunFlags = gunFlags;
                    g_hdr->gunPose = toPose(handsOut.gun);
                    g_hdr->aimRay = toPose(handsOut.aimRay);
                }
                g_hdr->scopeWant = scopeOk ? scope.Want() : 0u;
                g_hdr->scopeTanHalf = scope.TanHalf();
                g_hdr->scopeCamera = scope.Camera();
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
                    // A melee strike (the game's count): the gun hand, and the off hand on the foregrip; a soldier or a prop
                    // 50 ms at 0.6..1 by the strike's speed, the world 25 ms at 0.35 -- merged with this frame's other pulses
                    // (a second pulse on the same hand would cut the first short).
                    // The off-hand knife's strikes have their own count: the off hand only.
                    static std::uint32_t meleeSeen = g_hdr->meleeHits, knifeSeen = g_hdr->knifeHits;
                    for (int ch = 0; ch < 2; ++ch) {
                        const bool offKnife = ch == 1;
                        const std::uint32_t hits = offKnife ? g_hdr->knifeHits : g_hdr->meleeHits;
                        std::uint32_t& seen = offKnife ? knifeSeen : meleeSeen;
                        if (hits == seen) continue;
                        seen = hits;
                        _ReadWriteBarrier();
                        const std::uint32_t kind = offKnife ? g_hdr->knifeKind : g_hdr->meleeKind;
                        const float power = offKnife ? g_hdr->knifePower : g_hdr->meleePower;
                        const bool world = (kind & 0xFFu) == 3u;
                        const float amp = world ? 0.35f : 0.6f + 0.4f * power, ms = world ? 25.0f : 50.0f;
                        for (int h = 0; h < 2; ++h) {
                            if (offKnife ? h == handsOut.gunHand : h != handsOut.gunHand && !handsOut.twoHanded) continue;
                            const float a = offKnife || h == handsOut.gunHand ? amp : amp * 0.8f;
                            handsOut.pulse[h] = true;
                            handsOut.pulseAmp[h] = std::fmax(handsOut.pulseAmp[h], a);
                            handsOut.pulseMs[h] = std::fmax(handsOut.pulseMs[h], ms);
                        }
                        MLOG("host: melee strike %u (%s) -- pulse %.2f for %.0f ms%s", seen,
                             (kind & 0xFFu) == 1u ? "a soldier" : (kind & 0xFFu) == 2u ? "an actor" : "the world", amp, ms,
                             offKnife ? ", the off hand (the knife)" : handsOut.twoHanded ? ", both hands" : "");
                    }
                    // D74: the main gun's shots.
                    const std::uint32_t shots = g_hdr->gunShots;
                    if (shots != seenShots) {
                        const std::uint32_t n = shots - seenShots;
                        seenShots = shots;
                        if (shotHaptics > 0.0f && handsOut.gunValid) {
                            const int gh = handsOut.gunHand, oh = 1 - gh;
                            handsOut.pulseAmp[gh] = std::max(handsOut.pulseAmp[gh], shotHaptics);
                            handsOut.pulseMs[gh] = std::max(handsOut.pulseMs[gh], 35.0f);
                            if (handsOut.twoHanded) {
                                handsOut.pulseAmp[oh] = std::max(handsOut.pulseAmp[oh], 0.7f * shotHaptics);
                                handsOut.pulseMs[oh] = std::max(handsOut.pulseMs[oh], 35.0f);
                            }
                            static int logged = 0;
                            if (logged < 40) {
                                ++logged;
                                MLOG("host: %u shot(s) -- a %.2f pulse in the %s hand%s", n, shotHaptics, gh ? "right" : "left",
                                     handsOut.twoHanded ? " (and the foregrip hand)" : "");
                            }
                        }
                    }
                    // D77: the grab pickup. A grip closing that no gesture took (handsOut.consumed: a holster, the pouch, the
                    // foregrip, the off hand's items) with that hand within reach of a weapon or a crate the game lists
                    // (hdr->pickupNear) asks the game to take it; a light tick as a hand comes within reach, a pulse when it's
                    // taken.
                    {
                        static bool gripWas[2] = {false, false};
                        static std::uint32_t nearWas = 0, doneSeen = g_hdr->pickupDone;
                        static int lastHand = 1;
                        const std::uint32_t nearNow = g_hdr->pickupNear;
                        const bool menus = (menuOk && menu.Visible()) || g_hdr->gameUiMenu != 0;
                        for (int h = 0; h < 2; ++h) {
                            const float gv = pad.GripValue(session, h);
                            const bool grip = gripWas[h] ? gv > 0.4f : gv > 0.6f;  // (pressed, with some hysteresis)
                            if (grip && !gripWas[h] && !handsOut.consumed[h] && !menus && ((nearNow >> h) & 1u)) {
                                g_hdr->pickupReqHand = static_cast<std::uint32_t>(h);
                                MemoryBarrier();
                                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->pickupReqSeq));
                                lastHand = h;
                                MLOG("host: pickup -- the %s hand's grip closed within reach: take it", h ? "right" : "left");
                            }
                            gripWas[h] = grip;
                            if (((nearNow >> h) & 1u) && !((nearWas >> h) & 1u) && !menus) {
                                handsOut.pulseAmp[h] = std::max(handsOut.pulseAmp[h], 0.15f);
                                handsOut.pulseMs[h] = std::max(handsOut.pulseMs[h], 12.0f);
                            }
                        }
                        nearWas = nearNow;
                        if (g_hdr->pickupDone != doneSeen) {
                            doneSeen = g_hdr->pickupDone;
                            handsOut.pulseAmp[lastHand] = std::max(handsOut.pulseAmp[lastHand], 0.6f);
                            handsOut.pulseMs[lastHand] = std::max(handsOut.pulseMs[lastHand], 60.0f);
                            MLOG("host: pickup -- taken (a pulse in the %s hand)", lastHand ? "right" : "left");
                        }
                    }
                    for (int h = 0; h < 2; ++h) {
                        if (handsOut.pulseAmp[h] > 0.0f) pad.Pulse(session, h, handsOut.pulseAmp[h], handsOut.pulseMs[h]);
                        else if (handsOut.pulse[h]) pad.Pulse(session, h);
                        pad.SetMaskedFace(h, manualReload.ReleaseButton() == 1, handsOut.maskFace[h]);
                        pad.SetMaskedTrigger(h, handsOut.maskTrigger[h]);
                    }
                    pad.SetConsumed(handsOut.consumed[0], handsOut.consumed[1]);
                    pad.SetMaskedButtons(handsOut.maskSwitch ? 0x0200 : 0);  // Xbox RB: the game's own grenade switch
                    pad.SetRedirectB(handsOut.maskSwitchB);  // Xbox B: the switch weapon that would take the off-hand pistol
                    pad.SetTestTargets(handsOut.target, handsOut.targetOk, handsOut.align, handsOut.alignOk);
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
        // Physical crouch (GOAL A1): the game side bumps crouchReqSeq once per stance toggle it wants -- one pulse of the
        // game's crouch each (its own retry if the stance doesn't change).
        // D75: the parachute steered by the hands (the menu's Parachute: hands; [Controls] ChuteHands). With the chute open and
        // both grips held (on the risers), each hand's pull below the head turns that way; both pulled brake, both up dive (the
        // move stick, the game's own steering); a quick deep pull of both flares (Xbox A). The grips are the risers' while the
        // chute is open, so they don't also press their mapped buttons (the right grip's A would flare).
        {
            static bool steering = false, flareArmed = true;
            static float pullWas = 0.0f;
            static double pullWasT = 0.0;
            const double tNow = static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart);
            const bool chuteOpen = controllers && handsOk && menuOk && menu.ChuteHands() && g_hdr->airdrop >= 2 && menuHeadOk;
            bool on = false;
            float x = 0.0f, y = 0.0f;
            if (chuteOpen) {
                pad.SetConsumed(true, true);
                const bool both = (handBits & 3u) == 3u && pad.GripValue(session, 0) > 0.5f && pad.GripValue(session, 1) > 0.5f;
                if (both) {
                    auto pull = [&](int h) {
                        return std::clamp((menuHead.position.y + 0.10f - handPose[h].position.y) / 0.45f, 0.0f, 1.0f);
                    };
                    const float pl = pull(0), pr = pull(1), avg = 0.5f * (pl + pr);
                    x = std::clamp(1.5f * (pr - pl), -1.0f, 1.0f);
                    y = std::clamp(0.6f - 1.2f * avg, 0.0f, 1.0f);  // (a canopy never flies backwards: fully pulled = slowest)
                    on = true;
                    // The flare: both from above halfway to past 85% within 0.4 s.
                    if (avg < 0.5f) {
                        pullWas = avg;
                        pullWasT = tNow;
                        flareArmed = true;
                    } else if (flareArmed && std::min(pl, pr) > 0.85f && tNow - pullWasT < 0.4) {
                        flareArmed = false;
                        pad.PulseFlare();
                        MLOG("host: chute -- both risers pulled hard (%.2f -> %.2f in %.2f s): flare", pullWas, avg, tNow - pullWasT);
                    }
                    static int logged = 0;
                    if (logged < 30 && (!steering || static_cast<int>(tNow * 2.0) % 4 == 0)) {
                        ++logged;
                        MLOG("host: chute -- the hands steer: pull left %.2f right %.2f -> the move stick x %.2f y %.2f", pl, pr, x, y);
                    }
                }
            }
            if (on != steering) MLOG("host: chute -- %s", on ? "steering by the risers (both grips held)" : "the risers let go: the stick's");
            steering = on;
            pad.SetChute(on, x, y);
        }
        static std::uint16_t seenCrouchReq = g_hdr->crouchReqSeq;
        if (controllers && g_hdr->crouchReqSeq != seenCrouchReq) {
            seenCrouchReq = g_hdr->crouchReqSeq;
            pad.PulseCrouch();
            MLOG("host: crouch request %u -- the game's crouch pulsed", static_cast<unsigned>(seenCrouchReq));
        }
        if (controllers)
            pad.Update(session, static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart),
                       menuOk && menu.Visible(), menuOk ? menu.SnapTurnDegrees() : 0, g_hdr);
        if (vignetteOk) {
            if (menuOk) vignette.SetStrength(menu.VignetteStrength());
            vignette.Update(dt, controllers ? pad.Motion() : 0.0f);
        }
        static std::uint32_t allWeaponsPawn = 0;  // the pawn "Give all weapons" was for
        if (menuOk && menu.TakeGiveAllRequest() && g_hdr) {
            // The game runs its own cheats for it (vr_view.cpp RunHostCommand: EnableCheats, a GiveWeapon per
            // [Weapon] GiveAllList class, GiveAmmo).
            static const char kGiveAll[] = "mohavr giveall";
            std::memcpy(g_hdr->cmd, kGiveAll, sizeof(kGiveAll));
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->cmdSeq));
            // The game's switch weapon cycles only the slot weapons: switch weapon steps through all of them now.
            pad.SetAllWeapons(true);
            allWeaponsPawn = g_hdr->reloadPawnSeq;
            MLOG("host: give all weapons -> the game; switch weapon now steps through everything carried (NextWeapon)");
        }
        if (pad.AllWeapons() && g_hdr && g_hdr->reloadPawnSeq != allWeaponsPawn) {
            pad.SetAllWeapons(false);  // a new pawn (a death, a level): the given guns are gone
            MLOG("host: a new pawn -- switch weapon is the game's own again");
        }
        if (pad.TakeNextWeapon() && g_hdr) {
            static const char kNext[] = "NextWeapon";
            std::memcpy(g_hdr->cmd, kNext, sizeof(kNext));
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->cmdSeq));
        }
        if (pad.TakeRedirectB() && g_hdr) {  // the off-hand pistol kept: the long gun changes instead (PistolKeep)
            static const char kPrimary[] = "SwitchPrimary";
            std::memcpy(g_hdr->cmd, kPrimary, sizeof(kPrimary));
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->cmdSeq));
        }
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
            lastScope = g_hdr->slotScope[slot];
            const mohavr::shared::SlotHud lastHud = g_hdr->slotHud[slot];  // (likewise, before the ack lets the game reuse the slot)
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
            if (rgba) {
                D3D11_TEXTURE2D_DESC ld{};
                last->GetDesc(&ld);
                blit.Run(ctx, last, lastRgba, ld.Width, ld.Height);
            }
            if (wristOk) wrist.TakeFrame(slot, lastHud);  // (its HUD texture, under the same fence)
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
                if (wristOk) {  // (the wrist HUD: the HUD texture received and the atlas last composed)
                    const std::wstring dir = capturePath.substr(0, capturePath.find_last_of(L'\\'));
                    Inspect(dev, ctx, wrist.HudTexture(), "the HUD texture", (dir + L"\\host_capture_hud.bmp").c_str());
                    Inspect(dev, ctx, wrist.Atlas(), "the wrist HUD's atlas", (dir + L"\\host_capture_wrist.bmp").c_str());
                }
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

        // The wrist HUD's gate and placement (every XR frame): the off hand = the opposite of the hand holding the gun (the
        // live gun hand: a cross-draw moves the panels to the other wrist), the menu's Gun hand until the hands run.
        if (wristOk) {
            mohavr::host::WristHud::In win{};
            win.hasView = lastMeta.hasView != 0;
            win.gameMenu = g_hdr->gameUiMenu != 0;
            win.modMenu = menuOk && menu.Visible();
            win.wristPage = menuOk && menu.WristPageOpen();
            win.offHand = handsOk ? 1 - handsOut.gunHand : (menuOk && menu.StartLeft() ? 1 : 0);
            win.offPose = handPose[win.offHand];
            win.offTracked = handsOk && (pad.TrackedBits() & (1u << win.offHand)) != 0;
            win.foregrip = handsOk && handsOut.twoHanded && handsOut.gunHand != win.offHand;
            win.gunOk = handsOk && handsOut.gunValid && (handBits & (1u << handsOut.gunHand)) != 0;
            if (win.gunOk) win.gunPose = handPose[handsOut.gunHand];
            win.head = menuHead;
            win.headOk = menuHeadOk;
            win.now = static_cast<double>(qpcNow.QuadPart) / static_cast<double>(qpf.QuadPart);
            if (menuOk) win.set = menu.Hud();
            wrist.Update(win);
            wristHand = win.offHand;
            wristGate = wrist.PanelsUp();
        }

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
                ctx->CopySubresourceRegion(images[idx].texture, D3D11CalcSubresource(0, slice, 1), 0, 0, 0, rgba ? lastRgba : last, 0,
                                           nullptr);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_OK(xrReleaseSwapchainImage(swapchain, &ri), "xrReleaseSwapchainImage");

            const XrRect2Di full{{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
            if (lastMeta.hasView) {
                screenUp = false;
                // Every view is submitted with the exact pose and FOV it was rendered with, so the runtime can
                // reproject it and the world stays locked.
                //   stereo (M4): left half = eye 0, right half = eye 1, each with its own pose/fov;
                //   mono   (M3): the whole image, eye 0's pose/fov, in both eyes (slice e).
                // (v24: with a scope column each eye is lastScope.eyeWidth wide and the column follows.)
                const int32_t half = lastScope.eyeWidth ? static_cast<int32_t>(lastScope.eyeWidth) : static_cast<int32_t>(width / 2);
                const int32_t rightW = lastScope.eyeWidth ? half : static_cast<int32_t>(width) - half;
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
                                                              : XrRect2Di{{half, 0}, {rightW, static_cast<int32_t>(height)}};
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
            // The comfort vignette (GOAL A2): right over the game's image, under everything the host draws.
            if (vignetteOk && lastMeta.hasView)
                if (const XrCompositionLayerBaseHeader* vl = vignette.Layer(viewSpace)) layers[layerCount++] = vl;
            // M7: the reticle where the shot will land, along the aiming hand's ray (gameplay only).
            // Shown with the menu open too, so the Gun fit page can line the barrel up with it.
            if (reticleOk && lastMeta.hasView && menuHeadOk && (!menuOk || menu.RedDot())) {
                const std::uint32_t src = g_hdr->aimSource;
                const float d = g_hdr->aimDistance;
                if ((src == 2 || src == 3) && !(scopeOk && scope.Active())) {  // (not while looking through a scope)
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
                // The off-hand pistol's dot (OFFPISTOL-DESIGN 4.10): along this frame's off line, as far as the game's trace of
                // that line went -- the shots land on it.
                XrPosef offRay{};
                if (reticleOffOk && handsOk && offhandPistol.DotRay(offRay))
                    if (const XrCompositionLayerBaseHeader* rl = reticleOff.Layer(local, offRay, menuHead, g_hdr->pistolAimDistance))
                        layers[layerCount++] = rl;
            }
            // The scope's lens (SCOPE-DESIGN): the scope view through the drawn eyepiece, for the looking eye.
            if (scopeOk && lastMeta.hasView && !(prevLocal != XR_NULL_HANDLE && shown <= prevLocalUntil))  // (not mid-recentre)
                if (const XrCompositionLayerBaseHeader* sl = scope.Layer(local, last, shown, lastScope, eyeNow, eyesNowOk))
                    layers[layerCount++] = sl;
            // The wrist HUD: the rest quad (head-locked) and the two wrist panels -- counted before the markers (they take what
            // is left; the menu always fits in the last slot).
            // Mid-recentre only the LOCAL panels are dropped; the head-locked rest (hits, objectives, the fade) stays.
            if (wristOk)
                layerCount += static_cast<uint32_t>(wrist.Layers(local, viewSpace, layers + layerCount, 15 - static_cast<int>(layerCount),
                                                                 !(prevLocal != XR_NULL_HANDLE && shown <= prevLocalUntil)));
            // The gesture spots' rings ([Hands] Rings / the menu; all holsters while its Holsters page is open).
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
        if (mirrorOk) mirror.Update(ctx, last, lastMeta, shown > 0, lastScope.eyeWidth, wristOk ? wrist.MirrorHud() : nullptr);
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
