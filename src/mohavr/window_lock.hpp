// Keeps the game window at the size its device renders (Render.LockWindow). The stereo pair fills the
// whole backbuffer, and the host splits it down the middle. If the window's client shrinks (Windows fits
// a window bigger than the desktop -- e.g. 2880x1620 on a 1440p screen -- when the player drags it), UE3
// draws the viewport into the top-left corner of the unchanged backbuffer: each eye half then holds parts
// of both eyes (headset rounds 5-6, "cross-eyed after tabbing out and moving the window"). Moving the
// window stays allowed; only size changes are refused.
#pragma once

namespace mohavr::winlock {

// Call per Present with the backbuffer size. Finds the game's top-level window (class
// LaunchUnrealUWindowsClient) and, once its client area is exactly the backbuffer (the game has finished
// sizing it), subclasses it and pins that size. Cheap after that.
void Watch(unsigned backbufferW, unsigned backbufferH);

}  // namespace mohavr::winlock
