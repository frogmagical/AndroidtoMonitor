// a2m-sender - AndroidtoMonitor M2 Windows sender MVP.
//
//   DXGI Desktop Duplication (virtual display)
//     -> D3D11 VideoProcessor BGRA->NV12 (GPU, no CPU copy)
//     -> Media Foundation H.264 encoder MFT (HW preferred, low latency, CBR, no B-frames)
//     -> TCP to localhost:<port>, forwarded to the phone by `adb forward`
//
// See docs/REQUIREMENTS.md §4 and docs/PROTOCOL.md.

#include <winsock2.h>
#include <windows.h>
#include <mfapi.h>
#include <objbase.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "protocol.h"
#include "sender.h"
#include "stats.h"
#include "util.h"

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            a2m::LogLine("INFO", "shutdown requested");
            g_running.store(false);
            return TRUE;
        default:
            return FALSE;
    }
}

struct Options {
    int port = 5001;
    uint32_t bitrate = 12000000;
    int fps = 30;
    std::string serial = "1b2f0fc";
    std::string adbPath = R"(C:\Users\daiki\tools\platform-tools\adb.exe)";
    int width = 1080;
    int height = 2400;
    double durationSec = 0.0;  // 0 = until Ctrl+C
    bool skipAdb = false;
    bool drawCursor = true;
    // CBR per REQUIREMENTS §4.2. Measured on the NVIDIA MFT the mode is inert (CBR
    // and peak-constrained VBR produce byte-identical output), so the spec value
    // stays the default and --rc exists only to re-test on other drivers/GPUs.
    a2m::RateControlMode rc = a2m::RateControlMode::Cbr;
};

bool ParseBitrate(const std::string& s, uint32_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double value = strtod(s.c_str(), &end);
    if (end == s.c_str() || value <= 0) return false;
    double scale = 1.0;
    if (*end == 'k' || *end == 'K') scale = 1e3;
    else if (*end == 'm' || *end == 'M') scale = 1e6;
    else if (*end != '\0') return false;
    *out = static_cast<uint32_t>(value * scale);
    return true;
}

void PrintUsage() {
    printf(
        "a2m-sender - AndroidtoMonitor M2 Windows sender\n"
        "\n"
        "  --port <n>        TCP port (default 5001)\n"
        "  --bitrate <b>     target bitrate, accepts 12M / 12000k / 12000000 (default 12M)\n"
        "  --fps <n>         capture/encode frame rate (default 30)\n"
        "  --serial <s>      adb device serial (default 1b2f0fc)\n"
        "  --adb <path>      adb.exe path (default C:\\Users\\daiki\\tools\\platform-tools\\adb.exe)\n"
        "  --width <n>       virtual display width to look for (default 1080)\n"
        "  --height <n>      virtual display height to look for (default 2400)\n"
        "  --duration <sec>  stop after N seconds (default 0 = run until Ctrl+C)\n"
        "  --rc <mode>       rate control: cbr (default) | vbr (peak-constrained)\n"
        "  --no-cursor       do not composite the mouse cursor\n"
        "  --no-adb          skip the `adb forward` step\n"
        "  --list-outputs    print all DXGI outputs and exit\n"
        "  --help\n");
}

bool ParseArgs(int argc, char** argv, Options* opt, bool* listOnly) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return false;
        } else if (arg == "--list-outputs") {
            *listOnly = true;
        } else if (arg == "--no-adb") {
            opt->skipAdb = true;
        } else if (arg == "--no-cursor") {
            opt->drawCursor = false;
        } else if (arg == "--rc") {
            const char* v = next("--rc");
            if (!v) return false;
            const std::string mode = v;
            if (mode == "cbr") {
                opt->rc = a2m::RateControlMode::Cbr;
            } else if (mode == "vbr") {
                opt->rc = a2m::RateControlMode::PeakConstrainedVbr;
            } else {
                fprintf(stderr, "bad --rc value '%s' (expected cbr or vbr)\n", v);
                return false;
            }
        } else if (arg == "--port") {
            const char* v = next("--port");
            if (!v) return false;
            opt->port = atoi(v);
        } else if (arg == "--bitrate") {
            const char* v = next("--bitrate");
            if (!v) return false;
            if (!ParseBitrate(v, &opt->bitrate)) {
                fprintf(stderr, "bad --bitrate value '%s'\n", v);
                return false;
            }
        } else if (arg == "--fps") {
            const char* v = next("--fps");
            if (!v) return false;
            opt->fps = atoi(v);
        } else if (arg == "--serial") {
            const char* v = next("--serial");
            if (!v) return false;
            opt->serial = v;
        } else if (arg == "--adb") {
            const char* v = next("--adb");
            if (!v) return false;
            opt->adbPath = v;
        } else if (arg == "--width") {
            const char* v = next("--width");
            if (!v) return false;
            opt->width = atoi(v);
        } else if (arg == "--height") {
            const char* v = next("--height");
            if (!v) return false;
            opt->height = atoi(v);
        } else if (arg == "--duration") {
            const char* v = next("--duration");
            if (!v) return false;
            opt->durationSec = atof(v);
        } else {
            fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
            PrintUsage();
            return false;
        }
    }
    if (opt->fps <= 0 || opt->fps > 240) {
        fprintf(stderr, "--fps out of range\n");
        return false;
    }
    return true;
}

// Runs a child process, capturing stdout+stderr. Returns the exit code, or -1.
int RunProcess(const std::wstring& commandLine, std::string* output) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = commandLine;
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return -1;
    }

    char buf[512];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = '\0';
        if (output) *output += buf;
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

// Without this, DXGI reports DPI-scaled desktop rectangles (a 1080x2400 virtual
// display at 125% scaling looks like 864x1920) and output matching fails.
// Loaded dynamically so the binary still runs on builds without the API.
void MakeProcessDpiAware() {
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setCtx =
            reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// REQUIREMENTS §4.1: the phone listens, the PC connects through `adb forward`.
bool SetupAdbForward(const Options& opt) {
    wchar_t cmd[1024];
    const std::wstring adbW = a2m::Utf8ToWide(opt.adbPath);
    const std::wstring serialW = a2m::Utf8ToWide(opt.serial);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%s\" -s %s forward tcp:%d tcp:%d", adbW.c_str(),
             serialW.c_str(), opt.port, opt.port);

    std::string output;
    const int rc = RunProcess(cmd, &output);
    if (rc != 0) {
        LOGE("adb forward failed (exit=%d): %s", rc, Trim(output).c_str());
        LOGE("check: adb at '%s', device '%s' connected and authorised (REQUIREMENTS §8)",
             opt.adbPath.c_str(), opt.serial.c_str());
        return false;
    }
    LOGI("adb forward tcp:%d tcp:%d (serial=%s) ok", opt.port, opt.port, opt.serial.c_str());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    bool listOnly = false;
    if (!ParseArgs(argc, argv, &opt, &listOnly)) return 1;

    MakeProcessDpiAware();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOGE("CoInitializeEx failed: %s", a2m::HrString(hr).c_str());
        return 1;
    }

    if (listOnly) {
        for (const auto& o : a2m::DesktopCapture::EnumerateOutputs()) {
            printf("adapter[%d] output[%d] %s on '%s'  %dx%d  attached=%s\n", o.adapterIndex,
                   o.outputIndex, o.deviceName.c_str(), o.adapterName.c_str(), o.width, o.height,
                   o.attached ? "yes" : "no");
        }
        CoUninitialize();
        return 0;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("WSAStartup failed");
        CoUninitialize();
        return 1;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        LOGE("MFStartup failed: %s", a2m::HrString(hr).c_str());
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    {
        LOGI("a2m-sender starting: %dx%d @%dfps, %.1f Mbps, port %d", opt.width, opt.height,
             opt.fps, opt.bitrate / 1e6, opt.port);

        if (!opt.skipAdb && !SetupAdbForward(opt)) {
            exitCode = 1;
            goto cleanup;
        }

        {
            a2m::DesktopCapture capture;
            if (!capture.Initialize(opt.width, opt.height, opt.drawCursor)) {
                exitCode = 1;
                goto cleanup;
            }

            a2m::Stats stats;
            a2m::Encoder encoder;
            if (!encoder.Initialize(capture.Device(), opt.width, opt.height, opt.fps, opt.bitrate,
                                    opt.rc)) {
                LOGE("encoder initialization failed");
                exitCode = 1;
                goto cleanup;
            }

            a2m::Sender sender(&stats);
            encoder.SetFrameCallback(
                [&sender](a2m::EncodedFrame&& f) { sender.Enqueue(std::move(f)); });
            sender.Start(opt.port, opt.width, opt.height, opt.fps,
                         [&encoder] { encoder.ForceKeyFrame(); });

            LARGE_INTEGER freqLi;
            QueryPerformanceFrequency(&freqLi);
            const int64_t qpcFreq = freqLi.QuadPart;
            const int64_t frameTicks = qpcFreq / opt.fps;

            const int64_t startQpc = a2m::QpcNow();
            int64_t nextFrameQpc = startQpc;
            int64_t nextStatsQpc = startQpc + qpcFreq;
            uint64_t submitted = 0;
            uint64_t resent = 0;          // frames re-sent because the screen did not change
            uint64_t resentInterval = 0;
            uint64_t encoderDropsPrev = 0;
            bool freshSinceSubmit = false;
            bool warnedNoFrame = false;

            // AcquireNextFrame blocks inside the D3D11 device lock, and the encoder
            // MFT shares that device (multithread protection is mandatory for it),
            // so a long DDA wait starves the encoder. Measured: a ~16ms timeout on a
            // static screen dropped the encoder to ~19fps with ~63ms capture->encode
            // latency. We therefore poll DDA with a short timeout and do the rest of
            // the frame pacing on a high-resolution timer outside the lock.
            constexpr int kDdaTimeoutMs = 2;

            while (g_running.load()) {
                // A pointer-only update counts as a frame change: otherwise the
                // cursor would appear frozen whenever the desktop itself is static.
                if (capture.PumpFrame(kDdaTimeoutMs).Any()) freshSinceSubmit = true;

                int64_t afterPump = a2m::QpcNow();
                if (capture.HasFrame() && afterPump < nextFrameQpc) {
                    // Sleep out the rest of the frame interval without holding the
                    // device lock, waking early enough to keep polling for changes.
                    const int64_t pollTicks = qpcFreq / 250;  // ~4ms
                    const int64_t wakeAt = (nextFrameQpc < afterPump + pollTicks)
                                               ? nextFrameQpc
                                               : afterPump + pollTicks;
                    a2m::SleepUntilQpc(wakeAt);
                    afterPump = a2m::QpcNow();
                }

                if (!capture.HasFrame()) {
                    if (!warnedNoFrame && a2m::QpcDeltaMs(startQpc, afterPump) > 3000.0) {
                        warnedNoFrame = true;
                        LOGW("no desktop frame yet - the virtual display may be completely "
                             "static; DDA only delivers frames on change");
                    }
                    continue;
                }
                if (afterPump < nextFrameQpc) continue;

                // Pace at the configured fps; after a long stall resync instead of
                // bursting a backlog of frames at the encoder.
                nextFrameQpc += frameTicks;
                if (nextFrameQpc < afterPump - frameTicks) nextFrameQpc = afterPump + frameTicks;

                if (!freshSinceSubmit) {
                    ++resent;
                    ++resentInterval;
                }
                freshSinceSubmit = false;

                const int64_t captureQpc = afterPump;
                ID3D11Texture2D* nv12 = capture.ConvertLatestToNv12();
                if (nv12) {
                    // Sample time is wall-clock elapsed, never a frame counter
                    // (M0-REPORT.md §2).
                    const int64_t sampleTime100ns =
                        static_cast<int64_t>(a2m::QpcDeltaMs(startQpc, captureQpc) * 10000.0);
                    encoder.SubmitFrame(nv12, sampleTime100ns, captureQpc);
                    ++submitted;
                }

                if (a2m::QpcNow() >= nextStatsQpc) {
                    nextStatsQpc += qpcFreq;
                    const a2m::StatsSnapshot s = stats.FlushInterval();
                    const uint64_t encDropsNow = encoder.EncoderDrops();
                    const uint64_t encDropsInterval = encDropsNow - encoderDropsPrev;
                    encoderDropsPrev = encDropsNow;
                    const a2m::CursorStats& cs = capture.Cursor();
                    LOGI("fps=%d idr=%d cap2enc_p50=%.1fms/p95=%.1fms "
                         "enc2sent_p50=%.1fms/p95=%.1fms bytes=%llu (%.1f Mbps) "
                         "drops=%llu(send)/%llu(enc) reconn=%llu resent=%llu cursor=%s conn=%s",
                         s.framesSent, s.idrSent, s.captureToEncodeP50Ms, s.captureToEncodeP95Ms,
                         s.encodeToSentP50Ms, s.encodeToSentP95Ms,
                         static_cast<unsigned long long>(s.bytesSent), s.bytesSent * 8.0 / 1e6,
                         static_cast<unsigned long long>(s.drops),
                         static_cast<unsigned long long>(encDropsInterval),
                         static_cast<unsigned long long>(s.reconnects),
                         static_cast<unsigned long long>(resentInterval),
                         cs.visible ? "on" : "off", sender.IsConnected() ? "up" : "down");
                    resentInterval = 0;
                }

                if (opt.durationSec > 0 &&
                    a2m::QpcDeltaMs(startQpc, a2m::QpcNow()) >= opt.durationSec * 1000.0) {
                    LOGI("duration reached, stopping");
                    break;
                }
            }

            LOGI("stopping...");
            sender.Stop();
            encoder.Shutdown();

            const a2m::CursorStats& cs = capture.Cursor();
            LOGI("final: encoder='%s' (%s) submitted=%llu resent_static=%llu sent=%llu "
                 "bytes=%llu drops=%llu encoder_drops=%llu reconnects=%llu "
                 "cursor_shapes=%llu(mono=%llu color=%llu masked=%llu inverting=%llu) "
                 "cursor_unsupported=%llu",
                 encoder.EncoderName().c_str(), encoder.IsHardware() ? "HW" : "SW",
                 static_cast<unsigned long long>(submitted),
                 static_cast<unsigned long long>(resent),
                 static_cast<unsigned long long>(stats.TotalFrames()),
                 static_cast<unsigned long long>(stats.TotalBytes()),
                 static_cast<unsigned long long>(stats.TotalDrops()),
                 static_cast<unsigned long long>(encoder.EncoderDrops()),
                 static_cast<unsigned long long>(stats.TotalReconnects()),
                 static_cast<unsigned long long>(cs.shapeUpdates),
                 static_cast<unsigned long long>(cs.monochromeShapes),
                 static_cast<unsigned long long>(cs.colorShapes),
                 static_cast<unsigned long long>(cs.maskedColorShapes),
                 static_cast<unsigned long long>(cs.invertingShapes),
                 static_cast<unsigned long long>(cs.unsupportedShapes));
        }
    }

cleanup:
    MFShutdown();
    WSACleanup();
    CoUninitialize();
    return exitCode;
}
