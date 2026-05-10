package com.example.baqarah.data

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File

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

    private class Pending(
        val key: Long,
        val typeface: Typeface,
        val text: String,
        val fontSizePx: Float,
        val advance: Float,
        val width: Int,
        val height: Int,
        val drawOffsetX: Float,
        val drawOffsetY: Float,
        val originX: Float,
        val originY: Float,
    )

    private val refs = HashMap<Long, GlyphRef>()
    private val pending = HashMap<Long, Pending>()
    private val pages = ArrayList<Bitmap>()
    private val finalizedImages = ArrayList<ImageBitmap>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { isSubpixelText = true }
    private var sealed = false

    val pagesCount: Int get() = if (sealed) finalizedImages.size else pages.size
    fun page(i: Int): ImageBitmap = finalizedImages[i]

    @Synchronized
    fun add(page: Int, codepoint: Int, fontSizePx: Float, typeface: Typeface) {
        check(!sealed) { "Atlas already sealed" }
        val sizeBucket = fontSizePx.toInt().coerceIn(1, 0xFFFF)
        val key = packKey(page, codepoint, sizeBucket)
        if (pending.containsKey(key)) return

        paint.typeface = typeface
        paint.textSize = fontSizePx
        val text = String(Character.toChars(codepoint))
        val bounds = Rect()
        paint.getTextBounds(text, 0, text.length, bounds)
        val advance = paint.measureText(text)

        val pad = 1
        val w = bounds.width() + 2 * pad
        val h = bounds.height() + 2 * pad
        if (w <= 0 || h <= 0 || w > pageSize || h > pageSize) return

        pending[key] = Pending(
            key = key,
            typeface = typeface,
            text = text,
            fontSizePx = fontSizePx,
            advance = advance,
            width = w,
            height = h,
            drawOffsetX = (-bounds.left + pad).toFloat(),
            drawOffsetY = (-bounds.top + pad).toFloat(),
            originX = (bounds.left - pad).toFloat(),
            originY = (bounds.top - pad).toFloat(),
        )
    }

    fun get(page: Int, codepoint: Int, fontSizePx: Float): GlyphRef? {
        val sizeBucket = fontSizePx.toInt().coerceIn(1, 0xFFFF)
        return refs[packKey(page, codepoint, sizeBucket)]
    }

    @Synchronized
    fun seal(saveDir: File? = null) {
        if (sealed) return
        sealed = true

        val sorted = pending.values.sortedWith(
            compareByDescending<Pending> { it.height }.thenByDescending { it.width }
        )

        var pageIndex = -1
        var cursorX = 0
        var cursorY = 0
        var rowHeight = 0
        var canvas: Canvas? = null

        for (g in sorted) {
            if (pageIndex < 0 || cursorX + g.width > pageSize) {
                cursorX = 0
                cursorY += rowHeight
                rowHeight = 0
            }
            if (pageIndex < 0 || cursorY + g.height > pageSize) {
                val bmp = Bitmap.createBitmap(pageSize, pageSize, Bitmap.Config.ARGB_8888)
                pages.add(bmp)
                canvas = Canvas(bmp)
                pageIndex = pages.size - 1
                cursorX = 0
                cursorY = 0
                rowHeight = 0
            }
            paint.typeface = g.typeface
            paint.textSize = g.fontSizePx
            canvas!!.drawText(g.text, cursorX + g.drawOffsetX, cursorY + g.drawOffsetY, paint)
            refs[g.key] = GlyphRef(
                atlasIndex = pageIndex,
                srcX = cursorX,
                srcY = cursorY,
                width = g.width,
                height = g.height,
                advance = g.advance,
                originX = g.originX,
                originY = g.originY,
            )
            cursorX += g.width
            if (g.height > rowHeight) rowHeight = g.height
        }

        pending.clear()

        if (saveDir != null) {
            saveDir.mkdirs()
            writeIndexTo(File(saveDir, FILE_INDEX))
            for ((i, sw) in pages.withIndex()) {
                File(saveDir, atlasFileName(i)).outputStream().use { out ->
                    sw.compress(Bitmap.CompressFormat.PNG, 100, out)
                }
            }
        }

        for (sw in pages) {
            val hw = sw.copy(Bitmap.Config.HARDWARE, false)
            if (hw != null) {
                finalizedImages.add(hw.asImageBitmap())
                sw.recycle()
            } else {
                finalizedImages.add(sw.asImageBitmap())
            }
        }
        pages.clear()
    }

    private fun writeIndexTo(file: File) {
        DataOutputStream(BufferedOutputStream(file.outputStream())).use { out ->
            out.writeInt(VERSION)
            out.writeInt(refs.size)
            for ((key, r) in refs) {
                out.writeLong(key)
                out.writeInt(r.atlasIndex)
                out.writeInt(r.srcX)
                out.writeInt(r.srcY)
                out.writeInt(r.width)
                out.writeInt(r.height)
                out.writeFloat(r.advance)
                out.writeFloat(r.originX)
                out.writeFloat(r.originY)
            }
        }
    }

    private fun packKey(page: Int, codepoint: Int, sizeBucket: Int): Long =
        (sizeBucket.toLong() shl 48) or
            ((page.toLong() and 0xFFFF) shl 32) or
            (codepoint.toLong() and 0xFFFFFFFFL)

    companion object {
        private const val VERSION = 1
        private const val FILE_INDEX = "glyph_index.bin"

        private fun atlasFileName(i: Int) = "atlas_$i.png"

        suspend fun loadFrom(dir: File): GlyphAtlas? = coroutineScope {
            val indexFile = File(dir, FILE_INDEX)
            if (!indexFile.exists()) return@coroutineScope null
            val atlas = GlyphAtlas()
            DataInputStream(BufferedInputStream(indexFile.inputStream())).use { inp ->
                val version = inp.readInt()
                if (version != VERSION) return@coroutineScope null
                val count = inp.readInt()
                repeat(count) {
                    val key = inp.readLong()
                    atlas.refs[key] = GlyphRef(
                        atlasIndex = inp.readInt(),
                        srcX = inp.readInt(),
                        srcY = inp.readInt(),
                        width = inp.readInt(),
                        height = inp.readInt(),
                        advance = inp.readFloat(),
                        originX = inp.readFloat(),
                        originY = inp.readFloat(),
                    )
                }
            }
            val pngFiles = mutableListOf<File>()
            var i = 0
            while (true) {
                val f = File(dir, atlasFileName(i))
                if (!f.exists()) break
                pngFiles.add(f); i++
            }
            if (pngFiles.isEmpty()) return@coroutineScope null
            val opts = BitmapFactory.Options().apply {
                inPreferredConfig = Bitmap.Config.HARDWARE
            }
            val deferred = pngFiles.map { f ->
                async(Dispatchers.IO) {
                    BitmapFactory.decodeFile(f.absolutePath, opts)?.asImageBitmap()
                }
            }
            val images = deferred.map { it.await() ?: return@coroutineScope null }
            atlas.finalizedImages.addAll(images)
            atlas.sealed = true
            atlas
        }
    }
}
