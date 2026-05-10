package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import com.example.baqarah.BaqarahApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class VulkanDebugActivity : Activity() {

    private var view: VulkanCanvasView? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val v = VulkanCanvasView(this)
        view = v
        setContentView(v)

        loadAndRenderOneGlyph(v)
    }

    override fun onDestroy() {
        scope.cancel()
        view?.release()
        view = null
        super.onDestroy()
    }

    private fun loadAndRenderOneGlyph(v: VulkanCanvasView) {
        val app = application as BaqarahApp
        val screen = resources.displayMetrics
        val side = minOf(screen.widthPixels, screen.heightPixels) * 0.6f
        val cx = screen.widthPixels * 0.5f
        val cy = screen.heightPixels * 0.4f

        scope.launch {
            try {
                val verses = withContext(Dispatchers.IO) {
                    app.ayahCache.loadVerses(1)
                        ?: app.quranRepository.versesByChapter(1).also {
                            app.ayahCache.saveVerses(1, it)
                        }
                }
                val firstWord = verses.first().words.first()
                val firstCp = firstWord.codeV2!!.codePointAt(0)
                val pageNumber = firstWord.pageNumber
                Log.i(TAG, "rendering U+${firstCp.toString(16)} from page $pageNumber")

                // Force the font to be on disk (FontRepository downloads if missing).
                app.fontRepository.typefaceForPage(pageNumber)
                val ttfBytes = withContext(Dispatchers.IO) {
                    File(filesDir, "qpc-v4/p$pageNumber.ttf").readBytes()
                }
                Log.i(TAG, "loaded p$pageNumber.ttf: ${ttfBytes.size} bytes")

                v.setOutlineFromTtf(
                    ttfBytes = ttfBytes,
                    codepoint = firstCp,
                    dstX = cx - side / 2f,
                    dstY = cy - side / 2f,
                    dstW = side,
                    dstH = side,
                )
            } catch (t: Throwable) {
                Log.e(TAG, "glyph load failed", t)
            }
        }
    }

    companion object {
        private const val TAG = "BaqarahVkDebug"
    }
}
