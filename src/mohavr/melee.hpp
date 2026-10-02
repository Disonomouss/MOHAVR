// Physical melee (MELEE-DESIGN; the player, 2026-10-03): a swing of the drawn gun's butt (a pistol's grip; the M12's
// bayonet at level 2) through a soldier does the game's own melee -- its damage, credit and impact effects -- with no
// button and no animation. The speed comes from the host's gun pose in LOCAL (walking and turning never count), the
// contact from the drawn gun in the world, the damage from the game's own TakeDamage. Game thread (the pre-draw).
//
// Tests (Debug.GameCommands): "mohavr melee" (a dump), "mohavr melee enemy [dist [right]]" (the nearest axis soldier
// moved in front of the player, his AI off), "mohavr melee enemy health <n>", "mohavr melee enemy off", "mohavr melee
// status", "mohavr melee hit [head]" (the strike executor alone, on the test soldier).
#pragma once
#include <cstdint>

namespace mohavr {
struct Config;
namespace shared {
struct Header;
}
}  // namespace mohavr

namespace mohavr::melee {

// From view::Install: the config, and whether the arm bake runs (the drawn gun's move comes from it).
void Configure(const Config& cfg, bool bake);
// Once per Draw, after offpistol::OnDraw.
void OnDraw(shared::Header* hdr);
// "mohavr melee ..." test lines; false for any other line.
bool TestCommand(const wchar_t* line);

}  // namespace mohavr::melee
