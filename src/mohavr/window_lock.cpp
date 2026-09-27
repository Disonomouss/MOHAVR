#include "window_lock.hpp"

#include <windows.h>

#include "log.hpp"

namespace mohavr::winlock {
namespace {

HWND    g_hwnd = nullptr;
WNDPROC g_orig = nullptr;
int     g_w = 0, g_h = 0;  // the pinned outer size
int     g_refused = 0;
unsigned g_calls = 0;

UINT g_applyStyleMsg = 0;  // registered when the lock is installed (not in DllMain)

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_applyStyleMsg && g_applyStyleMsg) {
        SetWindowLongW(h, GWL_STYLE, GetWindowLongW(h, GWL_STYLE) & ~WS_MAXIMIZEBOX);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        MLOG("window: maximize button removed");
        return 0;
    }
    // Maximizing can't be held at the render size -- Windows clamps a maximized window to the screen
    // whatever size is asked for (measured: asked 2896x1659, got 2576x1408; headset round 9). So there is
    // no maximizing: the maximize box is removed (Lock), SC_MAXIMIZE is swallowed, and a window that gets
    // maximized anyway (restored from the taskbar into a maximized state) is restored straight back.
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MAXIMIZE) {
        if (g_refused++ < 10) MLOG("window: refused to maximize (it would shrink the game's view)");
        return 0;
    }
    if (msg == WM_SIZE && wp == SIZE_MAXIMIZED) {
        if (g_refused++ < 10) MLOG("window: maximized anyway -- restoring it to the render size");
        PostMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0);
    }
    if (msg == WM_WINDOWPOSCHANGING && lp) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        // Minimizing is not a resize the game renders at: let Windows have it (forcing the full size onto a
        // minimized window, round 7, was left alone since). Only the move INTO the minimized state (the iconic size, ~160x28) is let through: restoring from
        // minimized also arrives while WS_MINIMIZE is set, and restoring into a maximized window resized the
        // game to the desktop (headset round 9: each eye 1280x1369 in a 2880x1620 frame -> magnified, with a
        // strip of the other eye).
        const bool minimized = (GetWindowLongW(h, GWL_STYLE) & WS_MINIMIZE) != 0;
        const bool toIconic = minimized && pos->cx <= GetSystemMetrics(SM_CXMINIMIZED) * 2 &&
                              pos->cy <= GetSystemMetrics(SM_CYMINIMIZED) * 2;
        if (!toIconic && !(pos->flags & SWP_NOSIZE) && (pos->cx != g_w || pos->cy != g_h)) {
            if (g_refused++ < 10) MLOG("window: refused a resize to %dx%d (kept %dx%d, the render size)", pos->cx, pos->cy, g_w, g_h);
            pos->cx = g_w;
            pos->cy = g_h;
        }
    }
    const LRESULT r = CallWindowProcW(g_orig, h, msg, wp, lp);
    if (msg == WM_GETMINMAXINFO && lp) {
        // Windows caps a window at about the desktop's size; the pinned size may be bigger.
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        if (mm->ptMaxTrackSize.x < g_w) mm->ptMaxTrackSize.x = g_w;
        if (mm->ptMaxTrackSize.y < g_h) mm->ptMaxTrackSize.y = g_h;
        if (mm->ptMaxSize.x < g_w) mm->ptMaxSize.x = g_w;
        if (mm->ptMaxSize.y < g_h) mm->ptMaxSize.y = g_h;
    }
    return r;
}

BOOL CALLBACK FindGame(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (lstrcmpW(cls, L"LaunchUnrealUWindowsClient") != 0) return TRUE;
    *reinterpret_cast<HWND*>(lp) = h;
    return FALSE;
}

}  // namespace

void Watch(unsigned bbW, unsigned bbH) {
    if (g_hwnd || (++g_calls % 30) != 0 || g_calls > 30 * 20000) return;
    HWND h = nullptr;
    EnumWindows(FindGame, reinterpret_cast<LPARAM>(&h));
    if (!h) return;
    RECT c{};
    if (!GetClientRect(h, &c) || static_cast<unsigned>(c.right) != bbW || static_cast<unsigned>(c.bottom) != bbH) {
        static bool logged = false;
        if (!logged && c.right) {
            logged = true;
            MLOG("window: game window client %ldx%ld, backbuffer %ux%u -- waiting until they match to lock the size",
                 c.right, c.bottom, bbW, bbH);
        }
        return;
    }
    RECT r{};
    GetWindowRect(h, &r);
    g_w = r.right - r.left;
    g_h = r.bottom - r.top;
    g_applyStyleMsg = RegisterWindowMessageW(L"MOHAVR.WindowLock.ApplyStyle");
    g_orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc)));
    if (!g_orig) {
        MLOG("window: subclassing failed (%lu) -- the window size is not locked", GetLastError());
        g_calls = 30 * 20000 + 1;  // give up
        return;
    }
    g_hwnd = h;
    // No maximize button (and so no title-bar double-click or Win+Up maximize either) -- applied on the window's
    // own thread: Watch runs on the render thread, and a style change sends messages to the game's main thread,
    // which may be waiting on the render thread (it deadlocked).
    if (g_applyStyleMsg) PostMessageW(h, g_applyStyleMsg, 0, 0);
    MLOG("window: size locked at %dx%d (client %ldx%ld = the backbuffer); moving it is still fine", g_w, g_h, c.right,
         c.bottom);
}

}  // namespace mohavr::winlock
