// Head tracking and headset projection (M3) -- game side.
//
// Two kinds of MidHook in ULocalPlayer::CalcSceneView (ENGINE-NOTES 5g), both on the game thread:
//   * view merge (0x10C19B3C): ViewRotation := game yaw o head orientation (the game's own pitch
//     and roll are dropped); ViewLocation += yaw-rotated head position;
//   * after FPerspectiveMatrix (0x10C19EAB / 0x10C19DAF): the projection becomes the headset's
//     asymmetric FOV (mono: union of both eyes, widened to the viewport aspect).
// Head pose and eye FOVs come from MOHAVR-host.exe via the shared block (bridge).
// The pose/FOV each frame was rendered with is handed back to the host with that frame.
#pragma once
#include "../common/shared_frame.hpp"

namespace mohavr {
struct Config;
}

namespace mohavr::view {

// Verifies every hook site's bytes and installs the MidHooks. Call once, outside DllMain's
// loader lock is not required (no LoadLibrary) -- but the shared block must exist before the
// hooks do anything useful; until then they pass the game's values through untouched.
bool Install(const Config& cfg);

// Called by the bridge on the render thread when publishing a frame: the render pose/FOV of the
// frame being presented. Returns false (meta.hasView = 0) if head tracking wasn't applied.
bool MetaForPresentedFrame(shared::SlotMeta& meta);

// M7: a tracked pose (head or controller, OpenXR LOCAL) as a ray in the world, mapped exactly like the
// eyes of the player's last head-tracked view: `pos` and unit `fwd` in Unreal units/axes. `unitsPerMeter`
// = the scale in use. Game thread; false before the first such view.
bool PoseToWorld(const shared::Pose& p, float (&pos)[3], float (&fwd)[3], float& unitsPerMeter);

}  // namespace mohavr::view
