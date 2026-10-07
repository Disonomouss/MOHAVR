// Mounted guns (GOAL D1, MOUNTED-DESIGN.md): test commands to find, reach and man the level's MG42 nests in the simulator.
// A nest is a MOHAMountedGun -- a MOHAPawn, so it is in WorldInfo.PawnList -- and the player mans it through its
// MOHAMountedGunCSA's UsedBy event (MyCSA; the player standing, the nest not mounted, its weapon attached).
//   mohavr mg list        the nests, nearest first: where, how far, who mans it, its weapon
//   mohavr mg goto [n]    the player moved to the n-th nearest nest's use spot (its CSA), facing the way the gun does
//   mohavr mg use [n]     the n-th nearest nest manned (MyCSA.UsedBy(the player))
//   mohavr mg where       the player's controller state, the gun in hand, the pawn's rMGRot / aim blends
#include "mounted.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "addresses.hpp"
#include "aim.hpp"
#include "log.hpp"
#include "names.hpp"
#include "script_call.hpp"

namespace mohavr::mounted {
using namespace script;
namespace {

struct Nest { std::uintptr_t mg; float dist; };

std::vector<Nest> Nests(std::uintptr_t pawn) {
    std::vector<Nest> out;
    float from[3];
    names::ReadVector(pawn + addr::kActorLocation, from);
    std::uintptr_t q = Obj(Obj(pawn, "WorldInfo"), "PawnList");
    for (int n = 0; q && n < 1024; q = Obj(q, "NextPawn"), ++n) {
        if (!names::IsA(q, "MOHAMountedGun") || Bit(q, "bDeleteMe")) continue;
        float loc[3];
        names::ReadVector(q + addr::kActorLocation, loc);
        const float dx = loc[0] - from[0], dy = loc[1] - from[1], dz = loc[2] - from[2];
        out.push_back({q, std::sqrt(dx * dx + dy * dy + dz * dz)});
    }
    std::sort(out.begin(), out.end(), [](const Nest& a, const Nest& b) { return a.dist < b.dist; });
    return out;
}

bool g_mountedGame = true;
bool g_logged = false;

}  // namespace

void Configure(bool mountedGame) { g_mountedGame = mountedGame; }

bool GameHandles(std::uintptr_t pawn) {
    const bool m = g_mountedGame && pawn && names::IsA(Obj(pawn, "Weapon"), "MOHAMountedGunWeapon");
    if (m != g_logged) {
        g_logged = m;
        MLOG("mounted: %s", m ? "a mounted gun in hand -- the game draws it on its mount and aims it ([Weapon] MountedGame)"
                              : "off the mounted gun -- the gun in the hand again");
    }
    return m;
}

bool TestCommand(const wchar_t* line) {
    if (std::wcsncmp(line, L"mohavr mg", 9) != 0) return false;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn) {
        MLOG("mounted: test -- no pawn");
        return true;
    }
    const std::uintptr_t ctrl = Obj(pawn, "Controller");
    int idx = 0;
    if (!std::wcsncmp(line, L"mohavr mg where", 15)) {
        const int ro = names::PropertyOffset(pawn, "rMGRot");
        const int* r = ro >= 0 ? reinterpret_cast<const int*>(pawn + ro) : nullptr;
        MLOG("mounted: test -- the controller in state %s; the gun in hand %s (%s); rMGRot %d %d; aim blends right %.2f up %.2f; "
             "bMountedOnMG %d", names::StateName(ctrl).c_str(), names::Name(Obj(pawn, "Weapon")).c_str(),
             names::ClassName(Obj(pawn, "Weapon")).c_str(), r ? r[0] : 0, r ? r[1] : 0, Float(pawn, "fAimRightBlend", 0.0f),
             Float(pawn, "fAimUpBlend", 0.0f), Bit(pawn, "bMountedOnMG") ? 1 : 0);
        return true;
    }
    const std::vector<Nest> nests = Nests(pawn);
    if (!std::wcsncmp(line, L"mohavr mg list", 14)) {
        MLOG("mounted: test -- %zu nests in the pawn list", nests.size());
        for (size_t i = 0; i < nests.size() && i < 12; ++i) {
            const std::uintptr_t mg = nests[i].mg, csa = Obj(mg, "MyCSA");
            float loc[3], cl[3] = {0, 0, 0};
            names::ReadVector(mg + addr::kActorLocation, loc);
            if (csa) names::ReadVector(csa + addr::kActorLocation, cl);
            MLOG("mounted:   #%zu %s at %.0f %.0f %.0f (%.1f m), yaw %d; gunner %s; weapon %s; CSA %s at %.0f %.0f %.0f", i,
                 names::Name(mg).c_str(), loc[0], loc[1], loc[2], nests[i].dist / 100.0f,
                 reinterpret_cast<const int*>(mg + addr::kActorRotation)[1] & 0xFFFF, names::Name(Obj(mg, "Gunner")).c_str(),
                 names::Name(Obj(mg, "Weapon")).c_str(), names::Name(csa).c_str(), cl[0], cl[1], cl[2]);
        }
        return true;
    }
    if (swscanf_s(line, L"mohavr mg goto %d", &idx) == 1 || !std::wcscmp(line, L"mohavr mg goto")) {
        if (idx < 0 || idx >= static_cast<int>(nests.size())) {
            MLOG("mounted: test -- no nest #%d (%zu found)", idx, nests.size());
            return true;
        }
        const std::uintptr_t mg = nests[idx].mg, csa = Obj(mg, "MyCSA");
        float spot[3];
        names::ReadVector((csa ? csa : mg) + addr::kActorLocation, spot);
        spot[2] += 10.0f;
        const int yaw = reinterpret_cast<const int*>(mg + addr::kActorRotation)[1];
        const int rot[3] = {0, yaw, 0};
        Call mv(pawn, "ClientSetLocation");
        const bool ran = mv.ok && mv.Set("NewLocation", spot, sizeof(spot)) && mv.Set("NewRotation", rot, sizeof(rot)) && mv.Run();
        float to[3];
        names::ReadVector(pawn + addr::kActorLocation, to);
        MLOG("mounted: test -- moved to nest #%d %s's use spot %.0f %.0f %.0f (now at %.0f %.0f %.0f; ClientSetLocation %s)", idx,
             names::Name(mg).c_str(), spot[0], spot[1], spot[2], to[0], to[1], to[2], ran ? "ran" : "FAILED");
        return true;
    }
    if (swscanf_s(line, L"mohavr mg use %d", &idx) == 1 || !std::wcscmp(line, L"mohavr mg use")) {
        if (idx < 0 || idx >= static_cast<int>(nests.size())) {
            MLOG("mounted: test -- no nest #%d (%zu found)", idx, nests.size());
            return true;
        }
        const std::uintptr_t mg = nests[idx].mg, csa = Obj(mg, "MyCSA");
        Call use(csa, "UsedBy");
        const bool ran = use.ok && use.Set("User", &pawn, sizeof(pawn)) && use.Run();
        MLOG("mounted: test -- %s.UsedBy(the player) %s; the controller now in state %s", names::Name(csa).c_str(),
             ran ? "ran" : "FAILED", names::StateName(ctrl).c_str());
        return true;
    }
    MLOG("mounted: test -- mohavr mg list | goto [n] | use [n] | where");
    return true;
}

}  // namespace mohavr::mounted
