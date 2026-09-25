// MOHAVR entry. Loaded by the Windows loader as MOHA's dinput8.dll (D5), so this runs before
// the SteamStub entry and before WinMain -- early enough to hook Direct3DCreate9.
//
// DllMain does only loader-lock-safe work: open the log, read the ini, verify the build,
// patch one IAT slot. No LoadLibrary, no threads.
#include <windows.h>

#include <string>

#include "build_check.hpp"
#include "config.hpp"
#include "hooks_d3d9.hpp"
#include "log.hpp"

#define MOHAVR_VERSION "0.3.0-m2c"

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
    if (cfg.hookD3D9) mohavr::hooks::InstallDirect3DCreate9(cfg);
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
