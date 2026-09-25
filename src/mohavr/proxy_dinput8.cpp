// dinput8.dll proxy (D5). The game imports only DirectInput8Create; the other exports are
// forwarded too so anything else that loads "dinput8.dll" from this folder keeps working.
//
// The real DLL is loaded lazily on first use, never from DllMain (no LoadLibrary under the
// loader lock). GetSystemDirectoryW returns SysWOW64 in this 32-bit process.
#include <windows.h>
#include <unknwn.h>

#include <atomic>

#include "log.hpp"

namespace {

HMODULE LoadReal() {
    static std::atomic<HMODULE> real{nullptr};
    HMODULE h = real.load();
    if (h) return h;
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return nullptr;
    lstrcatW(path, L"\\dinput8.dll");
    h = LoadLibraryW(path);
    if (h) {
        real.store(h);
        MLOG("dinput8 proxy: loaded system dinput8.dll");
    } else {
        MLOG("dinput8 proxy: FAILED to load system dinput8.dll (error %lu)", GetLastError());
    }
    return h;
}

template <typename Fn>
Fn Real(const char* name) {
    HMODULE h = LoadReal();
    return h ? reinterpret_cast<Fn>(GetProcAddress(h, name)) : nullptr;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    static std::atomic<int> calls{0};
    const int n = ++calls;
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    static Fn real = Real<Fn>("DirectInput8Create");
    const HRESULT hr = real ? real(inst, version, riid, out, outer) : E_FAIL;
    if (n <= 4) MLOG("DirectInput8Create #%d (version 0x%04lX) -> 0x%08lX", n, version, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    static Fn real = Real<Fn>("DllCanUnloadNow");
    return real ? real() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    static Fn real = Real<Fn>("DllGetClassObject");
    return real ? real(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    static Fn real = Real<Fn>("DllRegisterServer");
    return real ? real() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    static Fn real = Real<Fn>("DllUnregisterServer");
    return real ? real() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
    using Fn = const void*(WINAPI*)();
    static Fn real = Real<Fn>("GetdfDIJoystick");
    return real ? real() : nullptr;
}

}  // extern "C"
