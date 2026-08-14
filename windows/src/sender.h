// TCP client for PROTOCOL.md v1: connects to localhost:<port> (which adb forward
// maps onto the phone's listener), sends handshake -> video -> heartbeat, drops
// backlog to keep latency low, and reconnects with exponential backoff.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "encoder.h"
#include "stats.h"

namespace a2m {

class Sender {
public:
    Sender(Stats* stats) : stats_(stats) {}
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // `onNeedIdr` is invoked after every (re)connect so the encoder emits a fresh
    // IDR + SPS/PPS, per REQUIREMENTS §3 / PROTOCOL.md reconnect rules.
    void Start(int port, int width, int height, int fps, std::function<void()> onNeedIdr);
    void Stop();

    // Called from the encoder callback thread.
    void Enqueue(EncodedFrame&& frame);

    bool IsConnected() const { return connected_.load(); }

private:
    void ThreadMain();
    bool TryConnect();
    void CloseSocket();
    bool SendAll(const uint8_t* data, size_t size);
    bool SendMessage(uint8_t type, uint16_t flags, const uint8_t* payload, size_t payloadLen);
    bool SendHandshake();
    void ClearQueue();

    Stats* stats_ = nullptr;
    std::function<void()> onNeedIdr_;

    int port_ = 5001;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;

    SOCKET socket_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    bool everConnected_ = false;

    uint32_t seq_ = 0;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<EncodedFrame> queue_;

    static constexpr size_t kMaxQueue = 2;
};

}  // namespace a2m
