// D89 (the player: "Have option for the ammo model to appear in the ammo holster when gun unloaded. On by default."): with
// the gun's magazine out (the manual reload's state), a spare -- the gun's own magazine bone, from a clone of its class
// default WeaponMeshComponent, as the rack eject's round (D54) -- stands in the belt pouch, its grab point where the hand
// takes one, until a magazine is taken.
#pragma once
#include <cstdint>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::pouchmag {

void Configure(bool on, bool bake);
// The reload bake (game thread), per update of the gun in hand: whether a spare is wanted now, and its look -- the
// attachment class, the magazine bone, that bone's component-space matrix in the gun as seated (row-major 4x4), the grab
// point (component space) and the drawn gun's scale (world units per mesh unit).
void Want(bool show, const std::string& attachment, const std::string& bone, const float* boneComp, const float (&grab)[3],
          float scale);
// Per Draw (game thread): the carrier attached or detached; its frame from the host's pouch (hdr->pouchPos).
void OnDraw(shared::Header* hdr);
// The arm bake: whether `comp` is the spare's carrier; its bone and world frame (false: nothing drawn).
bool IsCarrier(std::uintptr_t comp);
bool BoneFrame(std::uintptr_t comp, int& bone, float (&world)[16]);

}  // namespace mohavr::pouchmag
