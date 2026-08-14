// Small shared helpers: logging, clocks, string conversion, HRESULT formatting.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace a2m {

// ---------------------------------------------------------------------------
// Logging (thread-safe, timestamped, stdout)
// ---------------------------------------------------------------------------
void LogLine(const char* level, const char* fmt, ...);

#define LOGI(...) ::a2m::LogLine("INFO", __VA_ARGS__)
#define LOGW(...) ::a2m::LogLine("WARN", __VA_ARGS__)
#define LOGE(...) ::a2m::LogLine("ERROR", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

// Unix epoch microseconds from the wall clock. This is what goes on the wire as
// pts_us (PROTOCOL.md) - M0-REPORT.md §2 lesson: sender timestamps must be wall
// clock based, never frame-counter derived.
uint64_t NowUnixMicros();

// QueryPerformanceCounter ticks, for measuring intervals inside this process.
int64_t QpcNow();
double QpcDeltaMs(int64_t fromTicks, int64_t toTicks);

// Sleeps until `targetQpc`, using a high-resolution waitable timer when available.
void SleepUntilQpc(int64_t targetQpc);

// ---------------------------------------------------------------------------
// Strings / errors
// ---------------------------------------------------------------------------
std::string WideToUtf8(const wchar_t* w);
std::wstring Utf8ToWide(const std::string& s);
std::string HrString(HRESULT hr);

}  // namespace a2m
