#include "build_check.hpp"

#include <windows.h>

#include <cstring>

#include "addresses.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr {
namespace {
bool g_ea = false;
}

bool IsEaBuild() { return g_ea; }

namespace {
const IMAGE_NT_HEADERS32* Nt() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    return reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
}

// D92/D95: is a SecuROM-spliced instruction the same code -- the site `jmp [ptr]` and the thunk it reaches exactly the
// original instruction followed by a jmp to the next one? Never written back: SecuROM's code reads these bytes as data.
bool g_spliceOk[sizeof(addr::kDiscSplices) / sizeof(addr::kDiscSplices[0])] = {};

bool VerifySplice(const addr::Splice& sp) {
    if (patch::BytesMatch(sp.va, sp.original, sp.size)) return false;  // (not spliced: the plain signature decides)
    const std::uintptr_t lo = addr::kImageBase, hi = addr::kImageBase + Nt()->OptionalHeader.SizeOfImage;
    const auto* site = reinterpret_cast<const std::uint8_t*>(sp.va);
    if (sp.size != 6 || site[0] != 0xFF || site[1] != 0x25) {
        MLOG("build check: disc -- %s is not a spliced jmp [ptr]", sp.name);
        return false;
    }
    std::uint32_t ptr = 0, thunk = 0;
    std::memcpy(&ptr, site + 2, 4);
    if (ptr < lo || ptr + 4 > hi) return false;
    std::memcpy(&thunk, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(ptr)), 4);
    if (thunk < lo || thunk + sp.size + 5 > hi) return false;
    const auto* t = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(thunk));
    std::int32_t rel = 0;
    std::memcpy(&rel, t + sp.size + 1, 4);
    const bool same = std::memcmp(t, sp.original, sp.size) == 0 && t[sp.size] == 0xE9 &&
                      thunk + sp.size + 5 + static_cast<std::uint32_t>(rel) == sp.va + sp.size;
    if (!same) {
        MLOG("build check: disc -- %s's thunk at 0x%08X is not the original instruction and a jmp back", sp.name, thunk);
        return false;
    }
    MLOG("build check: disc -- %s is SecuROM's splice of the same instruction (thunk 0x%08X; kept as it is)", sp.name, thunk);
    return true;
}
}  // namespace

bool CodeMatches(std::uintptr_t va, const std::uint8_t* expected, std::size_t size) {
    if (patch::BytesMatch(va, expected, size)) return true;
    if (size > 64) return false;
    std::uint8_t buf[64];
    if (!patch::BytesMatch(va, reinterpret_cast<const std::uint8_t*>(va), size)) return false;  // (readable)
    std::memcpy(buf, reinterpret_cast<const void*>(va), size);
    bool any = false;
    for (std::size_t i = 0; i < sizeof(addr::kDiscSplices) / sizeof(addr::kDiscSplices[0]); ++i) {
        const addr::Splice& sp = addr::kDiscSplices[i];
        if (!g_spliceOk[i] || sp.va + sp.size <= va || sp.va >= va + size) continue;
        for (std::size_t k = 0; k < sp.size; ++k) {
            const std::uintptr_t at = sp.va + k;
            if (at >= va && at < va + size) buf[at - va] = sp.original[k];
        }
        any = true;
    }
    return any && std::memcmp(buf, expected, size) == 0;
}

bool IsDiscWrapper() { return Nt()->OptionalHeader.AddressOfEntryPoint == addr::kDiscEntryRva; }

bool CheckBuild(int testMode) {
    const bool forceFail = testMode == 1;
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
    // D92: or the disc's no-DVD exe (its own header: a 2015 repack of the same build), checked once unpacked.
    const bool disc = nt->OptionalHeader.AddressOfEntryPoint == addr::kDiscEntryRva;
    const char* store = ea ? "EA app" : disc ? "disc (a no-DVD exe)" : "Steam";
    g_ea = ea;
    const std::uint32_t discStamp = forceFail ? (addr::kDiscTimeDateStamp ^ 1u) : addr::kDiscTimeDateStamp;
    struct Field { const char* name; std::uint32_t have, want; } fields[] = {
        {"TimeDateStamp", nt->FileHeader.TimeDateStamp,          disc ? discStamp : wantStamp},
        {"SizeOfImage",   nt->OptionalHeader.SizeOfImage,        ea ? addr::kEaSizeOfImage : disc ? addr::kDiscSizeOfImage : addr::kSizeOfImage},
        {"EntryPoint",    nt->OptionalHeader.AddressOfEntryPoint, ea ? addr::kEaEntryRva : disc ? addr::kDiscEntryRva : addr::kEntryRva},
    };
    bool ok = true;
    for (const auto& f : fields) {
        if (f.have != f.want) {
            MLOG("build check: FAIL -- %s is 0x%08X, expected 0x%08X", f.name, f.have, f.want);
            ok = false;
        }
    }
    // D80 (a tester's EA copy stood down on this alone): the header's CheckSum is the file's checksum, not the code's -- a
    // re-signed exe (the Authenticode signature is part of the file) or a header patcher that recomputes it (a 4 GB tool)
    // changes it with the code untouched. Logged, never a reason to stand down: the fields above and the signatures below
    // (every patched site's bytes) decide.
    {
        const std::uint32_t have = testMode == 2 ? (nt->OptionalHeader.CheckSum ^ 0x5604u) : nt->OptionalHeader.CheckSum;
        const std::uint32_t known = ea ? addr::kEaCheckSum : addr::kCheckSum;
        if (have != known && !disc)  // (the disc's repack has none)
            MLOG("build check: note -- the header's CheckSum is 0x%08X, not the known 0x%08X (a re-signed or header-patched exe: "
                 "the code is checked below)%s", have, known, testMode == 2 ? " [Debug.TestWrongBuild=2: simulated]" : "");
    }
    if (disc && ok)
        for (std::size_t i = 0; i < sizeof(addr::kDiscSplices) / sizeof(addr::kDiscSplices[0]); ++i)
            g_spliceOk[i] = VerifySplice(addr::kDiscSplices[i]);
    for (const auto& s : addr::kSignatures) {
        if (!CodeMatches(s.va, s.bytes, s.size)) {
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
