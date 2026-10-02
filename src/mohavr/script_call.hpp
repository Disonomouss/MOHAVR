// Script calls through ProcessEvent, and reflection helpers for them (the off-hand grenade's executor, the off-hand
// pistol's spike; factored from offhand.cpp -- reload.cpp keeps its own older copy).
//
// An actor's vtable +0xF0 is AActor::ProcessEvent (0x10DB1FA0), a component's / plain object's UObject::ProcessEvent
// (0x109CE980, pinned with its bytes). Either refuses a function with a native index (iNative != 0); natives without one
// run. Optional parameters get no defaults through ProcessEvent: every call fills every parameter (the buffer starts
// zeroed).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mohavr::script {

// UObject::ProcessEvent's bytes checked once (standing rule 4): components and plain objects can be called.
bool ObjectProcessEventOk();
// Reads under SEH (0 on a fault).
std::uint32_t ReadU32(std::uintptr_t at);
// Reflection by property name (names::PropertyOffset / BoolProperty); 0 / fallback when the object or property is missing.
bool Bit(std::uintptr_t obj, const char* name);
void SetBit(std::uintptr_t obj, const char* name, bool on);
std::uintptr_t Obj(std::uintptr_t obj, const char* name);
void SetObj(std::uintptr_t obj, const char* name, std::uintptr_t v);
float Float(std::uintptr_t obj, const char* name, float fallback);
int Int(std::uintptr_t obj, const char* name, int fallback);

// A script function of `obj`'s class (found up its class chain by name), callable through its ProcessEvent, with its
// parameters' offsets; ok false (logged unless quiet) if anything is off.
struct Call {
    std::uintptr_t obj = 0, fn = 0, pe = 0;
    std::uint8_t   parms[256] = {};
    bool           ok = false;

    Call(std::uintptr_t o, const char* name, bool quiet = false);
    int  Off(const char* parm) const;                       // the parameter's offset in parms, -1 if none
    bool Set(const char* parm, const void* v, size_t n);    // false (logged) if no such parameter or it wouldn't fit
    bool Run();                                             // false if not ok, or it faulted (logged)
    std::uintptr_t ReturnObject() const;
    bool ReturnBool() const;
    int  ReturnInt() const;
    // A parameter's bytes (an out parameter or a struct return) when n of them fit; null otherwise.
    const std::uint8_t* At(const char* parm, size_t n) const;
    std::uint16_t Native() const;      // the function's native index (UFunction +0x90)
    std::uint16_t ParmsSize() const;   // UFunction +0x9E
    std::uint32_t Flags() const;       // FunctionFlags, UFunction +0x8C
};

}  // namespace mohavr::script
