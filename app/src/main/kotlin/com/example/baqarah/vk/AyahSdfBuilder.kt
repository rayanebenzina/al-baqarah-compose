package com.example.baqarah.vk

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import com.example.baqarah.data.Verse
import kotlin.math.max
import kotlin.math.sqrt

class AyahSdfBuilder {

    data class Result(
        val atlasAlpha: ByteArray,
        val atlasW: Int,
        val atlasH: Int,
        /** 8 floats per quad: [dstX, dstY, dstW, dstH, u0, v0, u1, v1] */
        val quads: FloatArray,
        val quadCount: Int,
        val totalHeightPx: Float,
    )

    fun build(
        verse: Verse,
        typefaces: Map<Int, Typeface>,
        fontSizePx: Float,
        screenWidthPx: Float,
        padding: Int = 12,
    ): Result {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            isSubpixelText = true
            color = Color.WHITE
            textSize = fontSizePx
        }

        val unique = LinkedHashMap<Long, Pending>()
        for (word in verse.words) {
            val code = word.codeV2 ?: continue
            val tf = typefaces[word.pageNumber] ?: continue
            paint.typeface = tf
            var i = 0
            while (i < code.length) {
                val cp = code.codePointAt(i)
                val key = (word.pageNumber.toLong() shl 32) or cp.toLong()
                if (key !in unique) {
                    val text = String(Character.toChars(cp))
                    val bounds = Rect()
                    paint.getTextBounds(text, 0, text.length, bounds)
                    val advance = paint.measureText(text)
                    val w = bounds.width() + 2 * padding
                    val h = bounds.height() + 2 * padding
                    if (w > 0 && h > 0) {
                        unique[key] = Pending(
                            pageNumber = word.pageNumber,
                            codepoint = cp,
                            typeface = tf,
                            text = text,
                            w = w, h = h,
                            drawOffsetX = (-bounds.left + padding).toFloat(),
                            drawOffsetY = (-bounds.top + padding).toFloat(),
                            advance = advance,
                            originX = (bounds.left - padding).toFloat(),
                            originY = (bounds.top - padding).toFloat(),
                        )
                    }
                }
                i += Character.charCount(cp)
            }
        }

        val sortedByHeight = unique.values.sortedByDescending { it.h }
        val totalArea = sortedByHeight.sumOf { it.w * it.h }
        val approxSide = sqrt(totalArea.toFloat() * 1.4f).toInt()
        var atlasW = 64
        while (atlasW < approxSide) atlasW *= 2
        val placements: HashMap<Long, IntArray>
        var atlasH: Int
        while (true) {
            val pm = HashMap<Long, IntArray>(unique.size)
            val packer = ShelfPacker(atlasW)
            var ok = true
            for (g in sortedByHeight) {
                val pos = packer.place(g.w, g.h)
                if (pos == null) {
                    ok = false; break
                }
                val key = (g.pageNumber.toLong() shl 32) or g.codepoint.toLong()
                pm[key] = pos
            }
            if (ok) {
                placements = pm
                atlasH = nextPow2(packer.totalHeight)
                break
            }
            atlasW *= 2
            check(atlasW <= 4096) { "atlas pack failed: too large" }
        }

        val atlasBmp = Bitmap.createBitmap(atlasW, atlasH, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(atlasBmp)
        for (g in unique.values) {
            val key = (g.pageNumber.toLong() shl 32) or g.codepoint.toLong()
            val pos = placements[key] ?: continue
            paint.typeface = g.typeface
            canvas.drawText(g.text, pos[0] + g.drawOffsetX, pos[1] + g.drawOffsetY, paint)
        }

        val space = fontSizePx * 0.18f
        val lineHeight = fontSizePx * 1.6f
        val ascent = fontSizePx * 1.0f
        var lineX = screenWidthPx
        var baselineY = ascent
        val quadBuf = ArrayList<Float>(verse.words.size * 8)
        var quadCount = 0

        for (word in verse.words) {
            val code = word.codeV2 ?: continue
            val refs = ArrayList<Pending>()
            var i = 0
            while (i < code.length) {
                val cp = code.codePointAt(i)
                val key = (word.pageNumber.toLong() shl 32) or cp.toLong()
                unique[key]?.let { refs.add(it) }
                i += Character.charCount(cp)
            }
            if (refs.isEmpty()) continue
            val wordAdvance = refs.sumOf { it.advance.toDouble() }.toFloat()
            if (lineX < screenWidthPx && lineX - wordAdvance < 0f) {
                baselineY += lineHeight
                lineX = screenWidthPx
            }
            var cursor = lineX
            for (g in refs) {
                val glyphLeft = cursor - g.advance
                val dstX = glyphLeft + g.originX
                val dstY = baselineY + g.originY
                val key = (g.pageNumber.toLong() shl 32) or g.codepoint.toLong()
                val pos = placements[key] ?: continue
                val sx = pos[0]; val sy = pos[1]
                val u0 = sx.toFloat() / atlasW
                val v0 = sy.toFloat() / atlasH
                val u1 = (sx + g.w).toFloat() / atlasW
                val v1 = (sy + g.h).toFloat() / atlasH
                quadBuf.add(dstX); quadBuf.add(dstY)
                quadBuf.add(g.w.toFloat()); quadBuf.add(g.h.toFloat())
                quadBuf.add(u0); quadBuf.add(v0)
                quadBuf.add(u1); quadBuf.add(v1)
                quadCount++
                cursor = glyphLeft
            }
            lineX = cursor - space
        }

        val pixels = IntArray(atlasW * atlasH)
        atlasBmp.getPixels(pixels, 0, atlasW, 0, 0, atlasW, atlasH)
        val alpha = ByteArray(atlasW * atlasH)
        for (i in pixels.indices) alpha[i] = ((pixels[i] ushr 24) and 0xFF).toByte()
        atlasBmp.recycle()

        return Result(
            atlasAlpha = alpha,
            atlasW = atlasW,
            atlasH = atlasH,
            quads = quadBuf.toFloatArray(),
            quadCount = quadCount,
            totalHeightPx = baselineY + (lineHeight - ascent),
        )
    }

    private data class Pending(
        val pageNumber: Int,
        val codepoint: Int,
        val typeface: Typeface,
        val text: String,
        val w: Int, val h: Int,
        val drawOffsetX: Float, val drawOffsetY: Float,
        val advance: Float,
        val originX: Float, val originY: Float,
    )

    private class ShelfPacker(private val width: Int) {
        private var shelfY = 0
        private var shelfHeight = 0
        private var cursorX = 0
        var totalHeight = 0
            private set

        fun place(w: Int, h: Int): IntArray? {
            if (w > width) return null
            if (cursorX + w > width) {
                shelfY += shelfHeight
                shelfHeight = 0
                cursorX = 0
            }
            val x = cursorX
            val y = shelfY
            cursorX += w
            shelfHeight = max(shelfHeight, h)
            totalHeight = shelfY + shelfHeight
            return intArrayOf(x, y)
        }
    }

    private fun nextPow2(v: Int): Int {
        var x = 1
        while (x < v) x *= 2
        return x
    }
}
