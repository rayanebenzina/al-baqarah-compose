package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.os.SystemClock
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
        loadAndRenderSurah(v)
    }

    override fun onDestroy() {
        scope.cancel()
        view?.release()
        view = null
        super.onDestroy()
    }

    private fun loadAndRenderSurah(v: VulkanCanvasView) {
        val app = application as BaqarahApp
        val screen = resources.displayMetrics
        val screenWidthPx = screen.widthPixels.toFloat()
        val leftMargin = screenWidthPx * 0.05f
        val rightMargin = screenWidthPx * 0.05f
        val topMargin = 120f
        val fontSizePx = 80f
        val lineSpacingPx = fontSizePx * 1.5f
        val ayahSpacingPx = fontSizePx * 0.6f

        scope.launch {
            try {
                val t0 = SystemClock.elapsedRealtime()
                val verses = withContext(Dispatchers.IO) {
                    app.ayahCache.loadVerses(SURAH)
                        ?: app.quranRepository.versesByChapter(SURAH).also {
                            app.ayahCache.saveVerses(SURAH, it)
                        }
                }.take(VERSE_COUNT)
                val t1 = SystemClock.elapsedRealtime()
                Log.i(TAG, "verses loaded: ${verses.size} (${t1 - t0} ms)")

                // Discover the set of pages referenced by these verses.
                val pageNumbers = verses.flatMap { it.words.map { w -> w.pageNumber } }.toSortedSet().toList()

                // Load each page's typeface (forces download/cache) and read its
                // bytes off-disk.
                val ttfs = withContext(Dispatchers.IO) {
                    pageNumbers.map { p ->
                        async {
                            app.fontRepository.typefaceForPage(p)
                            File(filesDir, "qpc-v4/p$p.ttf").readBytes()
                        }
                    }.awaitAll()
                }
                val t2 = SystemClock.elapsedRealtime()
                Log.i(TAG, "ttfs loaded: ${ttfs.size} pages (${t2 - t1} ms, ${ttfs.sumOf { it.size }} bytes)")

                val pageToIndex = HashMap<Int, Int>(pageNumbers.size)
                pageNumbers.forEachIndexed { i, p -> pageToIndex[p] = i }

                // Flatten codepoints + parallel font index + verseStarts.
                val codepoints = ArrayList<Int>(verses.size * 12)
                val fontIndices = ArrayList<Int>(verses.size * 12)
                val verseStarts = IntArray(verses.size + 1)
                for ((vi, verse) in verses.withIndex()) {
                    verseStarts[vi] = codepoints.size
                    for (word in verse.words) {
                        val code = word.codeV2 ?: continue
                        val fontIdx = pageToIndex[word.pageNumber] ?: continue
                        var i = 0
                        while (i < code.length) {
                            val cp = code.codePointAt(i)
                            codepoints.add(cp)
                            fontIndices.add(fontIdx)
                            i += Character.charCount(cp)
                        }
                    }
                }
                verseStarts[verses.size] = codepoints.size
                val t3 = SystemClock.elapsedRealtime()
                Log.i(TAG, "flattened ${codepoints.size} codepoints (${t3 - t2} ms)")

                v.setColrSurah(
                    ttfs = ttfs.toTypedArray(),
                    codepoints = codepoints.toIntArray(),
                    fontIndices = fontIndices.toIntArray(),
                    verseStarts = verseStarts,
                    screenWidthPx = screenWidthPx,
                    leftMarginPx = leftMargin,
                    rightMarginPx = rightMargin,
                    topMarginPx = topMargin,
                    fontSizePx = fontSizePx,
                    lineSpacingPx = lineSpacingPx,
                    ayahSpacingPx = ayahSpacingPx,
                ) { totalHeightPx ->
                    Log.i(TAG, "surah uploaded, contentHeight=$totalHeightPx")
                    v.setContentHeight(totalHeightPx)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "surah render failed", t)
            }
        }
    }

    companion object {
        private const val TAG = "BaqarahVkDebug"
        private const val SURAH = 2  // Al-Baqarah
        private const val VERSE_COUNT = 286
    }
}
