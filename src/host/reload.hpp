// Manual reload (D21, RELOAD-DESIGN.md) -- host side. M1: the player's toggle ([Weapon] ManualReload), "engaged" (the
// game's side alive, the hands tracked, a converted gun in hand), and the events to the game (shared block v14, an
// ordered ring). The test channel (pad_cmd.txt "reload=eject|take|insert|rack|drop") queues events; the gestures (M3/M4)
// will use the same queue.
#pragma once
#include <cstdint>
#include <deque>
#include <string>

#include "../common/shared_frame.hpp"

namespace mohavr::host {

class ManualReload {
public:
    void Init(const std::wstring& ini);
    // Per XR frame, before the view block is written.
    void Update(shared::Header* hdr, double now, bool handsOk);
    // Written inside the view seqlock.
    std::uint32_t Flags() const { return flags_; }
    std::uint32_t KeyHash() const { return keyHash_; }
    void Queue(std::uint32_t type, double now);

private:
    bool          on_ = false;
    std::uint32_t flags_ = 0, keyHash_ = 0;
    std::uint32_t lastGeoSeq_ = 0;
    double        geoAt_ = -1.0;
    shared::ReloadGeo geo_{};
    struct Pending {
        std::uint32_t type;
        double        at;
    };
    std::deque<Pending> pending_;
    std::string   loggedKey_;
    std::uint32_t loggedState_ = 0xFFFFFFFFu;
    std::int32_t  loggedClip_ = -1, loggedReserve_ = -1;
    bool          loggedEngaged_ = false;
};

}  // namespace mohavr::host
