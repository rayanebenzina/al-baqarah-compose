package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.view.WindowManager

class VulkanDebugActivity : Activity() {

    private var view: VulkanCanvasView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val v = VulkanCanvasView(this)
        view = v
        setContentView(v)

        val (curves, count) = buildTestSquare()
        val screen = resources.displayMetrics
        val side = minOf(screen.widthPixels, screen.heightPixels) * 0.6f
        val cx = screen.widthPixels * 0.5f
        val cy = screen.heightPixels * 0.4f
        v.setOutlineGlyph(
            curves = curves,
            curveCount = count,
            dstX = cx - side / 2f,
            dstY = cy - side / 2f,
            dstW = side,
            dstH = side,
        )
    }

    override fun onDestroy() {
        view?.release()
        view = null
        super.onDestroy()
    }

    /**
     * A 4-curve square outline in UV space [0.2, 0.8]^2, traced
     * counter-clockwise. Each edge is encoded as a quadratic with the
     * control point at the segment midpoint so the curve is collinear
     * with the straight edge.
     */
    private fun buildTestSquare(): Pair<FloatArray, Int> {
        val a = 0.2f
        val b = 0.8f
        val m = 0.5f
        // 4 curves, each 3 vec2 = 6 floats.
        val curves = floatArrayOf(
            // Bottom: (a, a) -> (m, a) -> (b, a)
            a, a, m, a, b, a,
            // Right:  (b, a) -> (b, m) -> (b, b)
            b, a, b, m, b, b,
            // Top:    (b, b) -> (m, b) -> (a, b)
            b, b, m, b, a, b,
            // Left:   (a, b) -> (a, m) -> (a, a)
            a, b, a, m, a, a,
        )
        return curves to 4
    }
}
