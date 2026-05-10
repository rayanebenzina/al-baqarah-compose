package com.example.baqarah.data

class LayoutPlan(
    /** Flat array of 7 ints per quad: [atlasIndex, srcX, srcY, w, h, dstX, dstY] */
    val quadData: IntArray,
    val totalHeightPx: Float,
) {
    val quadCount: Int get() = quadData.size / 7
}

fun buildPlan(
    verse: Verse,
    atlas: GlyphAtlas,
    widthPx: Float,
    fontSizePx: Float,
): LayoutPlan {
    val space = fontSizePx * 0.18f
    val lineHeight = fontSizePx * 1.6f
    val ascent = fontSizePx * 1.0f

    var lineX = widthPx
    var baselineY = ascent
    val out = ArrayList<Int>(verse.words.size * 7)

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
            out.add(g.atlasIndex)
            out.add(g.srcX)
            out.add(g.srcY)
            out.add(g.width)
            out.add(g.height)
            out.add(dstX)
            out.add(dstY)
            cursor = glyphLeft
        }
        lineX = cursor - space
    }

    val totalHeightPx = baselineY + (lineHeight - ascent)
    return LayoutPlan(out.toIntArray(), totalHeightPx)
}
