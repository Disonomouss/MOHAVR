// The one header for engine addresses (standing rule 3).
//
// Valid ONLY for the build pinned in ENGINE-NOTES 1: MOHA.exe Steam build 3648,
// SHA-256 998BAB97...F04, linker timestamp 2008-02-23. The exe has no ASLR (ENGINE-NOTES 2),
// so these VAs are where the code actually is at run time. build_check.cpp verifies the
// header fields and the byte signatures below before anything is patched (D4).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mohavr::addr {

// --- PE header of the pinned build (ENGINE-NOTES 1-2; read from the file 2026-09-25) --------
inline constexpr std::uintptr_t kImageBase     = 0x10900000;
inline constexpr std::uint32_t  kTimeDateStamp = 0x47BFE8F8;  // 2008-02-23 09:35:52 UTC
inline constexpr std::uint32_t  kSizeOfImage   = 0x00F83000;
inline constexpr std::uint32_t  kCheckSum      = 0x00E9581D;
inline constexpr std::uint32_t  kEntryRva      = 0x00F2D2ED;  // SteamStub entry in .bind (ENGINE-NOTES 3)

// A byte signature at a fixed VA: what the pinned build has there.
struct Signature {
    const char*          name;
    std::uintptr_t       va;
    const std::uint8_t*  bytes;
    std::size_t          size;
};

// entry_OEP: call __security_init_cookie; jmp __tmainCRTStartup (ENGINE-NOTES 3-4, decoded payload)
inline constexpr std::uintptr_t kOep = 0x1112B7EA;
inline constexpr std::uint8_t   kOepBytes[] = {0xE8, 0xD5, 0x08, 0x00, 0x00, 0xE9, 0x35, 0xFD, 0xFF, 0xFF};

// WinMain prologue: push ebp; mov ebp,esp; push -1; push <SEH> (ENGINE-NOTES 4, Ghidra)
inline constexpr std::uintptr_t kWinMain = 0x10918200;
inline constexpr std::uint8_t   kWinMainBytes[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x00, 0xAA, 0x5D, 0x11};

// Import thunk: jmp dword ptr [0x112C6818] -- every Direct3DCreate9 call goes through the IAT
// slot below (ENGINE-NOTES 5b; 3 callers, all via this thunk).
inline constexpr std::uintptr_t kThunkDirect3DCreate9 = 0x10F29C30;
inline constexpr std::uint8_t   kThunkDirect3DCreate9Bytes[] = {0xFF, 0x25, 0x18, 0x68, 0x2C, 0x11};

// IAT slot for d3d9!Direct3DCreate9 (ENGINE-NOTES 5b, Ghidra import table).
inline constexpr std::uintptr_t kIatDirect3DCreate9 = 0x112C6818;

// InitD3D9Device: the CreateDevice call site, CALL EAX with EAX = IDirect3D9 vtbl[0x40]
// (ENGINE-NOTES 5b). Checked now as a build fingerprint; hooked in M2.
inline constexpr std::uintptr_t kCreateDeviceCall = 0x1090339A;
inline constexpr std::uint8_t   kCreateDeviceCallBytes[] = {0xFF, 0xD0};

inline constexpr Signature kSignatures[] = {
    {"entry_OEP",               kOep,                   kOepBytes,                  sizeof(kOepBytes)},
    {"WinMain",                 kWinMain,               kWinMainBytes,              sizeof(kWinMainBytes)},
    {"thunk Direct3DCreate9",   kThunkDirect3DCreate9,  kThunkDirect3DCreate9Bytes, sizeof(kThunkDirect3DCreate9Bytes)},
    {"CreateDevice call site",  kCreateDeviceCall,      kCreateDeviceCallBytes,     sizeof(kCreateDeviceCallBytes)},
};

}  // namespace mohavr::addr
