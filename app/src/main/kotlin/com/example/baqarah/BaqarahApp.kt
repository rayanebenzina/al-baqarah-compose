package com.example.baqarah

import android.app.Application
import com.example.baqarah.data.FontRepository
import com.example.baqarah.data.QuranRepository

class BaqarahApp : Application() {
    val quranRepository by lazy { QuranRepository() }
    val fontRepository by lazy { FontRepository(this) }
}
