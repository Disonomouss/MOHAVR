#include "carrier.hpp"

#include <cstring>

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
