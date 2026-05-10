package com.example.baqarah.data

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class SettingsRepository(context: Context) {

    private val prefs = context.getSharedPreferences("baqarah_settings", Context.MODE_PRIVATE)

    private val _fontSizePx = MutableStateFlow(prefs.getInt(KEY_FONT_SIZE, DEFAULT_FONT_SIZE))
    val fontSizePx: StateFlow<Int> = _fontSizePx.asStateFlow()

    fun setFontSize(px: Int) {
        val coerced = px.coerceIn(MIN_FONT_SIZE, MAX_FONT_SIZE)
        prefs.edit().putInt(KEY_FONT_SIZE, coerced).apply()
        _fontSizePx.value = coerced
    }

    companion object {
        const val DEFAULT_FONT_SIZE = 80
        const val MIN_FONT_SIZE = 48
        const val MAX_FONT_SIZE = 160

        val PRESETS: List<Pair<String, Int>> = listOf(
            "S" to 56,
            "M" to 80,
            "L" to 110,
            "XL" to 140,
        )

        private const val KEY_FONT_SIZE = "font_size_px"
    }
}
