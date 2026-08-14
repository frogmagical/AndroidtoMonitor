#!/usr/bin/env python3
"""Estimate the PC <-> Android clock offset via adb shell bracketing.

Each sample: record PC time (t0) immediately before invoking
`adb shell date +%s%N`, and PC time (t1) immediately after the subprocess
returns. The device's reported epoch time is assumed to have been sampled at
the (unknown) midpoint of [t0, t1] -- the standard NTP-style bracket
assumption, valid as long as outbound/inbound adb latency is roughly
symmetric. rtt = t1 - t0 bounds the per-sample uncertainty; the offset
estimate uses the median over all samples (robust to occasional slow calls),
and the reported uncertainty is min(rtt)/2 (the least-perturbed sample sets
the achievable precision).

offset_us is defined as (device_clock - pc_clock): add offset_us to a PC
epoch timestamp to get the equivalent device-clock timestamp, or subtract it
from a device-clock timestamp to get the equivalent PC-clock timestamp.

Usage:
    python clock_offset.py [--serial 1b2f0fc] [--samples 50] [--json offset.json]
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time

ADB_PATH = r"C:\Users\daiki\tools\platform-tools\adb.exe"
DEFAULT_SERIAL = "1b2f0fc"


def sample_once(serial: str) -> tuple[float, float]:
    """Returns (offset_us, rtt_us) for one bracketed adb shell round-trip."""
    t0 = time.time()
    result = subprocess.run(
        [ADB_PATH, "-s", serial, "shell", "date +%s%N"],
        capture_output=True, text=True, timeout=10,
    )
    t1 = time.time()
    if result.returncode != 0:
        raise RuntimeError(f"adb shell date failed: {result.stderr.strip()}")
    device_ns = int(result.stdout.strip())
    device_us = device_ns / 1000.0
    t0_us = t0 * 1_000_000.0
    t1_us = t1 * 1_000_000.0
    pc_mid_us = (t0_us + t1_us) / 2.0
    offset_us = device_us - pc_mid_us
    rtt_us = t1_us - t0_us
    return offset_us, rtt_us


def run_calibration(serial: str, samples: int, warmup: int = 3) -> dict:
    for _ in range(warmup):
        try:
            sample_once(serial)
        except Exception:
            pass

    offsets = []
    rtts = []
    for i in range(samples):
        try:
            offset_us, rtt_us = sample_once(serial)
        except Exception as e:
            print(f"  sample {i}: FAILED ({e})")
            continue
        offsets.append(offset_us)
        rtts.append(rtt_us)

    if not offsets:
        raise RuntimeError("no successful calibration samples")

    median_offset_us = statistics.median(offsets)
    min_rtt_us = min(rtts)
    uncertainty_us = min_rtt_us / 2.0
    mean_rtt_us = statistics.mean(rtts)
    stdev_offset_us = statistics.pstdev(offsets) if len(offsets) > 1 else 0.0

    return {
        "serial": serial,
        "n_samples": len(offsets),
        "median_offset_us": median_offset_us,
        "mean_offset_us": statistics.mean(offsets),
        "stdev_offset_us": stdev_offset_us,
        "min_rtt_us": min_rtt_us,
        "mean_rtt_us": mean_rtt_us,
        "uncertainty_us": uncertainty_us,
        "timestamp": time.time(),
        "raw_offsets_us": offsets,
        "raw_rtts_us": rtts,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    ap.add_argument("--samples", type=int, default=50)
    ap.add_argument("--json", default=None, help="write full result as JSON to this path")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not args.quiet:
        print(f"calibrating clock offset over {args.samples} adb shell round-trips (serial={args.serial}) ...")

    result = run_calibration(args.serial, args.samples)

    if not args.quiet:
        print(
            "median_offset_us=%.1f (device - pc; %.3fms) stdev=%.1fus "
            "min_rtt=%.1fus mean_rtt=%.1fus uncertainty(min_rtt/2)=%.1fus (%.3fms) n=%d"
            % (
                result["median_offset_us"], result["median_offset_us"] / 1000.0,
                result["stdev_offset_us"],
                result["min_rtt_us"], result["mean_rtt_us"],
                result["uncertainty_us"], result["uncertainty_us"] / 1000.0,
                result["n_samples"],
            )
        )

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)
        if not args.quiet:
            print(f"wrote {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
