#include "landing.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cstdint>
#include <cwchar>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "aim.hpp"
#include "bridge.hpp"
#include "config.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"

namespace mohavr::landing {
namespace {

Config               g_cfg;
safetyhook::InlineHook g_hook;

// MOHAPlayerPawn.EAirDropLandingType (MOHAPlayerPawn.uc): the HUD shows "greased" for 0 and 6, "flared" for 5, "hard" for
// the rest; only 6 skips the gear's removal (AirDropLandingDone).
const char* TypeName(unsigned t) {
    static const char* kNames[] = {"perfect", "hard (forward)", "hard (back)", "hard (left)", "hard (right)", "flared",
                                   "greased"};
    return t < sizeof(kNames) / sizeof(kNames[0]) ? kNames[t] : "unknown";
}

// The switch: the menu's (shared block v39 perfectLanding: 1 off, 2 on) once the host has set it, else the ini's.
bool On() {
    const shared::Header* hdr = bridge::SharedHeader();
    const std::uint32_t m = hdr ? hdr->perfectLanding : 0u;
    return m == 1u || m == 2u ? m == 2u : g_cfg.perfectLanding;
}

// AMOHAPlayerPawn::execPerformLandingDetection: thiscall (ECX = the pawn), [esp+4] FFrame&, [esp+8] Result, RET 8. It
// reads the out byte's address (the script passes the pawn's own eLandingType) and the velocity, and has the pawn's
// virtual (vtable +0x42C) write the verdict through it. The verdict is in eLandingType once it returns.
void __fastcall Hook_ExecPerformLandingDetection(std::uintptr_t self, void* /*edx*/, void* stack, void* result) {
    g_hook.thiscall<void>(self, stack, result);
    if (!self || self != aim::LocalPlayerPawn()) return;
    const int off = names::PropertyOffset(self, "eLandingType");
    if (off < 0) {
        MLOG("landing: the pawn has no eLandingType -- the game's verdict stands");
        return;
    }
    auto* type = reinterpret_cast<std::uint8_t*>(self + static_cast<std::uintptr_t>(off));
    const unsigned judged = *type;
    if (!On()) {
        MLOG("landing: the game judged it %s (%u)", TypeName(judged), judged);
        return;
    }
    *type = 6;  // AIRDROP_LANDING_GREASED
    MLOG("landing: the game judged it %s (%u) -- made greased (Comfort.PerfectLanding)", TypeName(judged), judged);
}

}  // namespace

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr landing", 14) != 0) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const int off = pawn ? names::PropertyOffset(pawn, "eLandingType") : -1;
    const unsigned t = off >= 0 ? *reinterpret_cast<const std::uint8_t*>(pawn + static_cast<std::uintptr_t>(off)) : 255u;
    MLOG("landing: test -- the pawn in %s, eLandingType %u (%s); PerfectLanding %s; hooked %s",
         pawn ? names::StateName(pawn).c_str() : "(no pawn)", t, TypeName(t), On() ? "on" : "off",
         g_hook ? "yes" : "no");
    return true;
}

bool Install(const Config& cfg) {
    g_cfg = cfg;
    // Standing rule 4: the native's first 64 bytes (through the out byte's read and the velocity's).
    if (!patch::BytesMatch(addr::kExecPerformLandingDetection, addr::kExecPerformLandingDetectionBytes,
                           sizeof(addr::kExecPerformLandingDetectionBytes))) {
        MLOG("landing: execPerformLandingDetection bytes differ -- no hook: the game judges every landing (PerfectLanding off)");
        return false;
    }
    auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kExecPerformLandingDetection),
                                              reinterpret_cast<void*>(&Hook_ExecPerformLandingDetection));
    if (!res) {
        MLOG("landing: inline hook failed (error %d) -- the game judges every landing", static_cast<int>(res.error().type));
        return false;
    }
    g_hook = std::move(*res);
    MLOG("landing: execPerformLandingDetection hooked at 0x%08X (PerfectLanding %s in the ini; the menu's Comfort tab "
         "changes it live)", static_cast<unsigned>(addr::kExecPerformLandingDetection), cfg.perfectLanding ? "on" : "off");
    return true;
}

}  // namespace mohavr::landing
