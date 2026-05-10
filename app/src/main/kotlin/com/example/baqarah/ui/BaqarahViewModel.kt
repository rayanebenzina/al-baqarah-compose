package com.example.baqarah.ui

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Typeface
import androidx.compose.ui.graphics.asImageBitmap
import android.os.SystemClock
import android.util.DisplayMetrics
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider.AndroidViewModelFactory.Companion.APPLICATION_KEY
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.AyahCache
import com.example.baqarah.data.FontRepository
import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.LayoutPlan
import com.example.baqarah.data.QuranRepository
import com.example.baqarah.data.SettingsRepository
import com.example.baqarah.data.Verse
import com.example.baqarah.data.buildPlan
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface BaqarahUiState {
    data object Loading : BaqarahUiState
    data class Ready(
        val verseIds: List<Int>,
        val verses: List<Verse>,
        val typefaces: Map<Int, Typeface>,
        val atlas: GlyphAtlas,
        val fontSizePx: Float,
        val prebuiltPlans: Map<Int, LayoutPlan>,
        val prebuiltWidthPx: Int,
    ) : BaqarahUiState
    data class Error(val message: String) : BaqarahUiState
}

class BaqarahViewModel(
    private val app: Application,
    private val quran: QuranRepository,
    private val fonts: FontRepository,
    private val cache: AyahCache,
    private val settings: SettingsRepository,
) : ViewModel() {

    private val _state = MutableStateFlow<BaqarahUiState>(BaqarahUiState.Loading)
    val state: StateFlow<BaqarahUiState> = _state.asStateFlow()

    val fontSize: StateFlow<Int> = settings.fontSizePx

    private var loadJob: Job? = null

    init { reload() }

    fun setFontSize(px: Int) {
        if (px == settings.fontSizePx.value) return
        settings.setFontSize(px)
        reload()
    }

    fun reload() {
        loadJob?.cancel()
        _state.value = BaqarahUiState.Loading
        val targetFontSize = settings.fontSizePx.value
        val started = SystemClock.elapsedRealtime()
        loadJob = viewModelScope.launch {
            runCatching { buildOrLoad(targetFontSize) }.fold(
                onSuccess = {
                    _state.value = it
                    Log.i("BaqarahPerf", "ready_ms=${SystemClock.elapsedRealtime() - started} fontSize=$targetFontSize")
                },
                onFailure = { _state.value = BaqarahUiState.Error(it.message ?: "Unknown error") },
            )
        }
    }

    private suspend fun buildOrLoad(targetFontSizePx: Int): BaqarahUiState.Ready = coroutineScope {
        val widthPx = estimateAyahWidthPx()
        val atlasDir = cache.atlasDir(targetFontSizePx)

        val plansJob = async(Dispatchers.IO) { cache.loadPlans(targetFontSizePx, widthPx) }
        val indexJob = async(Dispatchers.IO) { GlyphAtlas.readIndex(atlasDir) }
        val page0Job = async(Dispatchers.IO) {
            val f = java.io.File(atlasDir, "atlas_0.png")
            if (!f.exists() || f.length() == 0L) null
            else BitmapFactory.decodeFile(
                f.absolutePath,
                BitmapFactory.Options().apply { inPreferredConfig = Bitmap.Config.HARDWARE },
            )?.asImageBitmap()
        }

        val cachedPlans = plansJob.await()
        val indexResult = indexJob.await()
        val page0Image = page0Job.await()

        // On cache hit (plans + atlas index both present) we don't need verses.words at all.
        // Verses only matter when we have to rebuild the atlas.
        val needFullVerses = indexResult == null
        val verses: List<Verse> = if (needFullVerses) {
            cache.loadVerses() ?: quran.alBaqarah().also { cache.saveVerses(it) }
        } else {
            emptyList()
        }

        val (atlas, typefaces) = if (indexResult != null) {
            val (loaded, pngFiles) = indexResult
            if (page0Image != null) loaded.installPage(0, page0Image)
            val priorityPages = cachedPlans?.let { computePriorityPages(it, ayahLimit = 8) } ?: IntArray(0)
            val needSync = priorityPages.filter { it != 0 || page0Image == null }.toIntArray()
            GlyphAtlas.decodeProgressive(loaded, pngFiles, needSync, backgroundScope = viewModelScope)
            loaded to emptyMap<Int, Typeface>()
        } else {
            val pages = verses.flatMap { v -> v.words.map { it.pageNumber } }.toSet()
            val tfs = pages.associateWith { fonts.typefaceForPage(it) }
            val built = withContext(Dispatchers.Default) {
                buildAtlas(verses, tfs, targetFontSizePx.toFloat(), atlasDir)
            }
            built to tfs
        }

        val plans = cachedPlans ?: withContext(Dispatchers.Default) {
            verses.associate { it.id to buildPlan(it, atlas, widthPx.toFloat(), targetFontSizePx.toFloat()) }
        }.also { cache.savePlans(targetFontSizePx, widthPx, it) }

        val verseIds = plans.keys.sorted()
        BaqarahUiState.Ready(verseIds, verses, typefaces, atlas, targetFontSizePx.toFloat(), plans, widthPx)
    }

    private fun computePriorityPages(plans: Map<Int, LayoutPlan>, ayahLimit: Int): IntArray {
        val pages = HashSet<Int>()
        plans.entries.sortedBy { it.key }.take(ayahLimit).forEach { (_, plan) ->
            val data = plan.quadData
            var i = 0
            while (i < data.size) { pages.add(data[i]); i += 7 }
        }
        return pages.toIntArray()
    }

    private suspend fun buildAtlas(
        verses: List<Verse>,
        typefaces: Map<Int, Typeface>,
        fontSizePx: Float,
        saveDir: java.io.File,
    ): GlyphAtlas = withContext(Dispatchers.Default) {
        val atlas = GlyphAtlas()
        verses.forEach { v ->
            v.words.forEach { w ->
                val tf = typefaces[w.pageNumber] ?: return@forEach
                val code = w.codeV2 ?: return@forEach
                var i = 0
                while (i < code.length) {
                    val cp = code.codePointAt(i)
                    atlas.add(w.pageNumber, cp, fontSizePx, tf)
                    i += Character.charCount(cp)
                }
            }
        }
        atlas.seal(saveDir)
        atlas
    }

    private fun estimateAyahWidthPx(): Int {
        val dm: DisplayMetrics = app.resources.displayMetrics
        val scrollbarDp = 22f
        val startPaddingDp = 8f
        val endPaddingDp = 20f
        val totalPaddingPx = (scrollbarDp + startPaddingDp + endPaddingDp) * dm.density
        return (dm.widthPixels - totalPaddingPx).toInt().coerceAtLeast(0)
    }

    companion object {
        val Factory = viewModelFactory {
            initializer {
                val app = this[APPLICATION_KEY] as BaqarahApp
                BaqarahViewModel(
                    app = app,
                    quran = app.quranRepository,
                    fonts = app.fontRepository,
                    cache = app.ayahCache,
                    settings = app.settingsRepository,
                )
            }
        }
    }
}
