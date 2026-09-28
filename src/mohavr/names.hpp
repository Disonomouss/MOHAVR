// Engine object names (ENGINE-NOTES 5u): read-only, guarded against bad pointers. Game thread.
#pragma once
#include <cstdint>
#include <string>

namespace mohavr::names {

// The object's name ("MOHAWeap_Colt45_0" style: the FName plus _N), or "" if it can't be read.
std::string Name(std::uintptr_t object);
// The name of the object's class, or "".
std::string ClassName(std::uintptr_t object);
// The FName stored at `at` (8 bytes: index, number), or "".
std::string NameAt(std::uintptr_t at);
// Whether `object`'s class is `className` or derives from it (the class chain by UStruct.SuperField).
bool IsA(std::uintptr_t object, const char* className);
// UObject.Outer, or 0.
std::uintptr_t Outer(std::uintptr_t object);

// The byte offset of `object`'s script property `name` (searched through its class and supers; cached per class),
// or -1 if its class has no such property. Game thread.
int PropertyOffset(std::uintptr_t object, const char* name);
// Reads a pointer / 3 floats at `object + offset`; false if unreadable.
std::uintptr_t ReadPointer(std::uintptr_t at);
bool ReadVector(std::uintptr_t at, float (&v)[3]);

// Research (Debug.Reflect): logs the words after UObject in `object`'s class and in that class's first few linked
// fields, with their names -- to find UStruct::Children / UField::Next / UProperty::Offset.
void ProbeReflection(std::uintptr_t object);

}  // namespace mohavr::names
