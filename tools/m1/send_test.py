#!/usr/bin/env python3
"""M1 PC-side test sender (tools/m1/send_test.py).

Stand-in for M2's real Windows sender: captures the already-running virtual display via
ffmpeg (ddagrab), frames the raw H.264 Annex-B output per docs/PROTOCOL.md v1, and streams
it to the Android receiver app over `adb forward tcp:5001 tcp:5001`.

Usage:
    python send_test.py [--duration SECONDS] [--serial 1b2f0fc] [--bitrate 8M] [--fps 30]

Requires: adb (platform-tools) with the device authorized, ffmpeg on PATH, and the
Android app already running and listening on 127.0.0.1:5001 on-device.
"""
from __future__ import annotations

import argparse
import queue
import socket
import subprocess
import sys
import threading
import time

from protocol import (
    FLAG_IDR,
    FLAG_SPS_PPS,
    TYPE_HANDSHAKE,
    TYPE_HEARTBEAT,
    TYPE_VIDEO,
    assemble_access_units,
    build_frame,
    iter_nal_units,
)

ADB_PATH = r"C:\Users\daiki\tools\platform-tools\adb.exe"
DEFAULT_SERIAL = "1b2f0fc"
PORT = 5001
WIDTH = 1080
HEIGHT = 2400

# bounded queue between the ffmpeg-reader thread and the socket-sender thread; if the
# sender falls behind, old non-IDR access units are dropped (PROTOCOL.md: "詰まり時は
# 送信側で古いフレームを破棄")
SEND_QUEUE_MAXSIZE = 2


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def setup_adb_forward(serial: str) -> None:
    log(f"adb forward tcp:{PORT} tcp:{PORT} (serial={serial})")
    result = subprocess.run(
        [ADB_PATH, "-s", serial, "forward", f"tcp:{PORT}", f"tcp:{PORT}"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"adb forward failed: {result.stderr.strip()}")


def start_ffmpeg(fps: int, bitrate: str) -> subprocess.Popen:
    filter_arg = f"ddagrab=output_idx=2:framerate={fps},hwdownload,format=bgra,setpts=N/({fps}*TB)"
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-f", "lavfi", "-readrate", "1", "-readrate_initial_burst", "0",
        "-i", filter_arg,
        "-fps_mode", "passthrough",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-pix_fmt", "yuv420p",
        "-g", str(fps * 2),
        "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", "2M",
        "-x264-params", "aud=1:repeat-headers=1",
        "-f", "h264", "pipe:1",
    ]
    log("starting ffmpeg: " + " ".join(cmd))
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)


def stderr_drain(proc: subprocess.Popen) -> None:
    for line in iter(proc.stderr.readline, b""):
        if line:
            sys.stderr.write("[ffmpeg] " + line.decode(errors="replace"))


def now_us() -> int:
    return time.time_ns() // 1000


class SeqCounter:
    def __init__(self):
        self._lock = threading.Lock()
        self._seq = 0

    def next(self) -> int:
        with self._lock:
            s = self._seq
            self._seq += 1
            return s


def connect_with_retry(host: str, port: int, timeout_s: float = 15.0) -> socket.socket:
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=2.0)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return sock
        except OSError as e:
            last_err = e
            time.sleep(0.3)
    raise RuntimeError(f"could not connect to {host}:{port}: {last_err}")


def reader_thread(proc: subprocess.Popen, send_q: "queue.Queue", stop_evt: threading.Event, stats: dict):
    def chunks():
        while not stop_evt.is_set():
            data = proc.stdout.read(65536)
            if not data:
                return
            yield data

    for au, sps, pps in assemble_access_units(iter_nal_units(chunks())):
        if stop_evt.is_set():
            return
        payload = au.payload(sps, pps)
        if not payload:
            continue
        flags = (FLAG_IDR | FLAG_SPS_PPS) if au.is_idr else 0
        item = (payload, flags, au.is_idr)

        try:
            send_q.put_nowait(item)
        except queue.Full:
            # backlog: drop the oldest queued frame; if this frame is an IDR, clear the
            # whole backlog so the receiver resyncs on a clean boundary instead of a stale one
            try:
                send_q.get_nowait()
                stats["drops"] += 1
            except queue.Empty:
                pass
            if au.is_idr:
                while True:
                    try:
                        send_q.get_nowait()
                        stats["drops"] += 1
                    except queue.Empty:
                        break
            try:
                send_q.put_nowait(item)
            except queue.Full:
                stats["drops"] += 1


def sender_thread(sock: socket.socket, send_q: "queue.Queue", seq: SeqCounter, stop_evt: threading.Event, stats: dict):
    while not stop_evt.is_set():
        try:
            payload, flags, is_idr = send_q.get(timeout=0.5)
        except queue.Empty:
            continue
        pts_us = now_us()
        frame = build_frame(TYPE_VIDEO, flags, seq.next(), pts_us, payload)
        try:
            sock.sendall(frame)
        except OSError as e:
            log(f"send failed, stopping: {e}")
            stop_evt.set()
            return
        stats["frames"] += 1
        stats["bytes"] += len(payload)
        if is_idr:
            stats["idr_frames"] += 1


def heartbeat_thread(sock: socket.socket, seq: SeqCounter, stop_evt: threading.Event, send_lock: threading.Lock):
    while not stop_evt.wait(1.0):
        frame = build_frame(TYPE_HEARTBEAT, 0, seq.next(), now_us(), b"")
        try:
            with send_lock:
                sock.sendall(frame)
        except OSError as e:
            log(f"heartbeat send failed: {e}")
            stop_evt.set()
            return


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=0.0, help="seconds to stream, 0 = run until Ctrl+C")
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    ap.add_argument("--bitrate", default="8M")
    ap.add_argument("--fps", type=int, default=30)
    args = ap.parse_args()

    setup_adb_forward(args.serial)

    ffmpeg_proc = start_ffmpeg(args.fps, args.bitrate)
    threading.Thread(target=stderr_drain, args=(ffmpeg_proc,), daemon=True).start()

    log("connecting to localhost:%d ..." % PORT)
    sock = connect_with_retry("127.0.0.1", PORT)
    log("connected")

    seq = SeqCounter()
    send_lock = threading.Lock()

    handshake_payload = (
        '{"width":%d,"height":%d,"fps":%d,"codec":"h264"}' % (WIDTH, HEIGHT, args.fps)
    ).encode("utf-8")
    with send_lock:
        sock.sendall(build_frame(TYPE_HANDSHAKE, 0, seq.next(), now_us(), handshake_payload))
    log("handshake sent: " + handshake_payload.decode())

    send_q: "queue.Queue" = queue.Queue(maxsize=SEND_QUEUE_MAXSIZE)
    stop_evt = threading.Event()
    stats = {"frames": 0, "idr_frames": 0, "bytes": 0, "drops": 0}

    t_reader = threading.Thread(target=reader_thread, args=(ffmpeg_proc, send_q, stop_evt, stats), daemon=True)
    t_sender = threading.Thread(target=sender_thread, args=(sock, send_q, seq, stop_evt, stats), daemon=True)
    t_heartbeat = threading.Thread(target=heartbeat_thread, args=(sock, seq, stop_evt, send_lock), daemon=True)
    t_reader.start()
    t_sender.start()
    t_heartbeat.start()

    start = time.time()
    try:
        while not stop_evt.is_set():
            time.sleep(1.0)
            elapsed = time.time() - start
            log(
                "stats: frames=%d idr=%d bytes=%d drops=%d elapsed=%.1fs"
                % (stats["frames"], stats["idr_frames"], stats["bytes"], stats["drops"], elapsed)
            )
            if args.duration and elapsed >= args.duration:
                log("duration reached, stopping")
                break
            if ffmpeg_proc.poll() is not None:
                log(f"ffmpeg exited unexpectedly (code={ffmpeg_proc.returncode})")
                break
    except KeyboardInterrupt:
        log("interrupted")
    finally:
        stop_evt.set()
        try:
            sock.close()
        except OSError:
            pass
        if ffmpeg_proc.poll() is None:
            ffmpeg_proc.terminate()
            try:
                ffmpeg_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                ffmpeg_proc.kill()
        log(f"final stats: {stats}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
