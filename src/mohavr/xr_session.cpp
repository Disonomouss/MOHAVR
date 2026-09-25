#include "xr_session.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <vector>

#include "log.hpp"

namespace mohavr::xr {
namespace {

std::atomic<bool> g_started{false};
std::wstring      g_runtimeJson;

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

#define XR_CHECK(call)                                                                  \
    do {                                                                                \
        const XrResult r_ = (call);                                                     \
        if (XR_FAILED(r_)) { MLOG("xr: %s failed -> %d -- XR thread stops", #call, r_); return; } \
    } while (0)

struct Quad {
    XrSwapchain                           swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*>  rtvs;
    int32_t                               width = 1920, height = 1080;
};

void Run() {
    MLOG("xr: thread started");
    if (!g_runtimeJson.empty()) {
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", g_runtimeJson.c_str());
        char buf[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, g_runtimeJson.c_str(), -1, buf, MAX_PATH, nullptr, nullptr);
        MLOG("xr: XR_RUNTIME_JSON for this process = %s", buf);
    }

    // --- instance ---------------------------------------------------------------------------
    uint32_t extCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()));
    bool hasD3D11 = false;
    for (const auto& e : exts) hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!hasD3D11) { MLOG("xr: runtime lacks %s -- XR thread stops", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return; }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "MOHAVR");
    strcpy_s(ici.applicationInfo.engineName, "Unreal Engine 3 (MOHA)");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xrCreateInstance(&ici, &instance));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &ip);
    MLOG("xr: instance created -- runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_CHECK(xrGetSystem(instance, &sgi, &system));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &sp);
    MLOG("xr: system \"%s\" (max swapchain %ux%u)", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth,
         sp.graphicsProperties.maxSwapchainImageHeight);

    // --- D3D11 device on the runtime's adapter ----------------------------------------------
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(getReqs(instance, system, &reqs));

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(reqs.adapterLuid, IID_PPV_ARGS(&adapter)))) {
        MLOG("xr: could not find the runtime's adapter -- XR thread stops");
        return;
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    char adName[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, adName, sizeof(adName), nullptr, nullptr);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    adapter->Release();
    factory->Release();
    if (FAILED(hr)) { MLOG("xr: D3D11CreateDevice failed -> 0x%08lX -- XR thread stops", static_cast<unsigned long>(hr)); return; }
    MLOG("xr: D3D11 device on \"%s\" (feature level 0x%X)", adName, device->GetFeatureLevel());

    // --- session ----------------------------------------------------------------------------
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xrCreateSession(instance, &sci, &session));
    MLOG("xr: session created");

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace local = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &local));

    // --- quad swapchain ---------------------------------------------------------------------
    uint32_t fmtCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr));
    std::vector<int64_t> fmts(fmtCount);
    XR_CHECK(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data()));
    int64_t fmt = fmts.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM : fmts[0];
    for (int64_t f : fmts) {
        if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { fmt = f; break; }
    }
    Quad quad;
    XrSwapchainCreateInfo swci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swci.format = fmt;
    swci.sampleCount = 1;
    swci.width = static_cast<uint32_t>(quad.width);
    swci.height = static_cast<uint32_t>(quad.height);
    swci.faceCount = 1;
    swci.arraySize = 1;
    swci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(session, &swci, &quad.swapchain));
    uint32_t imgCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(quad.swapchain, 0, &imgCount, nullptr));
    quad.images.assign(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(quad.swapchain, imgCount, &imgCount,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(quad.images.data())));
    for (auto& img : quad.images) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = static_cast<DXGI_FORMAT>(fmt);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        device->CreateRenderTargetView(img.texture, &rd, &rtv);
        quad.rtvs.push_back(rtv);
    }
    MLOG("xr: quad swapchain %dx%d format %lld, %u images", quad.width, quad.height, fmt, imgCount);

    // --- event + frame loop -----------------------------------------------------------------
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    long frames = 0;
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                MLOG("xr: session state -> %s", StateName(state));
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(session, &bi));
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    MLOG("xr: session ending (%s) -- XR thread stops", StateName(state));
                    return;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                MLOG("xr: instance loss pending -- XR thread stops");
                return;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { Sleep(50); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR_CHECK(xrWaitFrame(session, nullptr, &fs));
        XR_CHECK(xrBeginFrame(session, nullptr));

        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[1];
        uint32_t layerCount = 0;
        if (fs.shouldRender) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XR_CHECK(xrAcquireSwapchainImage(quad.swapchain, &ai, &idx));
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xrWaitSwapchainImage(quad.swapchain, &wi));
            // Test pattern: a slow colour cycle, so a capture proves frames are advancing.
            const float t = static_cast<float>(frames) / 90.0f;
            const float color[4] = {0.5f + 0.5f * std::sin(t), 0.5f + 0.5f * std::sin(t + 2.1f),
                                    0.5f + 0.5f * std::sin(t + 4.2f), 1.0f};
            ctx->ClearRenderTargetView(quad.rtvs[idx], color);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            XR_CHECK(xrReleaseSwapchainImage(quad.swapchain, &ri));

            layer.space = local;
            layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            layer.subImage.swapchain = quad.swapchain;
            layer.subImage.imageRect = {{0, 0}, {quad.width, quad.height}};
            layer.pose.orientation.w = 1.0f;
            layer.pose.position = {0.0f, 0.0f, -2.0f};  // 2 m in front of where the head started
            layer.size = {1.6f, 0.9f};                   // 16:9, like the game's frame
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            layerCount = 1;
        }
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = layerCount;
        fe.layers = layers;
        XR_CHECK(xrEndFrame(session, &fe));

        ++frames;
        if (frames == 1) MLOG("xr: first frame submitted (quad layer)");
        else if (frames % 900 == 0) MLOG("xr: %ld frames submitted", frames);
    }
}

DWORD WINAPI ThreadMain(LPVOID) {
    Run();
    return 0;
}

}  // namespace

void Start(const std::wstring& runtimeJson) {
    if (g_started.exchange(true)) return;
    g_runtimeJson = runtimeJson;
    HANDLE t = CreateThread(nullptr, 0, &ThreadMain, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else MLOG("xr: CreateThread failed (%lu)", GetLastError());
}

}  // namespace mohavr::xr
