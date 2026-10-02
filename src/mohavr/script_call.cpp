#include "script_call.hpp"

#include <windows.h>

#include <cstring>

#include "addresses.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"

namespace mohavr::script {
namespace {

// No C++ objects here: SEH only (as reload.cpp's).
bool CallProcessEvent(std::uintptr_t pe, std::uintptr_t obj, std::uintptr_t fn, void* parms) {
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*)>(pe)(obj, nullptr, fn, parms, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool ObjectProcessEventOk() {
    static int ok = -1;
    if (ok < 0) {
        ok = patch::BytesMatch(addr::kObjectProcessEvent, addr::kObjectProcessEventBytes, sizeof(addr::kObjectProcessEventBytes)) ? 1 : 0;
        if (!ok) MLOG("script: UObject::ProcessEvent bytes differ -- no component calls");
    }
    return ok == 1;
}

std::uint32_t ReadU32(std::uintptr_t at) {
    __try {
        return *reinterpret_cast<const std::uint32_t*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Bit(std::uintptr_t obj, const char* name) {
    int o = -1;
    std::uint32_t m = 0;
    return obj && names::BoolProperty(obj, name, o, m) && (ReadU32(obj + o) & m) != 0;
}

void SetBit(std::uintptr_t obj, const char* name, bool on) {
    int o = -1;
    std::uint32_t m = 0;
    if (!obj || !names::BoolProperty(obj, name, o, m)) return;
    auto* w = reinterpret_cast<std::uint32_t*>(obj + o);
    *w = on ? (*w | m) : (*w & ~m);
}

std::uintptr_t Obj(std::uintptr_t obj, const char* name) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    return o >= 0 ? names::ReadPointer(obj + o) : 0;
}

void SetObj(std::uintptr_t obj, const char* name, std::uintptr_t v) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o >= 0) *reinterpret_cast<std::uintptr_t*>(obj + o) = v;
}

float Float(std::uintptr_t obj, const char* name, float fallback) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    if (o < 0) return fallback;
    float v = fallback;
    std::memcpy(&v, reinterpret_cast<const void*>(obj + o), sizeof(v));
    return v;
}

int Int(std::uintptr_t obj, const char* name, int fallback) {
    const int o = obj ? names::PropertyOffset(obj, name) : -1;
    return o >= 0 ? static_cast<int>(ReadU32(obj + o)) : fallback;
}

Call::Call(std::uintptr_t o, const char* name, bool quiet) : obj(o) {
    const std::uintptr_t cls = o ? names::ReadPointer(o + addr::kObjectClass) : 0;
    fn = cls ? names::FindFieldProbe(cls, name) : 0;
    const std::uintptr_t vt = o ? names::ReadPointer(o) : 0;
    pe = vt ? names::ReadPointer(vt + addr::kVtProcessEvent) : 0;
    const std::uint16_t native = fn ? Native() : 1;
    const std::uint16_t size = fn ? ParmsSize() : 0;
    ok = fn && names::ClassName(fn) == "Function" && native == 0 && size <= sizeof(parms) &&
         (pe == addr::kActorProcessEvent || (pe == addr::kObjectProcessEvent && ObjectProcessEventOk()));
    if (!ok && !quiet)
        MLOG("script: %s.%s can't be called (function %s, native index %u, parms %u, ProcessEvent 0x%08X)",
             names::Name(o).c_str(), name, fn ? names::ClassName(fn).c_str() : "none", native, size, static_cast<unsigned>(pe));
}

int Call::Off(const char* parm) const {
    const std::uintptr_t p = fn ? names::FindFieldProbe(fn, parm) : 0;
    const int o = p ? static_cast<int>(names::ReadPointer(p + addr::kPropertyOffset)) : -1;
    return o >= 0 && o < static_cast<int>(sizeof(parms)) - 16 ? o : -1;
}

bool Call::Set(const char* parm, const void* v, size_t n) {
    const int o = Off(parm);
    if (o < 0 || static_cast<size_t>(o) + n > sizeof(parms)) {
        MLOG("script: %s has no parameter %s (or it doesn't fit)", names::Name(fn).c_str(), parm);
        return false;
    }
    std::memcpy(parms + o, v, n);
    return true;
}

bool Call::Run() {
    if (!ok) return false;
    const bool done = CallProcessEvent(pe, obj, fn, parms);
    if (!done) MLOG("script: %s.%s FAULTED", names::Name(obj).c_str(), names::Name(fn).c_str());
    return done;
}

std::uintptr_t Call::ReturnObject() const {
    const int o = Off("ReturnValue");
    return o >= 0 ? *reinterpret_cast<const std::uintptr_t*>(parms + o) : 0;
}

bool Call::ReturnBool() const {
    const int o = Off("ReturnValue");
    return o >= 0 && (*reinterpret_cast<const std::uint32_t*>(parms + o) & 1u) != 0;
}

int Call::ReturnInt() const {
    const int o = Off("ReturnValue");
    return o >= 0 ? *reinterpret_cast<const int*>(parms + o) : 0;
}

const std::uint8_t* Call::At(const char* parm, size_t n) const {
    const std::uintptr_t p = fn ? names::FindFieldProbe(fn, parm) : 0;
    const int o = p ? static_cast<int>(names::ReadPointer(p + addr::kPropertyOffset)) : -1;
    return o >= 0 && static_cast<size_t>(o) + n <= sizeof(parms) ? parms + o : nullptr;
}

std::uint16_t Call::Native() const { return fn ? static_cast<std::uint16_t>(ReadU32(fn + addr::kFunctionNative) & 0xFFFF) : 0; }
std::uint16_t Call::ParmsSize() const { return fn ? static_cast<std::uint16_t>(ReadU32(fn + addr::kFunctionParmsSize) & 0xFFFF) : 0; }
std::uint32_t Call::Flags() const { return fn ? ReadU32(fn + addr::kFunctionFlags) : 0; }

}  // namespace mohavr::script
