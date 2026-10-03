#include "vr_view.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "bridge.hpp"
#include "throwing.hpp"
#include "muzzle.hpp"
#include "offhand.hpp"
#include "melee.hpp"
#include "offpistol.hpp"
#include "scope.hpp"
#include "knife.hpp"
#include "viewmodel.hpp"
#include "game_exec.hpp"
#include "config.hpp"
#include "crash_dump.hpp"
#include "reload.hpp"
#include "log.hpp"
#include "names.hpp"
#include "patch.hpp"

namespace mohavr::view {
namespace {

constexpr float kPi = 3.14159265358979f;

struct Vec3 { float x, y, z; };

// Unreal rotation units: 65536 = 360 degrees.
inline float UnrToRad(int u) { return static_cast<float>(u) * (kPi / 32768.0f); }
inline int RadToUnr(float r) { return static_cast<int>(std::lround(r * (32768.0f / kPi))); }

// OpenXR (right-handed, +X right, +Y up, -Z forward) -> Unreal (left-handed, +X forward,
// +Y right, +Z up):  ue = ( -xr.z, xr.x, xr.y ).
inline Vec3 XrToUe(float x, float y, float z) { return {-z, x, y}; }

// Rotate an OpenXR vector by quaternion q (x,y,z,w).
Vec3 QuatRotate(const shared::Pose& q, float vx, float vy, float vz) {
    // v' = v + 2*w*(u x v) + 2*(u x (u x v)), u = (qx,qy,qz)
    const float ux = q.qx, uy = q.qy, uz = q.qz, w = q.qw;
    const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    const float ccx = uy * cz - uz * cy, ccy = uz * cx - ux * cz, ccz = ux * cy - uy * cx;
    return {vx + 2.0f * (w * cx + ccx), vy + 2.0f * (w * cy + ccy), vz + 2.0f * (w * cz + ccz)};
}

inline Vec3 YawRotate(const Vec3& v, float yaw) {  // about Unreal +Z: X toward Y
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// --- state --------------------------------------------------------------------------------------
Config g_cfg;
SafetyHookMid    g_viewHook;
SafetyHookMid    g_projHookNormal;
SafetyHookMid    g_projHookConstrained;
SafetyHookMid    g_calcEntryHook;   // M4: CalcSceneView entry -> which eye, split-screen rect
SafetyHookInline g_drawHook;        // M4: UGameViewportClient::Draw -> two players during Draw

// The views the game thread applied this frame and last frame (render-thread lag, ENGINE-NOTES
// 5e). Written on the game thread, read on the render thread.
struct AppliedFrame {
    shared::Pose pose[2];
    shared::Fov  fov[2];
    bool         stereo;
    // Scopes (SCOPE-DESIGN): the eyes' width and the scope view this frame (scopeRect[2] 0: none).
    std::uint32_t eyeWidth;
    std::uint32_t scopeRect[4];
    shared::Pose scopeCam;
    float        scopeTan;
    shared::Pose gunPose;        // the host's gun pose this frame was drawn with (the lens stays on the drawn scope)
    bool         gunValid;
    DWORD        thread;
    bool         valid;
    bool         cinema;   // rendered as a flat full-screen image (menu/cutscene): no view, show on the quad
    LONGLONG     qpc;      // when its view was computed (diagnostics: the world's step per shown frame)
    std::uint32_t serial;  // one per committed frame
    bool         paced;    // its Draw waited for the previous Present (frame pacing): it is the one presented next
};
CRITICAL_SECTION g_lock;
AppliedFrame g_building{}, g_current{}, g_previous{};
std::uint32_t g_commitSerial = 0;

// Frame pacing (round 25, "slight jitter in general movement ... a slight rubber banding feeling"; hdr->pace, the host
// menu's Frame pacing, default the shipped [Bridge] Pace): uncapped, the game ran at 120-330 fps and the host took
// whichever frame was presented first after each headset frame, so the world time between two shown frames varied by up
// to a game frame (a few cm at a run) and 40-70% of the frames were rendered for nothing. Paced, each Draw first waits for the previous Draw's Present (so the frame the render thread presents is
// always the one committed last: MetaForPresentedFrame), then for the host's frame event (set once per headset frame,
// just after it wrote that frame's poses): one game frame per headset frame, the world stepping one frame at a time.
struct Pacing {
    std::uint32_t presentTarget = 0;  // PresentsSeen() once the last Draw's frame is presented
    bool          expectPresent = false;
    bool          paced = false;      // this Draw's waits both came in time
    unsigned      presentMisses = 0;  // Present waits timed out in a row
    DWORD         presentBackoff = 0; // no Present waits until then (Draws that don't present: a minimised window?)
    // 10 s statistics
    double        waitMs = 0.0, worstWaitMs = 0.0, since = 0.0;
    long          draws = 0, frameTimeouts = 0, presentTimeouts = 0;
    LONGLONG      last = 0;
} g_pace;

double QpcMs(LONGLONG q) {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(q) / static_cast<double>(f.QuadPart);
}

// Per CalcSceneView call (game thread only).
bool         g_thisViewActive = false;
int          g_thisEye = 0;          // 0 = left, 1 = right (mono: 0)
bool         g_thisStereo = false;
shared::Fov  g_thisFov{};            // before widening
float        g_thisAspect = 1.0f;    // viewport aspect of THIS view (half width in stereo)

// Stereo draw state (game thread only).
bool         g_inStereoDraw = false;
int          g_eyeCounter = 0;
void*        g_stereoPlayers[3] = {};
void*        g_leftViewState = nullptr;   // the player's own FSceneViewState
void*        g_rightViewState = nullptr;  // ours, for eye 1 (allocated once, lives for the process)

// Scopes (SCOPE-DESIGN): a third player in the stereo Draw renders the scope view into a column at the backbuffer's
// right ([Scope] Column px, square, taken from the eyes' width). Game thread, per Draw.
int          g_thisIndex = 0;             // this CalcSceneView's player in the stereo Draw (0, 1 the eyes; 2 the scope)
bool         g_thisScope = false;
int          g_viewCount = 2;             // players in this Draw
bool         g_committed = false;         // this Draw's frame record committed
void*        g_scopeViewState = nullptr;  // the scope view's own FSceneViewState (allocated once)
int          g_eyeW = 0;                  // px of each eye (0 before the first Draw)
int          g_colX = 0, g_colS = 0;      // the scope column: its x and side (px; 0 none)
float        g_scopeTanNow = 0.0f;        // this Draw's scope half-FOV tangent
shared::Pose g_scopeCam{};                // this Draw's scope camera from the host (LOCAL)
bool         g_scopeCamOk = false;        // the host wants the view (else Debug.ScopeView: the right eye's)
std::atomic<int> g_scopeViewX{-1};        // the scope view's x (px) while one renders, for the render thread's hooks
SafetyHookMid g_hudLoopHook;
void CommitBuilding();

// M5 cinema mode (Camera.CinemaScreen): UI menus and cinematic cameras are rendered flat and
// full-screen, without head tracking, and the host shows them on a world-locked screen.
bool  g_cinema = false;           // decided at the start of each Draw, holds until the next
bool  g_viewIsPlayers = true;     // last view: the controller's own yaw (not a matinee/menu camera)
DWORD g_cinemaFlipSince = 0;      // when the wanted mode first differed from g_cinema (debounce)

float g_ipd = 0.064f;             // metres, from the eye poses (HUD placement)
SafetyHookMid g_hudHook, g_hudMatrixHook;
float g_hudScalePending = 0.0f;   // set per eye by OnHudView, applied to that eye's canvas matrix

long g_views = 0;
bool g_loggedProj[2] = {false, false};
bool g_loggedStereo = false;

// Motion blur / depth of field off while head tracking (FSystemSettings ints, ENGINE-NOTES 5i).
// Re-asserted every view because the game re-applies its scalability options. Only ever writes
// 0 over a value that is 0 or 1 -- anything else means the address is wrong: stop touching it.
void ForceVrSettings() {
    static bool disabled = false;
    if (disabled) return;
    auto force = [](std::uintptr_t va, bool want, const char* what) {
        if (!want) return true;
        auto* p = reinterpret_cast<volatile int*>(va);
        const int v = *p;
        if (v != 0 && v != 1) {
            MLOG("view: %s at 0x%08X holds %d (expected 0/1) -- not touching FSystemSettings", what, static_cast<unsigned>(va), v);
            return false;
        }
        if (v == 1) {
            *p = 0;
            static int logged = 0;
            if (logged++ < 4) MLOG("view: %s forced off (FSystemSettings 0x%08X)", what, static_cast<unsigned>(va));
        }
        return true;
    };
    if (!force(addr::kSysAllowMotionBlur, g_cfg.noMotionBlur, "motion blur") ||
        !force(addr::kSysAllowDepthOfField, g_cfg.noDepthOfField, "depth of field"))
        disabled = true;
}

// Translation origin (OpenXR LOCAL), taken from the first TRACKED head pose (see OnViewPoint).
bool  g_haveOrigin = false;
float g_ox = 0, g_oy = 0, g_oz = 0;

// M7: how the player's last head-tracked view mapped LOCAL into the world (game thread; PoseToWorld).
struct WorldMap {
    bool         valid;
    float        base[3];  // the game's own (untracked) view location -- where its shots start
    float        yaw;      // the game's yaw (radians)
    shared::Pose head;     // that view's head pose (the translation reference without an origin)
    float        pitch;    // the game's own view pitch (radians) -- the first-person gun is placed with it
};
WorldMap g_world{};
// Debug.MuzzleFreeze: the left eye's final view location and rotation, last stereo frame.
float g_eye0Loc[3] = {};
int   g_eye0Rot[3] = {};
float g_gameCam[3] = {};  // the game's own camera at the player's last view (its shots start there)

float UnitsPerMeter(const shared::Header* hdr) {
    const float live = hdr ? hdr->unitsPerMeter : 0.0f;
    return (live > 1.0f && live < 1000.0f) ? live : g_cfg.unitsPerMeter;
}

// `recenterSeq` must be read BEFORE the views: the host publishes views in its new LOCAL space
// first and bumps the sequence after, so a new sequence always comes with new-space views.
void UpdateOrigin(const shared::Header* hdr, std::uint32_t recenterSeq, const shared::Pose& head) {
    // The host's Recentre (menu) re-creates its LOCAL space; poses jump, so take a fresh origin.
    static std::uint32_t seenRecenter = 0;
    if (recenterSeq != seenRecenter) {
        seenRecenter = recenterSeq;
        if (g_haveOrigin) MLOG("view: recentre #%u from the host -- taking a new position origin", seenRecenter);
        g_haveOrigin = false;
    }
    if (!(hdr->viewValid & 2u)) return;  // placeholder pose, not tracked yet
    // Virtual Desktop flags its pre-tracking placeholder as tracked (round 3: identity orientation
    // at y = -1.21, for ~20 ms). A real head is never exactly identity -- but the simulator's default
    // pose is, and stays so. So an identity pose only counts once it has persisted for 1 s.
    static DWORD identitySince = 0;
    if (head.qx == 0.0f && head.qy == 0.0f && head.qz == 0.0f && head.qw == 1.0f) {
        const DWORD now = GetTickCount();
        if (!identitySince) identitySince = now ? now : 1;
        if (now - identitySince < 1000) return;
    } else {
        identitySince = 0;
    }
    const float dx = head.px - g_ox, dy = head.py - g_oy, dz = head.pz - g_oz;
    // More than 1 m from the origin isn't plausible for a seated/standing player: the origin was
    // taken before the headset was on (or the play space moved) -> recentre.
    const bool implausible = g_haveOrigin && (dx * dx + dy * dy + dz * dz) > 1.0f;
    if (!g_haveOrigin || implausible) {
        MLOG("view: head position origin %s at (%.3f %.3f %.3f) m", g_haveOrigin ? "RECENTRED (head >1 m from origin)" : "set",
             head.px, head.py, head.pz);
        g_haveOrigin = true;
        g_ox = head.px; g_oy = head.py; g_oz = head.pz;
    }
}

// --- M4: UGameViewportClient::Draw -- make the engine draw two players (one per eye) ----------
// Only for the duration of Draw, GEngine->GamePlayers points at our 2-entry array holding the
// same ULocalPlayer twice; the real array is untouched and restored on return.
// A UI menu shows the Windows cursor: UE3 raises this thread's ShowCursor count to >= 0 for menus and
// drops it below 0 in play (measured: main menu 0, pause menu 0, gameplay -1). Read it without ever
// showing the cursor: decrement, then restore. Game thread (the one that owns the window).
bool UiMenuOpen() {
    const int cursor = ShowCursor(FALSE) + 1;
    ShowCursor(TRUE);
    return cursor >= 0;
}

void UpdateCinemaMode(bool menu) {
    if (!g_cfg.cinemaScreen) return;
    const bool camera = g_cfg.cinemaScreen >= 2 && !g_viewIsPlayers;
    const bool want = menu || camera;
    const DWORD now = GetTickCount();
    if (want == g_cinema) {
        g_cinemaFlipSince = 0;
        return;
    }
    // Debounce: the new mode must hold for 150 ms (a one-frame camera glitch mustn't flip the view).
    if (!g_cinemaFlipSince) g_cinemaFlipSince = now ? now : 1;
    if (now - g_cinemaFlipSince < 150) return;
    g_cinemaFlipSince = 0;
    g_cinema = want;
    static int logged = 0;
    if (logged++ < 40)
        MLOG("cinema: %s (%s)", want ? "ON -- flat full-screen image on the host's screen" : "off -- stereo, head-tracked",
             want ? (menu ? "UI menu: cursor shown" : "the view is not the player's (cinematic camera)") : "player view, no menu");
}

void CommitCinemaFrame() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    EnterCriticalSection(&g_lock);
    g_previous = g_current;
    g_current = AppliedFrame{};
    g_current.thread = GetCurrentThreadId();
    g_current.valid = true;
    g_current.cinema = true;
    g_current.qpc = q.QuadPart;
    g_current.serial = ++g_commitSerial;
    g_current.paced = g_pace.paced;
    LeaveCriticalSection(&g_lock);
}

// Frame pacing, at the start of each Draw (game thread): see Pacing.
void PaceBeforeDraw() {
    g_pace.paced = false;
    const shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !hdr->pace || !bridge::HostRunning()) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    bool presented = false;
    const DWORD now = GetTickCount();
    const bool backoff = g_pace.presentBackoff && static_cast<LONG>(now - g_pace.presentBackoff) < 0;
    if (g_pace.expectPresent && !backoff) {
        presented = bridge::WaitPresents(g_pace.presentTarget, 25);
        if (presented) {
            g_pace.presentMisses = 0;
        } else {
            ++g_pace.presentTimeouts;
            if (++g_pace.presentMisses >= 3) {
                // Draws without a Present (a minimised window?): stop waiting for them for a while.
                g_pace.presentMisses = 0;
                g_pace.presentBackoff = (now + 2000) | 1;
                static int logged = 0;
                if (logged++ < 10) MLOG("pace: three Draws in a row were not presented -- not waiting for Presents for 2 s");
            }
        }
    }
    const int frame = bridge::WaitHostFrame(25);
    if (frame < 0) return;  // no host running: nothing to pace to
    if (frame == 0) ++g_pace.frameTimeouts;
    g_pace.paced = presented && frame == 1;
    QueryPerformanceCounter(&t1);
    const double waited = QpcMs(t1.QuadPart - t0.QuadPart);
    g_pace.waitMs += waited;
    g_pace.worstWaitMs = waited > g_pace.worstWaitMs ? waited : g_pace.worstWaitMs;
    ++g_pace.draws;
    if (g_pace.last) g_pace.since += QpcMs(t1.QuadPart - g_pace.last);
    g_pace.last = t1.QuadPart;
    if (g_pace.since >= 10000.0) {
        MLOG("pace: %ld Draws in %.1f s (%.1f a second) -- waited %.2f ms a Draw (worst %.1f), %ld host frames and %ld Presents "
             "timed out", g_pace.draws, g_pace.since / 1000.0, 1000.0 * g_pace.draws / g_pace.since, g_pace.waitMs / g_pace.draws,
             g_pace.worstWaitMs, g_pace.frameTimeouts, g_pace.presentTimeouts);
        g_pace.waitMs = g_pace.worstWaitMs = g_pace.since = 0.0;
        g_pace.draws = g_pace.frameTimeouts = g_pace.presentTimeouts = 0;
    }
}

// At the end of each Draw: the Present the next Draw waits for. One Present per Draw; a Present from elsewhere (it
// runs ahead) is absorbed by the max, a missing one resyncs after its wait timed out.
void PaceAfterDraw() {
    const std::uint32_t seen = bridge::PresentsSeen();
    const std::uint32_t next = g_pace.expectPresent && g_pace.paced ? g_pace.presentTarget + 1 : seen + 1;
    g_pace.presentTarget = static_cast<std::int32_t>(next - (seen + 1)) > 0 ? next : seen + 1;
    g_pace.expectPresent = true;
}

// Weapon.HideViewModel / HideBody: the pawn's own exec functions, re-issued every 3 s (idempotent) so a
// new pawn (death, level load) gets them too. HideWeapon's flag survives weapon switches.
void ApplyWeaponCommands(const std::uintptr_t* players) {
    if (!(g_cfg.hideViewModel || g_cfg.hideBody) || !players || players[1] != 1 || !players[0]) return;
    static DWORD next = GetTickCount();
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 3000;
    const auto player = *reinterpret_cast<const std::uintptr_t*>(players[0]);
    static int logged = 0;
    if (g_cfg.hideViewModel) {
        const bool ok = gexec::Run(player, L"HideWeapon 0");
        if (logged < 6) { ++logged; MLOG("weapon: 'HideWeapon 0' -> %s", ok ? "handled" : "not handled (no pawn yet?)"); }
    }
    if (g_cfg.hideBody) {
        const bool ok = gexec::Run(player, L"RenderBody 0");
        if (logged < 6) { ++logged; MLOG("weapon: 'RenderBody 0' -> %s", ok ? "handled" : "not handled (no pawn yet?)"); }
    }
}

bool g_landingHeld = false;  // eye 0: the landing's view is held this frame (the hands' per-view work waits)

// [Weapon] LandingBody=0 (the player, round 41: "once landed, could the player body appear instead of the parachuting
// body -- it moves around and looks strange"): MOHAPlayerController's AirDropLanding and AirDropLanded states call
// RenderBody(true), showing the first-person body (legs, torso, gear: FPArms material 1) that play keeps hidden. While
// the landing's view is held (Camera.SteadyLanding) it is hidden again: RenderBody 0, re-issued every 0.5 s. RenderBody(
// false) keeps the material it replaces in BodyMatInst, so a second call would keep the hidden one and lose the body for
// good (the next airdrop, a briefing): the body's own, read after the first call, is put back when the landing ends --
// and for 10 s after it, in case the game's own EndState RenderBody(false) comes after ours.
void ApplyLandingBody(const std::uintptr_t* players) {
    static bool active = false;
    static std::uintptr_t pawn = 0, bodyMat = 0;
    static DWORD next = 0, endedAt = 0;
    if (!players || players[1] < 1 || !players[0]) return;
    const auto player = *reinterpret_cast<const std::uintptr_t*>(players[0]);
    const std::uintptr_t p = aim::LocalPlayerPawn();
    const int bo = p ? names::PropertyOffset(p, "BodyMatInst") : -1;
    const DWORD now = GetTickCount();
    if (g_cfg.landingBody == 0 && g_cfg.steadyLanding && g_landingHeld && p && bo >= 0) {
        if (!active || p != pawn) {
            active = true;
            pawn = p;
            bodyMat = 0;
            next = now;
            MLOG("weapon: landed -- the parachuting body hidden (Weapon.LandingBody=0)");
        }
        if (static_cast<LONG>(now - next) >= 0) {
            next = now + 500;
            gexec::Run(player, L"RenderBody 0");
            if (!bodyMat) bodyMat = names::ReadPointer(p + bo);
        }
        return;
    }
    if (active) {
        active = false;
        endedAt = now;
    }
    if (endedAt && now - endedAt < 10000 && p == pawn && bodyMat && bo >= 0 && names::ReadPointer(p + bo) != bodyMat) {
        *reinterpret_cast<std::uintptr_t*>(p + bo) = bodyMat;
        MLOG("weapon: the body's material kept for the game (BodyMatInst put back to %s)", names::Name(bodyMat).c_str());
    }
}

// Debug.GameCommands: console commands for scripted tests (e.g. "Suicide" for the death/reload test),
// one per line in %TEMP%\MOHAVR\game_cmd.txt, read and deleted twice a second on the game thread.
void RunTestCommands(const std::uintptr_t* players) {
    if (!g_cfg.debugGameCommands || !players || players[1] < 1 || !players[0]) return;
    static DWORD next = GetTickCount();
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 500;
    static std::wstring path;
    if (path.empty()) {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        path = std::wstring(tmp, n) + L"MOHAVR\\game_cmd.txt";
    }
    // Taken by a rename first, then read and deleted: a command written meanwhile is never deleted unread (GOAL A2).
    const std::wstring taken = path + L".taken";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
        !MoveFileExW(path.c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING))
        return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, taken.c_str(), L"r, ccs=UTF-8") != 0 || !f) {
        DeleteFileW(taken.c_str());
        return;
    }
    wchar_t line[256];
    const auto player = *reinterpret_cast<const std::uintptr_t*>(players[0]);
    while (fgetws(line, 256, f)) {
        line[wcscspn(line, L"\r\n")] = 0;
        if (!line[0]) continue;
        // GOAL A2/A4 (tests only): "mohavr upgradelevel <type> <level>" -- the pawn's WeaponUpgradeManager level for a
        // weapon type, which a gun given afterwards (not held yet) takes (EALAWeapon.AttachWeaponTo): the save has every
        // upgrade, so the lower levels (single rounds, the C96's stripper clip) need it. Nothing is saved: the harness
        // restores Saved\ after the run.
        int wtype = 0, level = 0;
        if (swscanf_s(line, L"mohavr upgradelevel %d %d", &wtype, &level) == 2) {
            const std::uintptr_t pawn = aim::LocalPlayerPawn();
            const int mo = pawn ? names::PropertyOffset(pawn, "WeaponUpgradeManager") : -1;
            const std::uintptr_t mgr = mo >= 0 ? names::ReadPointer(pawn + mo) : 0;
            const int lo = mgr ? names::PropertyOffset(mgr, "iUpgradeLevel") : -1;
            const std::uintptr_t data = lo >= 0 ? names::ReadPointer(mgr + lo) : 0;
            const int count = lo >= 0 ? *reinterpret_cast<const int*>(mgr + lo + 4) : 0;
            if (data && wtype >= 0 && wtype < count) {
                int* slot = reinterpret_cast<int*>(data + 4 * wtype);
                MLOG("test: weapon type %d's upgrade level %d -> %d", wtype, *slot, level);
                *slot = level;
            } else {
                MLOG("test: no upgrade level for weapon type %d (manager %p, %d entries)", wtype, reinterpret_cast<void*>(mgr), count);
            }
            continue;
        }
        if (offhand::TestCommand(line)) continue;  // "mohavr nade ..." (the off-hand grenade's spike)
        if (scope::TestCommand(line)) continue;      // "mohavr scope ..." (scopes)
        if (knife::TestCommand(line)) continue;      // "mohavr knife ..." (the off-hand knife)
        if (offpistol::TestCommand(line)) continue;  // "mohavr pistol ..." (the off-hand pistol)
        if (melee::TestCommand(line)) continue;      // "mohavr melee ..." (physical melee)
        const bool ok = gexec::Run(player, line);
        MLOG("test: game command '%ls' -> %s", line, ok ? "handled" : "not handled");
    }
    fclose(f);
    DeleteFileW(taken.c_str());
}

// M8: a console command from the host (holsters, the reload gesture), once per cmdSeq, on the game thread.
void RunHostCommand(const std::uintptr_t* players, const shared::Header* hdr) {
    static std::uint32_t seen = 0;
    if (!hdr || !players || players[1] < 1 || !players[0]) return;
    const std::uint32_t seq = hdr->cmdSeq;
    if (seq == seen) return;
    seen = seq;
    wchar_t cmd[64];
    int n = 0;
    for (; n < 63 && hdr->cmd[n]; ++n) cmd[n] = static_cast<wchar_t>(static_cast<unsigned char>(hdr->cmd[n]));
    cmd[n] = 0;
    if (!n) return;
    const auto player = *reinterpret_cast<const std::uintptr_t*>(players[0]);
    // The menu's "Give all weapons" (the player's request, 2026-10-01): the game's own cheats -- EnableCheats, a
    // GiveWeapon per [Weapon] GiveAllList class (comma-separated, MOHAGameNonNative), GiveAmmo. A class the level
    // doesn't hold gives nothing.
    if (!wcscmp(cmd, L"mohavr giveall")) {
        int given = 0, listed = 0;
        auto run = [&](const std::wstring& c) {
            const bool ok = gexec::Run(player, c.c_str());
            MLOG("hands: give all -- '%ls' -> %s", c.c_str(), ok ? "handled" : "not handled");
            return ok;
        };
        run(L"EnableCheats");
        std::string list = g_cfg.giveAllList;
        size_t p = 0;
        while (p < list.size()) {
            size_t e = list.find(',', p);
            if (e == std::string::npos) e = list.size();
            std::string cls = list.substr(p, e - p);
            p = e + 1;
            while (!cls.empty() && cls.front() == ' ') cls.erase(cls.begin());
            while (!cls.empty() && cls.back() == ' ') cls.pop_back();
            if (cls.empty()) continue;
            ++listed;
            if (run(L"GiveWeapon MOHAGameNonNative." + std::wstring(cls.begin(), cls.end()))) ++given;
        }
        run(L"GiveAmmo");
        // What the pawn now carries (its InventoryManager's chain): a class the level can't load gives nothing.
        std::string carried;
        int carriedN = 0;
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const int io = pawn ? names::PropertyOffset(pawn, "InvManager") : -1;
        const std::uintptr_t inv = io >= 0 ? names::ReadPointer(pawn + io) : 0;
        const int co = inv ? names::PropertyOffset(inv, "InventoryChain") : -1;
        std::uintptr_t item = co >= 0 ? names::ReadPointer(inv + co) : 0;
        while (item && carriedN < 64) {
            carried += (carriedN ? ", " : "") + names::ClassName(item);
            ++carriedN;
            const int no = names::PropertyOffset(item, "Inventory");
            item = no >= 0 ? names::ReadPointer(item + no) : 0;
        }
        MLOG("hands: give all weapons -- %d of %d classes handled; the pawn carries %d: %s", given, listed, carriedN, carried.c_str());
        return;
    }
    // The pouch reload (Hands.PouchReload): the gun hand's gun, or the pistol the off hand holds, reloaded at once.
    if (!wcscmp(cmd, L"mohavr pouchreload gun")) {
        reload::InstantReload(aim::LocalPlayerPawn());
        return;
    }
    if (!wcscmp(cmd, L"mohavr pouchreload off")) {
        offpistol::PouchRefill();
        return;
    }
    const bool ok = gexec::Run(player, cmd);
    MLOG("hands: game command '%ls' -> %s", cmd, ok ? "handled" : "not handled");
    // After "Give all weapons" switch weapon is the engine's NextWeapon, which walks the whole inventory: it never takes the
    // pistol the off hand holds ([OffHand] PistolKeep) -- one more step past it.
    if (ok && !wcscmp(cmd, L"NextWeapon") && offpistol::SkipHeldPistol()) {
        const bool again = gexec::Run(player, cmd);
        MLOG("hands: NextWeapon landed on the pistol in the off hand -- once more (%s)", again ? "handled" : "not handled");
    }
}

void __fastcall Hook_Draw(void* self, void* /*edx*/, void* viewport, void* canvas) {
    PaceBeforeDraw();
    const auto engine = *reinterpret_cast<std::uintptr_t*>(addr::kGEngine);
    auto* arr = engine ? reinterpret_cast<std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;  // Data, Num, Max
    shared::Header* hdr = bridge::SharedHeader();
    ApplyWeaponCommands(arr);
    ApplyLandingBody(arr);
    RunTestCommands(arr);
    crashdump::OnDraw();
    if (arr && arr[1] >= 1 && arr[0]) muzzle::OnDraw(*reinterpret_cast<const std::uintptr_t*>(arr[0]));
    RunHostCommand(arr, hdr);
    reload::OnDraw(hdr);
    offhand::OnDraw(hdr);
    offpistol::OnDraw(hdr);
    melee::OnDraw(hdr);
    scope::OnDraw(hdr);
    knife::OnDraw(hdr);
    const bool uiMenu = UiMenuOpen();
    if (hdr && hdr->gameUiMenu != (uiMenu ? 1u : 0u)) hdr->gameUiMenu = uiMenu ? 1u : 0u;  // the pad's menu layout
    UpdateCinemaMode(uiMenu);
    if (g_cinema) {
        g_drawHook.thiscall<void>(self, viewport, canvas);  // one full-screen view, the game's own camera
        CommitCinemaFrame();
        PaceAfterDraw();
        return;
    }
    const bool want = g_cfg.headTracking && g_cfg.stereo && hdr && (hdr->viewValid & 1u) && arr && arr[1] == 1 && arr[0];
    if (!want) {
        g_drawHook.thiscall<void>(self, viewport, canvas);
        PaceAfterDraw();
        return;
    }
    void* player = *reinterpret_cast<void**>(arr[0]);
    g_stereoPlayers[0] = g_stereoPlayers[1] = g_stereoPlayers[2] = player;
    // The host's scope request (SCOPE-DESIGN): the scope view renders as the third player, into a square column at the
    // backbuffer's right taken from the eyes' width -- only while a scope is at an eye (the eyes are full width otherwise:
    // no cost, the backbuffer and the menus' shape unchanged).
    std::uint32_t scopeWant = 0;
    float wantTan = 0.0f;
    if (!shared::ReadScopeWant(hdr, scopeWant, wantTan, g_scopeCam)) scopeWant = 0;
    g_scopeCamOk = (scopeWant & 1u) != 0;
    const bool scopeView = g_cfg.scopeColumn > 0 && (g_cfg.debugScopeView || g_scopeCamOk);
    {
        const int W = static_cast<int>(hdr->width), H = static_cast<int>(hdr->height);
        int col = scopeView ? g_cfg.scopeColumn : 0;
        if (col > W / 3) col = W / 3;
        g_eyeW = (W - col) / 2;
        g_colX = 2 * g_eyeW;
        g_colS = col > 0 ? (W - g_colX < H ? W - g_colX : H) : 0;
    }
    g_scopeTanNow = g_scopeCamOk ? wantTan : std::tan(0.5f * g_cfg.debugScopeFov * kPi / 180.0f);
    if (!(g_scopeTanNow > 0.001f && g_scopeTanNow < 2.0f)) g_scopeTanNow = std::tan(0.5f * 10.0f * kPi / 180.0f);
    g_viewCount = g_colS > 0 ? 3 : 2;
    if (g_cfg.scopeColumn > 0) {
        // The column's x at this width, whether or not it renders this Draw (the render thread may still be drawing the last
        // scope view): no eye view starts there (eye 1 at the half, or at the narrowed width).
        const int W = static_cast<int>(hdr->width);
        const int col = g_cfg.scopeColumn < W / 3 ? g_cfg.scopeColumn : W / 3;
        g_scopeViewX.store(2 * ((W - col) / 2));
    }
    g_committed = false;
    const std::uintptr_t savedData = arr[0];
    arr[0] = reinterpret_cast<std::uintptr_t>(g_stereoPlayers);
    arr[1] = static_cast<std::uintptr_t>(g_viewCount);
    g_inStereoDraw = true;
    g_eyeCounter = 0;

    g_drawHook.thiscall<void>(self, viewport, canvas);

    g_inStereoDraw = false;
    arr[0] = savedData;
    arr[1] = 1;
    if (!g_committed && g_eyeCounter >= 2) CommitBuilding();  // (a view the engine skipped: the frame is still the eyes')
    // Back to a full-screen player (and its own view state) for anything outside Draw.
    auto* lp = static_cast<std::uint8_t*>(player);
    void*& vs = *reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState);
    if (g_leftViewState && (vs == g_rightViewState || (g_scopeViewState && vs == g_scopeViewState))) vs = g_leftViewState;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginX) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginY) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeX) = 1.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeY) = 1.0f;
    if (!g_loggedStereo) {
        g_loggedStereo = true;
        MLOG("stereo: first two-player Draw -- %d CalcSceneView calls", g_eyeCounter);
    }
    static bool loggedScope = false;
    if (g_viewCount == 3 && !loggedScope) {
        loggedScope = true;
        MLOG("scope: first three-player Draw -- %d CalcSceneView calls; the eyes %d px each, the scope view %dx%d at x %d",
             g_eyeCounter, g_eyeW, g_colS, g_colS, g_colX);
    }
    PaceAfterDraw();
}

// CalcSceneView entry: stack arg 1 = ULocalPlayer* (this). In a stereo Draw, the first call is
// the left eye, the second the right: give each half of the viewport.
void OnCalcSceneViewEntry(SafetyHookContext& ctx) {
    g_thisEye = 0;
    g_thisStereo = false;
    g_thisScope = false;
    if (!g_inStereoDraw) return;
    auto* lp = *reinterpret_cast<std::uint8_t**>(ctx.esp + 4);
    if (!lp) return;
    const shared::Header* hdr = bridge::SharedHeader();
    const float W = hdr && hdr->width ? static_cast<float>(hdr->width) : 1.0f, H = hdr && hdr->height ? static_cast<float>(hdr->height) : 1.0f;
    g_thisIndex = g_eyeCounter++;
    g_thisStereo = true;
    if (g_thisIndex >= 2) {
        // The scope view (SCOPE-DESIGN): the square column at the right, its own view state (its occlusion history).
        g_thisScope = true;
        g_thisEye = 1;
        // (CalcSceneView truncates Origin x width in float: a quarter pixel in lands on the pixel meant.)
        *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginX) = (static_cast<float>(g_colX) + 0.25f) / W;
        *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginY) = 0.0f;
        *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeX) = (static_cast<float>(g_colS) + 0.25f) / W;
        *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeY) = (static_cast<float>(g_colS) + 0.25f) / H;
        auto* slot = reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState);
        if (!g_scopeViewState) {
            using AllocFn = void*(__cdecl*)();
            g_scopeViewState = reinterpret_cast<AllocFn>(addr::kAllocateViewState)();
            MLOG("scope: allocated the scope view's FSceneViewState %p", g_scopeViewState);
        }
        if (g_scopeViewState) *slot = g_scopeViewState;
        return;
    }
    // Debug.SwapEyeOrder: render the right eye first (experiment: which eye does per-frame-once work).
    const int eye = (g_thisIndex & 1) ^ (g_cfg.debugSwapEyes ? 1 : 0);
    g_thisEye = eye;
    // Debug.SwapHalves (experiment): the left eye in the right half and vice versa; the host swaps back.
    const float eyeFrac = (static_cast<float>(g_eyeW) + 0.25f) / W;  // (a quarter pixel in: the engine truncates)
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginX) = (eye ^ (g_cfg.debugSwapHalves ? 1 : 0)) ? eyeFrac : 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginY) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeX) = eyeFrac;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeY) = 1.0f;

    // Each eye needs its own FSceneViewState (occlusion/visibility history). Sharing the player's
    // one made the right eye consume the left eye's occlusion results -> heavy right-eye flicker
    // (headset round 3). Eye 1 gets a second state from the engine's own AllocateViewState.
    auto* slot = reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState);
    if (eye == 0) {
        if (*slot != g_rightViewState && (!g_scopeViewState || *slot != g_scopeViewState))
            g_leftViewState = *slot;  // follow the engine if it replaces its own
    } else if (g_cfg.stereoViewState) {
        if (!g_rightViewState) {
            using AllocFn = void*(__cdecl*)();
            g_rightViewState = reinterpret_cast<AllocFn>(addr::kAllocateViewState)();
            MLOG("stereo: allocated a right-eye FSceneViewState %p (left %p)", g_rightViewState, g_leftViewState);
        }
        if (g_rightViewState) *slot = g_rightViewState;
    }
}

// Debug.ViewState: the game's own camera (before the head is applied) for scripted tests --
// %TEMP%\MOHAVR\view_state.txt = "x y z yaw pitch" (Unreal units / rotator units), 5 times a second.
void WriteViewState(const float* loc, const int* rot, std::uintptr_t localPlayer) {
    static DWORD next = GetTickCount();
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 200;
    static std::wstring path;
    if (path.empty()) {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        path = std::wstring(tmp, n) + L"MOHAVR";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\view_state.txt";
    }
    const std::wstring part = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, part.c_str(), L"w") != 0 || !f) return;
    // Cursor probe (F): this thread's ShowCursor display count, read without ever showing it
    // (decrement, then restore), and the global cursor state.
    const int cursorCount = ShowCursor(FALSE) + 1;
    ShowCursor(TRUE);
    CURSORINFO ci{sizeof(ci)};
    GetCursorInfo(&ci);
    // The controller's own Location (+0xE8, just before Rotation) and yaw, for the cinema-camera check.
    const auto ctrl = localPlayer ? *reinterpret_cast<std::uintptr_t*>(localPlayer + addr::kLocalPlayerActor) : 0;
    const float* cl = ctrl ? reinterpret_cast<const float*>(ctrl + addr::kActorRotation - 12) : nullptr;
    const int cyaw = ctrl ? *reinterpret_cast<const int*>(ctrl + addr::kActorRotation + 4) : 0;
    const int cpitch = ctrl ? *reinterpret_cast<const int*>(ctrl + addr::kActorRotation) : 0;
    fprintf(f, "%.1f %.1f %.1f %d %d cursor %d %lu ctrl %.1f %.1f %.1f %d pitch %d\n", loc[0], loc[1], loc[2],
            rot[1] & 0xFFFF, rot[0] & 0xFFFF, cursorCount, ci.flags, cl ? cl[0] : 0.0f, cl ? cl[1] : 0.0f,
            cl ? cl[2] : 0.0f, cyaw & 0xFFFF, cpitch & 0xFFFF);
    fclose(f);
    MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Snap turn (Comfort, host menu): the host sums snap steps in hdr->snapYawTotal; each change is added
// to the PlayerController's Rotation.Yaw once -- the game then turns the body, movement and aim with
// it. Only while the controller's yaw is the one this view came from (within ~11 degrees: view
// shake), so a cutscene camera or a vehicle is never touched. `localPlayer` = EDI at the merge point.
void ApplySnapTurn(const shared::Header* hdr, std::uintptr_t localPlayer, const int* viewRot) {
    static bool          init = false;
    static std::int32_t  applied = 0;
    const std::int32_t   total = hdr->snapYawTotal;
    if (!init) { init = true; applied = total; }  // a total from before this game session is not a request
    if (total == applied) return;
    const std::int32_t delta = total - applied;
    applied = total;
    const auto ctrl = localPlayer ? *reinterpret_cast<std::uintptr_t*>(localPlayer + addr::kLocalPlayerActor) : 0;
    if (!ctrl) return;
    int* yaw = reinterpret_cast<int*>(ctrl + addr::kActorRotation + 4);
    const int diff = static_cast<std::int16_t>(static_cast<std::uint16_t>((*yaw - viewRot[1]) & 0xFFFF));
    static int logged = 0;
    if (diff < -2048 || diff > 2048) {
        if (logged++ < 8) MLOG("snap: skipped %+d -- controller yaw %d is not the view's %d (camera/cutscene?)", delta, *yaw, viewRot[1]);
        return;
    }
    *yaw += delta;
    if (logged++ < 8) MLOG("snap: controller yaw %+d (%.0f deg) -> %d", delta, delta * 360.0 / 65536.0, *yaw);
}

// Diagnostics (round 26, "a similar animation to sprint happens when walking over rough terrain or falling a small
// distance"): per jump or fall (the pawn's Physics == PHYS_Falling), from take-off to 0.8 s after landing (the landing
// animation plays then), how far the game camera moved against the body -- peak to peak forward/right/up in the body's
// frame -- and which jump activities played (Weapon.JumpArms).
void TrackJumpSway(std::uintptr_t pawn, const float* loc, int camYaw, LONGLONG now) {
    struct Air {
        bool     on = false;
        LONGLONG start = 0, landed = 0;
        float    lo[4] = {}, hi[4] = {};
        unsigned acts = 0;  // bit per jump activity seen (26-29)
        int      logged = 0;
    };
    static Air a;
    const int po = names::PropertyOffset(pawn, "Physics"), ao = names::PropertyOffset(pawn, "CurrentActivity");
    if (po < 0) return;
    const std::uint8_t physics = *reinterpret_cast<const std::uint8_t*>(pawn + po);
    const std::uint8_t act = ao >= 0 ? *reinterpret_cast<const std::uint8_t*>(pawn + ao) : 0;
    const float* pl = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    const int pyaw = *reinterpret_cast<const int*>(pawn + addr::kActorRotation + 4);
    const float yaw = UnrToRad(pyaw), c = std::cos(yaw), sn = std::sin(yaw);
    const float rx = loc[0] - pl[0], ry = loc[1] - pl[1];
    const float v[4] = {rx * c + ry * sn, -rx * sn + ry * c, loc[2] - pl[2],
                        static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>((camYaw - pyaw) & 0xFFFF))) * (360.0f / 65536.0f)};
    const bool falling = physics == 2;  // PHYS_Falling
    if (!a.on) {
        if (!falling) return;
        a = Air{true, now, 0, {v[0], v[1], v[2], v[3]}, {v[0], v[1], v[2], v[3]}, 0, a.logged};
    }
    for (int i = 0; i < 4; ++i) {
        a.lo[i] = v[i] < a.lo[i] ? v[i] : a.lo[i];
        a.hi[i] = v[i] > a.hi[i] ? v[i] : a.hi[i];
    }
    if (act >= 26 && act <= 29) a.acts |= 1u << (act - 26);
    if (falling) {
        a.landed = 0;
        return;
    }
    if (!a.landed) a.landed = now;
    if (QpcMs(now - a.landed) < 800.0) return;
    if (a.logged < 40) {
        ++a.logged;
        MLOG("view: in the air %.0f ms -- the game camera moved fwd %.1f right %.1f up %.1f cm against the body and turned %.1f deg "
             "(jump activities:%s%s%s%s; Weapon.JumpArms=%s)", QpcMs(a.landed - a.start), a.hi[0] - a.lo[0], a.hi[1] - a.lo[1],
             a.hi[2] - a.lo[2], a.hi[3] - a.lo[3], (a.acts & 1u) ? " start" : "", (a.acts & 2u) ? " falling" : "",
             (a.acts & 4u) ? " soft landing" : "", (a.acts & 8u) ? " hard landing" : "", g_cfg.jumpArms ? "idle" : "game");
    }
    a.on = false;
}

// Diagnostics (round 25, "slight jitter in general movement ... a slight rubber banding feeling"): while the body moves,
// how far the game camera -- the base of both eyes -- sways against it (the arms' walk animation moves the Cam socket;
// Weapon.WalkArms), per 5 s of moving: peak to peak forward/right/up in the body's frame, and the camera's yaw against
// the body's. A stance change inside a window shows as up to 65 cm up.
void TrackCameraSway(const float* loc, int camYaw) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    struct Sway {
        std::uintptr_t pawn = 0;
        LONGLONG last = 0;
        float    lastLoc[3] = {};
        float    lo[4] = {}, hi[4] = {};
        double   moving = 0.0, dist = 0.0;
        long     frames = 0;
        int      logged = 0;
    };
    static Sway s;
    if (!pawn) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const float* pl = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    const int pyaw = *reinterpret_cast<const int*>(pawn + addr::kActorRotation + 4);
    TrackJumpSway(pawn, loc, camYaw, q.QuadPart);
    const double dt = s.last ? QpcMs(q.QuadPart - s.last) / 1000.0 : 0.0;
    const float dx = pl[0] - s.lastLoc[0], dy = pl[1] - s.lastLoc[1];
    const float step = std::sqrt(dx * dx + dy * dy);
    const bool fresh = pawn != s.pawn || dt <= 0.0 || dt > 0.25;
    s.pawn = pawn;
    s.last = q.QuadPart;
    for (int i = 0; i < 3; ++i) s.lastLoc[i] = pl[i];
    if (fresh || step / dt < 50.0) {  // standing (or a jump in time): the window only counts moving frames
        if (fresh) s.frames = 0;
        return;
    }
    const float yaw = UnrToRad(pyaw), c = std::cos(yaw), sn = std::sin(yaw);
    const float rx = loc[0] - pl[0], ry = loc[1] - pl[1];
    const float v[4] = {rx * c + ry * sn, -rx * sn + ry * c, loc[2] - pl[2],
                        static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>((camYaw - pyaw) & 0xFFFF))) * (360.0f / 65536.0f)};
    for (int i = 0; i < 4; ++i) {
        s.lo[i] = s.frames ? (v[i] < s.lo[i] ? v[i] : s.lo[i]) : v[i];
        s.hi[i] = s.frames ? (v[i] > s.hi[i] ? v[i] : s.hi[i]) : v[i];
    }
    ++s.frames;
    s.moving += dt;
    s.dist += step;
    if (s.moving >= 5.0) {
        if (s.logged < 60) {
            ++s.logged;
            MLOG("view: moving %.1f s at %.0f u/s -- the game camera swayed fwd %.1f right %.1f up %.1f cm (peak to peak, in the "
                 "body's frame) and turned %.2f deg against it (Weapon.WalkArms=%s)", s.moving, s.dist / s.moving,
                 s.hi[0] - s.lo[0], s.hi[1] - s.lo[1], s.hi[2] - s.lo[2], s.hi[3] - s.lo[3], g_cfg.walkArms ? "idle" : "game");
        }
        s.moving = s.dist = 0.0;
        s.frames = 0;
    }
}

// The view is the local pawn's own eye: within 3 m of it. The landing roll's camera animation turns the view more than
// 11 degrees off the controller's yaw, so it is not "the player's view" (g_viewIsPlayers) -- but it is still the eye.
bool ViewAtPawn(std::uintptr_t pawn, const float* loc) {
    if (!pawn) return false;
    const float* pl = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    const float dx = loc[0] - pl[0], dy = loc[1] - pl[1], dz = loc[2] - pl[2];
    return dx * dx + dy * dy + dz * dz < 300.0f * 300.0f;
}

// The pawn's feet: its location less its collision cylinder's half height (the floor it stands on). False if unknown.
bool PawnFeet(std::uintptr_t pawn, float& feetZ, float& halfHeight) {
    const int co = names::PropertyOffset(pawn, "CylinderComponent");
    const std::uintptr_t cyl = co >= 0 ? *reinterpret_cast<const std::uintptr_t*>(pawn + co) : 0;
    const int ho = cyl ? names::PropertyOffset(cyl, "CollisionHeight") : -1;
    if (ho < 0) return false;
    halfHeight = *reinterpret_cast<const float*>(cyl + ho);
    if (!(halfHeight > 1.0f && halfHeight < 200.0f)) return false;
    feetZ = reinterpret_cast<const float*>(pawn + addr::kActorLocation)[2] - halfHeight;
    return true;
}

// Debug.EyeFloor (GOAL B1, "you clip into the ground when landing"): per frame, from the moment the pawn leaves the
// ground (Physics != PHYS_Walking) until 8 s after it is back, how high the final eye (the game camera + the head) and
// the game camera alone are above the floor under the eye: a trace down the eye's column from above the pawn's centre
// (the pawn itself ignored), so an eye already below the surface still measures negative. Also the pawn's physics,
// activity and height, and one summary line per landing with the lowest gaps.
void TrackEyeFloor(const float* eye, const float* cam, float upm, bool players) {
    const std::uintptr_t pawn = aim::LocalPlayerPawn();
    if (!pawn || !ViewAtPawn(pawn, cam)) return;
    struct Land {
        bool     on = false;
        LONGLONG start = 0, walking = 0;
        float    minEye = 1e9f, minCam = 1e9f, minEyeFeet = 1e9f, minCamFeet = 1e9f;
        double   minEyeAt = 0.0, minCamAt = 0.0, minEyeFeetAt = 0.0;
        long     lines = 0;
        int      landing = 0;  // the pawn's eLandingType seen (the last non-zero): the kind of parachute landing
    };
    static Land l;
    static long total = 0;
    const int po = names::PropertyOffset(pawn, "Physics"), ao = names::PropertyOffset(pawn, "CurrentActivity");
    if (po < 0) return;
    const std::uint8_t physics = *reinterpret_cast<const std::uint8_t*>(pawn + po);
    const std::uint8_t act = ao >= 0 ? *reinterpret_cast<const std::uint8_t*>(pawn + ao) : 0;
    const float* pl = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const bool walking = physics == 1;  // PHYS_Walking
    if (!l.on && walking) {
        // On the ground: every 2 s, how high the eye is above the feet (standing, crouched).
        static DWORD next = GetTickCount();
        const DWORD nowMs = GetTickCount();
        if (static_cast<LONG>(nowMs - next) >= 0) {
            next = nowMs + 2000;
            float feet = 0.0f, half = 0.0f;
            if (PawnFeet(pawn, feet, half) && total < 6000) {
                ++total;
                const float k = upm > 1.0f ? 100.0f / upm : 1.0f;
                MLOG("eyefloor: on the ground -- half height %.1f, activity %u: eye %.1f cm above the feet (game camera %.1f)", half,
                     act, (eye[2] - feet) * k, (cam[2] - feet) * k);
            }
        }
        return;
    }
    if (!l.on) {
        l = Land{true, q.QuadPart, 0, 1e9f, 1e9f, 1e9f, 1e9f, 0.0, 0.0, 0.0, 0, 0};
        const int be = names::PropertyOffset(pawn, "BaseEyeHeight"), ee = names::PropertyOffset(pawn, "EyeHeight");
        float fz = 0.0f, hh = 0.0f;
        PawnFeet(pawn, fz, hh);
        MLOG("eyefloor: off the ground (physics %u, activity %u, pawn z %.1f, half height %.1f, BaseEyeHeight %.1f, EyeHeight "
             "%.1f; Camera.MinEyeHeight %.0f cm)", physics, act, pl[2], hh, be >= 0 ? *reinterpret_cast<const float*>(pawn + be) : -1.0f,
             ee >= 0 ? *reinterpret_cast<const float*>(pawn + ee) : -1.0f, g_cfg.minEyeHeight);
    }
    if (walking && !l.walking) l.walking = q.QuadPart;
    if (!walking) l.walking = 0;
    {
        const int lo = names::PropertyOffset(pawn, "eLandingType");
        const int lt = lo >= 0 ? *reinterpret_cast<const std::uint8_t*>(pawn + lo) : 0;
        if (lt != 0) l.landing = lt;
    }
    const double t = QpcMs(q.QuadPart - l.start) / 1000.0;
    const float top[3] = {eye[0], eye[1], pl[2] + 40.0f}, bottom[3] = {eye[0], eye[1], pl[2] - 800.0f};
    float hit[3];
    const bool found = aim::WorldTrace(pawn, top, bottom, hit);
    const float k = upm > 1.0f ? 100.0f / upm : 1.0f;  // units -> cm
    const float gapEye = (eye[2] - hit[2]) * k, gapCam = (cam[2] - hit[2]) * k;
    if (found) {
        if (gapEye < l.minEye) { l.minEye = gapEye; l.minEyeAt = t; }
        if (gapCam < l.minCam) { l.minCam = gapCam; l.minCamAt = t; }
    }
    // Above the feet (the floor the pawn stands on; the trace's floor can be a surface under walkable collision).
    float feet = 0.0f, half = 0.0f;
    const bool haveFeet = PawnFeet(pawn, feet, half);
    const float eyeFeet = (eye[2] - feet) * k, camFeet = (cam[2] - feet) * k;
    if (haveFeet && walking) {  // on the ground: in the air the feet are wherever the fall is
        if (eyeFeet < l.minEyeFeet) { l.minEyeFeet = eyeFeet; l.minEyeFeetAt = t; }
        if (camFeet < l.minCamFeet) l.minCamFeet = camFeet;
    }
    if (total < 6000) {
        ++total;
        ++l.lines;
        MLOG("eyefloor: t %.3f phys %u act %u pawn z %.1f half %.1f%s | feet: cam %.1f eye %.1f cm | trace floor %s%.1f: cam %.1f "
             "eye %.1f cm", t, physics, act, pl[2], half, players ? "" : " (camera anim)", camFeet, eyeFeet, found ? "" : "(none) ",
             hit[2], gapCam, gapEye);
    }
    if (l.walking && QpcMs(q.QuadPart - l.walking) > 8000.0) {
        MLOG("eyefloor: landing summary -- %.1f s off the ground (eLandingType %d); on the ground the eye was at least %.1f cm above "
             "the feet (t %.2f; the game camera %.1f cm); above the traced floor: eye %.1f cm (t %.2f), game camera %.1f cm; "
             "Camera.MinEyeHeight %.0f cm; %ld lines", QpcMs(l.walking - l.start) / 1000.0, l.landing, l.minEyeFeet, l.minEyeFeetAt,
             l.minCamFeet, l.minEye, l.minEyeAt, l.minCam, g_cfg.minEyeHeight, l.lines);
        l.on = false;
    }
}

// Camera.SteadyLanding (the player, 2026-10-01: the landing "still freaks out in the same way" -- MinEyeHeight only kept
// it out of the ground): the parachute landing (CurrentActivity 41 CONTROLLED_LANDING, the roll; 42 REMOVE_GEAR, getting
// up) plays a camera animation -- down to 18 cm above the feet and turned about (ENGINE-NOTES 5an). Held instead at the
// pawn's standing eye, Location + BaseEyeHeight (where the game's camera is when the landing ends: 160.8 cm above the
// feet), facing the controller's yaw: at once (the touchdown frame already has the camera 50 cm down), eased out over
// 0.25 s. Decided on eye 0, applied to both. The first-person body (the arms and legs of the roll) is drawn re-based from
// the game's tumbling camera onto the held one (round 40: "the body visibly contorts around you"), as the flat game shows
// it: d = inverse(game camera) * held (level, the held yaw), eased with the view. True while held (eye 0's decision).
// UE3's FRotationMatrix rows (pitch p, yaw y, roll r): X forward, Y right, Z up; a world point p' = p * M.
struct M4f {
    float m[4][4];
};

M4f RotFrame(float p, float y, float r, const float* t) {
    const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    M4f m{};
    m.m[0][0] = cp * cy; m.m[0][1] = cp * sy; m.m[0][2] = sp;
    m.m[1][0] = sr * sp * cy - cr * sy; m.m[1][1] = sr * sp * sy + cr * cy; m.m[1][2] = -sr * cp;
    m.m[2][0] = -(cr * sp * cy + sr * sy); m.m[2][1] = cy * sr - cr * sp * sy; m.m[2][2] = cr * cp;
    m.m[3][0] = t[0]; m.m[3][1] = t[1]; m.m[3][2] = t[2]; m.m[3][3] = 1.0f;
    return m;
}

M4f RigidInv(const M4f& a) {
    M4f r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int j = 0; j < 3; ++j) r.m[3][j] = -(a.m[3][0] * r.m[0][j] + a.m[3][1] * r.m[1][j] + a.m[3][2] * r.m[2][j]);
    r.m[3][3] = 1.0f;
    return r;
}

M4f MulM(const M4f& a, const M4f& b) {
    M4f r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

void SteadyLanding(std::uintptr_t localPlayer, float* loc, int* rot) {
    static float w = 0.0f, held[3] = {0, 0, 0};
    static int heldYaw = 0;
    static LONGLONG last = 0;
    static bool wasOn = false;
    if (g_thisEye == 0) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const std::uintptr_t ctrl = localPlayer ? *reinterpret_cast<std::uintptr_t*>(localPlayer + addr::kLocalPlayerActor) : 0;
        const int ao = pawn ? names::PropertyOffset(pawn, "CurrentActivity") : -1;
        const int bo = pawn ? names::PropertyOffset(pawn, "BaseEyeHeight") : -1;
        const std::uint8_t act = ao >= 0 ? *reinterpret_cast<const std::uint8_t*>(pawn + ao) : 0;
        const bool atPawn = pawn && ViewAtPawn(pawn, loc);
        const bool on = atPawn && ctrl && bo >= 0 && (act == 41 || act == 42);
        if (on) {
            const float* pl = reinterpret_cast<const float*>(pawn + addr::kActorLocation);
            held[0] = pl[0];
            held[1] = pl[1];
            held[2] = pl[2] + *reinterpret_cast<const float*>(pawn + bo);
            heldYaw = *reinterpret_cast<const int*>(ctrl + addr::kActorRotation + 4);
        }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const double dt = last ? QpcMs(q.QuadPart - last) / 1000.0 : 0.0;
        last = q.QuadPart;
        const float step = static_cast<float>(dt < 0.1 ? dt : 0.1) / 0.25f;
        w = !atPawn ? 0.0f : on ? 1.0f : (w - step > 0.0f ? w - step : 0.0f);
        if (on != wasOn) {
            wasOn = on;
            MLOG("view: %s (Camera.SteadyLanding; activity %u)", on ? "the landing's camera animation left out -- the view held at "
                 "the standing eye and the controller's heading" : "the landing over -- back to the game's camera", act);
        }
    }
    if (g_thisEye == 0) g_landingHeld = w > 0.0f;
    if (w <= 0.0f) return;
    const float e = w * w * (3.0f - 2.0f * w);
    const float gameLoc[3] = {loc[0], loc[1], loc[2]};
    const int gameRot[3] = {rot[0], rot[1], rot[2]};
    for (int i = 0; i < 3; ++i) loc[i] += (held[i] - loc[i]) * e;
    const int dy = static_cast<std::int16_t>(static_cast<std::uint16_t>((heldYaw - rot[1]) & 0xFFFF));
    rot[1] = (rot[1] + static_cast<int>(std::lround(dy * e))) & 0xFFFF;
    if (g_thisEye == 0) {
        const float gp = UnrToRad(static_cast<std::int16_t>(gameRot[0] & 0xFFFF)), gr = UnrToRad(static_cast<std::int16_t>(gameRot[2] & 0xFFFF));
        const M4f game = RotFrame(gp, UnrToRad(gameRot[1]), gr, gameLoc);
        const M4f base = RotFrame(gp * (1.0f - e), UnrToRad(rot[1]), gr * (1.0f - e), loc);
        const M4f d = MulM(RigidInv(game), base);
        viewmodel::DrawWithoutHands(*reinterpret_cast<const float(*)[16]>(&d.m[0][0]));
    }
}

// --- the view merge hook ------------------------------------------------------------------------
void OnViewPoint(SafetyHookContext& ctx) {
    g_thisViewActive = false;
    shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !g_cfg.headTracking) return;

    shared::Pose head, eye[2];
    shared::Fov fov[2];
    const std::uint32_t recenterSeq = hdr->recenterSeq;
    if (!shared::ReadViews(hdr, head, eye, fov)) return;
    ForceVrSettings();
    UpdateOrigin(hdr, recenterSeq, head);
    {
        const float dx = eye[1].px - eye[0].px, dy = eye[1].py - eye[0].py, dz = eye[1].pz - eye[0].pz;
        const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (ipd > 0.04f && ipd < 0.09f) g_ipd = ipd;
    }

    auto* loc = *reinterpret_cast<float**>(ctx.ebp + 0x10);
    auto* rot = *reinterpret_cast<int**>(ctx.ebp + 0x14);
    if (!loc || !rot) return;
    if (g_cfg.debugViewState && g_thisEye == 0) WriteViewState(loc, rot, ctx.edi);
    if (g_thisEye == 0) {
        // Is this the player's own view? In first person the view's yaw is the controller's Rotation.Yaw
        // (measured equal within 2 units); a menu scene or matinee camera has its own.
        const auto ctrl = ctx.edi ? *reinterpret_cast<std::uintptr_t*>(ctx.edi + addr::kLocalPlayerActor) : 0;
        if (ctrl) {
            const int cyaw = *reinterpret_cast<const int*>(ctrl + addr::kActorRotation + 4);
            const int diff = static_cast<std::int16_t>(static_cast<std::uint16_t>((cyaw - rot[1]) & 0xFFFF));
            g_viewIsPlayers = diff >= -2048 && diff <= 2048;
            // Aim.HeadPitch: the controller's pitch follows the head. The view takes its pitch from the head
            // anyway, but the gun model and the shot direction follow the controller rotation (ENGINE-NOTES
            // 5o); left alone, stray mouse movement (tab-out, dragging the window) tilted the gun up in front
            // of the eyes (headset round 5). Only for the player's own view, never a cutscene camera.
            if (g_cfg.aimHeadPitch && !g_cinema && g_viewIsPlayers) {
                const Vec3 f = QuatRotate(head, 0.0f, 0.0f, -1.0f);  // LOCAL is gravity-aligned: +Y up
                const float pitch = std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z));
                *reinterpret_cast<int*>(ctrl + addr::kActorRotation) = RadToUnr(pitch) & 0xFFFF;
            }
        }
        if (!g_cinema) ApplySnapTurn(hdr, ctx.edi, rot);
    }
    if (g_cinema) return;  // flat: the game's own camera, untouched; the frame is marked "no view"

    // The game's own camera, where its shots start (aim): before JumpLift below and before the head.
    const float gameCam[3] = {loc[0], loc[1], loc[2]};
    // Camera.JumpLift=0 (round 26): a jump lifts the game's camera by fJumpCameraOffset (up to 8 units, along the view's
    // up axis) on top of the arms' Cam socket (GetPawnViewLocationNative 0x10E81DE0, ENGINE-NOTES 5ah). In VR it bumped
    // the whole view on every jump, and the gun -- placed against that camera -- dropped as far below the hand. Left out.
    if (!g_cfg.jumpLift && g_viewIsPlayers) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        const int jo = pawn ? names::PropertyOffset(pawn, "fJumpCameraOffset") : -1;
        const float lift = jo >= 0 ? *reinterpret_cast<const float*>(pawn + jo) : 0.0f;
        if (lift != 0.0f && lift > -50.0f && lift < 50.0f) {
            const float p = UnrToRad(static_cast<std::int16_t>(rot[0] & 0xFFFF)), y = UnrToRad(rot[1]);
            // FRotationMatrix's Z (up) axis, roll 0: (-sin p cos y, -sin p sin y, cos p).
            loc[0] += std::sin(p) * std::cos(y) * lift;
            loc[1] += std::sin(p) * std::sin(y) * lift;
            loc[2] -= std::cos(p) * lift;
        }
    }

    if (g_cfg.steadyLanding) SteadyLanding(ctx.edi, loc, rot);

    // Camera.MinEyeHeight (GOAL B, "you clip into the ground when landing"): the parachute's botched landing rolls the
    // arms' Cam socket down to the floor, and in VR the view keeps the head's orientation, so the eye looked out from
    // inside the ground. Keep the head's eye at least MinEyeHeight above the pawn's feet: the game camera is raised
    // before the hands' mapping below takes it (the hands stay with the view), by the same amount for both eyes (the
    // head's height, not each eye's).
    if (g_cfg.minEyeHeight > 0.0f) {
        const std::uintptr_t pawn = aim::LocalPlayerPawn();
        float feet = 0.0f, half = 0.0f;
        if (pawn && (g_viewIsPlayers || ViewAtPawn(pawn, loc)) && PawnFeet(pawn, feet, half)) {
            const float live = hdr->unitsPerMeter;
            const float s = (live > 1.0f && live < 1000.0f) ? live : g_cfg.unitsPerMeter;
            float headUp = 0.0f;  // the head's height against the origin, in units (as the translation below)
            if (g_cfg.headPosition && g_haveOrigin) headUp = (head.py - g_oy) * s;
            const float h = hdr->heightOffset;
            if (h > -1.0f && h < 1.0f) headUp += h * s;
            const float minZ = feet + g_cfg.minEyeHeight * s / 100.0f;
            const float raise = minZ - (loc[2] + headUp);
            if (raise > 0.0f) {
                loc[2] += raise;
                static DWORD lastLog = 0;
                static int logged = 0;
                const DWORD now = GetTickCount();
                if (g_thisEye == 0 && logged < 40 && now - lastLog > 1000) {
                    ++logged;
                    lastLog = now;
                    MLOG("view: the eye held %.0f cm above the feet (raised %.1f cm; Camera.MinEyeHeight)", g_cfg.minEyeHeight,
                         raise * 100.0f / s);
                }
            }
        }
    }

    const float gameYaw = UnrToRad(rot[1]);
    if (g_thisEye == 0 && g_viewIsPlayers) {
        // M7: the mapping this frame uses, for PoseToWorld (before the head moves `loc`).
        g_world = {true, {loc[0], loc[1], loc[2]}, gameYaw, head, UnrToRad(static_cast<std::int16_t>(rot[0] & 0xFFFF))};
        std::memcpy(g_gameCam, gameCam, sizeof(g_gameCam));
        TrackCameraSway(loc, rot[1]);
    }
    // Stereo: this eye's own pose (orientation and position); mono: the head. The scope view: the host's scope camera
    // (Debug.ScopeView without one: the right eye) -- mapped into the world exactly as the eyes are.
    const shared::Pose& p = g_thisScope ? (g_scopeCamOk ? g_scopeCam : eye[1]) : g_thisStereo ? eye[g_thisEye] : head;

    // View basis in Unreal axes, then turned by the game's yaw (body heading).
    const Vec3 fXr = QuatRotate(p, 0, 0, -1), rXr = QuatRotate(p, 1, 0, 0), uXr = QuatRotate(p, 0, 1, 0);
    const Vec3 F = YawRotate(XrToUe(fXr.x, fXr.y, fXr.z), gameYaw);
    const Vec3 R = YawRotate(XrToUe(rXr.x, rXr.y, rXr.z), gameYaw);
    const Vec3 U = YawRotate(XrToUe(uXr.x, uXr.y, uXr.z), gameYaw);

    // FMatrix::Rotator (UE3): pitch/yaw from X, roll from Y/Z against the unrolled Y axis.
    const float yaw = std::atan2(F.y, F.x);
    const float pitch = std::atan2(F.z, std::sqrt(F.x * F.x + F.y * F.y));
    const Vec3 SY{-std::sin(yaw), std::cos(yaw), 0.0f};
    const float roll = std::atan2(Dot(U, SY), Dot(R, SY));
    rot[0] = RadToUnr(pitch);
    rot[1] = RadToUnr(yaw);
    rot[2] = RadToUnr(roll);

    // Translation: head movement relative to the origin (HeadPosition), and in stereo the eye's own
    // offset from the head (half the IPD) -- the eye separation must not depend on the origin.
    //   position tracking on + origin known: p - origin (covers both)
    //   otherwise, stereo:                   p - head   (eye separation only)
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    bool translate = true;
    if (g_cfg.headPosition && g_haveOrigin) { ox = g_ox; oy = g_oy; oz = g_oz; }
    else if (g_thisStereo) { ox = head.px; oy = head.py; oz = head.pz; }
    else translate = false;
    if (translate) {
        const Vec3 d = YawRotate(XrToUe(p.px - ox, p.py - oy, p.pz - oz), gameYaw);
        // World scale: live from the host's menu (the player's saved setting) once set, else the ini.
        // It scales both head translation and, in stereo, the eye separation -> perceived world size.
        const float live = hdr->unitsPerMeter;
        const float s = (live > 1.0f && live < 1000.0f) ? live : g_cfg.unitsPerMeter;
        static float lastLogged = 0.0f;
        if (s != lastLogged) {
            MLOG("view: world scale %.1f units per metre%s", s, (live > 1.0f && live < 1000.0f) ? " (from the host menu/settings)" : " (ini)");
            lastLogged = s;
        }
        loc[0] += d.x * s;
        loc[1] += d.y * s;
        loc[2] += d.z * s;
    }
    // Seated/standing height offset from the menu (metres, player setting).
    {
        const float h = hdr->heightOffset;
        if (h > -1.0f && h < 1.0f && h != 0.0f) {
            const float live = hdr->unitsPerMeter;
            loc[2] += h * ((live > 1.0f && live < 1000.0f) ? live : g_cfg.unitsPerMeter);
        }
    }

    if (g_cfg.debugEyeFloor && g_thisEye == 0) {
        const float live = hdr->unitsPerMeter;
        TrackEyeFloor(loc, gameCam, (live > 1.0f && live < 1000.0f) ? live : g_cfg.unitsPerMeter, g_viewIsPlayers);
    }
    if (g_thisStereo && g_thisEye == 0) {
        for (int i = 0; i < 3; ++i) {
            g_eye0Loc[i] = loc[i];
            g_eye0Rot[i] = rot[i];
        }
    }
    if (g_thisStereo) {
        g_thisFov = fov[g_thisEye];
    } else {
        // Mono: one render that covers both eyes.
        g_thisFov.tanLeft = fov[0].tanLeft < fov[1].tanLeft ? fov[0].tanLeft : fov[1].tanLeft;
        g_thisFov.tanRight = fov[0].tanRight > fov[1].tanRight ? fov[0].tanRight : fov[1].tanRight;
        g_thisFov.tanUp = fov[0].tanUp > fov[1].tanUp ? fov[0].tanUp : fov[1].tanUp;
        g_thisFov.tanDown = fov[0].tanDown < fov[1].tanDown ? fov[0].tanDown : fov[1].tanDown;
    }

    // Record what this view was rendered with (the projection hook refines the FOV). A frame is
    // complete after eye 1 (stereo) or the single view (mono).
    LARGE_INTEGER viewQpc;
    QueryPerformanceCounter(&viewQpc);
    EnterCriticalSection(&g_lock);
    if (g_thisScope) {
        g_building.scopeCam = p;  // (the projection records its size and FOV)
        LeaveCriticalSection(&g_lock);
        g_thisViewActive = true;
        return;
    }
    if (g_thisEye == 0) g_building.qpc = viewQpc.QuadPart;
    if (g_thisStereo && g_thisIndex == 0) {  // a new frame: no scope view until one renders
        g_building.scopeRect[2] = g_building.scopeRect[3] = 0;
        g_building.eyeWidth = g_colS > 0 ? static_cast<std::uint32_t>(g_eyeW) : 0u;
        // The gun pose this frame's drawn gun was baked with: the last Draw's (the view model builds the move at Draw N and
        // the bake applies it in the next tick) -- the lens sits on the drawn eyepiece.
        static shared::Pose lastGun{};
        static bool lastGunOk = false;
        g_building.gunPose = lastGun;
        g_building.gunValid = g_colS > 0 && lastGunOk;
        shared::Pose aimRay{};
        std::uint32_t gf = 0;
        lastGunOk = shared::ReadGun(hdr, lastGun, aimRay, gf);
    }
    g_building.pose[g_thisEye] = p;
    g_building.fov[g_thisEye] = g_thisFov;
    g_building.stereo = g_thisStereo;
    if (!g_thisStereo) {
        g_building.pose[1] = p;
        g_building.fov[1] = g_thisFov;
    }
    LeaveCriticalSection(&g_lock);
    g_thisViewActive = true;
    if (g_thisEye == 0 && g_viewIsPlayers && ctx.edi && !g_landingHeld) {
        viewmodel::OnPlayerView();  // first: the aim follows the gun's barrel
        aim::OnPlayerView(*reinterpret_cast<std::uintptr_t*>(ctx.edi + addr::kLocalPlayerActor), g_gameCam);
        throwing::OnPlayerView();
    }

    if (++g_views == 1 || g_views % 4000 == 0) {
        MLOG("view #%ld (%s eye %d): pose q(%.3f %.3f %.3f %.3f) p(%.3f %.3f %.3f) -> rot P%d Y%d R%d (game yaw %d)", g_views,
             g_thisStereo ? "stereo" : "mono", g_thisEye, p.qx, p.qy, p.qz, p.qw, p.px, p.py, p.pz, rot[0], rot[1], rot[2],
             RadToUnr(gameYaw));
    }
}

// --- M5: the HUD as one head-locked panel (HUD.Mode=1) --------------------------------------------
// In a stereo Draw each "player" (eye) gets its HUD on a canvas the size of its half, laid out for a
// 960-wide screen at that half's edges -- a different place in each eye (round 3: "cross-eyed"). Just
// before each eye's HUD pass, give its canvas the rectangle where a panel HUD.Width wide, HUD.Distance
// ahead and HUD.Down below the eyes appears in THAT eye (its own projection and IPD offset): both eyes
// then fuse one flat HUD at that distance. The 3D views were already handed to the renderer.
// MOHA's HUD positions its elements by the canvas clip but draws them at fixed pixel sizes, so the
// canvas gets a virtual clip of panel/Scale and its matrix is scaled by Scale (OnHudMatrix): the HUD
// lays out on a larger virtual screen and is shrunk uniformly into the panel.
// The HUD loop's start (scopes): the scope view is the third player -- the loop runs over the eyes only.
void OnHudLoopStart(SafetyHookContext&) {
    if (!g_inStereoDraw || g_viewCount < 3) return;
    const auto engine = *reinterpret_cast<std::uintptr_t*>(addr::kGEngine);
    auto* arr = engine ? reinterpret_cast<std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;
    if (arr && arr[0] == reinterpret_cast<std::uintptr_t>(g_stereoPlayers) && arr[1] == 3) arr[1] = 2;
}

void OnHudView(SafetyHookContext& ctx) {
    if (!g_inStereoDraw || g_cfg.hudMode != 1) return;
    const shared::Header* hdr = bridge::SharedHeader();
    auto* view = reinterpret_cast<std::uint8_t*>(ctx.esi);
    if (!hdr || !view || !hdr->width || !hdr->height) return;
    const int eye = static_cast<int>(ctx.edx & 1);
    {
        // Diagnostics (headset round 8, "glass bowl" after tabbing back): log whenever an eye's view rect
        // changes -- the engine's own, before the HUD placement below rewrites it.
        static float seen[2][4] = {};
        static int logged = 0;
        const float* r = reinterpret_cast<const float*>(view + addr::kViewX);
        if ((r[0] != seen[eye][0] || r[1] != seen[eye][1] || r[2] != seen[eye][2] || r[3] != seen[eye][3]) && logged < 40) {
            ++logged;
            MLOG("diag: eye %d view rect x %.0f y %.0f  %.0f x %.0f (was %.0f %.0f %.0f %.0f)", eye, r[0], r[1], r[2], r[3],
                 seen[eye][0], seen[eye][1], seen[eye][2], seen[eye][3]);
            for (int i = 0; i < 4; ++i) seen[eye][i] = r[i];
        }
    }
    const shared::Fov f = g_building.fov[eye];  // this frame's widened FOV (game thread writes it)
    if (!(f.tanRight > f.tanLeft) || !(f.tanUp > f.tanDown)) return;
    const float eyeW = g_eyeW > 0 ? static_cast<float>(g_eyeW) : 0.5f * static_cast<float>(hdr->width), H = static_cast<float>(hdr->height);
    const float D = g_cfg.hudDistance;
    // Panel centre as seen from this eye: the eye sits +-IPD/2 along the head's right axis.
    const float ex = (eye == 0 ? -0.5f : 0.5f) * g_ipd;
    const float tx = -ex / D, ty = -g_cfg.hudDown / D;
    const float u = (tx - f.tanLeft) / (f.tanRight - f.tanLeft);
    const float v = (f.tanUp - ty) / (f.tanUp - f.tanDown);
    // Size: HUD.Width across, 16:9 (the HUD's own layout).
    const float halfW = 0.5f * g_cfg.hudWidth / D, halfH = halfW * 9.0f / 16.0f;
    const float pw = eyeW * 2.0f * halfW / (f.tanRight - f.tanLeft);
    const float ph = H * 2.0f * halfH / (f.tanUp - f.tanDown);
    const float x = static_cast<float>(eye) * eyeW + u * eyeW - 0.5f * pw;
    const float y = v * H - 0.5f * ph;
    const float s = g_cfg.hudScale;
    *reinterpret_cast<float*>(view + addr::kViewX) = x;
    *reinterpret_cast<float*>(view + addr::kViewY) = y;
    *reinterpret_cast<float*>(view + addr::kViewSizeX) = pw / s;
    *reinterpret_cast<float*>(view + addr::kViewSizeY) = ph / s;
    g_hudScalePending = s;
    static int logged = 0;
    if (logged < 2 && eye == logged) {
        ++logged;
        MLOG("hud: eye %d canvas -> x %.0f y %.0f  %.0f x %.0f px, virtual %.0f x %.0f at scale %.2f (panel %.2f m at %.2f m, "
             "%.2f m down, IPD %.1f mm)", eye, x, y, pw, ph, pw / s, ph / s, s, g_cfg.hudWidth, D, g_cfg.hudDown, g_ipd * 1000.0f);
    }
}

void OnHudMatrix(SafetyHookContext& ctx) {
    if (g_hudScalePending == 0.0f) return;
    auto* m = reinterpret_cast<float*>(ctx.esp + addr::kHudMatrixStackOffset);
    // Only an identity-plus-translation matrix, as Draw builds it -- anything else isn't ours to touch.
    if (m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[15] == 1.0f && m[1] == 0.0f && m[4] == 0.0f) {
        m[0] = g_hudScalePending;
        m[5] = g_hudScalePending;
    }
    g_hudScalePending = 0.0f;
}

// --- Render.DecalFix: decals in the right eye (ENGINE-NOTES 5r) ----------------------------------
// The engine's decal screen-box test (kDecalScreenBox) projects to absolute pixels but clamps to
// [0, SizeX]: in a view that starts at x = SizeX (our right eye) the box collapses and the decal is culled
// (headset round 7: bullet holes in the left eye only). For such views, the entry hook sets the view's
// X/Y to 0 for this one call (the render thread's own FViewInfo) and swaps the return address for a stub
// that restores X/Y and moves the resulting box back by them -- so the culling works and the scissor rect
// the caller builds from the box lands on the right half.
SafetyHookMid g_decalHook;
struct DecalCall {
    std::uintptr_t view = 0;
    float*         mn = nullptr;
    float*         mx = nullptr;
    float          x = 0.0f, y = 0.0f;
    std::uintptr_t ret = 0;  // the caller's return address (0 = no call pending)
    DWORD          thread = 0;
};
DecalCall g_decal;
long g_decalFixed = 0;

std::uintptr_t __cdecl DecalPost(int result) {
    auto* view = reinterpret_cast<std::uint8_t*>(g_decal.view);
    *reinterpret_cast<float*>(view + addr::kViewX) = g_decal.x;
    *reinterpret_cast<float*>(view + addr::kViewY) = g_decal.y;
    if (result) {
        g_decal.mn[0] += g_decal.x;
        g_decal.mn[1] += g_decal.y;
        g_decal.mx[0] += g_decal.x;
        g_decal.mx[1] += g_decal.y;
        if (g_decalFixed++ == 0)
            MLOG("decals: a right-eye decal box kept (offset %.0f,%.0f) -- decals show in both eyes", g_decal.x, g_decal.y);
    }
    const std::uintptr_t ret = g_decal.ret;
    g_decal.ret = 0;
    return ret;
}

// Reached by the function's own RET 0x10 (its arguments are gone; EAX = its result).
__declspec(naked) void DecalPostStub() {
    __asm {
        push eax            // keep the result
        push eax            // DecalPost(result)
        call DecalPost
        add  esp, 4
        mov  ecx, eax       // the caller's return address (ECX is caller-saved)
        pop  eax
        jmp  ecx
    }
}

void OnDecalScreenBox(SafetyHookContext& ctx) {
    auto* sp = reinterpret_cast<std::uintptr_t*>(ctx.esp);  // [0] return, [1] ?, [2] view, [3] min, [4] max
    const std::uintptr_t view = sp[2];
    if (!view || !sp[3] || !sp[4] || g_decal.ret) return;  // not re-entrant (it never is)
    auto* v = reinterpret_cast<std::uint8_t*>(view);
    const float x = *reinterpret_cast<float*>(v + addr::kViewX), y = *reinterpret_cast<float*>(v + addr::kViewY);
    if (x == 0.0f && y == 0.0f) return;  // the left eye (or a mono view): the engine's math is right
    g_decal.view = view;
    g_decal.mn = reinterpret_cast<float*>(sp[3]);
    g_decal.mx = reinterpret_cast<float*>(sp[4]);
    g_decal.x = x;
    g_decal.y = y;
    g_decal.ret = sp[0];
    g_decal.thread = GetCurrentThreadId();
    *reinterpret_cast<float*>(v + addr::kViewX) = 0.0f;
    *reinterpret_cast<float*>(v + addr::kViewY) = 0.0f;
    sp[0] = reinterpret_cast<std::uintptr_t>(&DecalPostStub);
}

// --- the projection hooks -----------------------------------------------------------------------
void CommitBuilding() {
    g_committed = true;
    EnterCriticalSection(&g_lock);
    g_previous = g_current;
    g_current = g_building;
    g_current.thread = GetCurrentThreadId();
    g_current.valid = true;
    g_current.serial = ++g_commitSerial;
    g_current.paced = g_pace.paced;
    LeaveCriticalSection(&g_lock);
}

void CommitFrameIfComplete() {
    // Called after the projection of each view: mono completes on its only view, stereo on its last (eye 1, or the scope
    // view when it renders).
    if (g_thisStereo && g_thisIndex != g_viewCount - 1) return;
    // With stereo running, the only views that describe the presented image are the ones computed inside the stereo
    // Draw. CalcSceneView is also called outside Draw (FUN_10B224E0, one mono view per frame) -- those must not overwrite
    // the stereo record.
    if (!g_thisStereo && g_drawHook && g_cfg.stereo) return;
    CommitBuilding();
}

void OnProjection(SafetyHookContext& ctx) {
    if (!g_thisViewActive) return;
    shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !hdr->width || !hdr->height) return;
    auto* m = reinterpret_cast<float*>(ctx.eax);  // 4x4 row-major, row vectors (ENGINE-NOTES 5g)
    if (!m) return;

    if (g_thisScope) {
        // The scope view: a square, symmetric frustum of the scope's FOV (depth terms as the engine built them).
        const float t = g_scopeTanNow;
        m[0] = 1.0f / t;
        m[1] = 0.0f;
        m[4] = 0.0f;
        m[5] = 1.0f / t;
        m[8] = 0.0f;
        m[9] = 0.0f;
        EnterCriticalSection(&g_lock);
        g_building.scopeTan = t;
        g_building.scopeRect[0] = static_cast<std::uint32_t>(g_colX);
        g_building.scopeRect[1] = 0;
        g_building.scopeRect[2] = g_building.scopeRect[3] = static_cast<std::uint32_t>(g_colS);
        LeaveCriticalSection(&g_lock);
        static int logged = 0;
        if (logged++ == 0) MLOG("scope: the scope view's projection -- %.1f deg square, %d px", 2.0f * std::atan(t) * 180.0f / kPi, g_colS);
        CommitFrameIfComplete();
        return;
    }
    if (g_cfg.headsetProjection) {
        // Widen to this view's aspect (its share of the viewport in stereo) so its whole rect is used and
        // nothing inside the eye's FOV is cut: grow the short axis around its centre.
        shared::Fov f = g_thisFov;
        const float share = g_thisStereo ? (g_eyeW > 0 ? static_cast<float>(g_eyeW) / static_cast<float>(hdr->width) : 0.5f) : 1.0f;
        const float aspect = share * static_cast<float>(hdr->width) / static_cast<float>(hdr->height);
        const float w = f.tanRight - f.tanLeft, h = f.tanUp - f.tanDown;
        if (w < aspect * h) {
            const float cx = 0.5f * (f.tanRight + f.tanLeft), hw = 0.5f * aspect * h;
            f.tanLeft = cx - hw;
            f.tanRight = cx + hw;
        } else {
            const float cy = 0.5f * (f.tanUp + f.tanDown), hh = 0.5f * w / aspect;
            f.tanDown = cy - hh;
            f.tanUp = cy + hh;
        }
        // Asymmetric perspective; depth terms (m[10], m[11], m[14]) stay as the engine built them
        // (near 5.0, infinite far) so culling and depth precision are unchanged.
        m[0] = 2.0f / (f.tanRight - f.tanLeft);
        m[1] = 0.0f;
        m[4] = 0.0f;
        m[5] = 2.0f / (f.tanUp - f.tanDown);
        m[8] = -(f.tanRight + f.tanLeft) / (f.tanRight - f.tanLeft);
        m[9] = -(f.tanUp + f.tanDown) / (f.tanUp - f.tanDown);

        EnterCriticalSection(&g_lock);
        g_building.fov[g_thisEye] = f;
        if (!g_thisStereo) g_building.fov[1] = f;
        LeaveCriticalSection(&g_lock);

        {
            // Diagnostics: log whenever this eye's final FOV changes (by more than rounding).
            static shared::Fov seenF[2] = {};
            static int loggedF = 0;
            const shared::Fov& o = seenF[g_thisEye];
            if (loggedF < 40 && (std::fabs(o.tanLeft - f.tanLeft) > 1e-3f || std::fabs(o.tanRight - f.tanRight) > 1e-3f ||
                                 std::fabs(o.tanUp - f.tanUp) > 1e-3f || std::fabs(o.tanDown - f.tanDown) > 1e-3f)) {
                ++loggedF;
                MLOG("diag: %s eye %d FOV L%.3f R%.3f U%.3f D%.3f (aspect %.3f)", g_thisStereo ? "stereo" : "mono", g_thisEye,
                     f.tanLeft, f.tanRight, f.tanUp, f.tanDown, aspect);
                seenF[g_thisEye] = f;
            }
        }
        if (!g_loggedProj[g_thisEye]) {
            g_loggedProj[g_thisEye] = true;
            MLOG("projection (%s eye %d): FOV L%.3f R%.3f U%.3f D%.3f, widened to %.3f aspect -> L%.3f R%.3f U%.3f D%.3f",
                 g_thisStereo ? "stereo" : "mono", g_thisEye, g_thisFov.tanLeft, g_thisFov.tanRight, g_thisFov.tanUp,
                 g_thisFov.tanDown, aspect, f.tanLeft, f.tanRight, f.tanUp, f.tanDown);
        }
    }
    CommitFrameIfComplete();
}

bool CheckCall(std::uintptr_t va, std::uintptr_t target) {
    if (*reinterpret_cast<const std::uint8_t*>(va) != 0xE8) return false;
    const auto rel = *reinterpret_cast<const std::int32_t*>(va + 1);
    return va + 5 + static_cast<std::uintptr_t>(rel) == target;
}

bool Hook(SafetyHookMid& slot, std::uintptr_t va, safetyhook::MidHookFn fn, const char* what) {
    auto res = safetyhook::MidHook::create(reinterpret_cast<void*>(va), fn);
    if (!res) {
        MLOG("view: MidHook %s at 0x%08X failed (error %d) -- standing down", what, static_cast<unsigned>(va),
             static_cast<int>(res.error().type));
        return false;
    }
    slot = std::move(*res);
    MLOG("view: MidHook %s at 0x%08X installed", what, static_cast<unsigned>(va));
    return true;
}

}  // namespace

bool Install(const Config& cfg) {
    g_cfg = cfg;
    InitializeCriticalSection(&g_lock);
    if (!cfg.headTracking) {
        MLOG("view: Camera.HeadTracking=0 -- no view hooks");
        return false;
    }
    // Standing rule 4: the build check already verified every signature in addresses.hpp; check
    // the call targets once more right here, next to the code that depends on them.
    if (!CheckCall(addr::kViewPointMerge + 3, 0x10BEDFF0) || !CheckCall(addr::kProjCallNormal, addr::kPerspectiveMatrix) ||
        !CheckCall(addr::kProjCallConstrained, addr::kPerspectiveMatrix)) {
        MLOG("view: call targets in CalcSceneView don't match -- standing down");
        return false;
    }
    if (!Hook(g_viewHook, addr::kViewPointMerge, OnViewPoint, "view merge")) return false;
    // The projection hooks also close each frame's pose record, so they're always installed;
    // Camera.HeadsetProjection=0 just leaves the engine's matrix alone.
    Hook(g_projHookNormal, addr::kProjAfterNormal, OnProjection, "projection (normal)");
    Hook(g_projHookConstrained, addr::kProjAfterConstrained, OnProjection, "projection (constrained)");

    if (cfg.stereo && cfg.decalFix) Hook(g_decalHook, addr::kDecalScreenBox, OnDecalScreenBox, "decal screen box (right-eye decals)");
    if (cfg.stereo) {
        if (Hook(g_calcEntryHook, addr::kCalcSceneView, OnCalcSceneViewEntry, "CalcSceneView entry")) {
            auto res = safetyhook::InlineHook::create(reinterpret_cast<void*>(addr::kViewportClientDraw),
                                                      reinterpret_cast<void*>(&Hook_Draw));
            if (res) {
                g_drawHook = std::move(*res);
                MLOG("stereo: inline hook UGameViewportClient::Draw at 0x%08X installed", static_cast<unsigned>(addr::kViewportClientDraw));
                // Frame pacing waits in this hook (PaceBeforeDraw), whenever the host's hdr->pace is on.
                bridge::SetPacingAvailable(true);
                if (cfg.hudMode == 1 && Hook(g_hudMatrixHook, addr::kHudMatrixPush, OnHudMatrix, "HUD canvas matrix"))
                    Hook(g_hudHook, addr::kHudViewRead, OnHudView, "HUD canvas (per eye)");
                if (cfg.scopeColumn > 0) Hook(g_hudLoopHook, addr::kHudLoopStart, OnHudLoopStart, "HUD loop start (scopes)");
            } else {
                MLOG("stereo: inline hook on Draw failed (error %d) -- mono", static_cast<int>(res.error().type));
            }
        }
    }
    aim::Install(cfg);        // M7: needs the view hook (PoseToWorld)
    viewmodel::Install(cfg);  // M8: likewise (GameCamera, PoseFrameToWorld)
    throwing::Configure(cfg);
    const bool armsOk = armsik::Install(cfg);  // M8: the arms reach from the body to the gun
    offhand::Configure(cfg, armsOk);          // (the grenade drawn in the off hand needs the bake)
    offpistol::Configure(cfg, armsOk);       // (likewise the pistol)
    melee::Configure(cfg, static_cast<bool>(g_drawHook) && armsOk && !cfg.hideViewModel);  // (physical melee: the drawn gun)
    scope::Configure(cfg, static_cast<bool>(g_drawHook) && armsOk && !cfg.hideViewModel);  // (scopes: the drawn gun's tube)
    knife::Configure(cfg, armsOk && !cfg.hideViewModel);  // (the off-hand knife: the bake draws it)
    muzzle::Install(cfg);     // round 26: the flash and the brass at the drawn gun (needs the bake's move)
    // D21 manual reload: the game's own reload is only ever blocked with the Draw hook and the arm bake running.
    reload::Install(cfg, static_cast<bool>(g_drawHook) && armsOk && !cfg.hideViewModel);
    MLOG("throw: Hands.Throw=%d (x%.2f)", cfg.throwByHand, cfg.throwScale);
    return true;
}

bool LastEye0(float (&loc)[3], int (&rot)[3], float (&fov)[4]) {
    EnterCriticalSection(&g_lock);
    const shared::Fov f = g_current.fov[0];
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < 3; ++i) {
        loc[i] = g_eye0Loc[i];
        rot[i] = g_eye0Rot[i];
    }
    fov[0] = f.tanLeft;
    fov[1] = f.tanRight;
    fov[2] = f.tanUp;
    fov[3] = f.tanDown;
    return true;
}

int ScopeColumnX() { return g_scopeViewX.load(); }

bool MetaForPresentedFrame(shared::SlotMeta& meta, PresentedFrameInfo* info, shared::SlotScope* scope) {
    EnterCriticalSection(&g_lock);
    // With UE3's render thread the frame being presented was computed one game frame earlier -- unless its Draw was
    // paced (frame pacing): the next Draw waits for this Present before it commits, so the last one committed is this.
    const AppliedFrame& v =
        (g_current.valid && (g_current.thread == GetCurrentThreadId() || g_current.paced)) ? g_current : g_previous;
    if (info) *info = PresentedFrameInfo{v.qpc, v.serial, v.paced};
    const bool ok = v.valid && !v.cinema;
    if (ok) {
        for (int e = 0; e < 2; ++e) {
            meta.pose[e] = v.pose[e];
            meta.fov[e] = v.fov[e];
        }
        meta.stereo = v.stereo ? (g_cfg.debugSwapHalves ? 2u : 1u) : 0u;  // 2: halves swapped (experiment)
    }
    if (scope) {
        *scope = shared::SlotScope{};
        if (ok && v.stereo) {
            scope->eyeWidth = v.eyeWidth;
            for (int i = 0; i < 4; ++i) scope->rect[i] = v.scopeRect[i];
            scope->camera = v.scopeCam;
            scope->tanHalf = v.scopeTan;
            scope->gunPose = v.gunPose;
            scope->flags = v.gunValid ? 1u : 0u;
        }
    }
    LeaveCriticalSection(&g_lock);
    meta.hasView = ok ? 1u : 0u;
    return ok;
}

bool PoseToWorld(const shared::Pose& p, float (&pos)[3], float (&fwd)[3], float& unitsPerMeter) {
    float axes[3][3];
    if (!PoseFrameToWorld(p, pos, axes, unitsPerMeter)) return false;
    for (int i = 0; i < 3; ++i) fwd[i] = axes[0][i];
    return true;
}

bool HeadInWorld(float (&pos)[3], float& yaw, float& unitsPerMeter) {
    if (!g_world.valid) return false;
    float fwd[3];
    if (!PoseToWorld(g_world.head, pos, fwd, unitsPerMeter)) return false;
    yaw = g_world.yaw;
    return true;
}

bool VectorToWorld(const float (&xr)[3], float (&ue)[3]) {
    if (!g_world.valid) return false;
    const float s = UnitsPerMeter(bridge::SharedHeader());
    const Vec3 d = YawRotate(XrToUe(xr[0], xr[1], xr[2]), g_world.yaw);
    ue[0] = d.x * s;
    ue[1] = d.y * s;
    ue[2] = d.z * s;
    return true;
}

bool LandingHeld() { return g_landingHeld; }

bool GameCamera(float (&loc)[3], float& pitch, float& yaw) {
    if (!g_world.valid) return false;
    for (int i = 0; i < 3; ++i) loc[i] = g_world.base[i];
    pitch = g_world.pitch;
    yaw = g_world.yaw;
    return true;
}

bool PoseFrameToWorld(const shared::Pose& p, float (&pos)[3], float (&axes)[3][3], float& unitsPerMeter) {
    if (!g_world.valid) return false;
    const shared::Header* hdr = bridge::SharedHeader();
    unitsPerMeter = UnitsPerMeter(hdr);
    // The same translation reference as the eyes: the origin when position tracking has one, else the head.
    const bool origin = g_cfg.headPosition && g_haveOrigin;
    const float ox = origin ? g_ox : g_world.head.px, oy = origin ? g_oy : g_world.head.py,
                oz = origin ? g_oz : g_world.head.pz;
    const Vec3 d = YawRotate(XrToUe(p.px - ox, p.py - oy, p.pz - oz), g_world.yaw);
    const float h = hdr ? hdr->heightOffset : 0.0f;
    const float lift = (h > -1.0f && h < 1.0f) ? h * unitsPerMeter : 0.0f;
    pos[0] = g_world.base[0] + d.x * unitsPerMeter;
    pos[1] = g_world.base[1] + d.y * unitsPerMeter;
    pos[2] = g_world.base[2] + d.z * unitsPerMeter + lift;
    const Vec3 f = QuatRotate(p, 0.0f, 0.0f, -1.0f), r = QuatRotate(p, 1.0f, 0.0f, 0.0f), u = QuatRotate(p, 0.0f, 1.0f, 0.0f);
    const Vec3 A[3] = {YawRotate(XrToUe(f.x, f.y, f.z), g_world.yaw), YawRotate(XrToUe(r.x, r.y, r.z), g_world.yaw),
                       YawRotate(XrToUe(u.x, u.y, u.z), g_world.yaw)};
    for (int i = 0; i < 3; ++i) {
        axes[i][0] = A[i].x;
        axes[i][1] = A[i].y;
        axes[i][2] = A[i].z;
    }
    return true;
}

}  // namespace mohavr::view
