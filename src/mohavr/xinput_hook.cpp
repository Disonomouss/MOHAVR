#include "xinput_hook.hpp"

#include <windows.h>

#include <cstdint>

#include "../common/shared_frame.hpp"
#include "addresses.hpp"
#include "bridge.hpp"
#include "loadout.hpp"
#include "log.hpp"
#include "patch.hpp"

namespace mohavr::xinput {
namespace {

// XINPUT_STATE without pulling in xinput.h (the game links the DLL by ordinal; so do we, through it).
struct XState {
    DWORD              packet;
    shared::PadState   pad;
};
static_assert(sizeof(XState) == 16, "XINPUT_STATE is 16 bytes");

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XState*);
PFN_XInputGetState g_real = nullptr;
bool               g_loggedFirst = false;

DWORD WINAPI Hook_XInputGetState(DWORD index, XState* state) {
    if (index == 0 && state) {
        const shared::Header* h = bridge::SharedHeader();
        if (h && h->hostState == static_cast<std::uint32_t>(shared::HostState::Running)) {
            shared::PadState p{};
            std::uint32_t seq = 0;
            // Mid-write (rare): try again once, then fall through to the last state the game saw.
            if (shared::ReadPad(h, p, seq) || shared::ReadPad(h, p, seq)) {
                state->packet = seq;
                state->pad = p;
                loadout::FilterPad(state->pad);  // D79
                if (!g_loggedFirst) {
                    g_loggedFirst = true;
                    MLOG("xinput: pad 0 now driven by the host (first poll, packet %u)", seq);
                }
                return ERROR_SUCCESS;
            }
        }
    }
    const DWORD r = g_real(index, state);
    if (r == ERROR_SUCCESS && index == 0 && state) loadout::FilterPad(state->pad);  // D79 (a real pad)
    return r;
}

}  // namespace

bool Install() {
    auto* slot = reinterpret_cast<void**>(addr::kIatXInputGetState);
    void* current = *slot;
    // Verify-before-patch (standing rule 4): the slot must point into a loaded XInput DLL, at exactly
    // that DLL's ordinal 2 (XInputGetState).
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(current), &mod)) {
        MLOG("xinput: IAT slot 0x%08X holds %p, not inside any module -- standing down", addr::kIatXInputGetState, current);
        return false;
    }
    wchar_t name[MAX_PATH] = L"";
    GetModuleFileNameW(mod, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    base = base ? base + 1 : name;
    void* real = reinterpret_cast<void*>(GetProcAddress(mod, MAKEINTRESOURCEA(2)));
    if (_wcsnicmp(base, L"xinput", 6) != 0 || real != current) {
        MLOG("xinput: IAT slot 0x%08X holds %p (%ls), expected that module's ordinal 2 %p -- standing down",
             addr::kIatXInputGetState, current, base, real);
        return false;
    }
    g_real = reinterpret_cast<PFN_XInputGetState>(real);
    if (!patch::SwapPointer(addr::kIatXInputGetState, real, reinterpret_cast<void*>(&Hook_XInputGetState))) {
        MLOG("xinput: IAT swap failed -- standing down");
        g_real = nullptr;
        return false;
    }
    MLOG("xinput: hooked XInputGetState (%ls ordinal 2) -- pad 0 follows the host while it drives it", base);
    return true;
}

}  // namespace mohavr::xinput
