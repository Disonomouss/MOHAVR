#include "build_check.hpp"

#include <windows.h>

#include "addresses.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr {
namespace {
bool g_ea = false;
}

bool IsEaBuild() { return g_ea; }

bool CheckBuild(bool forceFail) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base != addr::kImageBase) {
        MLOG("build check: FAIL -- exe loaded at 0x%08X, expected 0x%08X", base, addr::kImageBase);
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);

    const std::uint32_t wantStamp = forceFail ? (addr::kTimeDateStamp ^ 1u) : addr::kTimeDateStamp;
    // D58: the same build under one of two wrappers -- Steam's (SteamStub) or the EA app's (OOA). The wrapper decides the
    // header fields; the signatures below are the same for both.
    const bool ea = nt->OptionalHeader.AddressOfEntryPoint == addr::kEaEntryRva;
    const char* store = ea ? "EA app" : "Steam";
    g_ea = ea;
    struct Field { const char* name; std::uint32_t have, want; } fields[] = {
        {"TimeDateStamp", nt->FileHeader.TimeDateStamp,          wantStamp},
        {"SizeOfImage",   nt->OptionalHeader.SizeOfImage,        ea ? addr::kEaSizeOfImage : addr::kSizeOfImage},
        {"CheckSum",      nt->OptionalHeader.CheckSum,           ea ? addr::kEaCheckSum : addr::kCheckSum},
        {"EntryPoint",    nt->OptionalHeader.AddressOfEntryPoint, ea ? addr::kEaEntryRva : addr::kEntryRva},
    };
    bool ok = true;
    for (const auto& f : fields) {
        if (f.have != f.want) {
            MLOG("build check: FAIL -- %s is 0x%08X, expected 0x%08X", f.name, f.have, f.want);
            ok = false;
        }
    }
    for (const auto& s : addr::kSignatures) {
        if (!patch::BytesMatch(s.va, s.bytes, s.size)) {
            MLOG("build check: FAIL -- signature '%s' at 0x%08X does not match", s.name, s.va);
            ok = false;
        }
    }
    if (forceFail) MLOG("build check: Debug.TestWrongBuild=1 -- mismatch simulated");
    if (ok) {
        MLOG("build check: OK -- MOHA.exe build 3648, the %s's (timestamp 0x%08X, %u signatures)", store,
             addr::kTimeDateStamp, static_cast<unsigned>(sizeof(addr::kSignatures) / sizeof(addr::kSignatures[0])));
    }
    return ok;
}

}  // namespace mohavr
