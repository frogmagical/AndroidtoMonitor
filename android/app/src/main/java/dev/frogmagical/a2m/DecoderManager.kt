package dev.frogmagical.a2m

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.util.ArrayDeque

/**
 * Wraps a MediaCodec H.264 Surface decoder configured per REQUIREMENTS §4.3 / PROTOCOL.md:
 * low-latency mode, realtime priority, immediate feed/render, and a bounded input backlog
 * (>2 pending access units => drop non-IDR frames until the next IDR, per M0-REPORT §6).
 *
 * All queue state is guarded by [lock] since frames arrive from the network/session thread
 * while MediaCodec.Callback fires on [callbackThread].
 */
class DecoderManager(private val surface: Surface, private val stats: PerfStats) {

    companion object {
        private const val TAG = "A2M_DEC"
        private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val MAX_BACKLOG = 2
    }

    private class PendingFrame(val data: ByteArray, val size: Int, val ptsUs: Long)

    private var codec: MediaCodec? = null
    private val callbackThread = HandlerThread("A2M-Codec").apply { start() }
    private val callbackHandler = Handler(callbackThread.looper)

    private val lock = Object()
    private val pendingFrames = ArrayDeque<PendingFrame>()
    private val availableInputIndices = ArrayDeque<Int>()
    private val ptsToRecvNs = HashMap<Long, Long>()
    private var droppingUntilIdr = false

    /** true until the next IDR (post-handshake) has been fed; video frames are dropped meanwhile. */
    @Volatile var waitingForConfig = true
        private set

    /** (Re)configures the decoder for a new stream. Always fully tears down any prior codec
     * instance, matching "reconnect => reset decoder, wait for SPS/PPS+IDR" (REQUIREMENTS §4.3). */
    fun configure(width: Int, height: Int, fps: Int) {
        releaseCodecOnly()
        synchronized(lock) {
            pendingFrames.clear()
            availableInputIndices.clear()
            ptsToRecvNs.clear()
            droppingUntilIdr = false
        }
        waitingForConfig = true

        if (width <= 0 || height <= 0) {
            Log.e(TAG, "invalid handshake dimensions ${width}x$height, not configuring decoder")
            return
        }

        try {
            val format = MediaFormat.createVideoFormat(MIME, width, height)
            format.setInteger(MediaFormat.KEY_PRIORITY, 0) // 0 = realtime
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && isLowLatencySupported()) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, if (fps > 0) fps else 30)

            val mc = MediaCodec.createDecoderByType(MIME)
            mc.setCallback(codecCallback, callbackHandler)
            mc.configure(format, surface, null, 0)
            mc.start()
            codec = mc
            Log.i(TAG, "decoder configured ${width}x${height}@${fps}fps")
        } catch (e: Exception) {
            Log.e(TAG, "decoder configure failed: $e")
            codec = null
        }
    }

    private fun isLowLatencySupported(): Boolean {
        return try {
            val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
            for (info in list.codecInfos) {
                if (info.isEncoder) continue
                if (!info.supportedTypes.any { it.equals(MIME, ignoreCase = true) }) continue
                val caps = info.getCapabilitiesForType(MIME)
                if (caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) return true
            }
            false
        } catch (e: Exception) {
            Log.w(TAG, "low-latency capability query failed: $e")
            false
        }
    }

    /** Called from the network/session thread as Annex-B access units arrive. */
    fun onVideoFrame(data: ByteArray, length: Int, ptsUs: Long, flags: Int) {
        if (codec == null) return
        val isIdr = (flags and TcpServer.FLAG_IDR) != 0

        if (waitingForConfig && !isIdr) {
            stats.recordDrop()
            return
        }

        val frame = PendingFrame(data, length, ptsUs)
        synchronized(lock) {
            if (isIdr) {
                if (pendingFrames.isNotEmpty()) stats.recordDrop(pendingFrames.size)
                pendingFrames.clear()
                droppingUntilIdr = false
                waitingForConfig = false
            } else {
                if (droppingUntilIdr) {
                    stats.recordDrop()
                    return
                }
                if (pendingFrames.size >= MAX_BACKLOG) {
                    // Backlog exceeded: drop this frame and read through until the next IDR.
                    droppingUntilIdr = true
                    stats.recordDrop()
                    return
                }
            }
            pendingFrames.addLast(frame)
            ptsToRecvNs[ptsUs] = System.nanoTime()
            tryFeedLocked()
        }
    }

    private fun tryFeedLocked() {
        while (pendingFrames.isNotEmpty() && availableInputIndices.isNotEmpty()) {
            val idx = availableInputIndices.removeFirst()
            val f = pendingFrames.removeFirst()
            feed(idx, f)
        }
    }

    private fun feed(index: Int, f: PendingFrame) {
        val mc = codec ?: return
        try {
            val buf = mc.getInputBuffer(index) ?: return
            buf.clear()
            buf.put(f.data, 0, f.size)
            mc.queueInputBuffer(index, 0, f.size, f.ptsUs, 0)
        } catch (e: Exception) {
            Log.e(TAG, "queueInputBuffer failed: $e")
        }
    }

    private val codecCallback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
            synchronized(lock) {
                if (pendingFrames.isNotEmpty()) {
                    val f = pendingFrames.removeFirst()
                    feed(index, f)
                } else {
                    availableInputIndices.addLast(index)
                }
            }
        }

        override fun onOutputBufferAvailable(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            try {
                mc.releaseOutputBuffer(index, true) // render immediately; PTS not used for pacing
            } catch (e: Exception) {
                Log.e(TAG, "releaseOutputBuffer failed: $e")
                return
            }
            val renderTimeNs = System.nanoTime()
            val recvNs = synchronized(lock) { ptsToRecvNs.remove(info.presentationTimeUs) }
            if (recvNs != null) {
                stats.recordFrame(recvNs, renderTimeNs, info.presentationTimeUs)
            }
        }

        override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
            Log.i(TAG, "output format changed: $format")
        }

        override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
            Log.e(TAG, "codec error: $e")
        }
    }

    /** Marks the decoder as needing a fresh IDR (e.g. on TCP disconnect) without tearing down
     * the codec instance itself; the next handshake will call [configure] anyway. */
    fun reset() {
        waitingForConfig = true
        synchronized(lock) {
            pendingFrames.clear()
            availableInputIndices.clear()
            ptsToRecvNs.clear()
            droppingUntilIdr = false
        }
    }

    fun release() {
        releaseCodecOnly()
        callbackThread.quitSafely()
    }

    private fun releaseCodecOnly() {
        try {
            codec?.stop()
            codec?.release()
        } catch (e: Exception) {
            Log.w(TAG, "codec release error: $e")
        }
        codec = null
    }
}
