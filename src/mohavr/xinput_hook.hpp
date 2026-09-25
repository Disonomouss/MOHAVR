// The virtual Xbox pad (M6, Input.Controllers). MOHA polls XInputGetState(0..3) every frame while
// its window has focus -- WindowsClientInit always creates four XInput joystick slots, pad or no pad
// (ENGINE-NOTES 5k). A verified swap of the game's IAT slot answers pad 0 from the host's shared
// block (the headset's controllers, mapped to an Xbox layout by the host); everything else, and pad 0
// whenever the host isn't driving it, goes to the real XInput.
#pragma once

namespace mohavr::xinput {

// Installs the IAT hook if the slot holds exactly the loaded XInput DLL's ordinal-2 export.
// Logs and returns false (game untouched) otherwise.
bool Install();

}  // namespace mohavr::xinput
