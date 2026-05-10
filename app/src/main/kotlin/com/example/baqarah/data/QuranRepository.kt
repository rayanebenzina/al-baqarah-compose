package com.example.baqarah.data

class QuranRepository(private val api: QuranApi = QuranApi.create()) {
    suspend fun alBaqarah(): List<Verse> = api.versesByChapter(chapter = 2).verses
}
