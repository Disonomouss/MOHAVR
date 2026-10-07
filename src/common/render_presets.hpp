// Render resolution presets (D73): the game renders both eyes side by side, so its ResX = 2 x the per-eye width and ResY =
// the per-eye height. "custom" = the shipped [Render] ResX/ResY; "auto" = what the headset's runtime recommended in the last
// session (the host saves it, %LOCALAPPDATA%\MOHAVR\MOHAVR.headset.ini); the others = each headset's panel per eye.
// Shared by the game (render_res / config: applied at start) and the host (the menu's choice).
#pragma once

namespace mohavr::presets {

struct Preset {
    const char*    key;    // saved as [Render] Preset=<key> in the player's ini
    const char*    label;  // the menu's
    int            eyeW, eyeH;  // 0 = custom / auto (resolved elsewhere)
};

inline constexpr Preset kPresets[] = {
    {"custom", "Custom (the ini's ResX/ResY)", 0, 0},
    {"auto", "Auto (what the headset asks for)", 0, 0},
    {"quest2", "Meta Quest 2 / 3S", 1832, 1920},
    {"quest3", "Meta Quest 3", 2064, 2208},
    {"questpro", "Meta Quest Pro", 1800, 1920},
    {"pico4", "Pico 4", 2160, 2160},
    {"index", "Valve Index", 1440, 1600},
    {"reverbg2", "HP Reverb G2", 2160, 2160},
    {"vivepro2", "HTC Vive Pro 2", 2448, 2448},
    {"rifts", "Oculus Rift S", 1280, 1440},
    {"psvr2", "PlayStation VR2", 2000, 2040},
    {"beyond", "Bigscreen Beyond", 2560, 2560},
};
inline constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// Per-eye caps (the 32-bit game's address space: render targets grow with the pixel count; D57, D73).
inline constexpr int kMaxEye = 2560;

inline int Find(const char* key) {
    for (int i = 0; i < kPresetCount; ++i) {
        const char* a = kPresets[i].key;
        const char* b = key;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) ++a, ++b;
        if (!*a && !*b) return i;
    }
    return 0;  // unknown: custom
}

}  // namespace mohavr::presets
