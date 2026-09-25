#include "hooks_d3d9.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>

#include "addresses.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr::hooks {
namespace {

using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);
PFN_Direct3DCreate9 g_real = nullptr;
std::atomic<int>    g_calls{0};

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdkVersion) {
    const int n = ++g_calls;
    IDirect3D9* d3d = g_real(sdkVersion);
    // Three callers in the engine (caps check, mode enumeration, device init); log them all
    // once -- the first proves the hook sits in front of device creation.
    if (n <= 8) MLOG("Direct3DCreate9 #%d (SDK 0x%X) -> %p", n, sdkVersion, static_cast<void*>(d3d));
    return d3d;
}

}  // namespace

bool InstallDirect3DCreate9() {
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
    if (!patch::SwapPointer(addr::kIatDirect3DCreate9, reinterpret_cast<void*>(real),
                            reinterpret_cast<void*>(&Hook_Direct3DCreate9))) {
        MLOG("hook Direct3DCreate9: IAT swap failed -- standing down");
        return false;
    }
    MLOG("hook Direct3DCreate9: installed (IAT 0x%08X, real %p)", addr::kIatDirect3DCreate9, reinterpret_cast<void*>(real));
    return true;
}

}  // namespace mohavr::hooks
