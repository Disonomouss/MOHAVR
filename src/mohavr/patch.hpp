// Verified memory patching. Nothing is ever written without first checking that the target
// holds exactly what the pinned build is known to hold there (standing rule 4).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mohavr::patch {

// True if `size` bytes at `va` equal `expected`. Safe on unmapped memory (returns false).
bool BytesMatch(std::uintptr_t va, const std::uint8_t* expected, std::size_t size);

// Atomically replace a pointer-sized slot (an IAT entry) if it currently holds `expected`.
// Returns false, leaving memory untouched, if it doesn't.
bool SwapPointer(std::uintptr_t slot, void* expected, void* replacement);

}  // namespace mohavr::patch
