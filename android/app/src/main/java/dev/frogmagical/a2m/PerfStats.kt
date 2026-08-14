package dev.frogmagical.a2m

import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * Aggregates per-frame recv->render latency and throughput, logging a 1-second summary to
 * logcat tag "A2M_PERF" (per REQUIREMENTS §5 observability / M0-REPORT §6 latency proof).
 *
 * All mutation happens under [lock] so it's safe to call [recordFrame]/[recordBytes]/[recordDrop]
 * from network or codec-callback threads while [flush] runs on the main thread's Handler loop.
 */
class PerfStats {

    companion object {
        private const val TAG = "A2M_PERF"
    }

    private data class Snapshot(val frames: Int, val drops: Int, val bytes: Long, val latencies: List<Double>)

    private val lock = Object()
    private var frameCount = 0
    private var dropCount = 0
    private var byteCount = 0L
    private val latenciesMs = ArrayList<Double>()

    @Volatile var lastFps: Double = 0.0
        private set
    @Volatile var lastBitrateKbps: Double = 0.0
        private set
    @Volatile var lastP50Ms: Double = 0.0
        private set
    @Volatile var lastP95Ms: Double = 0.0
        private set
    @Volatile var lastDrops: Int = 0
        private set

    private val handler = Handler(Looper.getMainLooper())
    @Volatile private var running = false

    private val tick = object : Runnable {
        override fun run() {
            flush()
            if (running) handler.postDelayed(this, 1000)
        }
    }

    fun start() {
        if (running) return
        running = true
        handler.postDelayed(tick, 1000)
    }

    fun stop() {
        running = false
        handler.removeCallbacks(tick)
    }

    /** recvTimeNs/renderTimeNs are System.nanoTime() timestamps; ptsUs is the sender's pts for correlation only. */
    fun recordFrame(recvTimeNs: Long, renderTimeNs: Long, @Suppress("UNUSED_PARAMETER") ptsUs: Long) {
        val latencyMs = (renderTimeNs - recvTimeNs) / 1_000_000.0
        synchronized(lock) {
            frameCount++
            latenciesMs.add(latencyMs)
        }
    }

    fun recordBytes(n: Int) {
        synchronized(lock) { byteCount += n }
    }

    fun recordDrop(n: Int = 1) {
        synchronized(lock) { dropCount += n }
    }

    private fun flush() {
        val snap = synchronized(lock) {
            val s = Snapshot(frameCount, dropCount, byteCount, ArrayList(latenciesMs))
            frameCount = 0
            dropCount = 0
            byteCount = 0
            latenciesMs.clear()
            s
        }
        val sorted = snap.latencies.sorted()
        val p50 = percentile(sorted, 0.50)
        val p95 = percentile(sorted, 0.95)

        lastFps = snap.frames.toDouble()
        lastBitrateKbps = snap.bytes * 8.0 / 1000.0
        lastP50Ms = p50
        lastP95Ms = p95
        lastDrops = snap.drops

        Log.i(
            TAG,
            "frames=%d drops=%d recvToRenderP50ms=%.1f recvToRenderP95ms=%.1f bitrateKbps=%.0f".format(
                snap.frames, snap.drops, p50, p95, lastBitrateKbps
            )
        )
    }

    private fun percentile(sorted: List<Double>, p: Double): Double {
        if (sorted.isEmpty()) return 0.0
        val idx = (p * (sorted.size - 1)).toInt().coerceIn(0, sorted.size - 1)
        return sorted[idx]
    }
}
