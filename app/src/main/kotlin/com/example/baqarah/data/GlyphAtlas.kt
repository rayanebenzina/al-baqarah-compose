package com.example.baqarah.data

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap

data class GlyphRef(
    val atlasIndex: Int,
    val srcX: Int,
    val srcY: Int,
    val width: Int,
    val height: Int,
    val advance: Float,
    val originX: Float,
    val originY: Float,
)

class GlyphAtlas(private val pageSize: Int = 4096) {

    private val refs = HashMap<Long, GlyphRef>()
    private val pages = ArrayList<Bitmap>()
    private val canvases = ArrayList<Canvas>()
    private val finalizedImages = ArrayList<ImageBitmap>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { isSubpixelText = true }

    private var pageIndex = -1
    private var cursorX = 0
    private var cursorY = 0
    private var rowHeight = 0
    private var sealed = false

    val pagesCount: Int get() = if (sealed) finalizedImages.size else pages.size
    fun page(i: Int): ImageBitmap = finalizedImages[i]

    @Synchronized
    fun add(page: Int, codepoint: Int, fontSizePx: Float, typeface: Typeface) {
        check(!sealed) { "Atlas already sealed" }
        val sizeBucket = fontSizePx.toInt().coerceIn(1, 0xFFFF)
        val key = packKey(page, codepoint, sizeBucket)
        if (refs.containsKey(key)) return

        paint.typeface = typeface
        paint.textSize = fontSizePx
        val text = String(Character.toChars(codepoint))
        val bounds = Rect()
        paint.getTextBounds(text, 0, text.length, bounds)
        val advance = paint.measureText(text)

        val pad = 1
        val w = (bounds.width() + 2 * pad)
        val h = (bounds.height() + 2 * pad)
        if (w <= 0 || h <= 0) return
        if (w > pageSize || h > pageSize) return

        if (pageIndex < 0 || cursorX + w > pageSize) {
            cursorX = 0
            cursorY += rowHeight
            rowHeight = 0
        }
        if (pageIndex < 0 || cursorY + h > pageSize) {
            allocatePage()
        }

        val drawX = (cursorX - bounds.left + pad).toFloat()
        val drawY = (cursorY - bounds.top + pad).toFloat()
        canvases[pageIndex].drawText(text, drawX, drawY, paint)

        refs[key] = GlyphRef(
            atlasIndex = pageIndex,
            srcX = cursorX,
            srcY = cursorY,
            width = w,
            height = h,
            advance = advance,
            originX = (bounds.left - pad).toFloat(),
            originY = (bounds.top - pad).toFloat(),
        )
        cursorX += w
        if (h > rowHeight) rowHeight = h
    }

    fun get(page: Int, codepoint: Int, fontSizePx: Float): GlyphRef? {
        val sizeBucket = fontSizePx.toInt().coerceIn(1, 0xFFFF)
        return refs[packKey(page, codepoint, sizeBucket)]
    }

    @Synchronized
    fun seal() {
        if (sealed) return
        sealed = true
        for (sw in pages) {
            val hw = sw.copy(Bitmap.Config.HARDWARE, false)
            if (hw != null) {
                finalizedImages.add(hw.asImageBitmap())
                sw.recycle()
            } else {
                finalizedImages.add(sw.asImageBitmap())
            }
        }
        canvases.clear()
        pages.clear()
    }

    private fun allocatePage() {
        val bmp = Bitmap.createBitmap(pageSize, pageSize, Bitmap.Config.ARGB_8888)
        pages.add(bmp)
        canvases.add(Canvas(bmp))
        pageIndex = pages.size - 1
        cursorX = 0
        cursorY = 0
        rowHeight = 0
    }

    private fun packKey(page: Int, codepoint: Int, sizeBucket: Int): Long =
        (sizeBucket.toLong() shl 48) or
            ((page.toLong() and 0xFFFF) shl 32) or
            (codepoint.toLong() and 0xFFFFFFFFL)
}
