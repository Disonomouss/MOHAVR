#include "patch.hpp"

#include <windows.h>

#include <cstring>

namespace mohavr::patch {

namespace {

bool Readable(std::uintptr_t va, std::size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return va + size <= end;
}

}  // namespace

bool BytesMatch(std::uintptr_t va, const std::uint8_t* expected, std::size_t size) {
    if (!Readable(va, size)) return false;
    return std::memcmp(reinterpret_cast<const void*>(va), expected, size) == 0;
}

bool SwapPointer(std::uintptr_t slot, void* expected, void* replacement) {
    if (!Readable(slot, sizeof(void*))) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), PAGE_READWRITE, &old)) return false;
    void* seen = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), replacement, expected);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(void*), old, &tmp);
    return seen == expected;
}

bool WriteBytes(std::uintptr_t va, const std::uint8_t* expected, const std::uint8_t* replacement, std::size_t size) {
    if (!BytesMatch(va, expected, size)) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(va), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(va), replacement, size);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(va), size, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(va), size);
    return true;
}

}  // namespace mohavr::patch
