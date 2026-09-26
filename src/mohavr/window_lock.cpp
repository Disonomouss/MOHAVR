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

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && lp) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(pos->flags & SWP_NOSIZE) && (pos->cx != g_w || pos->cy != g_h)) {
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
    g_orig = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc)));
    if (!g_orig) {
        MLOG("window: subclassing failed (%lu) -- the window size is not locked", GetLastError());
        g_calls = 30 * 20000 + 1;  // give up
        return;
    }
    g_hwnd = h;
    MLOG("window: size locked at %dx%d (client %ldx%ld = the backbuffer); moving it is still fine", g_w, g_h, c.right,
         c.bottom);
}

}  // namespace mohavr::winlock
