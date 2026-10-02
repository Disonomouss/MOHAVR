// Scopes (SCOPE-DESIGN.md) -- game side. Once per Draw: whether the gun in hand has its scope on (the game's
// IsScopeEnabled), where the scope's tube is in the host's gun frame (the eyepiece, the objective, the eyepiece's radius,
// taken from the drawn gun while it's quiet, as physical melee takes its levers), the game's zoom (ScopeParams) and the
// real scope's magnification -- for the host (shared block v24, scopeSeq). The scope view itself is the third player
// of the stereo Draw (vr_view.cpp).
#pragma once
#include <cstdint>

#include "../common/shared_frame.hpp"
#include "config.hpp"

namespace mohavr::scope {

// `bake`: the arm bake runs (the drawn gun is the mod's) -- without it no geometry.
void Configure(const Config& cfg, bool bake);
void OnDraw(shared::Header* hdr);
// Debug.GameCommands: "mohavr scope info" logs the gun in hand's scope (its state, ScopeParams, the tube).
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::scope
