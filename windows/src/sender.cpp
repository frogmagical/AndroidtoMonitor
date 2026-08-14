#include "sender.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>

#include "protocol.h"
#include "util.h"

namespace a2m {

Sender::~Sender() { Stop(); }

void Sender::Start(int port, int width, int height, int fps, std::function<void()> onNeedIdr) {
    port_ = port;
    width_ = width;
    height_ = height;
    fps_ = fps;
    onNeedIdr_ = std::move(onNeedIdr);
    running_.store(true);
    thread_ = std::thread(&Sender::ThreadMain, this);
}

void Sender::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    CloseSocket();
}

void Sender::Enqueue(EncodedFrame&& frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (frame.isIdr) {
        // A fresh IDR makes everything queued behind it obsolete; dropping the
        // backlog lets the receiver resync on a clean boundary (matches
        // tools/m1/send_test.py).
        if (!queue_.empty()) {
            const uint64_t n = queue_.size();
            queue_.clear();
            lock.unlock();
            stats_->RecordDrop(n);
            lock.lock();
        }
    } else if (queue_.size() >= kMaxQueue) {
        // Backlog: throw away the oldest non-IDR frame (latency over quality).
        queue_.pop_front();
        lock.unlock();
        stats_->RecordDrop(1);
        lock.lock();
    }
    queue_.push_back(std::move(frame));
    lock.unlock();
    cv_.notify_one();
}

void Sender::ClearQueue() {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t n = queue_.size();
    queue_.clear();
    lock.unlock();
    if (n) stats_->RecordDrop(n);
}

void Sender::CloseSocket() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    connected_.store(false);
}

bool Sender::TryConnect() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               sizeof(nodelay));
    DWORD sendTimeout = 3000;  // ms - never block shutdown on a wedged socket
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout),
               sizeof(sendTimeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port_));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    socket_ = s;
    connected_.store(true);
    return true;
}

bool Sender::SendAll(const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int n = ::send(socket_, reinterpret_cast<const char*>(data + sent),
                             static_cast<int>(std::min<size_t>(size - sent, 1 << 20)), 0);
        if (n == SOCKET_ERROR || n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool Sender::SendMessage(uint8_t type, uint16_t flags, const uint8_t* payload, size_t payloadLen) {
    uint8_t header[kHeaderSize];
    // pts_us is stamped here, at send time, from the wall clock
    // (GetSystemTimePreciseAsFileTime) - M0-REPORT.md §2.
    WriteHeader(header, type, flags, seq_++, NowUnixMicros(), static_cast<uint32_t>(payloadLen));
    if (!SendAll(header, kHeaderSize)) return false;
    if (payloadLen > 0 && !SendAll(payload, payloadLen)) return false;
    return true;
}

bool Sender::SendHandshake() {
    const std::string json = BuildHandshakeJson(width_, height_, fps_);
    if (!SendMessage(kTypeHandshake, 0, reinterpret_cast<const uint8_t*>(json.data()),
                     json.size())) {
        return false;
    }
    LOGI("handshake sent: %s", json.c_str());
    return true;
}

void Sender::ThreadMain() {
    // `adb forward` keeps its local listener open even when nothing is listening
    // on the phone, so connect() succeeds and the failure only surfaces on the
    // first write. Backoff therefore has to key off short-lived sessions, not just
    // failed connects, otherwise a dead receiver produces a 1 Hz connect storm.
    constexpr int kMinBackoffMs = 100;
    constexpr int kMaxBackoffMs = 5000;
    constexpr double kHealthySessionMs = 3000.0;

    int backoffMs = 0;
    int64_t connectQpc = 0;
    int64_t nextHeartbeatQpc = QpcNow();

    // Marks the current connection as dead and schedules the next attempt.
    auto dropConnection = [&](const char* reason) {
        const double sessionMs = QpcDeltaMs(connectQpc, QpcNow());
        CloseSocket();
        if (sessionMs < kHealthySessionMs) {
            backoffMs = backoffMs ? std::min(backoffMs * 2, kMaxBackoffMs) : kMinBackoffMs;
            LOGW("%s after %.0f ms, retry in %d ms", reason, sessionMs, backoffMs);
        } else {
            backoffMs = 0;
            LOGW("%s after %.1f s", reason, sessionMs / 1000.0);
        }
    };

    while (running_.load()) {
        if (!connected_.load()) {
            // Sleep in slices so Stop() stays responsive.
            for (int slept = 0; slept < backoffMs && running_.load(); slept += 50) Sleep(50);
            if (!running_.load()) break;

            if (!TryConnect()) {
                backoffMs = backoffMs ? std::min(backoffMs * 2, kMaxBackoffMs) : kMinBackoffMs;
                LOGW("connect to 127.0.0.1:%d failed (is the Android app running?), retry in %d ms",
                     port_, backoffMs);
                continue;
            }
            connectQpc = QpcNow();
            if (everConnected_) {
                stats_->RecordReconnect();
                LOGI("reconnected to 127.0.0.1:%d", port_);
            } else {
                LOGI("connected to 127.0.0.1:%d", port_);
            }
            everConnected_ = true;

            ClearQueue();
            if (!SendHandshake()) {
                dropConnection("handshake failed");
                continue;
            }
            // Reconnect => receiver reset its decoder; it needs SPS/PPS + IDR.
            if (onNeedIdr_) onNeedIdr_();
            nextHeartbeatQpc = QpcNow();
        }

        EncodedFrame frame;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50),
                         [this] { return !queue_.empty() || !running_.load(); });
            if (!queue_.empty()) {
                frame = std::move(queue_.front());
                queue_.pop_front();
                haveFrame = true;
            }
        }
        if (!running_.load()) break;

        if (haveFrame) {
            if (!SendMessage(kTypeVideo, frame.flags, frame.payload.data(), frame.payload.size())) {
                dropConnection("video send failed");
                continue;
            }
            const int64_t sentQpc = QpcNow();
            stats_->RecordSentFrame(frame.payload.size(), frame.isIdr,
                                    QpcDeltaMs(frame.captureQpc, frame.encodeDoneQpc),
                                    QpcDeltaMs(frame.encodeDoneQpc, sentQpc));
        }

        // heartbeat every second regardless of video traffic (PROTOCOL.md type=3)
        const int64_t now = QpcNow();
        if (QpcDeltaMs(now, nextHeartbeatQpc) <= 0.0) {
            if (!SendMessage(kTypeHeartbeat, 0, nullptr, 0)) {
                dropConnection("heartbeat send failed");
                continue;
            }
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            nextHeartbeatQpc = now + freq.QuadPart;  // +1s
        }
    }

    CloseSocket();
}

}  // namespace a2m
