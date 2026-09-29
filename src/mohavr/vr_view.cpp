#include "vr_view.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>

#include "addresses.hpp"
#include "aim.hpp"
#include "arms_ik.hpp"
#include "bridge.hpp"
#include "throwing.hpp"
#include "viewmodel.hpp"
#include "game_exec.hpp"
#include "config.hpp"
#include "log.hpp"
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
void*        g_stereoPlayers[2] = {};
void*        g_leftViewState = nullptr;   // the player's own FSceneViewState
void*        g_rightViewState = nullptr;  // ours, for eye 1 (allocated once, lives for the process)

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
    static DWORD next = 0;
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

// Debug.GameCommands: console commands for scripted tests (e.g. "Suicide" for the death/reload test),
// one per line in %TEMP%\MOHAVR\game_cmd.txt, read and deleted twice a second on the game thread.
void RunTestCommands(const std::uintptr_t* players) {
    if (!g_cfg.debugGameCommands || !players || players[1] < 1 || !players[0]) return;
    static DWORD next = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - next) < 0) return;
    next = now + 500;
    static std::wstring path;
    if (path.empty()) {
        wchar_t tmp[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        path = std::wstring(tmp, n) + L"MOHAVR\\game_cmd.txt";
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[256];
    const auto player = *reinterpret_cast<const std::uintptr_t*>(players[0]);
    while (fgetws(line, 256, f)) {
        line[wcscspn(line, L"\r\n")] = 0;
        if (!line[0]) continue;
        const bool ok = gexec::Run(player, line);
        MLOG("test: game command '%ls' -> %s", line, ok ? "handled" : "not handled");
    }
    fclose(f);
    DeleteFileW(path.c_str());
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
    const bool ok = gexec::Run(player, cmd);
    MLOG("hands: game command '%ls' -> %s", cmd, ok ? "handled" : "not handled");
}

void __fastcall Hook_Draw(void* self, void* /*edx*/, void* viewport, void* canvas) {
    PaceBeforeDraw();
    const auto engine = *reinterpret_cast<std::uintptr_t*>(addr::kGEngine);
    auto* arr = engine ? reinterpret_cast<std::uintptr_t*>(engine + addr::kGamePlayersOffset) : nullptr;  // Data, Num, Max
    shared::Header* hdr = bridge::SharedHeader();
    ApplyWeaponCommands(arr);
    RunTestCommands(arr);
    RunHostCommand(arr, hdr);
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
    g_stereoPlayers[0] = g_stereoPlayers[1] = player;
    const std::uintptr_t savedData = arr[0];
    arr[0] = reinterpret_cast<std::uintptr_t>(g_stereoPlayers);
    arr[1] = 2;
    g_inStereoDraw = true;
    g_eyeCounter = 0;

    g_drawHook.thiscall<void>(self, viewport, canvas);

    g_inStereoDraw = false;
    arr[0] = savedData;
    arr[1] = 1;
    // Back to a full-screen player (and its own view state) for anything outside Draw.
    auto* lp = static_cast<std::uint8_t*>(player);
    if (g_leftViewState && *reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState) == g_rightViewState)
        *reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState) = g_leftViewState;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginX) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginY) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeX) = 1.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeY) = 1.0f;
    if (!g_loggedStereo) {
        g_loggedStereo = true;
        MLOG("stereo: first two-player Draw -- %d CalcSceneView calls", g_eyeCounter);
    }
    PaceAfterDraw();
}

// CalcSceneView entry: stack arg 1 = ULocalPlayer* (this). In a stereo Draw, the first call is
// the left eye, the second the right: give each half of the viewport.
void OnCalcSceneViewEntry(SafetyHookContext& ctx) {
    g_thisEye = 0;
    g_thisStereo = false;
    if (!g_inStereoDraw) return;
    auto* lp = *reinterpret_cast<std::uint8_t**>(ctx.esp + 4);
    if (!lp) return;
    // Debug.SwapEyeOrder: render the right eye first (experiment: which eye does per-frame-once work).
    const int eye = (g_eyeCounter++ & 1) ^ (g_cfg.debugSwapEyes ? 1 : 0);
    g_thisEye = eye;
    g_thisStereo = true;
    // Debug.SwapHalves (experiment): the left eye in the right half and vice versa; the host swaps back.
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginX) = (eye ^ (g_cfg.debugSwapHalves ? 1 : 0)) ? 0.5f : 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerOriginY) = 0.0f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeX) = 0.5f;
    *reinterpret_cast<float*>(lp + addr::kLocalPlayerSizeY) = 1.0f;

    // Each eye needs its own FSceneViewState (occlusion/visibility history). Sharing the player's
    // one made the right eye consume the left eye's occlusion results -> heavy right-eye flicker
    // (headset round 3). Eye 1 gets a second state from the engine's own AllocateViewState.
    auto* slot = reinterpret_cast<void**>(lp + addr::kLocalPlayerViewState);
    if (eye == 0) {
        if (*slot != g_rightViewState) g_leftViewState = *slot;  // follow the engine if it replaces its own
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
    static DWORD next = 0;
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

    const float gameYaw = UnrToRad(rot[1]);
    if (g_thisEye == 0 && g_viewIsPlayers) {
        // M7: the mapping this frame uses, for PoseToWorld (before the head moves `loc`).
        g_world = {true, {loc[0], loc[1], loc[2]}, gameYaw, head, UnrToRad(static_cast<std::int16_t>(rot[0] & 0xFFFF))};
        TrackCameraSway(loc, rot[1]);
    }
    // Stereo: this eye's own pose (orientation and position); mono: the head.
    const shared::Pose& p = g_thisStereo ? eye[g_thisEye] : head;

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
    if (g_thisEye == 0) g_building.qpc = viewQpc.QuadPart;
    g_building.pose[g_thisEye] = p;
    g_building.fov[g_thisEye] = g_thisFov;
    g_building.stereo = g_thisStereo;
    if (!g_thisStereo) {
        g_building.pose[1] = p;
        g_building.fov[1] = g_thisFov;
    }
    LeaveCriticalSection(&g_lock);
    g_thisViewActive = true;
    if (g_thisEye == 0 && g_viewIsPlayers && ctx.edi) {
        viewmodel::OnPlayerView();  // first: the aim follows the gun's barrel
        aim::OnPlayerView(*reinterpret_cast<std::uintptr_t*>(ctx.edi + addr::kLocalPlayerActor), g_world.base);
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
    const float eyeW = 0.5f * static_cast<float>(hdr->width), H = static_cast<float>(hdr->height);
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
void CommitFrameIfComplete() {
    // Called after the projection of each view: mono completes on its only view, stereo on eye 1.
    if (g_thisStereo && g_thisEye != 1) return;
    // With stereo running, the only views that describe the presented image are the two computed
    // inside the stereo Draw. CalcSceneView is also called outside Draw (FUN_10B224E0, one mono
    // view per frame) -- those must not overwrite the stereo record.
    if (!g_thisStereo && g_drawHook && g_cfg.stereo) return;
    EnterCriticalSection(&g_lock);
    g_previous = g_current;
    g_current = g_building;
    g_current.thread = GetCurrentThreadId();
    g_current.valid = true;
    g_current.serial = ++g_commitSerial;
    g_current.paced = g_pace.paced;
    LeaveCriticalSection(&g_lock);
}

void OnProjection(SafetyHookContext& ctx) {
    if (!g_thisViewActive) return;
    shared::Header* hdr = bridge::SharedHeader();
    if (!hdr || !hdr->width || !hdr->height) return;
    auto* m = reinterpret_cast<float*>(ctx.eax);  // 4x4 row-major, row vectors (ENGINE-NOTES 5g)
    if (!m) return;

    if (g_cfg.headsetProjection) {
        // Widen to this view's aspect (half the viewport in stereo) so its whole rect is used and
        // nothing inside the eye's FOV is cut: grow the short axis around its centre.
        shared::Fov f = g_thisFov;
        const float aspect = (g_thisStereo ? 0.5f : 1.0f) * static_cast<float>(hdr->width) / static_cast<float>(hdr->height);
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
            } else {
                MLOG("stereo: inline hook on Draw failed (error %d) -- mono", static_cast<int>(res.error().type));
            }
        }
    }
    aim::Install(cfg);        // M7: needs the view hook (PoseToWorld)
    viewmodel::Install(cfg);  // M8: likewise (GameCamera, PoseFrameToWorld)
    throwing::Configure(cfg);
    armsik::Install(cfg);     // M8: the arms reach from the body to the gun
    MLOG("throw: Hands.Throw=%d (x%.2f)", cfg.throwByHand, cfg.throwScale);
    return true;
}

bool MetaForPresentedFrame(shared::SlotMeta& meta, PresentedFrameInfo* info) {
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
