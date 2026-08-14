#include "protocol.h"

#include <cstdio>
#include <cstring>

namespace a2m {
namespace {

inline void PutU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void PutU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
inline void PutU64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }

// Returns the offset of the next 00 00 01 sequence at or after `from`, or npos.
size_t FindStartCode(const uint8_t* data, size_t size, size_t from) {
    if (size < 3) return static_cast<size_t>(-1);
    for (size_t i = from; i + 2 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
    }
    return static_cast<size_t>(-1);
}

}  // namespace

void WriteHeader(uint8_t* out, uint8_t type, uint16_t flags, uint32_t seq, uint64_t ptsUs,
                 uint32_t payloadLen) {
    // magic(4) version(1) type(1) flags(2) seq(4) pts_us(8) payload_len(4)
    out[0] = 'A';
    out[1] = '2';
    out[2] = 'M';
    out[3] = '1';
    out[4] = kProtocolVersion;
    out[5] = type;
    PutU16(out + 6, flags);
    PutU32(out + 8, seq);
    PutU64(out + 12, ptsUs);
    PutU32(out + 20, payloadLen);
}

std::string BuildHandshakeJson(int width, int height, int fps) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"width\":%d,\"height\":%d,\"fps\":%d,\"codec\":\"h264\"}", width,
             height, fps);
    return std::string(buf);
}

bool LooksLikeAnnexB(const uint8_t* data, size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) return true;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
    return false;
}

AnnexBScan ScanAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>* spsOut,
                      std::vector<uint8_t>* ppsOut) {
    AnnexBScan scan;
    size_t pos = FindStartCode(data, size, 0);
    while (pos != static_cast<size_t>(-1)) {
        const size_t nalStart = pos + 3;
        if (nalStart >= size) break;
        scan.valid = true;

        size_t next = FindStartCode(data, size, nalStart);
        size_t nalEnd = (next == static_cast<size_t>(-1)) ? size : next;
        // A 4-byte start code is "00 00 00 01": the leading zero belongs to the
        // delimiter, not to the previous NAL. Same trimming as protocol.py.
        while (nalEnd > nalStart && data[nalEnd - 1] == 0) --nalEnd;

        const uint8_t nalType = static_cast<uint8_t>(data[nalStart] & 0x1F);
        switch (nalType) {
            case kNalSliceIdr:
                scan.hasIdr = true;
                break;
            case kNalSps:
                scan.hasSps = true;
                if (spsOut) {
                    spsOut->assign({0, 0, 0, 1});
                    spsOut->insert(spsOut->end(), data + nalStart, data + nalEnd);
                }
                break;
            case kNalPps:
                scan.hasPps = true;
                if (ppsOut) {
                    ppsOut->assign({0, 0, 0, 1});
                    ppsOut->insert(ppsOut->end(), data + nalStart, data + nalEnd);
                }
                break;
            default:
                break;
        }
        pos = next;
    }
    return scan;
}

bool AvccToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
    size_t pos = 0;
    while (pos + 4 <= size) {
        const uint32_t len = (static_cast<uint32_t>(data[pos]) << 24) |
                             (static_cast<uint32_t>(data[pos + 1]) << 16) |
                             (static_cast<uint32_t>(data[pos + 2]) << 8) |
                             static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        if (len == 0 || pos + len > size) return false;
        const uint8_t startCode[4] = {0, 0, 0, 1};
        out->insert(out->end(), startCode, startCode + 4);
        out->insert(out->end(), data + pos, data + pos + len);
        pos += len;
    }
    return pos == size && !out->empty();
}

}  // namespace a2m
