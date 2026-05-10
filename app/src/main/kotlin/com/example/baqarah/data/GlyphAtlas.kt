package com.example.baqarah.data

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import androidx.compose.runtime.MutableIntState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
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

class GlyphAtlas(private val pageSize: Int = 2048) {

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
    private val pending = LinkedHashMap<Long, Pending>()
    private val pages = ArrayList<Bitmap>()
    private val finalizedImages = ArrayList<ImageBitmap?>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { isSubpixelText = true }
    private var sealed = false
    private val versionState: MutableIntState = mutableIntStateOf(0)

    val pagesCount: Int get() = if (sealed) finalizedImages.size else pages.size
    fun page(i: Int): ImageBitmap? = finalizedImages.getOrNull(i)
    val version: MutableIntState get() = versionState

    @Synchronized
    private fun setPage(i: Int, img: ImageBitmap) {
        while (finalizedImages.size <= i) finalizedImages.add(null)
        finalizedImages[i] = img
        versionState.intValue++
    }

    fun installPage(i: Int, img: ImageBitmap) = setPage(i, img)

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

        // Insertion-order packing for spatial locality (early ayah glyphs land on
        // early atlas pages — important for the lazy-loading priority window to
        // actually be a subset). Drop in-bucket height-sort because glyph heights
        // are nearly uniform after sorting; the locality win matters more.
        val sorted = pending.values.toList()

        var skyline = Skyline(pageSize)
        var pageIndex = -1
        var canvas: Canvas? = null

        for (g in sorted) {
            if (pageIndex < 0) {
                val bmp = Bitmap.createBitmap(pageSize, pageSize, Bitmap.Config.ARGB_8888)
                pages.add(bmp)
                canvas = Canvas(bmp)
                pageIndex = pages.size - 1
                skyline = Skyline(pageSize)
            }
            var pos = skyline.place(g.width, g.height, pageSize)
            if (pos == null) {
                val bmp = Bitmap.createBitmap(pageSize, pageSize, Bitmap.Config.ARGB_8888)
                pages.add(bmp)
                canvas = Canvas(bmp)
                pageIndex = pages.size - 1
                skyline = Skyline(pageSize)
                pos = skyline.place(g.width, g.height, pageSize) ?: continue
            }
            val (x, y) = pos
            paint.typeface = g.typeface
            paint.textSize = g.fontSizePx
            canvas!!.drawText(g.text, x + g.drawOffsetX, y + g.drawOffsetY, paint)
            refs[g.key] = GlyphRef(
                atlasIndex = pageIndex,
                srcX = x,
                srcY = y,
                width = g.width,
                height = g.height,
                advance = g.advance,
                originX = g.originX,
                originY = g.originY,
            )
        }

        pending.clear()

        val usedPages = (refs.values.maxOfOrNull { it.atlasIndex } ?: -1) + 1
        while (pages.size > usedPages) {
            pages.removeAt(pages.size - 1).recycle()
        }

        if (saveDir != null) {
            saveDir.mkdirs()
            writeIndexTo(File(saveDir, FILE_INDEX))
            for ((i, sw) in pages.withIndex()) {
                File(saveDir, atlasFileName(i, "png")).outputStream().use { out ->
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
        versionState.intValue++
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

    private class Skyline(private val width: Int) {
        private val xs = ArrayList<Int>()
        private val ys = ArrayList<Int>()
        private val ws = ArrayList<Int>()
        init { xs.add(0); ys.add(0); ws.add(width) }

        fun place(w: Int, h: Int, maxHeight: Int): Pair<Int, Int>? {
            var bestY = Int.MAX_VALUE
            var bestX = -1
            for (i in xs.indices) {
                val segX = xs[i]
                if (segX + w > width) break
                val topY = maxYOver(i, w) ?: continue
                if (topY + h > maxHeight) continue
                if (topY < bestY || (topY == bestY && segX < bestX)) {
                    bestY = topY
                    bestX = segX
                }
            }
            if (bestX < 0) return null
            insertSegment(bestX, bestY + h, w)
            return bestX to bestY
        }

        private fun maxYOver(startIdx: Int, w: Int): Int? {
            var widthLeft = w
            var maxY = 0
            var i = startIdx
            while (widthLeft > 0 && i < xs.size) {
                if (ys[i] > maxY) maxY = ys[i]
                widthLeft -= ws[i]
                i++
            }
            return if (widthLeft <= 0) maxY else null
        }

        private fun insertSegment(newX: Int, newY: Int, newW: Int) {
            val nx = ArrayList<Int>(xs.size + 2)
            val ny = ArrayList<Int>(xs.size + 2)
            val nw = ArrayList<Int>(xs.size + 2)
            var inserted = false
            for (i in xs.indices) {
                val sx = xs[i]; val sy = ys[i]; val sw = ws[i]
                val ex = sx + sw
                val nex = newX + newW
                if (ex <= newX || sx >= nex) {
                    if (!inserted && sx >= newX) {
                        nx.add(newX); ny.add(newY); nw.add(newW)
                        inserted = true
                    }
                    nx.add(sx); ny.add(sy); nw.add(sw)
                } else {
                    if (sx < newX) {
                        nx.add(sx); ny.add(sy); nw.add(newX - sx)
                    }
                    if (!inserted) {
                        nx.add(newX); ny.add(newY); nw.add(newW)
                        inserted = true
                    }
                    if (ex > nex) {
                        nx.add(nex); ny.add(sy); nw.add(ex - nex)
                    }
                }
            }
            if (!inserted) { nx.add(newX); ny.add(newY); nw.add(newW) }

            xs.clear(); ys.clear(); ws.clear()
            var i = 0
            while (i < nx.size) {
                var x = nx[i]; var y = ny[i]; var ww = nw[i]
                while (i + 1 < nx.size && ny[i + 1] == y && nx[i + 1] == x + ww) {
                    ww += nw[i + 1]
                    i++
                }
                xs.add(x); ys.add(y); ws.add(ww)
                i++
            }
        }
    }

    private fun packKey(page: Int, codepoint: Int, sizeBucket: Int): Long =
        (sizeBucket.toLong() shl 48) or
            ((page.toLong() and 0xFFFF) shl 32) or
            (codepoint.toLong() and 0xFFFFFFFFL)

    companion object {
        private const val VERSION = 7
        private const val FILE_INDEX = "glyph_index.bin"

        private fun atlasFileName(i: Int, ext: String = "png") = "atlas_$i.$ext"

        /** Read just the glyph index (no PNG decode). Returns an atlas with empty pageStates. */
        fun readIndex(dir: File): Pair<GlyphAtlas, List<File>>? {
            val indexFile = File(dir, FILE_INDEX)
            if (!indexFile.exists()) return null
            val atlas = GlyphAtlas()
            DataInputStream(BufferedInputStream(indexFile.inputStream())).use { inp ->
                val v = inp.readInt()
                if (v != VERSION) return null
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
                val webp = File(dir, atlasFileName(i, "webp"))
                val png = File(dir, atlasFileName(i, "png"))
                val f = when {
                    webp.exists() && webp.length() > 0 -> webp
                    png.exists() && png.length() > 0 -> png
                    else -> null
                } ?: break
                pngFiles.add(f); i++
            }
            if (pngFiles.isEmpty()) return null
            for (idx in pngFiles.indices) atlas.finalizedImages.add(null)
            atlas.sealed = true
            return atlas to pngFiles
        }

        /** Block on priority page decode (parallel). Stream remaining in [backgroundScope]. */
        suspend fun decodeProgressive(
            atlas: GlyphAtlas,
            pngFiles: List<File>,
            priorityPageIndices: IntArray,
            backgroundScope: CoroutineScope,
        ) = coroutineScope {
            val opts = BitmapFactory.Options().apply {
                inPreferredConfig = Bitmap.Config.HARDWARE
            }
            val prioritySet = priorityPageIndices.toHashSet().filter { it in pngFiles.indices }.toHashSet()

            if (prioritySet.isNotEmpty()) {
                prioritySet.map { idx ->
                    async(Dispatchers.IO) {
                        BitmapFactory.decodeFile(pngFiles[idx].absolutePath, opts)?.let {
                            atlas.setPage(idx, it.asImageBitmap())
                        }
                    }
                }.awaitAll()
            }

            val remaining = pngFiles.indices.filter { it !in prioritySet && atlas.page(it) == null }
            if (remaining.isNotEmpty()) {
                backgroundScope.launch(Dispatchers.IO) {
                    remaining.map { idx ->
                        async {
                            BitmapFactory.decodeFile(pngFiles[idx].absolutePath, opts)?.let {
                                atlas.setPage(idx, it.asImageBitmap())
                            }
                        }
                    }.awaitAll()
                }
            }
        }
    }
}
