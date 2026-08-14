#include "stats.h"

#include <algorithm>

namespace a2m {
namespace {

double Percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    idx = std::min(idx, sorted.size() - 1);
    return sorted[idx];
}

}  // namespace

void Stats::RecordSentFrame(size_t bytes, bool isIdr, double captureToEncodeMs,
                            double encodeToSentMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
    if (isIdr) ++idr_;
    bytes_ += bytes;
    ++totalFrames_;
    totalBytes_ += bytes;
    captureToEncode_.push_back(captureToEncodeMs);
    encodeToSent_.push_back(encodeToSentMs);
}

void Stats::RecordDrop(uint64_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    drops_ += n;
    totalDrops_ += n;
}

void Stats::RecordReconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++reconnects_;
    ++totalReconnects_;
}

StatsSnapshot Stats::FlushInterval() {
    std::vector<double> c2e;
    std::vector<double> e2s;
    StatsSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap.framesSent = frames_;
        snap.idrSent = idr_;
        snap.bytesSent = bytes_;
        snap.drops = drops_;
        snap.reconnects = reconnects_;
        c2e.swap(captureToEncode_);
        e2s.swap(encodeToSent_);
        frames_ = 0;
        idr_ = 0;
        bytes_ = 0;
        drops_ = 0;
        reconnects_ = 0;
    }
    snap.captureToEncodeP50Ms = Percentile(c2e, 0.50);
    snap.captureToEncodeP95Ms = Percentile(c2e, 0.95);
    snap.encodeToSentP50Ms = Percentile(e2s, 0.50);
    snap.encodeToSentP95Ms = Percentile(e2s, 0.95);
    return snap;
}

uint64_t Stats::TotalFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalFrames_;
}
uint64_t Stats::TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}
uint64_t Stats::TotalDrops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalDrops_;
}
uint64_t Stats::TotalReconnects() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalReconnects_;
}

}  // namespace a2m
