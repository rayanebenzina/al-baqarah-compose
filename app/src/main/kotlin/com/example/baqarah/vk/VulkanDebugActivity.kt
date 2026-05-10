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
                val verse = withContext(Dispatchers.IO) {
                    app.ayahCache.loadVerses(1)?.firstOrNull()
                        ?: app.quranRepository.versesByChapter(1).also {
                            app.ayahCache.saveVerses(1, it)
                        }.first()
                }
                val typefaces = loadTypefaces(app, verse)
                val result = withContext(Dispatchers.Default) {
                    AyahSdfBuilder().build(
                        verse = verse,
                        typefaces = typefaces,
                        fontSizePx = 110f,
                        screenWidthPx = displayWidth,
                        padding = 12,
                    )
                }
                Log.i(TAG, "ayah built: atlas=${result.atlasW}x${result.atlasH} quads=${result.quadCount} h=${result.totalHeightPx}")
                v.setAyahAtlas(
                    alpha = result.atlasAlpha,
                    w = result.atlasW,
                    h = result.atlasH,
                    spread = 8,
                    quads = result.quads,
                    quadCount = result.quadCount,
                )
            } catch (t: Throwable) {
                Log.e(TAG, "ayah load failed", t)
            }
        }
    }

    private suspend fun loadTypefaces(app: BaqarahApp, verse: Verse): Map<Int, Typeface> {
        val pages = verse.words.map { it.pageNumber }.toSet()
        return withContext(Dispatchers.IO) {
            pages.map { p -> async { p to app.fontRepository.typefaceForPage(p) } }
                .awaitAll()
                .toMap()
        }
    }

    companion object {
        private const val TAG = "BaqarahVkDebug"
    }
}
