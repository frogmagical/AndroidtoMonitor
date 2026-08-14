#!/usr/bin/env python3
"""Combine an A2M_PERF logcat capture with before/after clock_offset.py results to estimate
end-to-end (sender pts_us stamp -> on-device render) latency.

The app logs two independent 1-second-bucketed latency stats (see PerfStats.kt):
  - recvToRenderP50/P95ms: local, monotonic-clock (System.nanoTime) latency from "TCP payload
    fully received" to "MediaCodec output rendered". No cross-device clock issue here.
  - ptsToRecvP50/P95ms: RAW (recvEpochUs[device] - pts_us[PC]) latency, which bakes in
    whatever the PC<->Android wall-clock offset happens to be. Not meaningful on its own.

This script:
  1. Parses every A2M_PERF line in the logcat dump, keeping only buckets with frames>0
     (frames=0 buckets log 0.0 placeholders, not real zero-latency samples, and must be
     excluded).
  2. Reduces the per-bucket p50/p95 series to single overall figures using an explicit,
     documented (if imperfect) method: overall p50 = median of the bucket p50s, overall p95 =
     95th percentile of the bucket p95s (a "percentile of percentiles" -- a real per-frame
     pooled percentile is not derivable from 1 Hz aggregated logging alone; see docs/M1-REPORT.md
     for the limitation this implies).
  3. Corrects ptsToRecv using the mean of the before/after clock_offset.py median offsets
     (corrected_us = raw_us - offset_us), and separately reports the before-only/after-only
     corrected values so drift-driven sensitivity is visible.
  4. E2E p50/p95 = corrected_ptsToRecv (p50/p95) + recvToRender (p50/p95), matching the
     additive approximation requested for the M1 latency gate.

Usage:
    python measure_e2e.py --logcat e2e_logcat.txt --offset-before offset_before.json \
        --offset-after offset_after.json [--json e2e_result.json]
"""
from __future__ import annotations

import argparse
import json
import re
import statistics

LINE_RE = re.compile(
    r"frames=(?P<frames>\d+)\s+drops=(?P<drops>\d+)\s+"
    r"recvToRenderP50ms=(?P<r50>[-\d.]+)\s+recvToRenderP95ms=(?P<r95>[-\d.]+)\s+"
    r"ptsToRecvP50ms=(?P<p50>[-\d.]+)\s+ptsToRecvP95ms=(?P<p95>[-\d.]+)\s+"
    r"bitrateKbps=(?P<br>[-\d.]+)"
)


def parse_logcat(path: str) -> list[dict]:
    buckets = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            d = m.groupdict()
            buckets.append({
                "frames": int(d["frames"]),
                "drops": int(d["drops"]),
                "recvToRenderP50": float(d["r50"]),
                "recvToRenderP95": float(d["r95"]),
                "ptsToRecvP50": float(d["p50"]),
                "ptsToRecvP95": float(d["p95"]),
                "bitrateKbps": float(d["br"]),
            })
    return buckets


def pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = int(p * (len(s) - 1))
    idx = max(0, min(idx, len(s) - 1))
    return s[idx]


def load_offset(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logcat", required=True)
    ap.add_argument("--offset-before", required=True)
    ap.add_argument("--offset-after", required=True)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    buckets = parse_logcat(args.logcat)
    active = [b for b in buckets if b["frames"] > 0]
    total_frames = sum(b["frames"] for b in active)
    total_drops = sum(b["drops"] for b in buckets)

    if not active:
        print("no active (frames>0) buckets found in logcat -- nothing to analyze")
        return 1

    r50_series = [b["recvToRenderP50"] for b in active]
    r95_series = [b["recvToRenderP95"] for b in active]
    p50_series = [b["ptsToRecvP50"] for b in active]
    p95_series = [b["ptsToRecvP95"] for b in active]

    recvToRender_p50 = statistics.median(r50_series)
    recvToRender_p95 = pct(r95_series, 0.95)
    ptsToRecv_raw_p50 = statistics.median(p50_series)
    ptsToRecv_raw_p95 = pct(p95_series, 0.95)

    before = load_offset(args.offset_before)
    after = load_offset(args.offset_after)
    offset_before_ms = before["median_offset_us"] / 1000.0
    offset_after_ms = after["median_offset_us"] / 1000.0
    offset_mean_ms = (offset_before_ms + offset_after_ms) / 2.0
    drift_ms = offset_after_ms - offset_before_ms
    uncertainty_before_ms = before["uncertainty_us"] / 1000.0
    uncertainty_after_ms = after["uncertainty_us"] / 1000.0
    uncertainty_ms = max(uncertainty_before_ms, uncertainty_after_ms)

    def corrected(raw_ms: float, offset_ms: float) -> float:
        return raw_ms - offset_ms

    ptsToRecv_corrected_p50 = corrected(ptsToRecv_raw_p50, offset_mean_ms)
    ptsToRecv_corrected_p95 = corrected(ptsToRecv_raw_p95, offset_mean_ms)
    # sensitivity bounds using each individual calibration instead of the mean
    ptsToRecv_p50_using_before = corrected(ptsToRecv_raw_p50, offset_before_ms)
    ptsToRecv_p50_using_after = corrected(ptsToRecv_raw_p50, offset_after_ms)

    e2e_p50 = ptsToRecv_corrected_p50 + recvToRender_p50
    e2e_p95 = ptsToRecv_corrected_p95 + recvToRender_p95

    result = {
        "active_buckets": len(active),
        "total_buckets": len(buckets),
        "total_frames": total_frames,
        "total_drops": total_drops,
        "recvToRender_p50_ms": recvToRender_p50,
        "recvToRender_p95_ms": recvToRender_p95,
        "ptsToRecv_raw_p50_ms": ptsToRecv_raw_p50,
        "ptsToRecv_raw_p95_ms": ptsToRecv_raw_p95,
        "offset_before_ms": offset_before_ms,
        "offset_after_ms": offset_after_ms,
        "offset_mean_ms": offset_mean_ms,
        "offset_drift_ms": drift_ms,
        "offset_uncertainty_ms": uncertainty_ms,
        "ptsToRecv_corrected_p50_ms": ptsToRecv_corrected_p50,
        "ptsToRecv_corrected_p95_ms": ptsToRecv_corrected_p95,
        "ptsToRecv_p50_sensitivity_before_ms": ptsToRecv_p50_using_before,
        "ptsToRecv_p50_sensitivity_after_ms": ptsToRecv_p50_using_after,
        "e2e_p50_ms": e2e_p50,
        "e2e_p95_ms": e2e_p95,
        "gate_p95_ms": 100.0,
        "gate_pass": e2e_p95 <= 100.0,
    }

    print(f"active buckets: {len(active)}/{len(buckets)}  total_frames={total_frames}  total_drops={total_drops}")
    print(f"recvToRender:  p50={recvToRender_p50:.1f}ms  p95={recvToRender_p95:.1f}ms  (local monotonic clock, no offset issue)")
    print(f"ptsToRecv raw: p50={ptsToRecv_raw_p50:.1f}ms  p95={ptsToRecv_raw_p95:.1f}ms  (uncorrected, includes PC<->Android clock offset)")
    print(f"clock offset:  before={offset_before_ms:.1f}ms  after={offset_after_ms:.1f}ms  drift={drift_ms:+.1f}ms  "
          f"uncertainty=+/-{uncertainty_ms:.1f}ms  (mean used for correction={offset_mean_ms:.1f}ms)")
    print(f"ptsToRecv corrected: p50={ptsToRecv_corrected_p50:.1f}ms  p95={ptsToRecv_corrected_p95:.1f}ms  "
          f"(sensitivity using before-only offset: p50={ptsToRecv_p50_using_before:.1f}ms, after-only: p50={ptsToRecv_p50_using_after:.1f}ms)")
    print(f"E2E (pts_us send stamp -> render) estimate: p50={e2e_p50:.1f}ms  p95={e2e_p95:.1f}ms")
    print(f"gate p95<=100ms: {'PASS' if result['gate_pass'] else 'FAIL'} (p95={e2e_p95:.1f}ms)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)
        print(f"wrote {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
