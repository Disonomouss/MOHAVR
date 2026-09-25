// On-request copy of the game's backbuffer to a BMP, for the harness (D9).
//
// Needed because the game's window cannot be relied on to show the frame: under D3D9On12 the
// presents succeed but never reach the window (ENGINE-NOTES 5e), and in VR the frame goes to
// the headset. The harness signals the named event Local\MOHAVR_Capture; the next Present
// writes %TEMP%\MOHAVR\capture.bmp (written to .tmp first, then renamed). Nothing is written
// into the game folder.
#pragma once

struct IDirect3DDevice9;

namespace mohavr::capture {

void Init();                               // creates the event; call once, any thread
void OnPresent(IDirect3DDevice9* device);  // cheap when no request is pending

}  // namespace mohavr::capture
