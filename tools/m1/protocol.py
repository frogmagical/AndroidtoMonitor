"""AndroidtoMonitor transfer protocol v1 (docs/PROTOCOL.md) - framing helpers and a
minimal Annex-B H.264 access-unit assembler used by send_test.py.

Wire format (little-endian, 24-byte fixed header + payload):
  offset  size  field
  0       4     magic       ASCII "A2M1"
  4       1     version     1
  5       1     type        1=handshake 2=video 3=heartbeat
  6       2     flags       bit0=IDR bit1=SPS+PPS
  8       4     seq         u32, single counter across all message types
  12      8     pts_us      u64, sender's Unix epoch microseconds (latency measurement only)
  20      4     payload_len u32
  24      n     payload
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"A2M1"
VERSION = 1

TYPE_HANDSHAKE = 1
TYPE_VIDEO = 2
TYPE_HEARTBEAT = 3

FLAG_IDR = 0x1
FLAG_SPS_PPS = 0x2

MAX_PAYLOAD = 8 * 1024 * 1024

# magic(4s) version(B) type(B) flags(H) seq(I) pts_us(Q) payload_len(I) = 24 bytes
_HEADER = struct.Struct("<4sBBHIQI")
assert _HEADER.size == 24


def build_frame(msg_type: int, flags: int, seq: int, pts_us: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)} > {MAX_PAYLOAD}")
    header = _HEADER.pack(
        MAGIC, VERSION, msg_type, flags, seq & 0xFFFFFFFF, pts_us & 0xFFFFFFFFFFFFFFFF, len(payload)
    )
    return header + payload


# ---------------------------------------------------------------------------
# Annex-B NAL parsing / access-unit assembly
# ---------------------------------------------------------------------------

NAL_SLICE_NON_IDR = 1
NAL_SLICE_IDR = 5
NAL_SEI = 6
NAL_SPS = 7
NAL_PPS = 8
NAL_AUD = 9

START_CODE = b"\x00\x00\x01"


def iter_nal_units(chunk_iter):
    """Consume an iterable of byte chunks (e.g. a subprocess stdout reader) and yield
    (nal_type, nal_bytes_with_4byte_start_code) tuples. Buffers across chunk boundaries."""
    buf = bytearray()
    for chunk in chunk_iter:
        if not chunk:
            continue
        buf.extend(chunk)
        buf, nals = _drain_complete_nals(buf)
        for nal in nals:
            yield nal
    # flush whatever remains as a final NAL (stream ended)
    if len(buf) > 3:
        start = _find_start_code(buf, 0)
        if start is not None:
            nal_type = buf[start + 3] & 0x1F
            yield nal_type, b"\x00\x00\x00\x01" + bytes(buf[start + 3:])


def _find_start_code(buf: bytearray, from_idx: int):
    idx = buf.find(START_CODE, from_idx)
    return idx if idx != -1 else None


def _drain_complete_nals(buf: bytearray):
    """Return (remaining_buf, list_of_(type, bytes)) for every fully-buffered NAL unit."""
    offsets = []
    i = 0
    while True:
        idx = _find_start_code(buf, i)
        if idx is None:
            break
        offsets.append(idx)
        i = idx + 3

    nals = []
    if len(offsets) < 2:
        return buf, nals

    for j in range(len(offsets) - 1):
        nal_start = offsets[j] + 3
        nal_end = offsets[j + 1]
        # trailing zero byte(s) before a 4-byte start code belong to the padding, not the NAL
        while nal_end > nal_start and buf[nal_end - 1] == 0:
            nal_end -= 1
        nal_type = buf[nal_start] & 0x1F
        nals.append((nal_type, b"\x00\x00\x00\x01" + bytes(buf[nal_start:nal_end])))

    # keep from the last start code onward (it may still be incomplete)
    remaining = buf[offsets[-1]:]
    return remaining, nals


@dataclass
class AccessUnit:
    is_idr: bool = False
    nals: list = field(default_factory=list)  # list of (type, bytes), AUD/SPS/PPS excluded

    def payload(self, sps: bytes | None, pps: bytes | None) -> bytes:
        parts = []
        if self.is_idr and sps and pps:
            parts.append(sps)
            parts.append(pps)
        for _, data in self.nals:
            parts.append(data)
        return b"".join(parts)


def assemble_access_units(nal_stream):
    """Group a NAL stream (as produced by iter_nal_units) into AccessUnit objects, using
    AUD (type 9) markers as boundaries (ffmpeg is run with x264-params aud=1 to guarantee
    one per frame). SPS/PPS NALs are cached and stripped out of the AU body; the caller is
    responsible for re-prepending the cached copies to IDR access units so every IDR is
    self-contained, per PROTOCOL.md."""
    sps: bytes | None = None
    pps: bytes | None = None
    current = AccessUnit()
    have_content = False

    for nal_type, nal_bytes in nal_stream:
        if nal_type == NAL_AUD:
            if have_content:
                yield current, sps, pps
            current = AccessUnit()
            have_content = False
            continue
        if nal_type == NAL_SPS:
            sps = nal_bytes
            continue
        if nal_type == NAL_PPS:
            pps = nal_bytes
            continue
        if nal_type == NAL_SLICE_IDR:
            current.is_idr = True
        current.nals.append((nal_type, nal_bytes))
        have_content = True

    if have_content:
        yield current, sps, pps
