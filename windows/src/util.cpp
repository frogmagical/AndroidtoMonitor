#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace a2m {
namespace {

std::mutex g_logMutex;

LARGE_INTEGER QpcFrequency() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return freq;
}

}  // namespace

void LogLine(const char* level, const char* fmt, ...) {
    char body[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lock(g_logMutex);
    printf("[%02u:%02u:%02u.%03u] %-5s %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level,
           body);
    fflush(stdout);
}

uint64_t NowUnixMicros() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns units since 1601-01-01; 11644473600s to the Unix epoch.
    constexpr uint64_t kEpochDelta100ns = 116444736000000000ULL;
    return (u.QuadPart - kEpochDelta100ns) / 10ULL;
}

int64_t QpcNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double QpcDeltaMs(int64_t fromTicks, int64_t toTicks) {
    return static_cast<double>(toTicks - fromTicks) * 1000.0 /
           static_cast<double>(QpcFrequency().QuadPart);
}

void SleepUntilQpc(int64_t targetQpc) {
    static HANDLE timer = []() -> HANDLE {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!h) {
            h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        return h;
    }();

    for (;;) {
        const double remainMs = QpcDeltaMs(QpcNow(), targetQpc);
        if (remainMs <= 0.2) return;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(remainMs * 10000.0);  // relative, 100ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
        Sleep(static_cast<DWORD>(remainMs));
        return;
    }
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return {};
    std::wstring out(static_cast<size_t>(need) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), need);
    return out;
}

std::string HrString(HRESULT hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    std::string out(buf);

    LPWSTR msg = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n && msg) {
        std::string text = WideToUtf8(msg);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
        if (!text.empty()) out += " (" + text + ")";
    }
    if (msg) LocalFree(msg);
    return out;
}

}  // namespace a2m
