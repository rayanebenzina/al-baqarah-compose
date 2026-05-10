package com.example.baqarah.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.rememberGraphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.LayoutPlan
import com.example.baqarah.data.Verse
import com.example.baqarah.data.buildPlan

@Composable
fun V5AyahText(
    verse: Verse,
    atlas: GlyphAtlas,
    fontSizePx: Float,
    prebuiltPlans: Map<Int, LayoutPlan>,
    prebuiltWidthPx: Int,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val layoutDirection = LocalLayoutDirection.current
    val layer = rememberGraphicsLayer()

    BoxWithConstraints(modifier = modifier.fillMaxWidth()) {
        val widthPx = constraints.maxWidth.toFloat()
        val widthInt = widthPx.toInt()
        val plan = remember(verse.id, fontSizePx, widthInt, prebuiltWidthPx) {
            if (widthInt == prebuiltWidthPx) {
                prebuiltPlans[verse.id] ?: buildPlan(verse, atlas, widthPx, fontSizePx)
            } else {
                buildPlan(verse, atlas, widthPx, fontSizePx)
            }
        }
        val totalHeightDp = with(density) { plan.totalHeightPx.toDp() }
        val sizePx = IntSize(widthPx.toInt(), plan.totalHeightPx.toInt().coerceAtLeast(1))

        remember(layer, plan, sizePx) {
            layer.record(
                density = density,
                layoutDirection = layoutDirection,
                size = sizePx,
            ) {
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
            sizePx
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(totalHeightDp)
                .drawBehind { drawLayer(layer) },
        )
    }
}
