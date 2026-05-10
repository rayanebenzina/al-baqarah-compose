package com.example.baqarah.ui

import android.graphics.Typeface
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider.AndroidViewModelFactory.Companion.APPLICATION_KEY
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.FontRepository
import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.QuranRepository
import com.example.baqarah.data.Verse
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface BaqarahUiState {
    data object Loading : BaqarahUiState
    data class Ready(
        val verses: List<Verse>,
        val typefaces: Map<Int, Typeface>,
        val atlas: GlyphAtlas,
        val fontSizePx: Float,
    ) : BaqarahUiState
    data class Error(val message: String) : BaqarahUiState
}

class BaqarahViewModel(
    private val quran: QuranRepository,
    private val fonts: FontRepository,
) : ViewModel() {

    private val _state = MutableStateFlow<BaqarahUiState>(BaqarahUiState.Loading)
    val state: StateFlow<BaqarahUiState> = _state.asStateFlow()

    var fontSizePx: Float = 0f
        private set

    init { load() }

    fun load(targetFontSizePx: Float = fontSizePx.takeIf { it > 0f } ?: 80f) {
        fontSizePx = targetFontSizePx
        _state.value = BaqarahUiState.Loading
        viewModelScope.launch {
            runCatching {
                val verses = quran.alBaqarah()
                val pages = verses.flatMap { v -> v.words.map { it.pageNumber } }.toSet()
                val typefaces = pages.associateWith { fonts.typefaceForPage(it) }
                val atlas = buildAtlas(verses, typefaces, targetFontSizePx)
                BaqarahUiState.Ready(verses, typefaces, atlas, targetFontSizePx)
            }.fold(
                onSuccess = { _state.value = it },
                onFailure = { _state.value = BaqarahUiState.Error(it.message ?: "Unknown error") },
            )
        }
    }

    private suspend fun buildAtlas(
        verses: List<Verse>,
        typefaces: Map<Int, Typeface>,
        fontSizePx: Float,
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
        atlas.seal()
        atlas
    }

    companion object {
        val Factory = viewModelFactory {
            initializer {
                val app = this[APPLICATION_KEY] as BaqarahApp
                BaqarahViewModel(app.quranRepository, app.fontRepository)
            }
        }
    }
}
