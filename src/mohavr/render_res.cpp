#include "render_res.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "addresses.hpp"
#include "config.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr::render {
namespace {

std::wstring g_cmdLine;  // what the game gets from GetCommandLineW (lives for the process)

LPWSTR WINAPI Hook_GetCommandLineW() { return g_cmdLine.data(); }

// The EXE's import slot for kernel32!name, found by walking the game's own import table (by name, not ordinal): the one
// at kGameImportDirRva, which the EA app's exe keeps although its header names the DRM's table (D58).
void** FindImportSlot(HMODULE exe, const char* dll, const char* name) {
    auto* base = reinterpret_cast<std::uint8_t*>(exe);
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + addr::kGameImportDirRva); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        if (!d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char*>(ibn->Name), name)) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

}  // namespace

bool InstallResolution(const Config& cfg) {
    if (cfg.renderResX <= 0 || cfg.renderResY <= 0) return false;
    HMODULE exe = GetModuleHandleW(nullptr);
    void** slot = FindImportSlot(exe, "KERNEL32.dll", "GetCommandLineW");
    void* real = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCommandLineW"));
    if (!slot || !real || *slot != real) {
        MLOG("render: GetCommandLineW import %s -- Render.ResX/ResY not applied", !slot ? "not found" : "not the expected function");
        return false;
    }
    // Insert right after the program path, so ours are the first ResX=/ResY= the engine's Parse finds.
    const std::wstring orig = GetCommandLineW();
    size_t cut = 0;
    if (!orig.empty() && orig[0] == L'"') {
        cut = orig.find(L'"', 1);
        cut = cut == std::wstring::npos ? orig.size() : cut + 1;
    } else {
        cut = orig.find(L' ');
        if (cut == std::wstring::npos) cut = orig.size();
    }
    g_cmdLine = orig.substr(0, cut) + L" -windowed ResX=" + std::to_wstring(cfg.renderResX) + L" ResY=" +
                std::to_wstring(cfg.renderResY) + orig.substr(cut);
    if (!patch::SwapPointer(reinterpret_cast<std::uintptr_t>(slot), real, reinterpret_cast<void*>(&Hook_GetCommandLineW))) {
        MLOG("render: import swap failed -- Render.ResX/ResY not applied");
        return false;
    }
    MLOG("render: the game sees ResX=%d ResY=%d (windowed): each eye renders %dx%d -- %s", cfg.renderResX, cfg.renderResY,
         cfg.renderResX / 2, cfg.renderResY, cfg.renderPreset.c_str());
    return true;
}

}  // namespace mohavr::render
