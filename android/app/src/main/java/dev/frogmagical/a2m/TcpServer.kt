package dev.frogmagical.a2m

import android.util.Log
import java.io.DataInputStream
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicReference

/**
 * Callbacks invoked from network/session threads (NOT the UI thread).
 * Implementations must be thread-safe / dispatch to UI thread themselves if needed.
 */
interface A2mListener {
    fun onHandshake(width: Int, height: Int, fps: Int, codec: String)
    fun onVideoFrame(data: ByteArray, length: Int, ptsUs: Long, flags: Int, recvTimeNs: Long, recvEpochUs: Long)
    fun onConnected()
    fun onDisconnected()
}

/**
 * Implements PROTOCOL.md v1 server side: listens on 127.0.0.1:<port>, accepts a single
 * connection at a time (a new incoming connection preempts the previous one), parses the
 * 24-byte fixed header + payload framing, and dispatches handshake/video/heartbeat messages.
 */
class TcpServer(private val port: Int, private val listener: A2mListener) : Thread("A2M-TcpAccept") {

    companion object {
        private const val TAG = "A2M_NET"

        // "A2M1" as little-endian uint32 (bytes 41 32 4D 31)
        const val MAGIC = 0x314D3241
        const val VERSION: Int = 1
        const val HEADER_SIZE = 24

        const val TYPE_HANDSHAKE = 1
        const val TYPE_VIDEO = 2
        const val TYPE_HEARTBEAT = 3

        const val FLAG_IDR = 0x1
        const val FLAG_SPS_PPS = 0x2

        const val MAX_PAYLOAD = 8 * 1024 * 1024
        const val HEARTBEAT_TIMEOUT_MS = 3000L
    }

    @Volatile private var running = true
    private var serverSocket: ServerSocket? = null
    private val currentSocket = AtomicReference<Socket?>(null)
    @Volatile private var lastRecvAtMs: Long = Long.MAX_VALUE
    private var watchdog: Thread? = null

    override fun run() {
        try {
            serverSocket = ServerSocket().apply {
                reuseAddress = true
                bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), port))
            }
            Log.i(TAG, "listening on 127.0.0.1:$port")
        } catch (e: IOException) {
            Log.e(TAG, "listen failed: $e")
            return
        }

        startWatchdog()

        while (running) {
            val client: Socket = try {
                serverSocket?.accept() ?: break
            } catch (e: IOException) {
                if (running) Log.e(TAG, "accept error: $e")
                break
            }

            // New connection preempts any existing one (per PROTOCOL.md: single connection,
            // newest wins).
            currentSocket.getAndSet(client)?.let { old ->
                try { old.close() } catch (_: IOException) { /* ignore */ }
            }
            lastRecvAtMs = System.currentTimeMillis()
            Thread({ handleSession(client) }, "A2M-Session").start()
        }
    }

    fun shutdown() {
        running = false
        try { serverSocket?.close() } catch (_: IOException) { /* ignore */ }
        currentSocket.getAndSet(null)?.let {
            try { it.close() } catch (_: IOException) { /* ignore */ }
        }
        watchdog?.interrupt()
    }

    private fun startWatchdog() {
        watchdog = Thread({
            while (running) {
                try {
                    Thread.sleep(300)
                } catch (_: InterruptedException) {
                    break
                }
                val sock = currentSocket.get() ?: continue
                if (System.currentTimeMillis() - lastRecvAtMs > HEARTBEAT_TIMEOUT_MS) {
                    Log.w(TAG, "heartbeat timeout (>${HEARTBEAT_TIMEOUT_MS}ms), dropping connection")
                    try { sock.close() } catch (_: IOException) { /* ignore */ }
                }
            }
        }, "A2M-Watchdog").apply { isDaemon = true; start() }
    }

    private fun handleSession(socket: Socket) {
        listener.onConnected()
        val input = DataInputStream(socket.getInputStream())
        val headerBuf = ByteArray(HEADER_SIZE)
        try {
            socket.tcpNoDelay = true
        } catch (_: IOException) { /* best-effort */ }

        try {
            while (running && currentSocket.get() === socket) {
                input.readFully(headerBuf, 0, HEADER_SIZE)
                val bb = ByteBuffer.wrap(headerBuf).order(ByteOrder.LITTLE_ENDIAN)
                val magic = bb.int
                val version = bb.get().toInt() and 0xFF
                val type = bb.get().toInt() and 0xFF
                val flags = bb.short.toInt() and 0xFFFF
                @Suppress("UNUSED_VARIABLE") val seq = bb.int
                val ptsUs = bb.long
                val payloadLen = bb.int

                if (magic != MAGIC || version != VERSION) {
                    Log.e(TAG, "bad magic/version (magic=0x${magic.toString(16)} version=$version), disconnecting")
                    break
                }
                if (payloadLen < 0 || payloadLen > MAX_PAYLOAD) {
                    Log.e(TAG, "bad payload_len=$payloadLen, disconnecting")
                    break
                }

                lastRecvAtMs = System.currentTimeMillis()

                when (type) {
                    TYPE_HANDSHAKE -> {
                        val payload = ByteArray(payloadLen)
                        input.readFully(payload, 0, payloadLen)
                        parseHandshake(payload)
                    }
                    TYPE_VIDEO -> {
                        val payload = ByteArray(payloadLen)
                        input.readFully(payload, 0, payloadLen)
                        // Captured back-to-back so they represent the same instant: recvTimeNs
                        // (monotonic) drives the local recv->render measurement, recvEpochUs
                        // (wall clock) is compared against the sender's pts_us for the raw
                        // (clock-offset-uncorrected) send->recv latency.
                        val recvTimeNs = System.nanoTime()
                        val recvEpochUs = System.currentTimeMillis() * 1000L
                        listener.onVideoFrame(payload, payloadLen, ptsUs, flags, recvTimeNs, recvEpochUs)
                    }
                    TYPE_HEARTBEAT -> {
                        if (payloadLen > 0) skipFully(input, payloadLen)
                    }
                    else -> {
                        Log.w(TAG, "unknown type=$type, skipping payload")
                        if (payloadLen > 0) skipFully(input, payloadLen)
                    }
                }
            }
        } catch (e: IOException) {
            Log.i(TAG, "session ended: ${e.message}")
        } finally {
            try { socket.close() } catch (_: IOException) { /* ignore */ }
            currentSocket.compareAndSet(socket, null)
            listener.onDisconnected()
        }
    }

    private fun skipFully(input: DataInputStream, len: Int) {
        var remaining = len
        val tmp = ByteArray(minOf(remaining, 8192))
        while (remaining > 0) {
            val n = input.read(tmp, 0, minOf(remaining, tmp.size))
            if (n < 0) throw IOException("eof while skipping payload")
            remaining -= n
        }
    }

    private fun parseHandshake(payload: ByteArray) {
        try {
            val json = org.json.JSONObject(String(payload, Charsets.UTF_8))
            val width = json.optInt("width", 0)
            val height = json.optInt("height", 0)
            val fps = json.optInt("fps", 30)
            val codec = json.optString("codec", "h264")
            Log.i(TAG, "handshake: ${width}x${height}@${fps} codec=$codec")
            listener.onHandshake(width, height, fps, codec)
        } catch (e: Exception) {
            Log.e(TAG, "bad handshake json: $e")
        }
    }
}
