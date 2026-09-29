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
// ULocalPlayer's FExec subobject (+0x3C; the ctor FUN_10C18AF0 stores vtable 0x114F8AB0 there) and its
// slot 0, ULocalPlayer::Exec(const TCHAR* Cmd, FOutputDevice& Ar) (ENGINE-NOTES 5o).
inline constexpr std::uintptr_t kLocalPlayerFExec = 0x3C;
inline constexpr std::uintptr_t kLocalPlayerExec  = 0x10C1A220;
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

// UGameViewportClient::Draw, the per-player HUD loop (ENGINE-NOTES 5n): ESI = that player's FSceneView,
// EDX = the player index. The canvas takes its origin/clip from view+0x1C/0x20 (X, Y) and +0x24/0x28
// (SizeX, SizeY), then HUD.PostRender runs. `movss xmm0,[esi+24]; movss [esp+90],xmm0`.
inline constexpr std::uintptr_t kHudViewRead = 0x10C1530C;
inline constexpr std::uint8_t   kHudViewReadBytes[] = {0xF3, 0x0F, 0x10, 0x46, 0x24, 0xF3, 0x0F, 0x11, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kViewX = 0x1C, kViewY = 0x20, kViewSizeX = 0x24, kViewSizeY = 0x28;
// ...then it builds the canvas matrix (identity + translation view X/Y) at [ESP+0x130] (16 floats,
// row-major: M00 +0x130, M11 +0x144, X +0x160, Y +0x164), flushes (FUN_10B17930) and pushes it
// (FUN_10918FC0(&matrix) on canvas+0xC). Hooked after the flush: `add edi,0Ch; mov esi,edi; call 10C92CE0`.
inline constexpr std::uintptr_t kHudMatrixPush = 0x10C15440;
inline constexpr std::uint8_t   kHudMatrixPushBytes[] = {0x83, 0xC7, 0x0C, 0x8B, 0xF7, 0xE8, 0x96, 0xD8, 0x07, 0x00};
inline constexpr std::uintptr_t kHudMatrixStackOffset = 0x130;

// A decal's screen box (ENGINE-NOTES 5r): stdcall (EAX = an input, stack: ?, FSceneView*, float* min,
// float* max), RET 0x10, returns nonzero if the box is on screen. It projects to ABSOLUTE pixels (it adds
// view+0x1C/0x20) but clamps to [0, SizeX] x [0, SizeY] -- so for a view at x = SizeX (the right eye) the
// box collapses and every decal is culled. 5 callers (decals on each receiver kind).
inline constexpr std::uintptr_t kDecalScreenBox = 0x10A2ABB0;
inline constexpr std::uint8_t   kDecalScreenBoxBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x44, 0x01, 0x00, 0x00};

// --- M7 aim (ENGINE-NOTES 5s) ---
// UPawn::execGetBaseAimRotation(FFrame&, RESULT_DECL): the script native behind Pawn.GetBaseAimRotation()
// (native table entry "intAPawnexecGetBaseAimRotation" at 0x116158E0). thiscall (ECX = pawn), RET 8; it
// finishes the params, calls the C++ virtual (vtable + 0x354) and copies its FRotator to *Result. The
// player's shots take their base aim from it (PlayerController.GetAdjustedAimFor).
inline constexpr std::uintptr_t kExecGetBaseAimRotation = 0x10D39090;
inline constexpr std::uint8_t   kExecGetBaseAimRotationBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0x40, 0x1C, 0x01, 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1};
// UWorld::SingleLineCheck -- LTCG convention, read from AActor::execTrace (0x10CE85A0, the call at
// 0x10CE8994): stack (this = GWorld, FCheckResult* Hit, AActor* Source, FVector* End, FVector* Start,
// FVector* Extent), EAX = TraceFlags, ECX = light component (0); callee pops (RET 0x18); preserves
// EBX/ESI/EDI/EBP. Returns nonzero when nothing was hit.
inline constexpr std::uintptr_t kSingleLineCheck = 0x10B640A0;
inline constexpr std::uint8_t   kSingleLineCheckBytes[] = {0x51, 0x8B, 0x54, 0x24, 0x10, 0x53, 0x8B, 0x1D, 0x50, 0xE5, 0x6A, 0x11};
inline constexpr std::uintptr_t kGWorld = 0x116DCE78;       // UWorld* (execTrace: mov ecx,[0x116DCE78] before the call)
// UWeaponAccuracyComponent::execAddSpread (native table entry at 0x11612DE8 -> "intUWeaponAccuracyComponentexecAddSpread"):
// thiscall (ECX = the component), RET 8; reads BaseAim from the script stack and calls the component's virtual
// AddSpread (vtable +0x170); *Result = the spread aim.
inline constexpr std::uintptr_t kExecAddSpread = 0x10E4E310;
inline constexpr std::uint8_t   kExecAddSpreadBytes[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0x74, 0x24, 0x20, 0x8B, 0x46, 0x1C};
// The player's bullets: EALAWeapon.CalcWeaponFire -> execCalcWeaponFireNative (native table entry at 0x11612348 ->
// "intAEALAWeaponexecCalcWeaponFireNative", 0x10E4CF90) -> 0x10F0CF10 (TraceOwner, Start, End, Extent, ImpactList):
// traces with 0x10F0CDE0 and, on a Trigger/TriggerVolume, clears its bProjTarget (+0x78 bit 0x40000), recurses from
// the hit and sets it back. 0x10F0CDE0 calls SingleLineCheck(GWorld, &Hit, Source, End*, Start*, Extent*) with
// EAX = 0x268BF: TRACE_ProjTargets | 0x4000 | 0x800 (material) | 0x20000 (per-poly collision). The aim trace uses the
// same flags (round 22: its simple-collision trace had put the red dot where bullets don't go).
inline constexpr std::uintptr_t kBulletTraceFlagsSite = 0x10F0CE5C;  // mov eax,0x268BF
inline constexpr std::uint8_t   kBulletTraceFlagsSiteBytes[] = {0xB8, 0xBF, 0x68, 0x02, 0x00};
inline constexpr std::uint32_t  kTraceFlagsBullet = 0x268BF;
// The bullet's SingleLineCheck call (MidHook before it: [esp] GWorld, +4 &Hit, +8 Source, +0xC End*, +0x10 Start*,
// +0x14 Extent*; End/Start point at 0x10F0CF10's own by-value Start/End, which it goes on to use) and the instruction
// after it (MidHook: the Hit at esp+0x14, its Actor at esp+0x18, Location at esp+0x1C).
inline constexpr std::uintptr_t kBulletTraceCall = 0x10F0CE93;
inline constexpr std::uint8_t   kBulletTraceCallBytes[] = {0xE8, 0x08, 0x72, 0xC5, 0xFF};
inline constexpr std::uintptr_t kBulletTraceAfter = 0x10F0CE98;
inline constexpr std::uint8_t   kBulletTraceAfterBytes[] = {0x8D, 0x44, 0x24, 0x14, 0xE8, 0x1F, 0x03, 0xCE, 0xFF};
// UMOHAAnimNodePlayerActivity::TickAnim (0x10E66400; the first-person arms' activity blend list, VM_Tree's
// MOHAAnimNodePlayerActivities; research workflow wf_ae7690d5 + capstone on Ghidra's bytes, round 24): ESI = the node,
// EBX = its SkelComponent ([ESI+0x3C]), EDI = that component's Owner ([EBX+0x4C], class-checked a MOHAPlayerPawn).
// 0x10E6647B loads EAX = pawn.CurrentActivity (byte +0x884); 0x10E66482 compares it with the node's ActiveChildIndex
// (+0xE0), and if they differ (or the activity was set again: ECX) pushes EAX unchanged at 0x10E664A8 as the child to blend
// to (vtable +0x190 with PlaybackLength +0x890 and fActivityBlendTime +0x894). Child 2 = PLAYER_ACTIVITY_STAND_SPRINT (the
// <weapon>_sprint loops, 0.25 s in and out), 1 = walk, 0 = idle.
inline constexpr std::uintptr_t kActivityTickLoad = 0x10E6647B;  // movzx eax, byte [edi+0x884]
inline constexpr std::uint8_t   kActivityTickLoadBytes[] = {0x0F, 0xB6, 0x87, 0x84, 0x08, 0x00, 0x00};
inline constexpr std::uintptr_t kActivityTickCmp = 0x10E66482;   // cmp eax,[esi+0xE0]; jne +4 (MidHook here)
inline constexpr std::uint8_t   kActivityTickCmpBytes[] = {0x3B, 0x86, 0xE0, 0x00, 0x00, 0x00, 0x75, 0x04};
inline constexpr std::uintptr_t kActivityNodeActiveChild = 0xE0;  // AnimNodeBlendList.ActiveChildIndex (int)
// FCheckResult (execTrace's initialisation): Next +0, Actor +4, Location +8, Normal +0x14, Time +0x20 (1.0 =
// no hit), Item +0x24 (-1), then Material/Component/BoneName/...: 0x44 bytes in all.
inline constexpr std::uintptr_t kCheckResultActor = 0x04, kCheckResultLocation = 0x08, kCheckResultTime = 0x20,
                                kCheckResultItem = 0x24, kCheckResultSize = 0x44;
inline constexpr std::uintptr_t kActorLocation   = 0xE8;    // FVector (execTrace reads Actor+0xE8 as the default start)
// Pawn.Controller: APawn::execIsHumanControlled (0x10D38FF0) tests [pawn+0x1EC]. Its declared place (after
// three floats) puts AActor's size at 0x1E0, and Controller.Pawn is Controller's first variable -> +0x1E0.
// Used only when both point at each other (checked at run time).
inline constexpr std::uintptr_t kPawnController  = 0x1EC;
inline constexpr std::uintptr_t kControllerPawn  = 0x1E0;

// --- M8 the first-person gun (ENGINE-NOTES 5t) ---
// The MOHA skeletal-mesh scene proxy's per-view transform (vtable entry at 0x11595E00; the only caller of
// FPerspectiveMatrix besides CalcSceneView): thiscall (ECX = proxy; stack: const FSceneView*, FMatrix* OutLocalToWorld,
// FMatrix* OutWorldToLocal), RET 0xC. When the component's FOV (proxy+0xF0 -> +0x3D0) is nonzero -- the
// first-person arms and gun -- it bakes a flat-screen projection of that FOV into the matrices (per view, so per eye:
// the doubled gun in stereo); otherwise it copies the proxy's LocalToWorld (+0x20) and WorldToLocal (+0x60).
inline constexpr std::uintptr_t kViewModelTransform = 0x10EEA470;
inline constexpr std::uint8_t   kViewModelTransformBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0xD9};
inline constexpr std::uintptr_t kProxyComponent = 0xF0, kProxyLocalToWorld = 0x20, kProxyWorldToLocal = 0x60;
inline constexpr std::uintptr_t kMohaSkelMeshFov = 0x3D0;  // UMOHASkeletalMeshComponent.FOV (float)

// --- M8 arm IK (ENGINE-NOTES 5x) ---
// UMOHASkeletalMeshComponent::UpdateTransform (fastcall, ECX = component; slot in the arms' vtable 0x11587D38):
// bLockTranslation, then USkeletalMeshComponent::UpdateTransform 0x10CFAC10, which ends with
// MeshObject(+0x21C)->Update(LOD, this, ActiveMorphs) -- the copy of SpaceBases for the renderer.
inline constexpr std::uintptr_t kMohaSkelUpdateTransform = 0x10EEB550;
inline constexpr std::uint8_t   kMohaSkelUpdateTransformBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x44, 0x53, 0x8B, 0xD9};
// Inside USkeletalMeshComponent::UpdateTransform, just before MeshObject->Update(...) -- LocalToWorld and the
// attachments (the gun on the arms' prop bone) are final here: `mov eax,[ecx]; mov eax,[eax+0x10]; lea edx,[ebx+0x290]`,
// EBX = the component. The arms' IK and the gun's move are baked into SpaceBases at this point (MidHook).
inline constexpr std::uintptr_t kSkelMeshObjectUpdateCall = 0x10CFAFAD;
inline constexpr std::uint8_t   kSkelMeshObjectUpdateCallBytes[] = {0x8B, 0x01, 0x8B, 0x40, 0x10, 0x8D, 0x93, 0x90, 0x02, 0x00, 0x00};
// USkeletalMesh RefSkeleton (TArray<FMeshBone>) and FMeshBone's size (probe: 70 VM_Arms bones, names at 0, 68, ...;
// ParentIndex at +56).
inline constexpr std::uintptr_t kSkelMeshRefSkeleton = 0x7C;
inline constexpr std::uintptr_t kMeshBoneStride = 68;

// --- Object names (ENGINE-NOTES 5u) ---
// FName::ToString 0x109E2E80 (ECX = &FName {Index, Number}): FName::Names data = [0x116F4A54], entry string (wide)
// at entry + 0x10. UObject: Index +0x04 (-1 = uninitialised; GetName 0x1090BBD0), Outer +0x28 (GetPathName
// 0x109EBAD0), Name +0x2C (both). Class +0x34 (the UE3 order after Name; verified at run time by class names).
inline constexpr std::uintptr_t kGNamesData   = 0x116F4A54;
inline constexpr std::uintptr_t kNameEntryString = 0x10;
inline constexpr std::uintptr_t kObjectOuter  = 0x28, kObjectName = 0x2C, kObjectClass = 0x34;
// Reflection (ENGINE-NOTES 5v; Debug.Reflect probe 2026-09-27): ObjectArchetype +0x38 ends UObject (0x3C). UField:
// SuperField +0x3C, Next +0x40. UStruct: Children +0x4C, PropertiesSize +0x50. UProperty: ArrayDim +0x44,
// ElementSize +0x48, PropertyFlags +0x4C, Offset +0x64 -- verified: Actor.Location 0xE8 and Rotation 0xF4 (known),
// Velocity 0x100, Pawn.InvManager 0x3A4, Pawn.Weapon 0x3A8.
inline constexpr std::uintptr_t kFieldSuper = 0x3C, kFieldNext = 0x40, kStructChildren = 0x4C, kPropertyOffset = 0x64;

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
    {"Draw HUD loop view read",            kHudViewRead, kHudViewReadBytes, sizeof(kHudViewReadBytes)},
    {"Draw HUD matrix push",               kHudMatrixPush, kHudMatrixPushBytes, sizeof(kHudMatrixPushBytes)},
    {"decal screen box",                   kDecalScreenBox, kDecalScreenBoxBytes, sizeof(kDecalScreenBoxBytes)},
    {"execGetBaseAimRotation",             kExecGetBaseAimRotation, kExecGetBaseAimRotationBytes, sizeof(kExecGetBaseAimRotationBytes)},
    {"UWorld::SingleLineCheck",            kSingleLineCheck, kSingleLineCheckBytes, sizeof(kSingleLineCheckBytes)},
    {"view-model proxy transform",         kViewModelTransform, kViewModelTransformBytes, sizeof(kViewModelTransformBytes)},
    {"MOHA skel UpdateTransform",          kMohaSkelUpdateTransform, kMohaSkelUpdateTransformBytes, sizeof(kMohaSkelUpdateTransformBytes)},
    {"skel MeshObject->Update call",       kSkelMeshObjectUpdateCall, kSkelMeshObjectUpdateCallBytes, sizeof(kSkelMeshObjectUpdateCallBytes)},
    {"execAddSpread",                      kExecAddSpread, kExecAddSpreadBytes, sizeof(kExecAddSpreadBytes)},
    {"bullet trace flags",                 kBulletTraceFlagsSite, kBulletTraceFlagsSiteBytes, sizeof(kBulletTraceFlagsSiteBytes)},
    {"bullet trace call",                  kBulletTraceCall, kBulletTraceCallBytes, sizeof(kBulletTraceCallBytes)},
    {"bullet trace after",                 kBulletTraceAfter, kBulletTraceAfterBytes, sizeof(kBulletTraceAfterBytes)},
    {"activity tick load",                 kActivityTickLoad, kActivityTickLoadBytes, sizeof(kActivityTickLoadBytes)},
    {"activity tick compare",              kActivityTickCmp, kActivityTickCmpBytes, sizeof(kActivityTickCmpBytes)},
};

}  // namespace mohavr::addr
