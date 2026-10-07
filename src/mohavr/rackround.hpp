// The rack eject's visible round (D54; the player after round 50: "Sliding back the bolt/slide and pumping the shotgun
// should eject a round visibly and it should count as a round spent") -- game side.
// The game has no live-cartridge mesh (its shell meshes are empty cases, ENGINE-NOTES 5bq), so the round is a gun mesh's
// round bone: a clone of a class-default attachment's WeaponMeshComponent (FindObject, as the knife's template, D51)
// attached to the arms as a carrier; the arm bake (arms_ik BakeCarrier) draws only its round bone -- every other bone
// collapsed -- on a ballistic fall from the drawn gun's ShellEject port, and it lies at the feet a moment before it is gone.
// Two rounds may be in flight (the oldest is reused for a third).
#pragma once
#include <cstdint>
#include <string>

#include "config.hpp"

namespace mohavr::rackround {

// Where a gun's thrown round comes from ([ManualReload] <gun line> RackRound=<Attachment>.<bone> RackRoundLen=
// RackRoundScale=).
struct Source {
    std::string attachment;            // the class whose default WeaponMeshComponent holds the round (Attachment_Springfield)
    std::string bone;                  // the round's bone: its origin at the round's base, the round along its +Z
    float       len = 0.0f;            // the round's length along that Z (mesh units)
    float       scaleLen = 1.0f;       // drawn scaled along its length / across it (the StG44's short round from the G43's)
    float       scaleWidth = 1.0f;
};

// `bake`: the arm bake is installed (it draws the round); without it no round is thrown (the caller throws brass).
void Configure(const Config& cfg, bool bake);
// Throws a round from the port. `rows`: the drawn gun's mesh axes in the world (rows X, Y, Z, scale included; the round
// lies along mesh +Z as chambered); `port`: the ShellEject socket's world frame (rows forward, right, up, then its origin);
// `vel`: the port's own velocity (units/s); `floorZ`: where it comes to rest; `upm`: units per metre. The world is the
// bake's (the mirror world in left-hand mode). False (logged) when there is no template or no bone: the caller falls back
// to the gun's brass.
bool Throw(std::uintptr_t pawn, const Source& src, const float* rows, const float* port, const float* vel, float floorZ,
           float upm);
// GOAL E (the mission sweep): a fresh look-up of a source's template and bone in this level -- "found", or what is missing.
std::string CheckSource(std::uintptr_t pawn, const Source& src);
// Per Draw (game thread): the finished rounds detached; a new pawn starts over.
void OnDraw();
// The arm bake: whether `comp` is one of the round carriers; its round bone's index and world frame now (false: nothing
// drawn -- every bone collapsed).
bool IsCarrier(std::uintptr_t comp);
bool BoneFrame(std::uintptr_t comp, int& bone, float (&world)[16]);

}  // namespace mohavr::rackround
