#include "crash_dump.hpp"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cwchar>

#include "config.hpp"
#include "log.hpp"

namespace mohavr::crashdump {
namespace {

std::atomic<bool> g_written{false};
bool              g_test = false;

// The DLL an address is in, and its offset ("?" outside every module).
bool ModuleOf(const void* a, char* name, size_t cap, DWORD& off) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(a), &m) ||
        !m)
        return false;
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = path;
    for (DWORD i = 0; i < n; ++i)
        if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    snprintf(name, cap, "%s", base);
    off = static_cast<DWORD>(reinterpret_cast<const char*>(a) - reinterpret_cast<const char*>(m));
    return true;
}

bool Watched(const char* mod) {
    return !_stricmp(mod, "ucrtbase.dll") || !_stricmp(mod, "d3d9on12.dll") || !_stricmp(mod, "d3d9.dll") ||
           !_stricmp(mod, "d3d12.dll") || !_stricmp(mod, "d3d12core.dll");
}

void WriteDump(EXCEPTION_POINTERS* ep) {
    using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    auto write = dbg ? reinterpret_cast<WriteFn>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    tmp[n] = 0;
    swprintf_s(path, L"%lsMOHAVR\\crash-%lu%ls.dmp", tmp, GetCurrentProcessId(), g_test ? L"-test" : L"");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!write || f == INVALID_HANDLE_VALUE) {
        MLOG("crashdump: could not write %ls (dbghelp %p)", path, static_cast<void*>(dbg));
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                                 MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo |
                                                 MiniDumpWithFullMemoryInfo);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    const DWORD size = GetFileSize(f, nullptr);
    CloseHandle(f);
    MLOG("crashdump: %s %ls (%lu KB)", ok ? "wrote" : "FAILED to write", path, size / 1024);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    char mod[MAX_PATH];
    DWORD off = 0;
    if (!ModuleOf(er->ExceptionAddress, mod, sizeof(mod), off) || !Watched(mod)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_written.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    MLOG("crashdump: access violation (%s 0x%08lX) in %s+0x%lX, thread %lu%s", er->ExceptionInformation[0] ? "writing" : "reading",
         static_cast<unsigned long>(er->ExceptionInformation[1]), mod, off, GetCurrentThreadId(),
         g_test ? " -- Debug.CrashDumpTest" : "");
    MLOG("crashdump: eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx, c->Ecx,
         c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // The stack scan: every dword that points into a module (return addresses among them), nearest first.
    const auto* sp = reinterpret_cast<const DWORD*>(c->Esp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 40; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        const DWORD v = sp[i];
        char m2[MAX_PATH];
        DWORD o2 = 0;
        if (v < 0x10000 || !ModuleOf(reinterpret_cast<const void*>(static_cast<uintptr_t>(v)), m2, sizeof(m2), o2)) continue;
        if (!_stricmp(m2, "ntdll.dll") || !_stricmp(m2, "kernel32.dll") || !_stricmp(m2, "KERNELBASE.dll")) continue;
        ++shown;
        MLOG("crashdump:   [esp+%03X] %08lX %s+0x%lX", i * 4, v, m2, o2);
    }
    WriteDump(ep);
    return EXCEPTION_CONTINUE_SEARCH;  // the game's own handling, as without the mod's handler
}

}  // namespace

void Install(const Config& cfg) {
    g_test = cfg.debugCrashDumpTest;
    if (!cfg.debugCrashDump) return;
    if (AddVectoredExceptionHandler(1, &Handler))
        MLOG("crashdump: Debug.CrashDump=1 -- a crash in d3d9/d3d9on12/ucrtbase writes %%TEMP%%\\MOHAVR\\crash-<pid>.dmp");
}

void OnDraw() {
    if (!g_test) return;
    static bool done = false;
    if (done) return;
    done = true;
    using MemcpyFn = void*(__cdecl*)(void*, const void*, size_t);
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto cpy = ucrt ? reinterpret_cast<MemcpyFn>(GetProcAddress(ucrt, "memcpy")) : nullptr;
    if (!cpy) {
        MLOG("crashdump: test -- ucrtbase's memcpy not found");
        return;
    }
    char dst[64];
    __try {
        cpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(0x10)), sizeof(dst));  // a caught read at 0x10
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MLOG("crashdump: test -- the access violation was caught; the game carries on");
    }
    g_written = false;  // a real crash later still gets its own dump
    g_test = false;
}

}  // namespace mohavr::crashdump
