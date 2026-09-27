#include "names.hpp"

#include <windows.h>

#include <cstdio>

#include "addresses.hpp"

namespace mohavr::names {
namespace {

// Copies the FName at `fname` into `out` (ASCII; other characters as '?'). No C++ objects here: SEH only.
bool ReadFName(std::uintptr_t fname, char* out, int cap) {
    __try {
        const int index = *reinterpret_cast<const int*>(fname);
        const int number = *reinterpret_cast<const int*>(fname + 4);
        const auto names = *reinterpret_cast<const std::uintptr_t*>(addr::kGNamesData);
        if (!names || index < 0 || index > 0x400000) return false;
        const auto entry = *reinterpret_cast<const std::uintptr_t*>(names + static_cast<std::uintptr_t>(index) * 4);
        if (!entry) return false;
        const auto* s = reinterpret_cast<const wchar_t*>(entry + addr::kNameEntryString);
        int n = 0;
        for (; n < cap - 12 && s[n]; ++n) out[n] = s[n] < 128 ? static_cast<char>(s[n]) : '?';
        out[n] = 0;
        if (number > 0) sprintf_s(out + n, static_cast<size_t>(cap - n), "_%d", number - 1);
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t ReadPtr(std::uintptr_t at) {
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

std::string Name(std::uintptr_t object) {
    char buf[128];
    if (!object || !ReadFName(object + addr::kObjectName, buf, sizeof(buf))) return {};
    return buf;
}

std::string ClassName(std::uintptr_t object) {
    return object ? Name(ReadPtr(object + addr::kObjectClass)) : std::string();
}

std::uintptr_t Outer(std::uintptr_t object) {
    return object ? ReadPtr(object + addr::kObjectOuter) : 0;
}

}  // namespace mohavr::names
