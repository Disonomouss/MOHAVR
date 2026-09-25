#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace mohavr::log {
namespace {

HANDLE           g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
LARGE_INTEGER    g_t0{};
LARGE_INTEGER    g_freq{};

}  // namespace

void Open(const std::wstring& dir, const std::wstring& name) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    const std::wstring path = dir + L"\\" + name + L".log";
    const std::wstring prev = dir + L"\\" + name + L".prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}

double MsSinceStart() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return 1000.0 * static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

void Line(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    const int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%9.1f ms] [t%05lu] %s\r\n",
                           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, MsSinceStart(),
                           GetCurrentThreadId(), msg);
    if (n <= 0) return;

    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1), &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

}  // namespace mohavr::log
