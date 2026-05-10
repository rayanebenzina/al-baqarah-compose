package com.example.baqarah.vk

import android.content.Context
import android.os.HandlerThread
import android.os.Process
import android.util.AttributeSet
import android.util.Log
import android.view.Choreographer
import android.view.SurfaceHolder
import android.view.SurfaceView
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
            if (ok && !running.getAndSet(true)) {
                post { Choreographer.getInstance().postFrameCallback(this) }
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

    fun release() {
        running.set(false)
        Choreographer.getInstance().removeFrameCallback(this)
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
