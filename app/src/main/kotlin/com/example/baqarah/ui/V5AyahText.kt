package com.example.baqarah.ui

import android.graphics.Typeface
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.GlyphRef
import com.example.baqarah.data.Verse

@Composable
fun V5AyahText(
    verse: Verse,
    typefaces: Map<Int, Typeface>,
    atlas: GlyphAtlas,
    fontSizePx: Float,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current

    BoxWithConstraints(modifier = modifier.fillMaxWidth()) {
        val widthPx = constraints.maxWidth.toFloat()
        val plan = remember(verse.id, fontSizePx, widthPx) {
            buildPlan(verse, atlas, widthPx, fontSizePx)
        }
        val totalHeightDp = with(density) { plan.totalHeightPx.toDp() }
        Canvas(modifier = Modifier.fillMaxWidth().height(totalHeightDp)) {
            for (q in plan.quads) {
                drawImage(
                    image = atlas.page(q.atlasIndex),
                    srcOffset = IntOffset(q.srcX, q.srcY),
                    srcSize = IntSize(q.w, q.h),
                    dstOffset = IntOffset(q.dstX, q.dstY),
                    dstSize = IntSize(q.w, q.h),
                )
            }
        }
    }
}

private class GlyphQuad(
    val atlasIndex: Int,
    val srcX: Int,
    val srcY: Int,
    val w: Int,
    val h: Int,
    val dstX: Int,
    val dstY: Int,
)

private class LayoutPlan(
    val quads: List<GlyphQuad>,
    val totalHeightPx: Float,
)

private fun buildPlan(
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
