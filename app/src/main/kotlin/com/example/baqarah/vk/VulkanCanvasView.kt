package com.example.baqarah.vk

import android.content.Context
import android.os.HandlerThread
import android.os.Process
import android.util.AttributeSet
import android.util.Log
import android.view.Choreographer
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.OverScroller
import java.util.concurrent.atomic.AtomicBoolean

class VulkanCanvasView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private val renderer = NativeRenderer()
    private val renderThread = HandlerThread("BaqarahVkRender", Process.THREAD_PRIORITY_DISPLAY)
    private val surfaceReady = AtomicBoolean(false)
    private val running = AtomicBoolean(false)
    private lateinit var renderHandler: android.os.Handler

    private sealed interface Pending {
        data class Single(
            val ttfBytes: ByteArray, val codepoint: Int,
            val dstX: Float, val dstY: Float, val dstW: Float, val dstH: Float,
        ) : Pending
        data class Line(
            val ttfBytes: ByteArray, val codepoints: IntArray,
            val cursorX: Float, val baselineY: Float, val fontSizePx: Float,
        ) : Pending
        data class Surah(
            val ttfs: Array<ByteArray>,
            val codepoints: IntArray, val fontIndices: IntArray, val lineStarts: IntArray,
            val screenWidthPx: Float, val leftMarginPx: Float, val rightMarginPx: Float,
            val topMarginPx: Float, val fontSizePx: Float,
            val lineSpacingPx: Float,
        ) : Pending
    }
    private var pending: Pending? = null

    private var scrollY = 0f
    private var maxScrollY = 0f
    private val scroller by lazy { OverScroller(context) }
    private val velocityTracker = android.view.VelocityTracker.obtain()
    private val flingMinVelocity by lazy {
        android.view.ViewConfiguration.get(context).scaledMinimumFlingVelocity.toFloat()
    }
    private val flingMaxVelocity by lazy {
        android.view.ViewConfiguration.get(context).scaledMaximumFlingVelocity.toFloat()
    }
    private var lastTouchY = 0f
    private var dragging = false

    init {
        renderThread.start()
        renderHandler = android.os.Handler(renderThread.looper)
        holder.addCallback(this)
    }

    private val renderLoop = object : Runnable {
        override fun run() {
            if (!running.get()) return
            if (surfaceReady.get()) renderer.drawFrame()
            renderHandler.post(this)
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val surface = holder.surface
        renderHandler.post {
            val ok = renderer.attachSurface(surface)
            Log.i(TAG, "attachSurface ok=$ok")
            surfaceReady.set(ok)
            if (ok) {
                applyPending()
                if (!running.getAndSet(true)) {
                    renderHandler.post(renderLoop)
                }
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val surface = holder.surface
        renderHandler.post {
            renderer.detachSurface()
            val ok = renderer.attachSurface(surface)
            surfaceReady.set(ok)
            Log.i(TAG, "surfaceChanged ${width}x$height ok=$ok")
            if (ok) applyPending()
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady.set(false)
        running.set(false)
        renderHandler.removeCallbacks(renderLoop)
        renderHandler.post { renderer.detachSurface() }
    }

    fun setColrGlyph(
        ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ) {
        pending = Pending.Single(ttfBytes, codepoint, dstX, dstY, dstW, dstH)
        renderHandler.post { if (surfaceReady.get()) applyPending() }
    }

    fun setColrLine(
        ttfBytes: ByteArray, codepoints: IntArray,
        cursorX: Float, baselineY: Float, fontSizePx: Float,
    ) {
        pending = Pending.Line(ttfBytes, codepoints, cursorX, baselineY, fontSizePx)
        renderHandler.post { if (surfaceReady.get()) applyPending() }
    }

    /**
     * Schedule the surah upload on the render thread. The lambda is called
     * after the renderer finishes with the total content height (px, or -1
     * on failure) so the caller can configure scroll bounds.
     */
    fun setColrSurah(
        ttfs: Array<ByteArray>,
        codepoints: IntArray, fontIndices: IntArray, lineStarts: IntArray,
        screenWidthPx: Float, leftMarginPx: Float, rightMarginPx: Float,
        topMarginPx: Float, fontSizePx: Float,
        lineSpacingPx: Float,
        onUploaded: (totalHeightPx: Float) -> Unit = {},
    ) {
        val p = Pending.Surah(
            ttfs, codepoints, fontIndices, lineStarts,
            screenWidthPx, leftMarginPx, rightMarginPx,
            topMarginPx, fontSizePx, lineSpacingPx,
        )
        pending = p
        renderHandler.post {
            if (surfaceReady.get()) {
                val h = renderer.uploadColrSurah(
                    p.ttfs, p.codepoints, p.fontIndices, p.lineStarts,
                    p.screenWidthPx, p.leftMarginPx, p.rightMarginPx,
                    p.topMarginPx, p.fontSizePx, p.lineSpacingPx,
                )
                Log.i(TAG, "uploadColrSurah h=$h")
                post { onUploaded(h) }
            }
        }
    }

    fun setContentHeight(totalContentPx: Float) {
        maxScrollY = maxOf(0f, totalContentPx - height.toFloat())
        scrollY = scrollY.coerceIn(0f, maxScrollY)
        renderer.setScrollY(scrollY)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        velocityTracker.addMovement(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (!scroller.isFinished) scroller.forceFinished(true)
                lastTouchY = event.y
                dragging = true
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging) return false
                val dy = lastTouchY - event.y
                lastTouchY = event.y
                applyScrollDelta(dy)
                return true
            }
            MotionEvent.ACTION_UP -> {
                dragging = false
                velocityTracker.computeCurrentVelocity(1000, flingMaxVelocity)
                val vy = -velocityTracker.yVelocity
                velocityTracker.clear()
                if (kotlin.math.abs(vy) > flingMinVelocity) {
                    scroller.fling(
                        0, scrollY.toInt(), 0, vy.toInt(),
                        0, 0, 0, maxScrollY.toInt(),
                    )
                    Choreographer.getInstance().postFrameCallback(flingCallback)
                }
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                dragging = false
                velocityTracker.clear()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun applyScrollDelta(dy: Float) {
        scrollY = (scrollY + dy).coerceIn(0f, maxScrollY)
        renderer.setScrollY(scrollY)
    }

    private val flingCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (scroller.computeScrollOffset()) {
                scrollY = scroller.currY.toFloat().coerceIn(0f, maxScrollY)
                renderer.setScrollY(scrollY)
                Choreographer.getInstance().postFrameCallback(this)
            }
        }
    }

    private fun applyPending() {
        when (val p = pending) {
            is Pending.Single -> {
                val ok = renderer.uploadColrFromTtf(p.ttfBytes, p.codepoint, p.dstX, p.dstY, p.dstW, p.dstH)
                Log.i(TAG, "uploadColrFromTtf ok=$ok cp=U+${p.codepoint.toString(16)}")
            }
            is Pending.Line -> {
                val ok = renderer.uploadColrLineFromTtf(p.ttfBytes, p.codepoints, p.cursorX, p.baselineY, p.fontSizePx)
                Log.i(TAG, "uploadColrLineFromTtf ok=$ok cps=${p.codepoints.size}")
            }
            is Pending.Surah -> {
                // Surah upload is handled inline in setColrSurah to capture
                // the returned content height; nothing to re-apply here.
            }
            null -> Unit
        }
    }

    fun release() {
        running.set(false)
        Choreographer.getInstance().removeFrameCallback(flingCallback)
        renderHandler.removeCallbacks(renderLoop)
        velocityTracker.recycle()
        renderHandler.post {
            renderer.detachSurface()
            renderer.release()
            renderThread.quitSafely()
        }
    }

    companion object {
        private const val TAG = "BaqarahVkView"
    }
}
