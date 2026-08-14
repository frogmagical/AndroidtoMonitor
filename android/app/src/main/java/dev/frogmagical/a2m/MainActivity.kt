package dev.frogmagical.a2m

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.TextView

/**
 * M1 receiver MVP: TCP listen -> MediaCodec Surface decode -> fullscreen SurfaceView.
 * See docs/PROTOCOL.md (wire format) and docs/REQUIREMENTS.md §4.3 (decode requirements).
 */
class MainActivity : Activity(), A2mListener, SurfaceHolder.Callback {

    companion object {
        private const val PORT = 5001
    }

    private lateinit var statusText: TextView
    private lateinit var surfaceView: SurfaceView

    private var server: TcpServer? = null
    private var decoder: DecoderManager? = null
    private val stats = PerfStats()

    @Volatile private var connected = false
    @Volatile private var lastWidth = 0
    @Volatile private var lastHeight = 0
    @Volatile private var lastFps = 0

    private val uiHandler = Handler(Looper.getMainLooper())
    private val statusUpdater = object : Runnable {
        override fun run() {
            updateStatusText()
            uiHandler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyImmersiveFullscreen()
        setContentView(R.layout.activity_main)

        surfaceView = findViewById(R.id.surfaceView)
        statusText = findViewById(R.id.statusText)
        surfaceView.holder.addCallback(this)

        stats.start()
        uiHandler.post(statusUpdater)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) applyImmersiveFullscreen()
    }

    @Suppress("DEPRECATION")
    private fun applyImmersiveFullscreen() {
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            )
    }

    override fun onDestroy() {
        super.onDestroy()
        stats.stop()
        uiHandler.removeCallbacks(statusUpdater)
        server?.shutdown()
        server = null
        decoder?.release()
        decoder = null
    }

    // ---- SurfaceHolder.Callback ----

    override fun surfaceCreated(holder: SurfaceHolder) {
        decoder = DecoderManager(holder.surface, stats)
        server = TcpServer(PORT, this).apply { start() }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        server?.shutdown()
        server = null
        decoder?.release()
        decoder = null
    }

    // ---- A2mListener (invoked from network/session threads, not the UI thread) ----

    override fun onHandshake(width: Int, height: Int, fps: Int, codec: String) {
        lastWidth = width
        lastHeight = height
        lastFps = fps
        decoder?.configure(width, height, fps)
    }

    override fun onVideoFrame(data: ByteArray, length: Int, ptsUs: Long, flags: Int, recvTimeNs: Long, recvEpochUs: Long) {
        stats.recordBytes(length)
        stats.recordPtsToRecv(ptsUs, recvEpochUs)
        decoder?.onVideoFrame(data, length, ptsUs, flags)
    }

    override fun onConnected() {
        connected = true
    }

    override fun onDisconnected() {
        connected = false
        decoder?.reset()
    }

    // ---- UI ----

    private fun updateStatusText() {
        val text = if (connected) {
            "A2M connected %dx%d@%d | fps=%.0f p50=%.1fms p95=%.1fms drops=%d br=%.0fkbps\nrawSend->recv p50=%.1fms p95=%.1fms (uncalibrated)".format(
                lastWidth, lastHeight, lastFps,
                stats.lastFps, stats.lastP50Ms, stats.lastP95Ms, stats.lastDrops, stats.lastBitrateKbps,
                stats.lastPtsToRecvP50Ms, stats.lastPtsToRecvP95Ms
            )
        } else {
            "A2M waiting on 127.0.0.1:$PORT"
        }
        statusText.text = text
    }
}
