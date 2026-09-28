#include "names.hpp"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "addresses.hpp"
#include "log.hpp"

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

std::string NameAt(std::uintptr_t at) {
    char buf[128];
    if (!at || !ReadFName(at, buf, sizeof(buf))) return {};
    return buf;
}

std::string ClassName(std::uintptr_t object) {
    return object ? Name(ReadPtr(object + addr::kObjectClass)) : std::string();
}

std::uintptr_t Outer(std::uintptr_t object) {
    return object ? ReadPtr(object + addr::kObjectOuter) : 0;
}

// A field of `strct` or its supers by name.
std::uintptr_t FindFieldProbe(std::uintptr_t strct, const char* name) {
    int depth = 0;
    for (std::uintptr_t s = strct; s && depth < 64; s = ReadPtr(s + addr::kFieldSuper), ++depth) {
        int count = 0;  // guards against a bad list, per struct
        for (std::uintptr_t f = ReadPtr(s + addr::kStructChildren); f && count < 20000; f = ReadPtr(f + addr::kFieldNext), ++count)
            if (Name(f) == name) return f;
    }
    return 0;
}

int PropertyOffset(std::uintptr_t object, const char* name) {
    struct Entry { std::uintptr_t cls; std::string name; int offset; };
    static std::vector<Entry> cache;
    const std::uintptr_t cls = ReadPtr(object + addr::kObjectClass);
    if (!cls) return -1;
    for (const Entry& e : cache)
        if (e.cls == cls && e.name == name) return e.offset;
    const std::uintptr_t f = FindFieldProbe(cls, name);
    int off = -1;
    if (f && ClassName(f).find("Property") != std::string::npos) off = static_cast<int>(ReadPtr(f + addr::kPropertyOffset));
    if (off < 0 || off > 0x10000) off = -1;
    cache.push_back({cls, name, off});
    MLOG("reflect: %s.%s at %s0x%X", Name(cls).c_str(), name, off < 0 ? "(none) " : "", off < 0 ? 0 : off);
    return off;
}

std::uintptr_t ReadPointer(std::uintptr_t at) { return ReadPtr(at); }

bool ReadVector(std::uintptr_t at, float (&v)[3]) {
    __try {
        for (int i = 0; i < 3; ++i) v[i] = reinterpret_cast<const float*>(at)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ProbeReflection(std::uintptr_t object) {
    const std::uintptr_t cls0 = ReadPtr(object + addr::kObjectClass);
    for (const char* prop : {"Location", "Rotation", "Velocity", "Weapon", "InvManager"}) {
        const std::uintptr_t f = FindFieldProbe(cls0, prop);
        std::string words;
        for (std::uintptr_t off = 0x44; off < 0x7C && f; off += 4) {
            char b[24];
            sprintf_s(b, " %02X:%X", static_cast<unsigned>(off), static_cast<unsigned>(ReadPtr(f + off)));
            words += b;
        }
        MLOG("reflect: property %s = %08X (%s)%s", prop, static_cast<unsigned>(f), f ? ClassName(f).c_str() : "-", words.c_str());
    }
    const std::uintptr_t cls = ReadPtr(object + addr::kObjectClass);
    MLOG("reflect: object %08X '%s' class %08X '%s'", static_cast<unsigned>(object), Name(object).c_str(),
         static_cast<unsigned>(cls), Name(cls).c_str());
    auto dump = [](const char* what, std::uintptr_t o) {
        for (std::uintptr_t off = 0x38; off < 0x90; off += 4) {
            const std::uintptr_t v = ReadPtr(o + off);
            const std::string n = Name(v), c = ClassName(v);
            MLOG("reflect:   %s +%02X = %08X %s%s%s", what, static_cast<unsigned>(off), static_cast<unsigned>(v), n.c_str(),
                 c.empty() ? "" : " : ", c.c_str());
        }
    };
    dump("class", cls);
    // The class's words that point at a *Property / Function / Const / Enum / State: candidates for Children.
    for (std::uintptr_t off = 0x38; off < 0x90; off += 4) {
        const std::uintptr_t v = ReadPtr(cls + off);
        const std::string c = ClassName(v);
        if (c.find("Property") == std::string::npos && c != "Function" && c != "Const" && c != "Enum" && c != "State") continue;
        MLOG("reflect: class +%02X -> first field '%s' : %s", static_cast<unsigned>(off), Name(v).c_str(), c.c_str());
        dump("field", v);
        break;
    }
}

}  // namespace mohavr::names
