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
     * Resolve a COLR composite glyph from the supplied TTF: parse the
     * codepoint's base glyph index, look up its colour layers from the
     * COLR/CPAL tables, extract each layer's outline, and upload them as
     * N stacked quads to the renderer. Falls back to a single white
     * layer using the base glyph itself when the font has no COLR data.
     */
    fun uploadColrFromTtf(
        ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean = nUploadColrFromTtf(handle, ttfBytes, codepoint, dstX, dstY, dstW, dstH)

    /**
     * Lay out N codepoints right-to-left from (cursorX, baselineY) and
     * render each via the COLR pipeline. Single draw call.
     */
    fun uploadColrLineFromTtf(
        ttfBytes: ByteArray, codepoints: IntArray,
        cursorX: Float, baselineY: Float, fontSizePx: Float,
    ): Boolean = nUploadColrLineFromTtf(handle, ttfBytes, codepoints, cursorX, baselineY, fontSizePx)

    /**
     * Render an entire surah. Each codepoint picks its TTF via
     * fontIndices[i] (parallel array); lineStarts[] partitions codepoints
     * into Mushaf-page lines (last entry = total codepoint count). Each
     * line is laid out on its own screen row using the glyphs' natural
     * advances — QPC v4 glyph metrics are calibrated per Mushaf line.
     * If `firstLineDecorate` is true the first line is centered and a
     * procedural Mushaf-style frame is drawn around it. Returns the
     * total content height in pixels (use to size the scroll range), or
     * -1 on failure.
     */
    fun uploadColrSurah(
        ttfs: Array<ByteArray>,
        codepoints: IntArray, fontIndices: IntArray, lineStarts: IntArray,
        screenWidthPx: Float, leftMarginPx: Float, rightMarginPx: Float,
        topMarginPx: Float, fontSizePx: Float,
        lineSpacingPx: Float,
        firstLineDecorate: Boolean,
        frameSeed: Int,
    ): Float = nUploadColrSurah(
        handle, ttfs, codepoints, fontIndices, lineStarts,
        screenWidthPx, leftMarginPx, rightMarginPx, topMarginPx, fontSizePx,
        lineSpacingPx, firstLineDecorate, frameSeed,
    )

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
    private external fun nUploadColrFromTtf(
        handle: Long, ttfBytes: ByteArray, codepoint: Int,
        dstX: Float, dstY: Float, dstW: Float, dstH: Float,
    ): Boolean
    private external fun nUploadColrLineFromTtf(
        handle: Long, ttfBytes: ByteArray, codepoints: IntArray,
        cursorX: Float, baselineY: Float, fontSizePx: Float,
    ): Boolean
    private external fun nUploadColrSurah(
        handle: Long,
        ttfs: Array<ByteArray>,
        codepoints: IntArray, fontIndices: IntArray, lineStarts: IntArray,
        screenWidthPx: Float, leftMarginPx: Float, rightMarginPx: Float,
        topMarginPx: Float, fontSizePx: Float,
        lineSpacingPx: Float,
        firstLineDecorate: Boolean,
        frameSeed: Int,
    ): Float
}
