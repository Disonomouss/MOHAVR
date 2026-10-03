// The wrist HUD's game side (WRISTHUD-DESIGN.md, D55; proven by the W0 spike, work/research/wristhud): with the HUD placed on
// the wrist, the game's HUD pass is drawn once, into a render target of the mod's own, instead of into the eye images. The
// bridge copies it beside each frame into a second shared texture ring (shared block v28); the host crops the wrist groups
// out of it onto the off hand and shows the rest head-locked.
//
// Game thread: at the HUD loop's start the stereo Draw's player count is cut to 1 (eye 0 only); eye 0's canvas becomes
// WristCanvas x 9/16 at (0,0) and its matrix scales the backbuffer-sized clip mapping so that canvas pixel p lands on texture
// pixel p. Between the matrix push and the closing flush, every FCanvas::Flush with a pending batch records that batch (an
// SPSC ring); after the closing flush the HUD elements' rectangles are read (reflection) for that pass. Render thread:
// FlushCommand::Execute finds its batch in the ring and draws it with the mod's render target bound and premultiplied alpha
// forced (the game's canvas blend writes no alpha: ENGINE-NOTES 5br); the old target, depth surface, viewport and scissor
// are restored after. After each Present the texture is cleared, so a frame without a HUD pass publishes an empty one.
#pragma once
#include <cstdint>
#include <string>

#include "../common/shared_frame.hpp"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace mohavr {
struct Config;
}

namespace mohavr::hudtex {

// Installs the hooks (FCanvas::Flush entry, after the HUD loop's closing flush, FlushCommand::Execute) when [HUD] Mode=1
// and [HUD] Redirect=1. Each prologue (and the FlushCommand vtable's slot) is checked first; a mismatch stands down.
bool Install(const Config& cfg);
bool Installed();
void CanvasSize(int& w, int& h);    // the texture's (and the wrist pass canvas's) size

// The bridge, once its HUD texture ring is shared (render thread): only then can a wrist pass be shown.
void SetShared(bool ok);

// Game thread, at the HUD loop's start in a stereo Draw: true = this Draw's HUD goes to the texture (the caller cuts the
// loop to one player) -- the place is the wrist (the host's hdr->hudPlace, else [HUD] Place), the render target and the
// shared ring are ready, and no pass has failed. False otherwise (the screen panel).
bool PlanDraw(const shared::Header* hdr);
bool ThisDraw();                    // PlanDraw said yes for the current Draw
// Game thread, at the HUD matrix push of a wrist Draw: the pass opens (batches flushed from now on are the HUD's).
// `localPlayer` = the ULocalPlayer whose HUD draws (its HUD's elements are read after the pass).
void BeginPass(std::uintptr_t localPlayer);
// The canvas matrix wasn't the identity-plus-translation Draw builds: this pass draws where it lands (eye 0), and the
// wrist HUD falls back to the screen panel for the rest of the session (logged once).
void FailPass();
void EndDraw();                     // Hook_Draw after the original: closes anything left open

// Render thread (Present, before the bridge): creates the render target lazily, keeps the alpha filter hooked, logs.
void OnPresent(IDirect3DDevice9* dev);
// The bridge's publish (render thread, inside Present): the texture (AddRef'd: the caller Releases it) and what it holds
// this frame (flags, canvas, the live resolution scale and the element rectangles of the pass drawn); nullptr if none.
IDirect3DSurface9* ForPublish(shared::SlotHud& slot);
// Render thread, after the bridge's publish: clears the texture for the next frame (if anything was drawn).
void EndFrame(IDirect3DDevice9* dev);
// Render thread (a harness capture): writes the texture to <dir>\capture-hud.bmp (32-bit, alpha kept).
void OnCapture(IDirect3DDevice9* dev, const std::wstring& dir);
// Main thread, before IDirect3DDevice9::Reset: releases the render target (D3DPOOL_DEFAULT).
void OnBeforeReset();

// Tests (Debug.GameCommands, game thread): "mohavr hud hit <yaw>" (a hit indicator from that direction, rotator units),
// "mohavr hud objective" (an objective message), "mohavr hud badges" (the level badges, as on a weapon switch), "mohavr hud
// status" (the place, the passes, the HUD). `player`
// = the ULocalPlayer. False if the line isn't one of these.
bool TestCommand(std::uintptr_t player, const wchar_t* line);

}  // namespace mohavr::hudtex
