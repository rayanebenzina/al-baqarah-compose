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

@Composable
fun V5AyahText(
    verseId: Int,
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
        val plan = remember(verseId, fontSizePx, prebuiltWidthPx) {
            prebuiltPlans[verseId]
        } ?: return@BoxWithConstraints
        val totalHeightDp = with(density) { plan.totalHeightPx.toDp() }
        val sizePx = IntSize(widthPx.toInt(), plan.totalHeightPx.toInt().coerceAtLeast(1))

        remember(layer, plan, sizePx) {
            layer.record(
                density = density,
                layoutDirection = layoutDirection,
                size = sizePx,
            ) {
                val data = plan.quadData
                var i = 0
                while (i < data.size) {
                    val img = atlas.page(data[i])
                    if (img != null) {
                        val srcX = data[i + 1]; val srcY = data[i + 2]
                        val w = data[i + 3]; val h = data[i + 4]
                        val dstX = data[i + 5]; val dstY = data[i + 6]
                        drawImage(
                            image = img,
                            srcOffset = IntOffset(srcX, srcY),
                            srcSize = IntSize(w, h),
                            dstOffset = IntOffset(dstX, dstY),
                            dstSize = IntSize(w, h),
                        )
                    }
                    i += 7
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
