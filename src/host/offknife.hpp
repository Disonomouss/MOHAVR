// The off-hand knife (OFFKNIFE-DESIGN.md), host side: the off hand takes the MP40's Dagger from a holster ([Holsters]
// LowerBack=Knife) and puts it back. Level-triggered: the host holds whether it is held (knifeFlags, inside the view seqlock),
// the game draws or sheathes the knife to match and publishes whether a draw can happen (knifeCaps, v25).
//   * A draw: the off hand's press at a knife holster, while a draw can happen (the Dagger earned, a gun in the other hand);
//     otherwise a short refusal pulse.
//   * Held: the off hand's presses are the knife's -- with KnifeHold=toggle a press at any holster puts it back; with
//     KnifeHold=grip letting go does. The foregrip, the reload's spots, the grenade and the pistol are out of reach.
//   * Let go of by the host: the switch off, a new pawn, the Dagger no longer earned.
#pragma once
#include <cstdint>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class OffHandKnife {
public:
    // Reads [OffHand] KnifeHold / KnifeGrip from the shipped ini.
    void Init(const std::wstring& ini);
    // The switch (the menu's Weapons tab "Off-hand knife"; the shipped [OffHand] Knife until the player toggles it).
    void SetOn(bool on);
    bool On() const { return on_; }
    // Per XR frame, before the hands: the game's caps (a held knife is let go of when it may not stay).
    void Poll(const shared::Header* hdr, double now);
    bool Holding() const { return held_; }

    struct Pulse {
        float amp = 0.0f, ms = 0.0f;
    };
    // The off hand pressed at a knife holster (with nothing else held): draws, or refuses. True = the press was the knife's.
    bool DrawPress(Pulse& p);
    // The off hand pressed while it holds the knife: at a holster (toggle) it goes back.
    void HeldPress(bool atHolster, Pulse& p);
    // Per frame: the off hand's grip (grip mode: letting go puts it back).
    void Frame(bool gripHeld, Pulse& p);
    // knifeFlags: bit0 on, bit1 held, bit2 the icepick grip.
    std::uint32_t Flags() const;
    // +1 on every draw and put back (the off hand's pose jumps then: physical melee takes no speed across it).
    std::uint32_t Epoch() const { return epoch_; }

private:
    void Let(const char* why);
    bool          on_ = false;
    bool          gripMode_ = false;  // [OffHand] KnifeHold=grip (else toggle)
    bool          icepick_ = false;   // [OffHand] KnifeGrip=icepick (else forward)
    bool          held_ = false;
    bool          drawnSeen_ = false; // the game has drawn this hold (caps bit3)
    double        drawAt_ = 0.0, now_ = 0.0;      // when it was drawn (s): a draw the game hasn't made in 2.5 s is let go
    bool          haveInfo_ = false;
    shared::KnifeInfo info_{};
    std::uint32_t pawnSeq_ = 0;
    std::uint32_t epoch_ = 0;
};

}  // namespace mohavr::host
