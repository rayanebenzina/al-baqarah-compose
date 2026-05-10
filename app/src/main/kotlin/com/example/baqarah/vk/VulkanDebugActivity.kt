package com.example.baqarah.vk

import android.app.Activity
import android.graphics.Typeface
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.Verse
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class VulkanDebugActivity : Activity() {

    private var view: VulkanCanvasView? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val v = VulkanCanvasView(this)
        view = v
        setContentView(v)

        loadAndUploadAyah(v)
    }

    override fun onDestroy() {
        scope.cancel()
        view?.release()
        view = null
        super.onDestroy()
    }

    private fun loadAndUploadAyah(v: VulkanCanvasView): Job {
        val app = application as BaqarahApp
        val displayWidth = resources.displayMetrics.widthPixels.toFloat()

        return scope.launch {
            try {
                val t0 = android.os.SystemClock.elapsedRealtime()
                val verses = withContext(Dispatchers.IO) {
                    (app.ayahCache.loadVerses(SURAH)
                        ?: app.quranRepository.versesByChapter(SURAH).also {
                            app.ayahCache.saveVerses(SURAH, it)
                        })
                        .take(VERSE_COUNT)
                }
                val t1 = android.os.SystemClock.elapsedRealtime()
                val typefaces = loadTypefaces(app, verses)
                val t2 = android.os.SystemClock.elapsedRealtime()
                val result = withContext(Dispatchers.Default) {
                    AyahSdfBuilder().build(
                        verses = verses,
                        typefaces = typefaces,
                        fontSizePx = 64f,
                        screenWidthPx = displayWidth,
                        padding = 12,
                    )
                }
                val t3 = android.os.SystemClock.elapsedRealtime()
                Log.i(TAG, "surah built: verses=${verses.size} atlas=${result.atlasW}x${result.atlasH} cells=${result.cellCount} quads=${result.quadCount} h=${result.totalHeightPx}")
                Log.i(TAG, "timings ms: verses=${t1-t0} fonts=${t2-t1} build=${t3-t2}")
                v.setAyahAtlas(
                    alpha = result.atlasAlpha,
                    w = result.atlasW,
                    h = result.atlasH,
                    spread = 8,
                    cells = result.cells,
                    cellCount = result.cellCount,
                    quads = result.quads,
                    quadCount = result.quadCount,
                )
                v.setContentHeight(result.totalHeightPx)
            } catch (t: Throwable) {
                Log.e(TAG, "surah load failed", t)
            }
        }
    }

    private suspend fun loadTypefaces(app: BaqarahApp, verses: List<Verse>): Map<Int, Typeface> {
        val pages = verses.flatMap { v -> v.words.map { it.pageNumber } }.toSet()
        return withContext(Dispatchers.IO) {
            pages.map { p -> async { p to app.fontRepository.typefaceForPage(p) } }
                .awaitAll()
                .toMap()
        }
    }

    companion object {
        private const val TAG = "BaqarahVkDebug"
        private const val SURAH = 2  // Al-Baqarah
        private const val VERSE_COUNT = 286
    }
}
