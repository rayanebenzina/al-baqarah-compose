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
        /** 4 ints per cell in the atlas: [x, y, w, h] */
        val cells: IntArray,
        val cellCount: Int,
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
    ): Result = build(
        verses = listOf(verse),
        typefaces = typefaces,
        fontSizePx = fontSizePx,
        screenWidthPx = screenWidthPx,
        padding = padding,
    )

    fun build(
        verses: List<Verse>,
        typefaces: Map<Int, Typeface>,
        fontSizePx: Float,
        screenWidthPx: Float,
        startTopPx: Float = 24f,
        ayahSpacingPx: Float = 28f,
        padding: Int = 12,
    ): Result {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            isSubpixelText = true
            color = Color.WHITE
            textSize = fontSizePx
        }

        val unique = LinkedHashMap<Long, Pending>()
        for (verse in verses) {
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
        }

        val sortedByHeight = unique.values.sortedByDescending { it.h }
        val totalArea = sortedByHeight.sumOf { it.w * it.h }
        val approxSide = sqrt(totalArea.toFloat() * 1.4f).toInt()
        val maxAtlasW = 8192
        var atlasW = 64
        while (atlasW < approxSide && atlasW < maxAtlasW) atlasW *= 2
        atlasW = atlasW.coerceAtMost(maxAtlasW)
        val placements = HashMap<Long, IntArray>(unique.size)
        val packer = ShelfPacker(atlasW)
        for (g in sortedByHeight) {
            val pos = packer.place(g.w, g.h)
                ?: error("glyph too wide for atlas: ${g.w}x${g.h} in atlasW=$atlasW")
            val key = (g.pageNumber.toLong() shl 32) or g.codepoint.toLong()
            placements[key] = pos
        }
        val atlasH = nextPow2(packer.totalHeight)

        val atlasBmp = Bitmap.createBitmap(atlasW, atlasH, Bitmap.Config.ALPHA_8)
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
        val quadBuf = ArrayList<Float>(verses.sumOf { it.words.size } * 8)
        var quadCount = 0
        var contentY = startTopPx

        for (verse in verses) {
            var lineX = screenWidthPx
            var baselineY = contentY + ascent
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
            // baselineY is at the last rendered baseline; advance past descent + spacing.
            contentY = baselineY + (lineHeight - ascent) + ayahSpacingPx
        }

        val alpha = ByteArray(atlasW * atlasH)
        atlasBmp.copyPixelsToBuffer(java.nio.ByteBuffer.wrap(alpha))
        atlasBmp.recycle()

        val cellArr = IntArray(unique.size * 4)
        var ci = 0
        for ((key, g) in unique) {
            val pos = placements[key] ?: continue
            cellArr[ci++] = pos[0]
            cellArr[ci++] = pos[1]
            cellArr[ci++] = g.w
            cellArr[ci++] = g.h
        }
        val cellCount = ci / 4

        return Result(
            atlasAlpha = alpha,
            atlasW = atlasW,
            atlasH = atlasH,
            cells = cellArr,
            cellCount = cellCount,
            quads = quadBuf.toFloatArray(),
            quadCount = quadCount,
            totalHeightPx = contentY,
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
