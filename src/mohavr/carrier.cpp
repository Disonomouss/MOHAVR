#include "carrier.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"

namespace mohavr::carrier {
using namespace script;  // Call, Obj, Bit, ... (script_call.hpp)
namespace {

template <class T>
void CopyField(std::uintptr_t to, std::uintptr_t from, const char* name) {
    const int o = names::PropertyOffset(from, name), p = names::PropertyOffset(to, name);
    if (o >= 0 && p >= 0) std::memcpy(reinterpret_cast<void*>(to + p), reinterpret_cast<const void*>(from + o), sizeof(T));
}

}  // namespace

std::uintptr_t Component(const Slot& slot) { return slot.pawn && slot.pawn == aim::LocalPlayerPawn() ? slot.comp : 0; }

void Mul16(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[4 * i + k] * b[4 * k + j];
            r[4 * i + j] = s;
        }
    std::memcpy(out, r, sizeof(r));
}

namespace {
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}
}  // namespace

void FitFromIni(const char* key, float (&fit)[4], const char*& from) {
    const std::string k(key ? key : "");
    const std::wstring wkey(k.begin(), k.end());
    const std::wstring shipped = ModuleDir() + L"\\MOHAVR.ini";
    wchar_t v[128] = L"", local[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    from = "the player's ini";
    if (n && n < MAX_PATH && !k.empty())
        GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", v, 128, (std::wstring(local) + L"\\MOHAVR\\MOHAVR.user.ini").c_str());
    if (!v[0] && !k.empty()) {
        GetPrivateProfileStringW(L"GunFit", wkey.c_str(), L"", v, 128, shipped.c_str());
        from = "the shipped ini";
    }
    fit[3] = 0.0f;
    if (v[0] && swscanf_s(v, L"%f %f %f %f", &fit[0], &fit[1], &fit[2], &fit[3]) >= 3) return;
    auto num = [&](const wchar_t* name, float def) {
        wchar_t b[32] = L"";
        GetPrivateProfileStringW(L"Weapon", name, L"", b, 32, shipped.c_str());
        return b[0] ? static_cast<float>(_wtof(b)) : def;
    };
    fit[0] = num(L"GripX", 34.0f), fit[1] = num(L"GripY", 11.0f), fit[2] = num(L"GripZ", -17.0f), fit[3] = 0.0f;
    from = "the default";
}

void MirroredHold(const float (&c)[12], const float (&fit)[4], float (&out)[16]) {
    // M_right: the camera pose, origin moved back by the grip. x M_y: every row's right component negated. S x: the X row
    // negated.
    const float m[16] = {-c[0], c[1], -c[2], 0, c[3], -c[4], c[5], 0, c[6], -c[7], c[8], 0,
                         c[9] - fit[0], -(c[10] - fit[1]), c[11] - fit[2], 1};
    const float a = fit[3] * 0.0174533f, cs = std::cos(a), sn = std::sin(a);
    const float pitch[16] = {cs, 0, sn, 0, 0, 1, 0, 0, -sn, 0, cs, 0, 0, 0, 0, 1};
    Mul16(m, pitch, out);
}

void Detach(Slot& slot, const char* who, bool trace) {
    if (!slot.comp) return;
    if (slot.arms && slot.pawn == aim::LocalPlayerPawn()) {
        Call detach(slot.arms, "DetachComponent");
        if (detach.Set("Component", &slot.comp, sizeof(slot.comp)) && detach.Run() && trace)
            MLOG("%s: carrier %s detached", who, names::Name(slot.comp).c_str());
    }
    slot = Slot{};
}

bool Attach(Slot& slot, std::uintptr_t pawn, std::uintptr_t weapon, const char* who, bool trace) {
    Detach(slot, who, trace);
    const std::uintptr_t arms = Obj(pawn, "FPArms"), tmpl = Obj(weapon, "DroppedPickupMesh");
    if (!arms || !tmpl) {
        MLOG("%s: carrier -- arms %s, pickup mesh %s: none (nothing drawn in the hand)", who, names::Name(arms).c_str(),
             names::Name(tmpl).c_str());
        return false;
    }
    Call clone(tmpl, "Clone");
    if (!clone.Set("InOuter", &weapon, sizeof(weapon)) || !clone.Run()) return false;
    const std::uintptr_t c = clone.ReturnObject();
    if (!c || !names::IsA(c, "MOHASkeletalMeshComponent")) {
        MLOG("%s: carrier -- Clone gave %s (%s)", who, names::Name(c).c_str(), names::ClassName(c).c_str());
        return false;
    }
    // Drawn as the arms are: their depth group, light environment, LOD and first-person FOV (the proxy hook then draws it
    // in true 3D like them, through the mirror in left-hand mode); no collision, their shadow settings; no physics asset.
    CopyField<std::uint8_t>(c, arms, "DepthPriorityGroup");
    CopyField<std::uintptr_t>(c, arms, "LightEnvironment");
    CopyField<int>(c, arms, "ForcedLodModel");
    CopyField<int>(c, arms, "iMinLODLevel");
    std::memcpy(reinterpret_cast<void*>(c + addr::kMohaSkelMeshFov), reinterpret_cast<const void*>(arms + addr::kMohaSkelMeshFov), 4);
    const int bo = names::PropertyOffset(c, "fCustomBoundsSize");
    if (bo >= 0) *reinterpret_cast<float*>(c + bo) = 200.0f;
    for (const char* b : {"CollideActors", "BlockActors", "BlockZeroExtent", "BlockNonZeroExtent", "BlockRigidBody"}) SetBit(c, b, false);
    SetBit(c, "CastShadow", Bit(arms, "CastShadow"));
    SetBit(c, "bCastDynamicShadow", Bit(arms, "bCastDynamicShadow"));
    SetObj(c, "PhysicsAsset", 0);
    // The arms' 'Camera' bone's FName (8 bytes) from their skeleton.
    const std::uintptr_t mesh = Obj(arms, "SkeletalMesh");
    const std::uintptr_t data = mesh ? names::ReadPointer(mesh + addr::kSkelMeshRefSkeleton) : 0;
    const int num = mesh ? static_cast<int>(ReadU32(mesh + addr::kSkelMeshRefSkeleton + 4)) : 0;
    std::uint8_t bone[8] = {};
    bool found = false;
    for (int i = 0; i < num && i < 512 && data && !found; ++i)
        if (names::NameAt(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride) == "Camera") {
            std::memcpy(bone, reinterpret_cast<const void*>(data + static_cast<std::uintptr_t>(i) * addr::kMeshBoneStride), 8);
            found = true;
        }
    slot = Slot{c, arms, pawn};  // known to the bake before its first update
    Call attach(arms, "AttachComponent");
    const float one[3] = {1.0f, 1.0f, 1.0f};  // RelativeScale: no default through ProcessEvent
    if (!found || !attach.Set("Component", &c, sizeof(c)) || !attach.Set("BoneName", bone, 8) ||
        !attach.Set("RelativeScale", one, sizeof(one)) || !attach.Run()) {
        MLOG("%s: carrier -- not attached (the Camera bone %s)", who, found ? "found" : "missing");
        slot = Slot{};
        return false;
    }
    if (!Bit(c, "bAttached")) MLOG("%s: carrier %s -- AttachComponent left it unattached", who, names::Name(c).c_str());
    else if (trace)
        MLOG("%s: carrier %s (mesh %s) attached to the arms' Camera bone", who, names::Name(c).c_str(),
             names::Name(Obj(c, "SkeletalMesh")).c_str());
    return true;
}

}  // namespace mohavr::carrier
