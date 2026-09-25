#include "hooks_d3d9.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <atomic>

#include "addresses.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "frame_capture.hpp"
#include "log.hpp"
#include "patch.hpp"
#include "vr_view.hpp"
#include "xr_session.hpp"

namespace mohavr::hooks {
namespace {

using PFN_Direct3DCreate9     = IDirect3D9*(WINAPI*)(UINT);
using PFN_Direct3DCreate9On12 = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);

PFN_Direct3DCreate9     g_real     = nullptr;
PFN_Direct3DCreate9On12 g_realOn12 = nullptr;
bool                    g_useOn12  = false;
Config                  g_cfg;
std::atomic<int>        g_calls{0};

// --- IDirect3D9::CreateDevice (vtable slot 16) -------------------------------------------
// The game calls it once from InitD3D9Device (0x1090339A), retrying with Sleep(500) while it
// returns DEVICELOST / NOTAVAILABLE (ENGINE-NOTES 5b). M2: observe; later: force the
// headset-sized windowed backbuffer here.
using PFN_CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
constexpr int     kSlotCreateDevice = 16;
PFN_CreateDevice  g_realCreateDevice = nullptr;
std::atomic<int>  g_createDeviceCalls{0};

// --- IDirect3DDevice9 vtable: CreateAdditionalSwapChain (13), Present (17) ------------------
// UE3 may present through additional swap chains (one per viewport); log which path is used.
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PFN_CreateAdditionalSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**);
using PFN_SwapChainPresent = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
constexpr int kSlotCreateAdditionalSwapChain = 13;
constexpr int kSlotReset                     = 16;
constexpr int kSlotPresent                   = 17;
constexpr int kSlotSwapChainPresent          = 3;

PFN_Present                   g_realPresent = nullptr;
PFN_CreateAdditionalSwapChain g_realCreateSwapChain = nullptr;
PFN_SwapChainPresent          g_realSwapChainPresent = nullptr;
std::atomic<long>             g_presents{0};
std::atomic<long>             g_scPresents{0};
std::atomic<int>              g_presentFailures{0};

void LogPresent(const char* what, long n, HRESULT hr) {
    // First frames, then a heartbeat every 600 frames; failures (first 10) always.
    if (FAILED(hr)) {
        if (++g_presentFailures <= 10) MLOG("%s #%ld FAILED -> 0x%08lX", what, n, static_cast<unsigned long>(hr));
    } else if (n <= 3 || n % 600 == 0) {
        MLOG("%s #%ld -> 0x%08lX", what, n, static_cast<unsigned long>(hr));
    }
}

// --- IDirect3DDevice9::Reset (slot 16) -----------------------------------------------------------
// Alt-tab out of fullscreen loses the device; the game then loops on Reset, which D3D9 refuses
// while ANY D3DPOOL_DEFAULT resource exists -- including the bridge's own render target. Found in
// headset round 2 (the game hung). Release ours first, recreate lazily after.
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
PFN_Reset        g_realReset = nullptr;
std::atomic<int> g_resets{0};

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    bridge::OnBeforeReset();
    const HRESULT hr = g_realReset(dev, pp);
    const int n = ++g_resets;
    if (n <= 5 || FAILED(hr))
        MLOG("Reset #%d %ux%u windowed %d -> 0x%08lX", n, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? pp->Windowed : 0, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) bridge::OnAfterReset(pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
    // Before Present: the finished frame is still in the backbuffer.
    capture::OnPresent(dev);
    bridge::OnPresent(dev);
    const HRESULT hr = g_realPresent(dev, src, dst, wnd, dirty);
    LogPresent("device Present", ++g_presents, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_SwapChainPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                                const RGNDATA* dirty, DWORD flags) {
    const HRESULT hr = g_realSwapChainPresent(sc, src, dst, wnd, dirty, flags);
    LogPresent("swapchain Present", ++g_scPresents, hr);
    return hr;
}

bool HookVtableSlot(void* obj, int slot, void* hook, void** real, const char* what) {
    auto** vtbl = *reinterpret_cast<void***>(obj);
    void* current = vtbl[slot];
    if (current == hook) return true;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(current), &owner)) {
        MLOG("hook %s: vtbl[%d] %p is not inside a module -- not hooking", what, slot, current);
        return false;
    }
    *real = current;
    if (!patch::SwapPointer(reinterpret_cast<std::uintptr_t>(&vtbl[slot]), current, hook)) {
        *real = nullptr;
        MLOG("hook %s: vtable swap failed", what);
        return false;
    }
    wchar_t name[MAX_PATH] = L"?";
    GetModuleFileNameW(owner, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    MLOG("hook %s: installed (vtbl %p slot %d, real %p in %ls)", what, static_cast<void*>(vtbl), slot, current,
         base ? base + 1 : name);
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_CreateAdditionalSwapChain(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp,
                                                         IDirect3DSwapChain9** out) {
    const HRESULT hr = g_realCreateSwapChain(dev, pp, out);
    MLOG("CreateAdditionalSwapChain %ux%u fmt %d swap %d windowed %d hwnd %p interval 0x%X -> 0x%08lX",
         pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0,
         pp ? pp->SwapEffect : 0, pp ? pp->Windowed : 0, pp ? static_cast<void*>(pp->hDeviceWindow) : nullptr,
         pp ? pp->PresentationInterval : 0, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && out && *out && !g_realSwapChainPresent) {
        HookVtableSlot(*out, kSlotSwapChainPresent, reinterpret_cast<void*>(&Hook_SwapChainPresent),
                       reinterpret_cast<void**>(&g_realSwapChainPresent), "IDirect3DSwapChain9::Present");
    }
    return hr;
}

void HookDevice(IDirect3DDevice9* dev) {
    if (!dev) return;
    capture::Init();
    if (!g_realPresent)
        HookVtableSlot(dev, kSlotPresent, reinterpret_cast<void*>(&Hook_Present),
                       reinterpret_cast<void**>(&g_realPresent), "IDirect3DDevice9::Present");
    if (!g_realCreateSwapChain)
        HookVtableSlot(dev, kSlotCreateAdditionalSwapChain, reinterpret_cast<void*>(&Hook_CreateAdditionalSwapChain),
                       reinterpret_cast<void**>(&g_realCreateSwapChain), "IDirect3DDevice9::CreateAdditionalSwapChain");
    if (!g_realReset)
        HookVtableSlot(dev, kSlotReset, reinterpret_cast<void*>(&Hook_Reset), reinterpret_cast<void**>(&g_realReset),
                       "IDirect3DDevice9::Reset");
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const int n = ++g_createDeviceCalls;
    const HRESULT hr = g_realCreateDevice(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out) HookDevice(*out);
    if (SUCCEEDED(hr)) {
        if (g_cfg.bridgeHost && g_useOn12) {
            bridge::StartHost(g_cfg.xrRuntimeJson, g_cfg.unitsPerMeter, g_cfg.bridgeMirror, g_cfg.controllers);  // D10: OpenXR out of process
            // M3: main thread, before the first CalcSceneView can run.
            static bool viewInstalled = false;
            if (!viewInstalled) { viewInstalled = true; view::Install(g_cfg); }
        }
        else if (g_cfg.xrEnabled && !g_cfg.bridgeHost) xr::Start(g_cfg.xrRuntimeJson);  // diagnostic, in process
    }
    // First few calls in full; after that only every 20th (the game's retry loop is 2/s).
    if (n <= 3 || n % 20 == 0) {
        MLOG("CreateDevice #%d adapter %u type %d flags 0x%lX -- %ux%u fmt %d count %u ms %d swap %d windowed %d "
             "autodepth %d/%d ppflags 0x%lX refresh %u interval 0x%X -> 0x%08lX",
             n, adapter, type, flags, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? pp->BackBufferFormat : 0, pp ? pp->BackBufferCount : 0, pp ? pp->MultiSampleType : 0,
             pp ? pp->SwapEffect : 0, pp ? pp->Windowed : 0, pp ? pp->EnableAutoDepthStencil : 0,
             pp ? pp->AutoDepthStencilFormat : 0, pp ? pp->Flags : 0, pp ? pp->FullScreen_RefreshRateInHz : 0,
             pp ? pp->PresentationInterval : 0, static_cast<unsigned long>(hr));
    }
    return hr;
}

void HookCreateDevice(IDirect3D9* d3d) {
    if (!d3d || g_realCreateDevice) return;
    auto** vtbl = *reinterpret_cast<void***>(d3d);
    const auto slot = reinterpret_cast<std::uintptr_t>(&vtbl[kSlotCreateDevice]);
    void* current = vtbl[kSlotCreateDevice];
    // The vtable lives in d3d9.dll (or d3d9on12's). Verify the slot points into a loaded image
    // before taking it over.
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(current), &owner)) {
        MLOG("hook CreateDevice: vtbl[16] %p is not inside a module -- not hooking", current);
        return;
    }
    wchar_t name[MAX_PATH] = L"?";
    GetModuleFileNameW(owner, name, MAX_PATH);
    g_realCreateDevice = reinterpret_cast<PFN_CreateDevice>(current);
    if (!patch::SwapPointer(slot, current, reinterpret_cast<void*>(&Hook_CreateDevice))) {
        g_realCreateDevice = nullptr;
        MLOG("hook CreateDevice: vtable swap failed");
        return;
    }
    const wchar_t* base = wcsrchr(name, L'\\');
    MLOG("hook CreateDevice: installed (vtbl %p slot 16, real %p in %ls)", static_cast<void*>(vtbl), current, base ? base + 1 : name);
}

IDirect3D9* CreateOn12(UINT sdkVersion) {
    // No D3D12 device supplied: 9On12 creates its own on the default adapter. M2 later passes
    // ours instead, so the bridge and the game share one device and queue.
    D3D9ON12_ARGS args{};
    args.Enable9On12 = TRUE;
    IDirect3D9* d3d = g_realOn12(sdkVersion, &args, 1);
    return d3d;
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdkVersion) {
    const int n = ++g_calls;
    IDirect3D9* d3d = nullptr;
    const char* via = "Direct3DCreate9";
    if (g_useOn12 && g_realOn12) {
        d3d = CreateOn12(sdkVersion);
        if (d3d) {
            via = "Direct3DCreate9On12";
        } else {
            MLOG("Direct3DCreate9On12 returned null -- falling back to plain Direct3DCreate9");
        }
    }
    if (!d3d) d3d = g_real(sdkVersion);
    // Three callers in the engine (caps check, mode enumeration, device init); in practice
    // one call (ENGINE-NOTES 5d). Log the first few.
    if (n <= 8) MLOG("Direct3DCreate9 #%d (SDK 0x%X) via %s -> %p", n, sdkVersion, via, static_cast<void*>(d3d));
    HookCreateDevice(d3d);
    return d3d;
}

}  // namespace

bool InstallDirect3DCreate9(const Config& cfg) {
    g_cfg = cfg;
    const bool useD3D9On12 = cfg.d3d9On12;
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        MLOG("hook Direct3DCreate9: d3d9.dll not loaded yet -- standing down");
        return false;
    }
    auto real = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(d3d9, "Direct3DCreate9"));
    auto* slot = reinterpret_cast<void**>(addr::kIatDirect3DCreate9);
    if (!real) {
        MLOG("hook Direct3DCreate9: GetProcAddress failed -- standing down");
        return false;
    }
    // Verify-before-patch: the slot must hold exactly the real export (standing rule 4).
    if (!patch::BytesMatch(addr::kIatDirect3DCreate9, reinterpret_cast<const std::uint8_t*>(&real), sizeof(real))) {
        MLOG("hook Direct3DCreate9: IAT slot 0x%08X holds %p, expected d3d9!Direct3DCreate9 %p -- standing down",
             addr::kIatDirect3DCreate9, *slot, reinterpret_cast<void*>(real));
        return false;
    }
    g_real = real;
    if (useD3D9On12) {
        g_realOn12 = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(d3d9, "Direct3DCreate9On12"));
        g_useOn12 = g_realOn12 != nullptr;
        MLOG("Bridge.D3D9On12=1: Direct3DCreate9On12 %s", g_realOn12 ? "available" : "NOT exported by this d3d9.dll -- plain D3D9 instead");
    }
    if (!patch::SwapPointer(addr::kIatDirect3DCreate9, reinterpret_cast<void*>(real),
                            reinterpret_cast<void*>(&Hook_Direct3DCreate9))) {
        MLOG("hook Direct3DCreate9: IAT swap failed -- standing down");
        return false;
    }
    MLOG("hook Direct3DCreate9: installed (IAT 0x%08X, real %p)", addr::kIatDirect3DCreate9, reinterpret_cast<void*>(real));
    return true;
}

}  // namespace mohavr::hooks
