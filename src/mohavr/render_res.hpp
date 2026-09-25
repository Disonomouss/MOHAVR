// Render resolution (Render.ResX / Render.ResY, M9): the eye images are halves of the game's
// backbuffer, so a bigger backbuffer = sharper eyes. UE3 takes its resolution from `ResX=` / `ResY=`
// on the command line (first match wins; ENGINE-NOTES 5c/5p), so the game's own import of
// GetCommandLineW is swapped to return the command line with `-windowed ResX=.. ResY=..` put first.
// Nothing on disk changes; the Steam launch options are untouched.
#pragma once

namespace mohavr {
struct Config;
}

namespace mohavr::render {

// Before WinMain (DllMain). No-op when Render.ResX/ResY are 0.
bool InstallResolution(const Config& cfg);

}  // namespace mohavr::render
