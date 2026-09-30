#include "reload.hpp"

#include <windows.h>

#include "../mohavr/log.hpp"

namespace mohavr::host {
namespace {
const char* kEventName[] = {"?", "EJECT", "INSERT", "RACK", "TAKE", "DROP"};
}

void ManualReload::Init(const std::wstring& ini) {
    on_ = GetPrivateProfileIntW(L"Weapon", L"ManualReload", 0, ini.c_str()) != 0;
    MLOG("reload: Weapon.ManualReload=%d", on_ ? 1 : 0);
}

void ManualReload::Queue(std::uint32_t type, double now) {
    if (type < shared::kReloadEject || type > shared::kReloadDrop) return;
    pending_.push_back({type, now});
}

void ManualReload::Update(shared::Header* hdr, double now, bool handsOk) {
    if (!hdr) return;
    std::uint32_t seq = 0;
    shared::ReloadGeo g{};
    if (shared::ReadReloadGeo(hdr, g, seq) && seq != lastGeoSeq_) {
        lastGeoSeq_ = seq;
        geoAt_ = now;
        geo_ = g;
        keyHash_ = shared::KeyHash(geo_.key);
    }
    const bool alive = geoAt_ >= 0.0 && now - geoAt_ < 0.25;
    const bool engaged = on_ && alive && handsOk && (geo_.caps & 1u);
    flags_ = (on_ ? 1u : 0u) | (engaged ? 16u : 0u);
    if (engaged != loggedEngaged_) {
        loggedEngaged_ = engaged;
        MLOG("reload: %s (%s)", engaged ? "engaged" : "not engaged",
             !on_ ? "switched off" : !alive ? "the game's side is quiet" : !handsOk ? "no hands" :
             !(geo_.caps & 1u) ? "no converted gun in hand" : "the game's side alive, hands tracked, a converted gun in hand");
    }
    if (geo_.key != loggedKey_ || geo_.state != loggedState_) {  // not every shot: the game logs the events' ammo
        loggedKey_ = geo_.key;
        loggedState_ = geo_.state;
        loggedClip_ = geo_.clip;
        loggedReserve_ = geo_.reserve;
        MLOG("reload: %s -- clip %d/%d, reserve %d; magazine %s%s%s%s%s", geo_.key[0] ? geo_.key : "(no gun)", geo_.clip,
             geo_.max, geo_.reserve, (geo_.state & 1u) ? "in" : "out", (geo_.state & 2u) ? ", pending" : "",
             (geo_.state & 4u) ? ", ready" : "", (geo_.state & 16u) ? ", rack needed" : "",
             (geo_.caps & 1u) ? "" : " (not converted)");
    }
    // Events, in order; never more than the ring holds unread (RELOAD-DESIGN 4).
    while (!pending_.empty()) {
        const Pending p = pending_.front();
        if (hdr->reloadEvtSeq - hdr->reloadEvtAck >= 8u) {
            if (now - p.at > 0.25) {
                MLOG("reload: %s dropped -- the game has not taken the last 8 events", kEventName[p.type]);
                pending_.pop_front();
            }
            break;
        }
        const std::uint32_t s = hdr->reloadEvtSeq;
        hdr->reloadEvt[s % 8u] = p.type | ((keyHash_ & 0xFFFFFFu) << 8);
        _ReadWriteBarrier();
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->reloadEvtSeq));
        MLOG("reload: sent %s for %s", kEventName[p.type], geo_.key[0] ? geo_.key : "(no gun)");
        pending_.pop_front();
    }
}

}  // namespace mohavr::host
