// MOHAVR entry. Loaded by the Windows loader as MOHA's dinput8.dll (D5), so this runs before
// the SteamStub entry and before WinMain -- early enough to hook Direct3DCreate9.
//
// DllMain does only loader-lock-safe work: open the log, read the ini, verify the build,
// patch one IAT slot. No LoadLibrary, no threads.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "build_check.hpp"
#include "config.hpp"
#include "crash_dump.hpp"
#include "hooks_d3d9.hpp"
#include "log.hpp"
#include "render_res.hpp"
#include "xinput_hook.hpp"

#define MOHAVR_VERSION "0.8.1"

namespace {

std::wstring ModuleDir(HMODULE self) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path, n);
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash);
}

std::string Narrow(const wchar_t* w) {
    char buf[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

void Init(HMODULE self) {
    // Paths are anchored to this DLL's folder: the game hasn't set its working directory yet
    // (lessons 2).
    const std::wstring dir = ModuleDir(self);
    mohavr::log::Open(dir);
    MLOG("MOHAVR %s (built %s %s) -- pid %lu", MOHAVR_VERSION, __DATE__, __TIME__, GetCurrentProcessId());
    MLOG("command line: %s", Narrow(GetCommandLineW()).c_str());

    const mohavr::Config cfg = mohavr::LoadConfig(dir);
    if (!cfg.enabled) {
        MLOG("STAND DOWN: General.Enabled=0 -- acting as a plain dinput8 proxy");
        return;
    }
    if (!mohavr::CheckBuild(cfg.testWrongBuild)) {
        MLOG("STAND DOWN: MOHA.exe is not the pinned build -- no hooks installed, game runs unmodded");
        return;
    }
    {
        // D57: whether MOHA.exe is large address aware (the setup's optional step): 4 GB of address space, else 2 GB -- the
        // first mission in VR comes near 2 GB.
        const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        MEMORYSTATUSEX ms{sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        MLOG("memory: MOHA.exe large address aware: %s -- %llu MB of address space",
             (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "yes" : "no (the setup's 4 GB option is off)",
             static_cast<unsigned long long>(ms.ullTotalVirtual >> 20));
        if (cfg.debugReserveLow > 0) {  // tests: push the game's and the mod's allocations above 2 GB
            std::size_t want = static_cast<std::size_t>(cfg.debugReserveLow) << 20, got = 0;
            int blocks = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            for (std::uintptr_t a = 0x10000; a < 0x7FFF0000 && got < want && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
                 a = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
                if (mbi.State != MEM_FREE) continue;
                // (a reservation starts on 64 KB; the low 2 GB only)
                const std::uintptr_t lo = (reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + 0xFFFF) & ~static_cast<std::uintptr_t>(0xFFFF);
                const std::uintptr_t hi = (std::min)(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + static_cast<std::uintptr_t>(mbi.RegionSize),
                                                     static_cast<std::uintptr_t>(0x80000000u)) & ~static_cast<std::uintptr_t>(0xFFFF);
                if (hi <= lo || hi - lo < (1u << 20)) continue;
                const std::size_t take = (std::min)(static_cast<std::size_t>(hi - lo), want - got);
                if (VirtualAlloc(reinterpret_cast<void*>(lo), take, MEM_RESERVE, PAGE_NOACCESS)) got += take, ++blocks;
            }
            MLOG("memory: Debug.ReserveLow -- %zu MB of the low 2 GB reserved in %d blocks (the rest allocates higher)", got >> 20, blocks);
        }
    }
    mohavr::crashdump::Install(cfg);
    if (cfg.hookD3D9) mohavr::hooks::InstallDirect3DCreate9(cfg);
    mohavr::render::InstallResolution(cfg);
    if (cfg.controllers && cfg.bridgeHost) mohavr::xinput::Install();
    MLOG("init done in DllMain (%.1f ms)", mohavr::log::MsSinceStart());
}

}  // namespace

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        Init(self);
    }
    return TRUE;
}
