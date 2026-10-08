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
#include "vr_view.hpp"
#include "bridge.hpp"

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
bool g_mountedHands = false;
bool g_handsNow = false;  // D78: the hands aim the MG42 manned now (OnDraw)
bool g_manned = false;    // D78: a mounted gun manned now (OnDraw)
float g_lineYaw = 0.0f, g_linePitch = 0.0f;  // D78: the hand's line now (degrees, against the body; the test's "mg where")
bool g_logged = false;

}  // namespace

void Configure(bool mountedGame, bool mountedHands) {
    g_mountedGame = mountedGame;
    g_mountedHands = mountedHands;
}

bool HandsNow() { return g_handsNow; }
bool Manned() { return g_manned; }

void OnDraw(shared::Header* hdr) {
    if (!hdr) return;
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    const bool manned = pawn && GameHandles(pawn);
    g_manned = manned;
    const std::uint32_t mode = hdr->mgMode;
    const bool hands = manned && (mode == 2u || (mode == 0u && g_mountedHands));
    if (hands != g_handsNow) MLOG("mounted: %s", hands ? "the gun hand aims the MG42 ([Weapon] MountedHands)" : "the hands let the MG42 go");
    g_handsNow = hands;
    hdr->mgState = (manned ? 1u : 0u) | (hands ? 2u : 0u);
    if (!hands) return;
    // The aim line (the host's, as for a held gun: the gun hand, or the line to the foregrip hand) in the world.
    shared::Pose gun, ray;
    std::uint32_t flags = 0;
    float pos[3], dir[3], upm = 100.0f;
    if (!shared::ReadGun(hdr, gun, ray, flags) || !view::PoseToWorld(ray, pos, dir, upm)) return;
    const std::uintptr_t ctrl = Obj(pawn, "Controller"), weapon = Obj(pawn, "Weapon");
    if (!ctrl) return;
    const int ro = names::PropertyOffset(pawn, "rMGRot");
    if (ro < 0) return;
    int* r = reinterpret_cast<int*>(pawn + ro);
    // Against the body's heading (the controller's yaw: the eyes keep it, vr_view) and level (the controller's pitch is held
    // at 0 while the hands aim: the camera's pitch is the controller's plus rMGRot's).
    const int cyaw = reinterpret_cast<const int*>(ctrl + addr::kActorRotation)[1];
    constexpr float kU = 32768.0f / 3.14159265f;  // radians -> Unreal units
    const float yaw = std::atan2(dir[1], dir[0]) * kU - static_cast<float>(cyaw);
    const float pitch = std::atan2(dir[2], std::sqrt(dir[0] * dir[0] + dir[1] * dir[1])) * kU;
    float wy = std::fmod(yaw, 65536.0f);
    if (wy > 32768.0f) wy -= 65536.0f;
    if (wy < -32768.0f) wy += 65536.0f;
    const float maxYaw = Float(weapon, "fMaxYaw", 8192.0f), maxPitch = Float(weapon, "fMaxPitch", 5461.0f);
    const float y = std::fmax(-maxYaw, std::fmin(maxYaw, wy)), p = std::fmax(-maxPitch, std::fmin(maxPitch, pitch));
    g_lineYaw = wy / kU * 57.2958f;
    g_linePitch = pitch / kU * 57.2958f;
    r[0] = static_cast<int>(p);
    r[1] = static_cast<int>(y);
    r[2] = 0;
    const int rb = names::PropertyOffset(pawn, "fAimRightBlend"), ub = names::PropertyOffset(pawn, "fAimUpBlend");
    if (rb >= 0) *reinterpret_cast<float*>(pawn + rb) = maxYaw > 0.0f ? y / maxYaw : 0.0f;
    if (ub >= 0) *reinterpret_cast<float*>(pawn + ub) = maxPitch > 0.0f ? p / maxPitch : 0.0f;
    {
        // Research: on each change of the hand's line, the absolute yaws (deg): the hand's line, the drawn barrel (mesh +Z),
        // the controller, the game camera.
        static float lastYaw = 1e9f;
        static int traced = 0;
        const float hy = std::atan2(dir[1], dir[0]) * 57.2958f;
        if (traced < 12 && std::fabs(hy - lastYaw) > 2.0f) {
            ++traced;
            lastYaw = hy;
            const std::uintptr_t mesh = Obj(weapon, "Mesh");
            const int lo = mesh ? names::PropertyOffset(mesh, "LocalToWorld") : -1;
            const float* m = lo >= 0 ? reinterpret_cast<const float*>(mesh + lo) : nullptr;
            float cl[3], cp = 0.0f, cy = 0.0f;
            view::GameCamera(cl, cp, cy);
            MLOG("mounted: trace -- the hand's line yaw %.1f; the barrel %.1f; the controller %.1f; the game camera %.1f; rMGRot yaw %d "
                 "(the gun %s, flags %u, ray from %.0f %.0f %.0f)", hy, m ? std::atan2(m[9], m[8]) * 57.2958f : 0.0f, cyaw * 360.0f / 65536.0f,
                 cy * 57.2958f, r[1], names::Name(Obj(pawn, "Weapon")).c_str(), flags, pos[0], pos[1], pos[2]);
        }
    }
    static DWORD lastLog = 0;
    static int logged = 0;
    const DWORD now = GetTickCount();
    if (logged < 40 && now - lastLog > 1000) {
        lastLog = now;
        ++logged;
        // The drawn gun's barrel (its first-person mesh's axes) against the hand's line, for the check.
        std::string axes;
        const std::uintptr_t mesh = Obj(weapon, "Mesh");
        const int lo = mesh ? names::PropertyOffset(mesh, "LocalToWorld") : -1;
        if (lo >= 0) {
            const float* m = reinterpret_cast<const float*>(mesh + lo);
            char b[200];
            for (int a = 0; a < 3; ++a) {
                const float* ax = m + 4 * a;
                const float n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                const float c = n > 1e-6f ? (ax[0] * dir[0] + ax[1] * dir[1] + ax[2] * dir[2]) / n : 0.0f;
                sprintf_s(b, " %c %.1f", "XYZ"[a], std::acos(std::fmax(-1.0f, std::fmin(1.0f, c))) * 57.2958f);
                axes += b;
            }
        }
        MLOG("mounted: the hands aim -- the hand's line yaw %+.1f pitch %+.1f deg (against the body) -> rMGRot %d %d%s; the mesh's "
             "axes off the hand's line (deg):%s", wy / kU * 57.2958f, pitch / kU * 57.2958f, r[0], r[1],
             (y != wy || p != pitch) ? " (at the mount's limit)" : "", axes.c_str());
    }
}

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
        // D78 research: the game camera (before the head), the controller's rotation and the nest's.
        float cl[3] = {0, 0, 0}, cp = 0.0f, cy = 0.0f;
        const bool cam = view::GameCamera(cl, cp, cy);
        const int* cr = reinterpret_cast<const int*>(ctrl + addr::kActorRotation);
        const std::uintptr_t mg = Obj(ctrl, "MountedGun") ? Obj(Obj(ctrl, "MountedGun"), "Owner") : 0;
        const std::uintptr_t nest = Obj(pawn, "Base");
        MLOG("mounted: test -- the hand's line now yaw %+.2f pitch %+.2f deg (the hands aim: %d)", g_lineYaw, g_linePitch, g_handsNow ? 1 : 0);
        MLOG("mounted: test -- the game camera %s at %.1f %.1f %.1f pitch %.2f yaw %.2f deg; the controller's rotation %d %d; "
             "the nest %s yaw %d (its CSA's owner %s)", cam ? "" : "(none)", cl[0], cl[1], cl[2], cp * 57.2958f, cy * 57.2958f,
             cr[0], cr[1], names::Name(nest).c_str(), nest ? reinterpret_cast<const int*>(nest + addr::kActorRotation)[1] : 0,
             names::Name(mg).c_str());
        return true;
    }
    if (!std::wcsncmp(line, L"mohavr mg aim", 13)) {  // D78 research: rMGRot (yaw, pitch) set as the stick would
        int yaw = 0, pitch = 0;
        swscanf_s(line, L"mohavr mg aim %d %d", &yaw, &pitch);
        const int ro = names::PropertyOffset(pawn, "rMGRot");
        if (ro >= 0) {
            int* r = reinterpret_cast<int*>(pawn + ro);
            r[0] = pitch;
            r[1] = yaw;
        }
        MLOG("mounted: test -- rMGRot set to pitch %d yaw %d", pitch, yaw);
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
