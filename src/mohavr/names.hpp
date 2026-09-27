// Engine object names (ENGINE-NOTES 5u): read-only, guarded against bad pointers. Game thread.
#pragma once
#include <cstdint>
#include <string>

namespace mohavr::names {

// The object's name ("MOHAWeap_Colt45_0" style: the FName plus _N), or "" if it can't be read.
std::string Name(std::uintptr_t object);
// The name of the object's class, or "".
std::string ClassName(std::uintptr_t object);
// UObject.Outer, or 0.
std::uintptr_t Outer(std::uintptr_t object);

}  // namespace mohavr::names
