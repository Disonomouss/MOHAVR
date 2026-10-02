#include "mirror.hpp"

#include <algorithm>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {

constexpr wchar_t kClass[] = L"MOHAVR_Mirror";
constexpr wchar_t kTitle[] = L"MOHAVR mirror";

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const bool overlay = GetWindowLongPtrW(h, GWLP_USERDATA) == 1;
    switch (msg) {
    case WM_NCHITTEST:
        if (overlay) return HTTRANSPARENT;
        break;
    case WM_MOUSEACTIVATE:
        if (overlay) return MA_NOACTIVATE;
        break;
    case WM_CLOSE:
        // Closing the mirror window only hides it -- the host (and VR) carry on.
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

struct FindCtx {
    DWORD pid;
    HWND  best;
    long  bestArea;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r{};
    GetClientRect(h, &r);
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) {
        c->best = h;
        c->bestArea = area;
    }
    return TRUE;
}

}  // namespace

bool Mirror::Init(ID3D11Device* dev, DWORD gamePid, int mode) {
    dev_ = dev;
    gamePid_ = gamePid;
    mode_ = mode;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    if (mode_ == 1) {
        // Layered + transparent = mouse clicks go through to the game; no-activate = it never takes focus.
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                kClass, kTitle, WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd_) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    } else {
        RECT r{0, 0, 960, 540};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        hwnd_ = CreateWindowExW(0, kClass, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!hwnd_) {
        MLOG("mirror: CreateWindow failed (%lu) -- no mirror", GetLastError());
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, mode_);

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = dev_->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        // Blt model: flip-model swapchains can't present to a layered window. 16x16 until the first frame.
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = bufW_ = 16;
        sd.Height = bufH_ = 16;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChainForHwnd(dev_, hwnd_, &sd, nullptr, nullptr, &swap_);
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    if (FAILED(hr)) {
        MLOG("mirror: swapchain failed (0x%08lX) -- no mirror", static_cast<unsigned long>(hr));
        Shutdown();
        return false;
    }
    if (mode_ == 2) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);  // never steal the game's focus
    MLOG("mirror: %s", mode_ == 1 ? "over the game window (click-through, shown while the game is in front)"
                                  : "own window \"MOHAVR mirror\"");
    return true;
}

HWND Mirror::FindGameWindow() const {
    FindCtx c{gamePid_, nullptr, 0};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

bool Mirror::Place() {
    if (!game_ || !IsWindow(game_)) {
        game_ = nullptr;
        const DWORD now = GetTickCount();
        if (static_cast<LONG>(now - nextFind_) < 0) return false;
        nextFind_ = now + 1000;
        game_ = FindGameWindow();
        if (!game_) return false;
        MLOG("mirror: game window %p", static_cast<void*>(game_));
    }
    if (GetForegroundWindow() != game_ || IsIconic(game_)) return false;
    RECT c{};
    GetClientRect(game_, &c);
    POINT tl{0, 0};
    ClientToScreen(game_, &tl);
    const RECT want{tl.x, tl.y, tl.x + c.right, tl.y + c.bottom};
    if (want.right - want.left < 16 || want.bottom - want.top < 16) return false;
    if (!shown_ || !EqualRect(&want, &placed_)) {
        SetWindowPos(hwnd_, HWND_TOPMOST, want.left, want.top, want.right - want.left, want.bottom - want.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        placed_ = want;
    }
    return true;
}

void Mirror::Update(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const shared::SlotMeta& meta, bool haveFrame,
                    std::uint32_t eyeWidth) {
    if (!swap_) return;
    MSG m;
    while (PeekMessageW(&m, hwnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&m);

    if (mode_ == 1) {
        const bool show = haveFrame && Place();
        if (!show) {
            if (shown_) ShowWindow(hwnd_, SW_HIDE);
            shown_ = false;
            return;
        }
        shown_ = true;
    } else if (!haveFrame || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        return;
    }

    // Source: the left eye (left half) in stereo, else the whole frame -- cropped to the window's shape.
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    const UINT srcW = meta.stereo ? (eyeWidth ? eyeWidth : fd.Width / 2) : fd.Width, srcH = fd.Height;
    RECT wr{};
    GetClientRect(hwnd_, &wr);
    const float aspect = (wr.right > 0 && wr.bottom > 0) ? static_cast<float>(wr.right) / static_cast<float>(wr.bottom)
                                                         : 16.0f / 9.0f;
    UINT w = srcW, h = static_cast<UINT>(static_cast<float>(srcW) / aspect + 0.5f);
    if (h > srcH) {
        h = srcH;
        w = std::min(srcW, static_cast<UINT>(static_cast<float>(srcH) * aspect + 0.5f));
    }
    w = std::max(w, 16u);
    h = std::max(h, 16u);
    if (w != bufW_ || h != bufH_) {
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
        bufW_ = w;
        bufH_ = h;
    }
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    const UINT x0 = (srcW - w) / 2, y0 = (srcH - h) / 2;
    const D3D11_BOX box{x0, y0, 0, x0 + w, y0 + h, 1};
    ctx->CopySubresourceRegion(back, 0, 0, 0, 0, frame, 0, &box);
    back->Release();
    const HRESULT hr = swap_->Present(0, 0);
    if (++presents_ == 1) MLOG("mirror: first present (%ux%u of %ux%u, %s) -> 0x%08lX", w, h, fd.Width, fd.Height,
                               meta.stereo ? "left eye" : "mono", static_cast<unsigned long>(hr));
}

void Mirror::Shutdown() {
    if (swap_) swap_->Release();
    swap_ = nullptr;
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

}  // namespace mohavr::host
