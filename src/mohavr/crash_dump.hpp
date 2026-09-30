// Debug.CrashDump (round 29: the game crashed twice as the pause menu opened -- an access violation in ucrtbase's memcpy
// under d3d9on12.dll, copying a mesh's vertex range; ENGINE-NOTES 5ak). The small dumps Windows keeps hold no heap, so
// which mesh it was drawing stayed unknown. A vectored handler writes, once, a dump with the memory the stack points at
// (the mesh element, its buffers) to %TEMP%\MOHAVR\crash-<pid>.dmp and logs a stack scan, then lets the game's own
// handling carry on -- it changes nothing about the crash itself.
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::crashdump {

// DllMain-safe: registers the handler (no LoadLibrary until an exception is caught).
void Install(const Config& cfg);
// Once per Draw (game thread): Debug.CrashDumpTest runs a caught access violation in ucrtbase's memcpy once, to prove
// the dump and the log.
void OnDraw();

}  // namespace mohavr::crashdump
