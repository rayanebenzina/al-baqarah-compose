package com.example.baqarah.vk

import android.view.Surface

class NativeRenderer {

    private var handle: Long = 0L

    init {
        System.loadLibrary("baqarah_vk")
        handle = nCreate()
    }

    fun attachSurface(surface: Surface): Boolean = nAttachSurface(handle, surface)

    fun detachSurface() = nDetachSurface(handle)

    fun drawFrame(): Boolean = nDrawFrame(handle)

    fun setScrollY(y: Float) = nSetScrollY(handle, y)

    /**
     * Upload a quadratic-Bezier outline as the current renderable glyph.
     * `curves` holds `curveCount * 6` floats: (p0.x, p0.y, p1.x, p1.y,
     * p2.x, p2.y) per curve. The shape is drawn into the screen-space
     * rectangle (dstX, dstY)..(dstX + dstW, dstY + dstH) with UV
     * sweeping (0, 0) to (1, 1).
     */
    fun uploadOutlineGlyph(
        curves: FloatArray, curveCount: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean = nUploadOutlineGlyph(handle, curves, curveCount, dstX, dstY, dstW, dstH)

    /** Extract a glyph outline from a TTF (using stb_truetype) and upload. */
    fun uploadOutlineFromTtf(
        ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean = nUploadOutlineFromTtf(handle, ttfBytes, codepoint, dstX, dstY, dstW, dstH)

    fun release() {
        if (handle != 0L) {
            nDestroy(handle)
            handle = 0L
        }
    }

    private external fun nCreate(): Long
    private external fun nDestroy(handle: Long)
    private external fun nAttachSurface(handle: Long, surface: Surface): Boolean
    private external fun nDetachSurface(handle: Long)
    private external fun nDrawFrame(handle: Long): Boolean
    private external fun nSetScrollY(handle: Long, y: Float)
    private external fun nUploadOutlineGlyph(
        handle: Long, curves: FloatArray, curveCount: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean
    private external fun nUploadOutlineFromTtf(
        handle: Long, ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean
}
