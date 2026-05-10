package com.example.baqarah.ui

import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.GlyphRef
import com.example.baqarah.data.Verse

class GlyphQuad(
    val atlasIndex: Int,
    val srcX: Int,
    val srcY: Int,
    val w: Int,
    val h: Int,
    val dstX: Int,
    val dstY: Int,
)

class LayoutPlan(
    val quads: List<GlyphQuad>,
    val totalHeightPx: Float,
)

fun buildPlan(
    verse: Verse,
    atlas: GlyphAtlas,
    widthPx: Float,
    fontSizePx: Float,
): LayoutPlan {
    val quads = ArrayList<GlyphQuad>(verse.words.size + 8)
    val space = fontSizePx * 0.18f
    val lineHeight = fontSizePx * 1.6f
    val ascent = fontSizePx * 1.0f

    var lineX = widthPx
    var baselineY = ascent

    for (word in verse.words) {
        val code = word.codeV2 ?: continue
        val refs = ArrayList<GlyphRef>(2)
        var i = 0
        while (i < code.length) {
            val cp = code.codePointAt(i)
            atlas.get(word.pageNumber, cp, fontSizePx)?.let { refs.add(it) }
            i += Character.charCount(cp)
        }
        if (refs.isEmpty()) continue
        val wordAdvance = refs.sumOf { it.advance.toDouble() }.toFloat()

        if (lineX < widthPx && lineX - wordAdvance < 0f) {
            baselineY += lineHeight
            lineX = widthPx
        }

        var cursor = lineX
        for (g in refs) {
            val glyphLeft = cursor - g.advance
            val dstX = (glyphLeft + g.originX).toInt()
            val dstY = (baselineY + g.originY).toInt()
            quads.add(GlyphQuad(g.atlasIndex, g.srcX, g.srcY, g.width, g.height, dstX, dstY))
            cursor = glyphLeft
        }
        lineX = cursor - space
    }

    val totalHeightPx = baselineY + (lineHeight - ascent)
    return LayoutPlan(quads, totalHeightPx)
}
