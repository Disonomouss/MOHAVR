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

// IAT slot for XInputGetState (imported by ordinal 2; ENGINE-NOTES 5a/5k). Verified at install time
// against the loaded XInput DLL's ordinal-2 export.
inline constexpr std::uintptr_t kIatXInputGetState = 0x112C6804;

// Object layout (ENGINE-NOTES 5l): ULocalPlayer+0x40 = Actor (the APlayerController);
// AActor::Rotation (FRotator: Pitch, Yaw, Roll ints) at +0xF4 -- ULevel::MoveActor (FUN_10B62090,
// reached from AActor::execSetRotation 0x10D2E9D0) stores the new rotation to actor[0x3D..0x3F].
inline constexpr std::uintptr_t kLocalPlayerActor = 0x40;
inline constexpr std::uintptr_t kActorRotation    = 0xF4;

// InitD3D9Device: the CreateDevice call site, CALL EAX with EAX = IDirect3D9 vtbl[0x40]
// (ENGINE-NOTES 5b). Checked now as a build fingerprint; hooked in M2.
inline constexpr std::uintptr_t kCreateDeviceCall = 0x1090339A;
inline constexpr std::uint8_t   kCreateDeviceCallBytes[] = {0xFF, 0xD0};

// --- ULocalPlayer::CalcSceneView (ENGINE-NOTES 5g) ----------------------------------------------
// Stack args: [EBP+0x10] = FVector* ViewLocation, [EBP+0x14] = FRotator* ViewRotation.
inline constexpr std::uintptr_t kCalcSceneView = 0x10C19910;
inline constexpr std::uint8_t   kCalcSceneViewBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};  // push ebp; mov ebp,esp; and esp,-16

// Merge point after GetPlayerViewPoint / locked view: mov esi,[edi+0x40]; call 0x10BEDFF0 (rel32).
// MidHook here: the view location/rotation are final and about to be turned into matrices.
inline constexpr std::uintptr_t kViewPointMerge = 0x10C19B3C;
inline constexpr std::uint8_t   kViewPointMergeBytes[] = {0x8B, 0x77, 0x40, 0xE8, 0xAC, 0x44, 0xFD, 0xFF};  // read from the exe 2026-09-25

// FPerspectiveMatrix (0x10BED9F0; output ptr in ESI, returned in EAX). Its two call sites in
// CalcSceneView, and the instruction right after each call (MidHook: EAX -> the new matrix).
inline constexpr std::uintptr_t kPerspectiveMatrix = 0x10BED9F0;
inline constexpr std::uintptr_t kProjCallNormal       = 0x10C19EA6;  // call rel32 -> kPerspectiveMatrix
inline constexpr std::uintptr_t kProjAfterNormal      = 0x10C19EAB;  // mov ecx,0x10
inline constexpr std::uint8_t   kProjAfterNormalBytes[] = {0xB9, 0x10, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kProjCallConstrained  = 0x10C19DAA;  // call rel32 -> kPerspectiveMatrix
inline constexpr std::uintptr_t kProjAfterConstrained = 0x10C19DAF;  // mov esi,eax
inline constexpr std::uint8_t   kProjAfterConstrainedBytes[] = {0x8B, 0xF0};

// The call instructions themselves: E8 + rel32 to kPerspectiveMatrix (targets verified from the exe).
inline constexpr std::uint8_t kProjCallNormalBytes[]      = {0xE8, 0x45, 0x3B, 0xFD, 0xFF};
inline constexpr std::uint8_t kProjCallConstrainedBytes[] = {0xE8, 0x41, 0x3C, 0xFD, 0xFF};

// --- UGameViewportClient::Draw and GEngine->GamePlayers (ENGINE-NOTES 5j) -----------------------
// Draw(FViewport*, FCanvas*): __thiscall, single exit RET 8. It loops GEngine->GamePlayers twice
// (scene views: CalcSceneView per player; then per-player drawing) -- M4 stereo doubles the list
// for the duration of Draw only.
inline constexpr std::uintptr_t kGEngine              = 0x116DD964;  // UGameEngine* (global)
inline constexpr std::uintptr_t kGamePlayersOffset    = 0x2A4;       // TArray<ULocalPlayer*>: Data, Num (+4), Max (+8)
inline constexpr std::uintptr_t kViewportClientDraw   = 0x10C14230;
inline constexpr std::uint8_t   kViewportClientDrawBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68, 0x2B, 0x2D, 0x1D, 0x11};
inline constexpr std::uintptr_t kViewportClientDrawRet = 0x10C17A09;
inline constexpr std::uint8_t   kViewportClientDrawRetBytes[] = {0xC2, 0x08, 0x00};  // ret 8
// ULocalPlayer split-screen rect (fractions of the viewport), read at CalcSceneView entry.
inline constexpr std::uintptr_t kLocalPlayerOriginX = 0x68, kLocalPlayerOriginY = 0x6C;
inline constexpr std::uintptr_t kLocalPlayerSizeX   = 0x70, kLocalPlayerSizeY   = 0x74;
// ULocalPlayer::ViewState (FSceneViewStateInterface*, occlusion/visibility history) -- per view.
// LocalPlayer.uc member order: Origin, Size, PlayerPostProcess (+0x78), ViewState (+0x7C),
// ActorVisibilityHistory (+0x80). The ctor (FUN_10C18AF0) does `this->ViewState = AllocateViewState()`
// (store at 0x10C18B5F: mov [esi+0x7C],eax); vtable of the state 0x114D3730 (runtime + ctor FUN_10A92BA0).
inline constexpr std::uintptr_t kLocalPlayerViewState = 0x7C;
// AllocateViewState(): GMalloc->Malloc(0x180, 8) + FSceneViewState ctor; cdecl, no args, returns the state.
inline constexpr std::uintptr_t kAllocateViewState = 0x10A99220;
inline constexpr std::uint8_t   kAllocateViewStateBytes[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xAB, 0x6B, 0x1C};
// The ctor's call + store, proving the +0x7C offset: call AllocateViewState; cmp [esi+78],0; mov [esi+7C],eax
inline constexpr std::uintptr_t kLocalPlayerCtorViewStateStore = 0x10C18B56;
inline constexpr std::uint8_t   kLocalPlayerCtorViewStateStoreBytes[] = {0xE8, 0xC5, 0x06, 0xE8, 0xFF, 0x83, 0x7E, 0x78, 0x00, 0x89, 0x46, 0x7C};

// --- FSystemSettings (vtable at 0x116F56B8; ENGINE-NOTES 5i) -----------------------------------
// FUN_10ECC330 copies MOHAScalabilityOptions' bool bitfield (+0x3C, declaration order) into
// these ints: bit 1 bAllowDepthOfField -> 0x116F56D4, bit 5 bAllowMotionBlur -> 0x116F56D0.
inline constexpr std::uintptr_t kSysAllowMotionBlur   = 0x116F56D0;
inline constexpr std::uintptr_t kSysAllowDepthOfField = 0x116F56D4;
inline constexpr std::uintptr_t kSysScreenPercentage  = 0x116F56F8;  // float, read by CalcSceneView

inline constexpr Signature kSignatures[] = {
    {"entry_OEP",               kOep,                   kOepBytes,                  sizeof(kOepBytes)},
    {"WinMain",                 kWinMain,               kWinMainBytes,              sizeof(kWinMainBytes)},
    {"thunk Direct3DCreate9",   kThunkDirect3DCreate9,  kThunkDirect3DCreate9Bytes, sizeof(kThunkDirect3DCreate9Bytes)},
    {"CreateDevice call site",  kCreateDeviceCall,      kCreateDeviceCallBytes,     sizeof(kCreateDeviceCallBytes)},
    {"CalcSceneView prologue",  kCalcSceneView,         kCalcSceneViewBytes,        sizeof(kCalcSceneViewBytes)},
    {"CalcSceneView view merge", kViewPointMerge,       kViewPointMergeBytes,       sizeof(kViewPointMergeBytes)},
    {"FPerspectiveMatrix call (normal)",      kProjCallNormal,      kProjCallNormalBytes,      sizeof(kProjCallNormalBytes)},
    {"after FPerspectiveMatrix (normal)",     kProjAfterNormal,     kProjAfterNormalBytes,     sizeof(kProjAfterNormalBytes)},
    {"FPerspectiveMatrix call (constrained)", kProjCallConstrained, kProjCallConstrainedBytes, sizeof(kProjCallConstrainedBytes)},
    {"after FPerspectiveMatrix (constrained)", kProjAfterConstrained, kProjAfterConstrainedBytes, sizeof(kProjAfterConstrainedBytes)},
    {"UGameViewportClient::Draw prologue", kViewportClientDraw, kViewportClientDrawBytes, sizeof(kViewportClientDrawBytes)},
    {"UGameViewportClient::Draw ret 8",    kViewportClientDrawRet, kViewportClientDrawRetBytes, sizeof(kViewportClientDrawRetBytes)},
    {"AllocateViewState prologue",         kAllocateViewState, kAllocateViewStateBytes, sizeof(kAllocateViewStateBytes)},
    {"ULocalPlayer ctor ViewState store",  kLocalPlayerCtorViewStateStore, kLocalPlayerCtorViewStateStoreBytes, sizeof(kLocalPlayerCtorViewStateStoreBytes)},
};

}  // namespace mohavr::addr
