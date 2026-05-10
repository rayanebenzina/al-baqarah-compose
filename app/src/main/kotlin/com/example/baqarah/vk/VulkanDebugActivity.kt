package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.Verse
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
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
        loadAndRenderBismillah(v)
    }

    override fun onDestroy() {
        scope.cancel()
        view?.release()
        view = null
        super.onDestroy()
    }

    private fun loadAndRenderBismillah(v: VulkanCanvasView) {
        val app = application as BaqarahApp
        val screen = resources.displayMetrics
        // Right-edge anchor of the line, with some margin. Baseline near
        // the upper third of the screen for visibility.
        val rightMargin = screen.widthPixels * 0.06f
        val cursorX = screen.widthPixels - rightMargin
        val baselineY = screen.heightPixels * 0.35f
        val fontSizePx = 96f

        scope.launch {
            try {
                val verse = withContext(Dispatchers.IO) {
                    app.ayahCache.loadVerses(1)?.first()
                        ?: app.quranRepository.versesByChapter(1).also {
                            app.ayahCache.saveVerses(1, it)
                        }.first()
                }
                // Collect codepoints from every word, in source order. The
                // QPC v4 font's PUA encoding is already pre-shaped so a
                // simple sequential walk + RTL cursor advance lays out the
                // verse correctly.
                val codepoints = buildIntArray {
                    for (word in verse.words) {
                        val code = word.codeV2 ?: continue
                        var i = 0
                        while (i < code.length) {
                            val cp = code.codePointAt(i)
                            add(cp)
                            i += Character.charCount(cp)
                        }
                    }
                }
                Log.i(TAG, "loaded ${verse.words.size} words, ${codepoints.size} codepoints")

                // All words of Al-Fatiha v1 are on page 1 — just load that.
                val pageNumber = verse.words.first().pageNumber
                val ttfBytes = withContext(Dispatchers.IO) {
                    app.fontRepository.typefaceForPage(pageNumber)
                    File(filesDir, "qpc-v4/p$pageNumber.ttf").readBytes()
                }
                Log.i(TAG, "loaded p$pageNumber.ttf: ${ttfBytes.size} bytes")

                v.setColrLine(
                    ttfBytes = ttfBytes,
                    codepoints = codepoints,
                    cursorX = cursorX,
                    baselineY = baselineY,
                    fontSizePx = fontSizePx,
                )
            } catch (t: Throwable) {
                Log.e(TAG, "render failed", t)
            }
        }
    }

    private inline fun buildIntArray(block: IntArrayBuilder.() -> Unit): IntArray {
        val builder = IntArrayBuilder()
        builder.block()
        return builder.toIntArray()
    }

    private class IntArrayBuilder {
        private val list = ArrayList<Int>()
        fun add(v: Int) { list.add(v) }
        fun toIntArray(): IntArray = list.toIntArray()
    }

    companion object {
        private const val TAG = "BaqarahVkDebug"
    }
}
