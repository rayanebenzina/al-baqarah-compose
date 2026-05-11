package com.example.baqarah.vk

import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.Verse
import com.example.baqarah.data.Word
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Render-ready bundle for a contiguous run of Quran text: flat
 * codepoint / fontIndex arrays, one TTF per referenced page, and a
 * partition into screen lines.
 *
 * The partition rule depends on the mode: in `Mushaf`/`SurahByLine` a
 * new line begins whenever `(page_number, line_number)` changes, in
 * `SurahByVerse` a new line additionally begins at every verse
 * boundary.
 */
data class MushafContent(
    val ttfs: Array<ByteArray>,
    val codepoints: IntArray,
    val fontIndices: IntArray,
    val lineStarts: IntArray,
)

enum class LineMode {
    /** Break at Mushaf line boundary only — verses can share a line. */
    ByMushafLine,
    /** Break at Mushaf line OR verse boundary — every new verse starts fresh. */
    ByVerse,
}

object MushafLoader {

    suspend fun loadPage(app: BaqarahApp, page: Int): MushafContent {
        val verses = withContext(Dispatchers.IO) {
            app.ayahCache.loadVersesForPage(page)
                ?: app.quranRepository.versesByPage(page).also {
                    app.ayahCache.saveVersesForPage(page, it)
                }
        }
        return buildContent(app, verses, LineMode.ByMushafLine)
    }

    suspend fun loadSurah(app: BaqarahApp, surah: Int, mode: LineMode): MushafContent {
        val verses = withContext(Dispatchers.IO) {
            app.ayahCache.loadVerses(surah)
                ?: app.quranRepository.versesByChapter(surah).also {
                    app.ayahCache.saveVerses(surah, it)
                }
        }
        // Al-Fatihah (1) already includes the basmallah as verse 1;
        // At-Tawbah (9) has no basmallah. For every other surah, prepend
        // the basmallah by reusing Al-Fatihah verse 1's words and font
        // (page 1's QPC v4 TTF).
        val basmallahHeader: List<Word> = if (surah != 1 && surah != 9) {
            val fatihaVerses = withContext(Dispatchers.IO) {
                app.ayahCache.loadVerses(1)
                    ?: app.quranRepository.versesByChapter(1).also {
                        app.ayahCache.saveVerses(1, it)
                    }
            }
            // Drop the "end" (verse-number marker) word — when prepending
            // Al-Fatihah 1:1 as a surah header, the trailing ① would
            // mislabel the basmallah as verse 1 of the host surah.
            fatihaVerses.firstOrNull()?.words?.filter { it.charTypeName == "word" }
                .orEmpty()
        } else emptyList()
        // quran_titles.ttf (from quran/quran_android): ornate calligraphic
        // name-plate, one glyph per surah. Codepoint convention is
        // `0xFB8D + (surah-1)` with a 0x21 jump after the 37th glyph
        // (skips a TTF range the font reserves elsewhere).
        val titleAsset: ByteArray = withContext(Dispatchers.IO) {
            app.assets.open("quran_titles.ttf").use { it.readBytes() }
        }
        val i = surah - 1
        val titleCodepoint = 0xFB8D + i + if (i >= 37) 0x21 else 0
        return buildContent(app, verses, mode, basmallahHeader,
            titleAsset = titleAsset, titleCodepoint = titleCodepoint)
    }

    /**
     * Convert a list of verses into the renderer-ready bundle:
     * - resolve every referenced page's TTF (from cache or downloading)
     * - flatten codepoints with a parallel `fontIndices` array
     * - emit `lineStarts` at every place a new screen line should begin
     *
     * `basmallahHeader` is an optional set of words prepended as a
     * synthetic header line before the real verses (the QPC glyphs for
     * the basmallah, taken from Al-Fatihah 1:1). `titleAsset` +
     * `titleCodepoint` add an ornate surah-name-plate glyph on its own
     * line above the basmallah. Both extras render on their own screen
     * row regardless of `mode`.
     */
    private suspend fun buildContent(
        app: BaqarahApp,
        verses: List<Verse>,
        mode: LineMode,
        basmallahHeader: List<Word> = emptyList(),
        titleAsset: ByteArray? = null,
        titleCodepoint: Int? = null,
    ): MushafContent = coroutineScope {
        val pageNumbers = (basmallahHeader.map { it.pageNumber } +
            verses.flatMap { v -> v.words.map { it.pageNumber } })
            .toSortedSet().toList()
        val pageTtfs = withContext(Dispatchers.IO) {
            pageNumbers.map { p ->
                async {
                    app.fontRepository.typefaceForPage(p)
                    File(app.filesDir, "qpc-v4/p$p.ttf").readBytes()
                }
            }.awaitAll()
        }
        val pageToIndex = HashMap<Int, Int>(pageNumbers.size).apply {
            pageNumbers.forEachIndexed { i, p -> put(p, i) }
        }
        // Surah-title TTF goes after all page TTFs in the array; its
        // fontIndex is therefore pageTtfs.size.
        val titleFontIdx = pageTtfs.size
        val ttfs: List<ByteArray> =
            if (titleAsset != null) pageTtfs + titleAsset else pageTtfs

        val codepoints = ArrayList<Int>(verses.size * 12)
        val fontIndices = ArrayList<Int>(verses.size * 12)
        val lineStarts = ArrayList<Int>(verses.size * 2 + 3)

        // Synthetic surah-title line — single ornate plate glyph from
        // QuranTitles font, on its own row.
        if (titleAsset != null && titleCodepoint != null) {
            lineStarts.add(codepoints.size)
            codepoints.add(titleCodepoint)
            fontIndices.add(titleFontIdx)
        }

        // Synthetic basmallah header line (page-1 TTF, all on one line).
        if (basmallahHeader.isNotEmpty()) {
            lineStarts.add(codepoints.size)
            for (word in basmallahHeader) {
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

        var prevPage = -1
        var prevLine = -1
        var prevVerse = -1
        for (verse in verses) {
            for (word in verse.words) {
                val code = word.codeV2 ?: continue
                val fontIdx = pageToIndex[word.pageNumber] ?: continue
                val line = word.lineNumber ?: 0
                val verseChange = mode == LineMode.ByVerse && verse.id != prevVerse
                if (word.pageNumber != prevPage || line != prevLine || verseChange) {
                    lineStarts.add(codepoints.size)
                    prevPage = word.pageNumber
                    prevLine = line
                    prevVerse = verse.id
                }
                var i = 0
                while (i < code.length) {
                    val cp = code.codePointAt(i)
                    codepoints.add(cp)
                    fontIndices.add(fontIdx)
                    i += Character.charCount(cp)
                }
            }
        }
        lineStarts.add(codepoints.size)

        MushafContent(
            ttfs = ttfs.toTypedArray(),
            codepoints = codepoints.toIntArray(),
            fontIndices = fontIndices.toIntArray(),
            lineStarts = lineStarts.toIntArray(),
        )
    }
}

