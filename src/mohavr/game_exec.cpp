#include "game_exec.hpp"

#include <windows.h>

#include "addresses.hpp"
#include "log.hpp"

namespace mohavr::gexec {
namespace {

// Stands in for the FOutputDevice& the game's Exec wants: FOutputDevice::Logf formats and calls
// vtable slot 1, Serialize(const TCHAR*, EName) -- an MSVC class with a virtual destructor first has
// the same layout and calling convention. Whatever the command prints goes to MOHAVR.log (capped).
class LogDevice {
public:
    virtual ~LogDevice() = default;
    virtual void Serialize(const wchar_t* text, int /*event*/) {
        if (lines_ < 20 && text) {
            ++lines_;
            MLOG("game exec output: %ls", text);
        }
    }
    virtual void Flush() {}
    virtual void TearDown() {}

private:
    int bSuppressEventTag_ = 0;         // FOutputDevice's own members, in case anything reads them
    int bAutoEmitLineTerminator_ = 1;
    int lines_ = 0;
};

LogDevice g_log;

}  // namespace

bool Run(std::uintptr_t localPlayer, const wchar_t* cmd) {
    if (!localPlayer || !cmd) return false;
    void* fexec = reinterpret_cast<void*>(localPlayer + addr::kLocalPlayerFExec);
    auto* vtbl = *reinterpret_cast<std::uintptr_t**>(fexec);
    if (!vtbl || vtbl[0] != addr::kLocalPlayerExec) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            MLOG("game exec: LocalPlayer FExec vtable slot 0 is 0x%08X, expected 0x%08X -- not calling",
                 vtbl ? static_cast<unsigned>(vtbl[0]) : 0u, static_cast<unsigned>(addr::kLocalPlayerExec));
        }
        return false;
    }
    using ExecFn = int(__thiscall*)(void* self, const wchar_t* cmd, void* ar);
    return reinterpret_cast<ExecFn>(vtbl[0])(fexec, cmd, &g_log) != 0;
}

}  // namespace mohavr::gexec
