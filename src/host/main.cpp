// MOHAVR-host.exe -- the 64-bit side of the bridge (D10).
//
// Started by the game-side mod (dinput8.dll) with:  --game-pid <pid> [--runtime-json "<path>"]
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
#include <string>
#include <vector>

#include "../common/shared_frame.hpp"
#include "../mohavr/log.hpp"

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

int Run(DWORD gamePid, const std::wstring& runtimeJson) {
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
    SetState(HostState::Running, "session created");

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
    long xrFrames = 0;
    long newFrames = 0;
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

        // Head pose + eye views at the predicted display time -> the game (seqlock, M3).
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
            if (headOk && viewsOk) {
                auto toPose = [](const XrPosef& p) {
                    return mohavr::shared::Pose{p.position.x, p.position.y, p.position.z,
                                                p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
                };
                auto toFov = [](const XrFovf& f) {
                    return mohavr::shared::Fov{std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};
                };
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));  // odd: writing
                g_hdr->viewDisplayTime = fs.predictedDisplayTime;
                g_hdr->head = toPose(headLoc.pose);
                for (int e = 0; e < 2; ++e) {
                    g_hdr->eye[e] = toPose(views[e].pose);
                    g_hdr->eyeFov[e] = toFov(views[e].fov);
                }
                // Position TRACKED, not just VALID: before the headset reports real tracking, runtimes
                // hand out a placeholder pose (Virtual Desktop: identity at y = -1.187), which the game
                // must not take as its origin (headset round 2: the camera ended up ~1.2 m too high).
                const bool posTracked = (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
                                        (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
                g_hdr->viewValid = 1u | (posTracked ? 2u : 0u);
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_hdr->viewSeq));  // even: done
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

        // Take the newest game frame, if there is one we haven't taken (protocol: shared_frame.hpp).
        const std::uint64_t f = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_hdr->publishedFrame), 0, 0));
        if (f > shown) {
            const UINT slot = static_cast<UINT>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_hdr->publishedSlot), 0, 0));
            lastMeta = g_hdr->slotMeta[slot % kRing];  // written by the game before it published f
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
        }

        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        const XrCompositionLayerBaseHeader* layers[1];
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
                        pviews[e].subImage.imageRect = e == 0 ? XrRect2Di{{0, 0}, {half, static_cast<int32_t>(height)}}
                                                              : XrRect2Di{{half, 0}, {static_cast<int32_t>(width) - half,
                                                                                      static_cast<int32_t>(height)}};
                        pviews[e].subImage.imageArrayIndex = 0;
                    } else {
                        pviews[e].subImage.imageRect = full;
                        pviews[e].subImage.imageArrayIndex = e;
                    }
                }
                proj.space = local;
                proj.viewCount = 2;
                proj.views = pviews;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            } else {
                layer.space = local;
                layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                layer.subImage.swapchain = swapchain;
                layer.subImage.imageRect = full;
                layer.pose.orientation.w = 1.0f;
                layer.pose.position = {0.0f, 0.0f, -2.0f};
                layer.size = {1.6f, 1.6f * static_cast<float>(height) / static_cast<float>(width)};
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            }
            layerCount = 1;
        }
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
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--game-pid")) gamePid = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (!wcscmp(argv[i], L"--runtime-json")) runtimeJson = argv[++i];
    }
    LocalFree(argv);
    if (!gamePid) { MLOG("host: no --game-pid -- nothing to do"); return 2; }
    MLOG("host: game pid %lu", gamePid);
    const int rc = Run(gamePid, runtimeJson);
    MLOG("host: exit %d", rc);
    return rc;
}
