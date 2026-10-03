#include "offknife.hpp"

#include <windows.h>

#include "../mohavr/log.hpp"

namespace mohavr::host {

void OffHandKnife::Init(const std::wstring& ini) {
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"OffHand", L"KnifeHold", L"toggle", v, 32, ini.c_str());
    gripMode_ = !_wcsicmp(v, L"grip");
    GetPrivateProfileStringW(L"OffHand", L"KnifeGrip", L"forward", v, 32, ini.c_str());
    icepick_ = !_wcsicmp(v, L"icepick") || !_wcsicmp(v, L"reverse");
    MLOG("offknife: hold by %s, the %s grip", gripMode_ ? "the grip" : "a click", icepick_ ? "icepick" : "forward");
}

void OffHandKnife::SetOn(bool on) {
    if (on == on_) return;
    on_ = on;
    MLOG("offknife: %s", on ? "on" : "off");
    if (!on && held_) Let("switched off");
}

void OffHandKnife::Let(const char* why) {
    held_ = false;
    drawnSeen_ = false;
    ++epoch_;
    MLOG("offknife: let go (%s)", why);
}

void OffHandKnife::Poll(const shared::Header* hdr, double now) {
    now_ = now;
    shared::KnifeInfo k{};
    if (!hdr || !shared::ReadKnifeInfo(hdr, k)) return;
    if (haveInfo_ && k.pawnSeq != pawnSeq_ && held_) Let("a new pawn");
    pawnSeq_ = k.pawnSeq;
    haveInfo_ = true;
    info_ = k;
    if (!held_) return;
    if (!(k.caps & 4u)) {
        Let("the Dagger isn't earned");
        return;
    }
    // Drawn by the game? A draw it can't make (the template gone, the bake off) lets go after 2.5 s rather than holding an
    // invisible knife (the game retries meanwhile); once drawn, it may hide a while (a weapon switch) and stays held.
    if (k.caps & 8u) drawnSeen_ = true;
    else if (!drawnSeen_ && now - drawAt_ > 2.5) Let("the game didn't draw it");
}

bool OffHandKnife::DrawPress(Pulse& p) {
    if (!on_) return false;
    const bool can = haveInfo_ && (info_.caps & 3u) == 3u;
    if (!can) {
        p = {0.2f, 60.0f};
        const std::uint32_t r = (info_.state >> 8) & 0xFFu;
        MLOG("offknife: draw refused -- %s", !haveInfo_ ? "no word from the game" : !(info_.caps & 1u) ? "not installed (no template, or no arm bake)"
                                             : !(info_.caps & 4u) ? "the Dagger isn't earned (the MP40 at upgrade level 2)"
                                             : r == 1u ? "unavailable (no gun in hand, a cinematic, the parachute)"
                                             : r == 4u ? "the draw failed (no off-hand frame yet?)" : "not now");
        return true;
    }
    held_ = true;
    drawnSeen_ = false;
    drawAt_ = now_;
    ++epoch_;
    p = {0.5f, 40.0f};
    MLOG("offknife: drawn (%s grip)", icepick_ ? "icepick" : "forward");
    return true;
}

void OffHandKnife::HeldPress(bool atHolster, Pulse& p) {
    if (!held_ || gripMode_ || !atHolster) return;
    Let("put back");
    p = {0.4f, 30.0f};
}

void OffHandKnife::Frame(bool gripHeld, Pulse& p) {
    if (held_ && gripMode_ && !gripHeld) {
        Let("the grip let go");
        p = {0.4f, 30.0f};
    }
}

std::uint32_t OffHandKnife::Flags() const { return (on_ ? 1u : 0u) | (held_ ? 2u : 0u) | (icepick_ ? 4u : 0u); }

}  // namespace mohavr::host
