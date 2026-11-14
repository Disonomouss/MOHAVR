// The parachute landing ([Comfort] PerfectLanding, D108) -- game side.
//
// The pawn's AirDropLanding state judges the touchdown once, in its BeginState: PerformLandingDetection(eLandingType,
// Velocity) (a native: the flare's timing and the speed), then PostLandingTypeDetermination reads eLandingType for the
// HUD's icon, the stats, the level's landed events, the landing animation's length and, at its end, whether the gear
// comes off (every type but greased). A hook on the native's exec sets the local player's verdict to greased (6, the
// game's best: the "Greased landing" icon, the run-out, no gear-removal pause) when the switch is on, before any of that
// reads it. ENGINE-NOTES 5ca.
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
}

namespace mohavr::landing {

// Verifies execPerformLandingDetection's bytes and hooks it (always: the hook logs the game's verdict, and changes it
// only while the switch is on). Called by view::Install.
bool Install(const Config& cfg);
// Tests (Debug.GameCommands, the game thread): "mohavr landing" logs the pawn's state, eLandingType and the switch.
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::landing
