// D103: pointing at the game's menus. See pointer.hpp.
#include "pointer.hpp"

#include <windows.h>

#include <cwchar>

#include "addresses.hpp"
#include "config.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr::pointer {
namespace {

using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
GetCursorPosFn g_realGetCursorPos = nullptr;
bool g_installed = false;

// The pointed spot (client pixels) while it is held: the game's GetCursorPos answers it.
volatile LONG g_x = 0, g_y = 0;
volatile DWORD g_until = 0;  // GetTickCount() deadline; 0 = not pointing
HWND g_hwnd = nullptr;

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

HWND GameWindow() {
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        g_hwnd = nullptr;
        EnumWindows(FindGame, reinterpret_cast<LPARAM>(&g_hwnd));
    }
    return g_hwnd;
}

bool Pointing() {
    const DWORD until = g_until;
    return until && static_cast<LONG>(GetTickCount() - until) < 0;
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT p) {
    if (p && Pointing()) {
        HWND h = GameWindow();
        POINT c{g_x, g_y};
        if (h && ClientToScreen(h, &c)) {
            *p = c;
            return TRUE;
        }
    }
    return g_realGetCursorPos ? g_realGetCursorPos(p) : FALSE;
}

void Move(LONG x, LONG y, DWORD holdMs) {
    const bool moved = x != g_x || y != g_y || !Pointing();
    g_x = x;
    g_y = y;
    g_until = (GetTickCount() + holdMs) | 1u;
    if (moved)
        if (HWND h = GameWindow()) PostMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
}

void Button(bool down) {
    HWND h = GameWindow();
    if (!h) return;
    PostMessageW(h, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0,
                 MAKELPARAM(static_cast<WORD>(g_x), static_cast<WORD>(g_y)));
}

DWORD g_upAt = 0;  // a test click's button-up

}  // namespace

bool Install(const Config& cfg) {
    if (!cfg.pointerMenus) return false;
    void* real = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetCursorPos"));
    auto* slot = reinterpret_cast<void**>(addr::kIatGetCursorPos);
    if (!real || !patch::BytesMatch(addr::kIatGetCursorPos, reinterpret_cast<const std::uint8_t*>(&real), sizeof(real))) {
        MLOG("pointer: the game's GetCursorPos slot (0x%08X) doesn't hold user32's -- no menu pointer",
             static_cast<unsigned>(addr::kIatGetCursorPos));
        return false;
    }
    g_realGetCursorPos = reinterpret_cast<GetCursorPosFn>(real);
    if (!patch::SwapPointer(addr::kIatGetCursorPos, real, reinterpret_cast<void*>(&Hook_GetCursorPos))) {
        MLOG("pointer: the GetCursorPos swap failed -- no menu pointer");
        return false;
    }
    g_installed = true;
    (void)slot;
    MLOG("pointer: the game's GetCursorPos answered while a controller points at the flat screen ([Controls] PointerMenus)");
    return true;
}

void OnDraw(shared::Header* hdr) {
    if (!g_installed) return;
    if (g_upAt && static_cast<LONG>(GetTickCount() - g_upAt) >= 0) {
        g_upAt = 0;
        Button(false);
    }
    // The host's pointer: the cursor at the pointed spot (held 300 ms past the last frame that hit), the trigger's edges
    // as the left button.
    static bool down = false, pointing = false;
    const bool ok = hdr && hdr->pointerOk && hdr->width && hdr->height;
    if (ok) {
        const float u = hdr->pointerU, v = hdr->pointerV;
        const LONG x = static_cast<LONG>(u * static_cast<float>(hdr->width)), y = static_cast<LONG>(v * static_cast<float>(hdr->height));
        Move(x, y, 300);
        if (!pointing) {
            pointing = true;
            static int logged = 0;
            if (logged++ < 20) MLOG("pointer: pointing at the menu (%ld %ld)", x, y);
        }
        const bool want = hdr->pointerDown != 0;
        if (want != down) {
            down = want;
            Button(want);
            static int clicks = 0;
            if (want && clicks++ < 40) MLOG("pointer: click at %ld %ld", x, y);
        }
    } else {
        if (down) Button(false);
        down = false;
        pointing = false;
    }
}

bool TestCommand(const wchar_t* line) {
    if (!g_installed) return false;
    int x = 0, y = 0;
    if (swscanf_s(line, L"mohavr cursor %d %d", &x, &y) == 2) {
        Move(x, y, 15000);
        MLOG("pointer: test -- the cursor held at %d %d (client)", x, y);
        return true;
    }
    if (!std::wcscmp(line, L"mohavr click")) {
        Button(true);
        g_upAt = (GetTickCount() + 80) | 1u;
        MLOG("pointer: test -- a click at %ld %ld", g_x, g_y);
        return true;
    }
    return false;
}

}  // namespace mohavr::pointer
