package com.example.baqarah

import android.app.Application
import com.example.baqarah.data.AyahCache
import com.example.baqarah.data.FontRepository
import com.example.baqarah.data.QuranRepository
import com.example.baqarah.data.SettingsRepository

class BaqarahApp : Application() {
    val quranRepository by lazy { QuranRepository() }
    val fontRepository by lazy { FontRepository(this) }
    val ayahCache by lazy { AyahCache(this) }
    val settingsRepository by lazy { SettingsRepository(this) }
}
