// Game side of the out-of-process OpenXR bridge (D10). Requires Bridge.D3D9On12=1: every D3D9
// resource is then a D3D12 resource, so the finished frame can be copied into shared D3D12
// textures without leaving the GPU. MOHAVR-host.exe (x64) owns OpenXR and reads them.
//
// Threads: StartHost() runs on the main thread (from the CreateDevice hook); OnPresent() on the
// game's render thread. The game's D3D9 device is not multithreaded (flags 0x142), so all D3D9
// calls happen in OnPresent.
#pragma once
#include <string>

#include "../common/shared_frame.hpp"

struct IDirect3DDevice9;

namespace mohavr::bridge {

// The shared block, or null before StartHost / if it failed.
shared::Header* SharedHeader();

// Creates the shared-memory block and launches MOHAVR-host.exe from this DLL's folder.
// `defaultUnitsPerMeter` (the ini value) seeds the host's World Scale setting.
void StartHost(const std::wstring& runtimeJson, float defaultUnitsPerMeter, int mirror, bool controllers);  // Bridge.Mirror, Input.Controllers

// Per Present, before the real Present: lazy setup, then publish the frame if the host is ready
// for one. Never blocks on the CPU.
void OnPresent(IDirect3DDevice9* device);

// Device reset (alt-tab from fullscreen): D3D9 refuses Reset while our D3DPOOL_DEFAULT render
// target exists, so release it before and recreate it lazily after. A size change pauses the
// bridge (the shared textures and the host's swapchain have the old size).
void OnBeforeReset();
void OnAfterReset(unsigned width, unsigned height);

// Frame pacing (hdr->pace, the host menu's Frame pacing; vr_view.cpp, game thread): the host's frame event --
// auto-reset, set once per headset frame, just after the host wrote that frame's poses and took the last game frame.
// 1 when it came within `timeoutMs`, 0 on a timeout, -1 with no host running (no wait).
int WaitHostFrame(unsigned timeoutMs);
bool HostRunning();
// The Draw hook that paces is installed: while hdr->pace is on, a frame is published even when the host hasn't taken
// the last one yet (it takes the newest).
void SetPacingAvailable(bool on);
// OnPresent calls so far (render thread; counted after the frame's publish), and a wait until they reach `target`.
std::uint32_t PresentsSeen();
bool WaitPresents(std::uint32_t target, unsigned timeoutMs);

}  // namespace mohavr::bridge
