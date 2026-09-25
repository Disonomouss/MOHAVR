// Offline smoke test for the dinput8 proxy, run in a 32-bit process that is NOT MOHA:
//   - the proxy must stand down (build check fails: wrong image base) and log why;
//   - DirectInput8Create must still forward to the system DLL and succeed.
//
//   smoke.exe <path to dinput8.dll>
#include <windows.h>
#include <cstdio>

// IID_IDirectInput8W {BF798031-483A-4DA2-AA99-5D64ED369700}
static const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("usage: smoke <dinput8.dll>\n"); return 2; }
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { std::printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    auto create = reinterpret_cast<Fn>(GetProcAddress(h, "DirectInput8Create"));
    if (!create) { std::printf("FAIL no DirectInput8Create export\n"); return 1; }
    void* di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), 0x0800, kIID_IDirectInput8W, &di, nullptr);
    std::printf("DirectInput8Create -> 0x%08lX, object %p\n", static_cast<unsigned long>(hr), di);
    if (FAILED(hr) || !di) return 1;
    static_cast<IUnknown*>(di)->Release();
    std::printf("PASS\n");
    return 0;
}
