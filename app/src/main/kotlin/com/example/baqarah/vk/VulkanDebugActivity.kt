package com.example.baqarah.vk

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
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

        val (alpha, w, h) = renderTestGlyphAlpha("A", textPx = 200f, padding = 24)
        v.setTestGlyph(alpha, w, h, spread = 12)
    }

    override fun onDestroy() {
        view?.release()
        view = null
        super.onDestroy()
    }

    private fun renderTestGlyphAlpha(
        text: String,
        textPx: Float,
        padding: Int,
    ): Triple<ByteArray, Int, Int> {
        val paint = Paint().apply {
            color = Color.WHITE
            isAntiAlias = true
            textSize = textPx
            textAlign = Paint.Align.CENTER
        }
        val fm = paint.fontMetrics
        val textHeight = (fm.descent - fm.ascent).toInt() + 1
        val textWidth = paint.measureText(text).toInt() + 1
        val w = textWidth + 2 * padding
        val h = textHeight + 2 * padding
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        val baselineY = padding - fm.ascent
        canvas.drawText(text, w / 2f, baselineY, paint)

        val px = IntArray(w * h)
        bmp.getPixels(px, 0, w, 0, 0, w, h)
        val alpha = ByteArray(w * h)
        for (i in 0 until w * h) {
            alpha[i] = ((px[i] ushr 24) and 0xFF).toByte()
        }
        bmp.recycle()
        return Triple(alpha, w, h)
    }
}
