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
) : SurfaceView(context, attrs), SurfaceHolder.Callback, Choreographer.FrameCallback {

    private val renderer = NativeRenderer()
    private val renderThread = HandlerThread("BaqarahVkRender", Process.THREAD_PRIORITY_DISPLAY)
    private val surfaceReady = AtomicBoolean(false)
    private val running = AtomicBoolean(false)
    private lateinit var renderHandler: android.os.Handler

    private data class PendingColr(
        val ttfBytes: ByteArray, val codepoint: Int,
        val dstX: Float, val dstY: Float, val dstW: Float, val dstH: Float,
    )
    private var pending: PendingColr? = null

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

    override fun surfaceCreated(holder: SurfaceHolder) {
        val surface = holder.surface
        renderHandler.post {
            val ok = renderer.attachSurface(surface)
            Log.i(TAG, "attachSurface ok=$ok")
            surfaceReady.set(ok)
            if (ok) {
                applyPending()
                if (!running.getAndSet(true)) {
                    post { Choreographer.getInstance().postFrameCallback(this) }
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
        Choreographer.getInstance().removeFrameCallback(this)
        renderHandler.post { renderer.detachSurface() }
    }

    override fun doFrame(frameTimeNanos: Long) {
        if (!running.get()) return
        Choreographer.getInstance().postFrameCallback(this)
        if (!surfaceReady.get()) return
        renderHandler.post {
            if (surfaceReady.get()) renderer.drawFrame()
        }
    }

    fun setColrGlyph(
        ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ) {
        pending = PendingColr(ttfBytes, codepoint, dstX, dstY, dstW, dstH)
        renderHandler.post { if (surfaceReady.get()) applyPending() }
    }

    fun setContentHeight(totalContentPx: Float) {
        maxScrollY = maxOf(0f, totalContentPx - height.toFloat())
        scrollY = scrollY.coerceIn(0f, maxScrollY)
        renderHandler.post { renderer.setScrollY(scrollY) }
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
        renderHandler.post { renderer.setScrollY(scrollY) }
    }

    private val flingCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (scroller.computeScrollOffset()) {
                scrollY = scroller.currY.toFloat().coerceIn(0f, maxScrollY)
                renderHandler.post { renderer.setScrollY(scrollY) }
                Choreographer.getInstance().postFrameCallback(this)
            }
        }
    }

    private fun applyPending() {
        val p = pending ?: return
        val ok = renderer.uploadColrFromTtf(p.ttfBytes, p.codepoint, p.dstX, p.dstY, p.dstW, p.dstH)
        Log.i(TAG, "uploadColrFromTtf ok=$ok cp=U+${p.codepoint.toString(16)}")
    }

    fun release() {
        running.set(false)
        Choreographer.getInstance().removeFrameCallback(this)
        Choreographer.getInstance().removeFrameCallback(flingCallback)
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
