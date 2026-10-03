#include "frame_capture.hpp"

#include <windows.h>
#include <d3d9.h>

#include <string>

#include "hudtex.hpp"
#include "log.hpp"

namespace mohavr::capture {
namespace {

HANDLE       g_request = nullptr;
std::wstring g_dir;
int          g_count = 0;

bool WriteBmp(const std::wstring& path, const D3DLOCKED_RECT& lr, UINT w, UINT h) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h);  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    const DWORD rowBytes = w * 4;
    ih.biSizeImage = rowBytes * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &wr, nullptr) && WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
    const auto* src = static_cast<const BYTE*>(lr.pBits);
    for (UINT y = 0; ok && y < h; ++y) ok = WriteFile(f, src + static_cast<size_t>(y) * lr.Pitch, rowBytes, &wr, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

void Capture(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(hr)) hr = bb->GetDesc(&d);
    if (SUCCEEDED(hr) && d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) hr = E_NOTIMPL;
    if (SUCCEEDED(hr)) hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(bb, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        const std::wstring tmp = g_dir + L"\\capture.tmp";
        const std::wstring out = g_dir + L"\\capture.bmp";
        const bool ok = WriteBmp(tmp, lr, d.Width, d.Height);
        sys->UnlockRect();
        if (ok && MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (++g_count <= 3) MLOG("capture: backbuffer %ux%u fmt %d -> %%TEMP%%\\MOHAVR\\capture.bmp", d.Width, d.Height, d.Format);
        } else {
            MLOG("capture: writing the BMP failed (error %lu)", GetLastError());
        }
    } else {
        MLOG("capture: failed -> 0x%08lX (format %d, multisample %d)", static_cast<unsigned long>(hr), d.Format, d.MultiSampleType);
    }
    if (sys) sys->Release();
    if (bb) bb->Release();
    hudtex::OnCapture(dev, g_dir);  // the wrist HUD's texture beside it (capture-hud.bmp)
}

}  // namespace

void Init() {
    if (g_request) return;
    wchar_t tmp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    g_dir = std::wstring(tmp, n) + L"MOHAVR";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_request = CreateEventW(nullptr, FALSE, FALSE, L"Local\\MOHAVR_Capture");  // auto-reset
    MLOG("capture: %s (event Local\\MOHAVR_Capture)", g_request ? "ready" : "event creation FAILED");
}

void OnPresent(IDirect3DDevice9* device) {
    if (g_request && WaitForSingleObject(g_request, 0) == WAIT_OBJECT_0) Capture(device);
}

}  // namespace mohavr::capture
