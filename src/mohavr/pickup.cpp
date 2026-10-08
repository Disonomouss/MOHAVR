// D77: taking a weapon -- or a crate's ammo or grenades -- by closing a free grip on it ([Controls] GrabPickup; the menu's
// hdr->pickupMode). The game's way to them is a context-sensitive action (MOHACSA): touching its cylinder puts it in the
// player controller's csaList, and holding use calls its UsedBy(the pawn). A dropped weapon (MOHAWeaponDroppedPickup) and
// a placed one (MOHAWeaponPickupFactory) have a MOHAWeaponCSA, whose UsedBy swaps it for the gun of its kind the player
// carries (MOHAInventoryManager.SwapWeapon); a crate (MOHAPickupCrate) has a MOHAPickupCrateCSA, whose UsedBy gives its
// contents. Here, per Draw: each tracked hand's distance to each listed pickup's actor (hdr->pickupNear); when the host
// says a free grip closed there (hdr->pickupReqSeq), the nearest one to that hand is used -- IsUsableBy first, then
// UsedBy, as the game's hold does.
#include "pickup.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "bridge.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"
#include "vr_view.hpp"

namespace mohavr::pickup {
using namespace script;
namespace {

bool g_on = true;
constexpr float kReachM = 0.35f;  // metres from the hand to the pickup's actor

struct Spot {
    std::uintptr_t csa, item;  // the CSA and the thing taken (the dropped weapon, the factory, the crate)
    bool crate;
    float loc[3];
};

// MOHACSA.IsUsableBy(the controller): a weapon the player already carries, or one while a grenade is out, is not (the
// game's hold wouldn't take it either). Asked at most every half second per CSA.
bool Usable(std::uintptr_t csa, std::uintptr_t ctrl, bool fresh = false) {
    struct Seen {
        std::uintptr_t csa;
        DWORD at;
        bool can;
    };
    static Seen cache[8] = {};
    const DWORD now = GetTickCount();
    Seen* slot = &cache[0];
    for (Seen& c : cache) {
        if (c.csa == csa) {
            if (!fresh && now - c.at < 500) return c.can;
            slot = &c;
            break;
        }
        if (now - c.at > now - slot->at) slot = &c;
    }
    Call usable(csa, "IsUsableBy", true);
    bool can = true;
    if (usable.ok) {
        const int po = usable.Off("PC") >= 0 ? usable.Off("PC") : usable.Off("PlayerController");
        if (po >= 0) {
            std::memcpy(usable.parms + po, &ctrl, sizeof(ctrl));
            can = usable.Run() && usable.ReturnBool();
        }
    }
    *slot = {csa, now, can};
    return can;
}

// The pickups the player is at (the controller's csaList): weapons and crates, enabled and usable now.
std::vector<Spot> Spots(std::uintptr_t ctrl, bool usableOnly = true) {
    std::vector<Spot> out;
    const int lo = ctrl ? names::PropertyOffset(ctrl, "csaList") : -1;
    if (lo < 0) return out;
    const std::uintptr_t data = names::ReadPointer(ctrl + lo);
    const int num = static_cast<int>(names::ReadPointer(ctrl + lo + 4));
    for (int i = 0; data && i < num && i < 32; ++i) {
        const std::uintptr_t csa = names::ReadPointer(data + 4u * static_cast<std::uintptr_t>(i));
        if (!csa || Bit(csa, "bDeleteMe") || !Bit(csa, "bEnabled")) continue;
        Spot s{csa, 0, false, {0, 0, 0}};
        if (names::IsA(csa, "MOHAWeaponCSA")) {
            s.item = Obj(csa, "DroppedWeaponPickup");
            if (!s.item) s.item = Obj(csa, "WeaponPickup");
        } else if (names::IsA(csa, "MOHAPickupCrateCSA")) {
            s.item = Obj(csa, "InventoryPickup");
            s.crate = true;
        } else {
            continue;
        }
        if (usableOnly && !Usable(csa, ctrl)) continue;
        names::ReadVector((s.item ? s.item : csa) + addr::kActorLocation, s.loc);
        out.push_back(s);
    }
    return out;
}

// The hands in the world (hdr->hand: 0 left, 1 right).
int Hands(const shared::Header* hdr, float (&pos)[2][3], float& upm) {
    shared::Pose hand[2];
    std::uint32_t valid = 0;
    int bits = 0;
    if (!shared::ReadHands(hdr, hand, valid)) return 0;
    for (int h = 0; h < 2; ++h) {
        float fwd[3], u = 100.0f;
        if ((valid & (1u << h)) && view::PoseToWorld(hand[h], pos[h], fwd, u)) {
            bits |= 1 << h;
            upm = u;
        }
    }
    return bits;
}

float Dist(const float (&a)[3], const float (&b)[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The nearest spot within reach of hand h (-1 none).
int Nearest(const std::vector<Spot>& spots, const float (&hand)[3], float upm, float* outDist = nullptr) {
    int best = -1;
    float bd = kReachM * upm;
    for (int i = 0; i < static_cast<int>(spots.size()); ++i) {
        const float d = Dist(hand, spots[i].loc);
        if (d <= bd) {
            bd = d;
            best = i;
        }
    }
    if (outDist) *outDist = bd;
    return best;
}

// Whether the pawn carries a weapon of this class (its InventoryManager's chain).
bool Carried(std::uintptr_t pawn, const std::string& cls) {
    const std::uintptr_t inv = Obj(pawn, "InvManager");
    std::uintptr_t item = inv ? Obj(inv, "InventoryChain") : 0;
    for (int n = 0; item && n < 64; ++n, item = Obj(item, "Inventory"))
        if (names::ClassName(item) == cls) return true;
    return false;
}

std::string What(const Spot& s) {
    if (s.crate) return "a crate (" + names::ClassName(s.item) + ")";
    const std::uintptr_t inv = s.item ? Obj(s.item, "Inventory") : 0;
    if (inv) return names::ClassName(inv);
    const int co = s.item ? names::PropertyOffset(s.item, "WeaponPickupClass") : -1;
    if (co >= 0) return names::Name(names::ReadPointer(s.item + co));
    return names::ClassName(s.item ? s.item : s.csa);
}

// Takes spot s for the pawn, as the game's hold does: IsUsableBy(the controller), then UsedBy(the pawn).
bool Take(const Spot& s, std::uintptr_t pawn, std::uintptr_t ctrl, const char* why) {
    const bool can = Usable(s.csa, ctrl, true);
    const std::string what = What(s);
    if (!can) {
        MLOG("pickup: %s -- %s not usable now (IsUsableBy false)", why, what.c_str());
        return false;
    }
    Call use(s.csa, "UsedBy");
    const bool ran = use.ok && use.Set("User", &pawn, sizeof(pawn)) && use.Run();
    const bool took = ran && use.ReturnBool();
    const int wo = names::PropertyOffset(pawn, "Weapon");
    MLOG("pickup: %s -- %s.UsedBy(the player) %s%s; in hand now %s", why, names::Name(s.csa).c_str(), ran ? "ran" : "FAILED",
         ran ? (took ? " (taken)" : " (refused)") : "", wo >= 0 ? names::ClassName(names::ReadPointer(pawn + wo)).c_str() : "?");
    return took;
}

}  // namespace

void Configure(bool grabPickup) { g_on = grabPickup; }

void OnDraw(shared::Header* hdr) {
    if (!hdr) return;
    static std::uint32_t seenReq = hdr->pickupReqSeq;
    const std::uint32_t mode = hdr->pickupMode;
    const bool on = mode == 2u || (mode == 0u && g_on);
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t ctrl = pawn ? Obj(pawn, "Controller") : 0;
    std::uint32_t inReach = 0;
    std::vector<Spot> spots;
    float hand[2][3] = {}, upm = 100.0f;
    int hands = 0;
    if (on && pawn && ctrl) {
        spots = Spots(ctrl);
        if (!spots.empty()) hands = Hands(hdr, hand, upm);
        for (int h = 0; h < 2; ++h)
            if ((hands & (1 << h)) && Nearest(spots, hand[h], upm) >= 0) inReach |= 1u << h;
    }
    static std::uint32_t seenInReach = 0;
    if (inReach != seenInReach) {
        static int logged = 0;
        if (logged < 60) {
            ++logged;
            for (int h = 0; h < 2; ++h) {
                if (((inReach ^ seenInReach) >> h & 1u) == 0) continue;
                const int i = (inReach >> h & 1u) ? Nearest(spots, hand[h], upm) : -1;
                MLOG("pickup: the %s hand %s", h ? "right" : "left",
                     i >= 0 ? ("within reach of " + What(spots[static_cast<size_t>(i)])).c_str() : "out of reach");
            }
        }
        seenInReach = inReach;
    }
    hdr->pickupNear = inReach;
    const std::uint32_t req = hdr->pickupReqSeq;
    if (req == seenReq) return;
    seenReq = req;
    const int h = static_cast<int>(hdr->pickupReqHand & 1u);
    if (!on || !pawn || !ctrl || !(hands & (1 << h))) {
        MLOG("pickup: a grab with the %s hand -- nothing to take (%s)", h ? "right" : "left", !on ? "off" : "no hand or pawn");
        return;
    }
    const int i = Nearest(spots, hand[h], upm);
    if (i < 0) {
        MLOG("pickup: a grab with the %s hand -- nothing within reach any more", h ? "right" : "left");
        return;
    }
    if (Take(spots[static_cast<size_t>(i)], pawn, ctrl, h ? "the right hand's grab" : "the left hand's grab"))
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->pickupDone));
}

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr pickup", 13) != 0) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const std::uintptr_t ctrl = pawn ? Obj(pawn, "Controller") : 0;
    if (!ctrl) {
        MLOG("pickup: test -- no pawn");
        return true;
    }
    const std::vector<Spot> spots = Spots(ctrl, std::wcscmp(line, L"mohavr pickup list") != 0);
    if (!std::wcscmp(line, L"mohavr pickup list")) {
        // Where each is against the head (right, up, forward in metres, the head's yaw): the test puts a hand there.
        float head[3], yaw = 0.0f, upm = 100.0f;
        const bool hk = view::HeadInWorld(head, yaw, upm);
        MLOG("pickup: test -- %zu pickup(s) at the player (the controller's csaList)", spots.size());
        // The level's dropped weapons (WorldInfo.WeaponDroppedPickups), with their CSAs: what the list could hold.
        {
            const std::uintptr_t wi = Obj(pawn, "WorldInfo");
            const int ao = wi ? names::PropertyOffset(wi, "WeaponDroppedPickups") : -1;
            const std::uintptr_t data = ao >= 0 ? names::ReadPointer(wi + ao) : 0;
            const int num = ao >= 0 ? static_cast<int>(names::ReadPointer(wi + ao + 4)) : 0;
            float from[3];
            names::ReadVector(pawn + addr::kActorLocation, from);
            for (int i = 0; data && i < num && i < 20; ++i) {
                const std::uintptr_t p = names::ReadPointer(data + 4u * static_cast<std::uintptr_t>(i));
                if (!p) continue;
                float at[3];
                names::ReadVector(p + addr::kActorLocation, at);
                const std::uintptr_t csa = Obj(p, "WeaponCSA");
                MLOG("pickup: test --   dropped %s (%s, state %s) %.1f m from the pawn; its CSA %s%s", names::Name(p).c_str(),
                     names::ClassName(Obj(p, "Inventory")).c_str(), names::StateName(p).c_str(), Dist(at, from) / 100.0f,
                     csa ? names::Name(csa).c_str() : "none", csa ? (Bit(csa, "bEnabled") ? " (enabled)" : " (disabled)") : "");
            }
        }
        for (size_t i = 0; i < spots.size(); ++i) {
            const Spot& s = spots[i];
            float r = 0, u = 0, f = 0;
            if (hk) {
                const float dx = s.loc[0] - head[0], dy = s.loc[1] - head[1], dz = s.loc[2] - head[2];
                f = (dx * std::cos(yaw) + dy * std::sin(yaw)) / upm;
                r = (-dx * std::sin(yaw) + dy * std::cos(yaw)) / upm;
                u = dz / upm;
            }
            MLOG("pickup: test --   #%zu %s (%s%s) at %.0f %.0f %.0f: from the head right %.2f up %.2f forward %.2f m", i,
                 What(s).c_str(), names::Name(s.csa).c_str(), Usable(s.csa, ctrl, true) ? "" : ", not usable now", s.loc[0], s.loc[1],
                 s.loc[2], r, u, f);
        }
        float hand[2][3] = {}, hupm = 100.0f;
        const int hands = Hands(bridge::SharedHeader(), hand, hupm);
        for (int h = 0; h < 2 && hk; ++h) {
            if (!(hands & (1 << h))) {
                MLOG("pickup: test --   the %s hand: not tracked", h ? "right" : "left");
                continue;
            }
            const float dx = hand[h][0] - head[0], dy = hand[h][1] - head[1], dz = hand[h][2] - head[2];
            MLOG("pickup: test --   the %s hand from the head right %.2f up %.2f forward %.2f m%s%s", h ? "right" : "left",
                 (-dx * std::sin(yaw) + dy * std::cos(yaw)) / hupm, dz / hupm, (dx * std::cos(yaw) + dy * std::sin(yaw)) / hupm,
                 spots.empty() ? "" : "; to #0 ", spots.empty() ? "" : (std::to_string(Dist(hand[h], spots[0].loc) / hupm) + " m").c_str());
        }
        return true;
    }
    if (!std::wcscmp(line, L"mohavr pickup drop")) {
        // The nearest soldier's gun dropped 1 m ahead of the player (EALAWeapon.DropFrom, as the death drop table does): a
        // weapon on the ground, every time.
        float from[3];
        names::ReadVector(pawn + addr::kActorLocation, from);
        std::uintptr_t best = 0;
        float bd = 1e9f;
        std::uintptr_t q = Obj(Obj(pawn, "WorldInfo"), "PawnList");
        for (int n = 0; q && n < 1024; q = Obj(q, "NextPawn"), ++n) {
            if (q == pawn || !names::IsA(q, "MOHAAIPawn") || Bit(q, "bDeleteMe") || !names::IsA(Obj(q, "Weapon"), "EALAWeapon")) continue;
            if (Carried(pawn, names::ClassName(Obj(q, "Weapon")))) continue;  // (the player's own kind: not takeable)
            float at[3];
            names::ReadVector(q + addr::kActorLocation, at);
            const float d = Dist(at, from);
            if (d < bd) {
                bd = d;
                best = q;
            }
        }
        if (!best) {
            MLOG("pickup: test -- no soldier with a gun");
            return true;
        }
        const std::uintptr_t weapon = Obj(best, "Weapon");
        float at[3], spot[3];
        names::ReadVector(best + addr::kActorLocation, at);
        // 1 m ahead of the player (his yaw), at his height: wherever the soldier is.
        const float pyaw = static_cast<float>(reinterpret_cast<const int*>(pawn + addr::kActorRotation)[1]) * (3.14159265f / 32768.0f);
        spot[0] = from[0] + 100.0f * std::cos(pyaw);
        spot[1] = from[1] + 100.0f * std::sin(pyaw);
        spot[2] = from[2];
        const float zero[3] = {0, 0, 0};
        const int rot[3] = {0, 0, 0};
        const std::uint32_t yes = 1;
        Call drop(weapon, "DropFrom");
        const bool ran = drop.ok && drop.Set("StartLocation", spot, sizeof(spot)) && drop.Set("StartVelocity", zero, sizeof(zero)) &&
                         drop.Set("StartRotation", rot, sizeof(rot)) && drop.Set("bNoCollisionFail", &yes, sizeof(yes)) && drop.Run();
        MLOG("pickup: test -- %s's %s dropped at %.0f %.0f %.0f (%.1f m from the pawn): DropFrom %s, the pickup %s", names::Name(best).c_str(),
             names::ClassName(weapon).c_str(), spot[0], spot[1], spot[2], Dist(spot, from) / 100.0f, ran ? "ran" : "FAILED",
             ran ? names::Name(drop.ReturnObject()).c_str() : "-");
        return true;
    }
    if (!std::wcscmp(line, L"mohavr pickup take")) {  // the nearest to the pawn, without a hand (the executor alone)
        if (spots.empty()) {
            MLOG("pickup: test -- nothing to take");
            return true;
        }
        Take(spots[0], pawn, ctrl, "test");
        return true;
    }
    MLOG("pickup: test -- mohavr pickup list | take");
    return true;
}

}  // namespace mohavr::pickup
