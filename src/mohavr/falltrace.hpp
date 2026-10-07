// A falling object's path traced into the world (GOAL C3, D69): dropped magazines, spent cases and rack-ejected rounds
// used to fall straight to the feet's height through tables and walls -- and, drawn in the first-person depth group, they
// then showed through them. The path, from p0 with v0 under gravity, is traced in 20 ms steps (aim::WorldTrace, ignoring
// the pawn); a step that hits mostly going down has landed on what it hit (a table), a step that hits mostly sideways has
// met a wall: its sideways motion stops there and it drops to whatever is under that point. Game thread.
#pragma once
#include <cstdint>

namespace mohavr::falltrace {

struct Result {
    float stopT = 1e9f;   // seconds after the drop when the sideways motion stops (a wall); 1e9 = never
    float floorZ = 0.0f;  // where it comes to rest (units): the floor given, or the top of what it landed on
    bool  wall = false, top = false;
};

// p0, v0: units and units/s (the bake's world); g: units/s^2 (down); floorZ: the floor if nothing is in the way. `mirror`
// (row vectors, drawn = baked x M), when the bake is in the left-hand mode's mirror world, maps the points into the real
// world for the traces (heights and times are the same in both: the mirror is a vertical plane).
Result Trace(std::uintptr_t pawn, const float (&p0)[3], const float (&v0)[3], float g, float floorZ, const float* mirror);

// Tests (Debug.GameCommands): "mohavr falltrace <m/s> [up m/s]" -- a path from the eye along the view's heading, logged.
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::falltrace
