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

    fun uploadGlyphAlpha(pixels: ByteArray, w: Int, h: Int, spread: Int): Boolean =
        nUploadGlyphAlpha(handle, pixels, w, h, spread)

    fun uploadAyahAtlas(
        alpha: ByteArray, w: Int, h: Int, spread: Int,
        cells: IntArray, cellCount: Int,
        quads: FloatArray, quadCount: Int,
    ): Boolean = nUploadAyahAtlas(handle, alpha, w, h, spread, cells, cellCount, quads, quadCount)

    fun setScrollY(y: Float) = nSetScrollY(handle, y)

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
    private external fun nUploadGlyphAlpha(handle: Long, pixels: ByteArray, w: Int, h: Int, spread: Int): Boolean
    private external fun nUploadAyahAtlas(
        handle: Long, alpha: ByteArray, w: Int, h: Int, spread: Int,
        cells: IntArray, cellCount: Int,
        quads: FloatArray, quadCount: Int,
    ): Boolean
    private external fun nSetScrollY(handle: Long, y: Float)
}
