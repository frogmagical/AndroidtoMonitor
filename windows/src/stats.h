// Sender-side observability (REQUIREMENTS §5, M1-REPORT §7): one line per second
// with fps, capture->encode and encode->send latencies, bytes, drops, reconnects.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace a2m {

struct StatsSnapshot {
    int framesSent = 0;
    uint64_t bytesSent = 0;
    uint64_t drops = 0;
    uint64_t reconnects = 0;
    int idrSent = 0;
    double captureToEncodeP50Ms = 0.0;
    double captureToEncodeP95Ms = 0.0;
    double encodeToSentP50Ms = 0.0;
    double encodeToSentP95Ms = 0.0;
};

class Stats {
public:
    void RecordSentFrame(size_t bytes, bool isIdr, double captureToEncodeMs, double encodeToSentMs);
    void RecordDrop(uint64_t n = 1);
    void RecordReconnect();

    // Returns the last interval and resets the per-interval accumulators.
    StatsSnapshot FlushInterval();

    uint64_t TotalFrames() const;
    uint64_t TotalBytes() const;
    uint64_t TotalDrops() const;
    uint64_t TotalReconnects() const;

private:
    mutable std::mutex mutex_;
    int frames_ = 0;
    int idr_ = 0;
    uint64_t bytes_ = 0;
    uint64_t drops_ = 0;
    uint64_t reconnects_ = 0;
    std::vector<double> captureToEncode_;
    std::vector<double> encodeToSent_;

    uint64_t totalFrames_ = 0;
    uint64_t totalBytes_ = 0;
    uint64_t totalDrops_ = 0;
    uint64_t totalReconnects_ = 0;
};

}  // namespace a2m
