// MOHAVR.log, next to the DLL. Logs state transitions and first occurrences, not per-frame
// noise (lessons 1). Thread-safe; every line is written through immediately so a crash
// loses nothing.
#pragma once
#include <string>

namespace mohavr::log {

// Opens <dir>\<name>.log, keeping the previous run as <name>.prev.log.
void Open(const std::wstring& dir, const std::wstring& name = L"MOHAVR");
void Line(const char* fmt, ...);
double MsSinceStart();

}  // namespace mohavr::log

#define MLOG(...) ::mohavr::log::Line(__VA_ARGS__)
