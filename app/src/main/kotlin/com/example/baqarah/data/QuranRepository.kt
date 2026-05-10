package com.example.baqarah.data

class QuranRepository(private val api: QuranApi = QuranApi.create()) {
    suspend fun versesByChapter(chapter: Int): List<Verse> =
        api.versesByChapter(chapter = chapter, perPage = 300).verses
}
