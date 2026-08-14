// AndroidtoMonitor transfer protocol v1 - see docs/PROTOCOL.md.
// Wire format is little-endian, 24-byte fixed header + payload. This must stay
// byte-compatible with tools/m1/protocol.py and android/.../TcpServer.kt.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace a2m {

constexpr uint8_t kProtocolVersion = 1;

constexpr uint8_t kTypeHandshake = 1;
constexpr uint8_t kTypeVideo = 2;
constexpr uint8_t kTypeHeartbeat = 3;

constexpr uint16_t kFlagIdr = 0x1;
constexpr uint16_t kFlagSpsPps = 0x2;

constexpr size_t kHeaderSize = 24;
constexpr uint32_t kMaxPayload = 8u * 1024u * 1024u;

// Writes the 24-byte header into `out` (must have room for kHeaderSize bytes).
void WriteHeader(uint8_t* out, uint8_t type, uint16_t flags, uint32_t seq, uint64_t ptsUs,
                 uint32_t payloadLen);

// Builds the handshake JSON payload the receiver parses (TcpServer.parseHandshake).
std::string BuildHandshakeJson(int width, int height, int fps);

// ---------------------------------------------------------------------------
// Minimal H.264 Annex-B helpers
// ---------------------------------------------------------------------------

constexpr uint8_t kNalSliceNonIdr = 1;
constexpr uint8_t kNalSliceIdr = 5;
constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;
constexpr uint8_t kNalAud = 9;

struct AnnexBScan {
    bool valid = false;      // at least one start code found
    bool hasIdr = false;     // contains a NAL of type 5
    bool hasSps = false;     // contains a NAL of type 7
    bool hasPps = false;     // contains a NAL of type 8
};

// Scans an Annex-B buffer. When `spsOut`/`ppsOut` are non-null and the buffer
// carries parameter sets, they are overwritten with the NAL including a 4-byte
// start code (same shape as protocol.py's cached sps/pps).
AnnexBScan ScanAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>* spsOut,
                      std::vector<uint8_t>* ppsOut);

// True when the buffer begins with a 3- or 4-byte Annex-B start code.
bool LooksLikeAnnexB(const uint8_t* data, size_t size);

// Converts a length-prefixed (AVCC, 4-byte big-endian lengths) buffer to Annex-B
// in place of `out`. Returns false when the buffer does not parse as AVCC.
bool AvccToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>* out);

}  // namespace a2m
