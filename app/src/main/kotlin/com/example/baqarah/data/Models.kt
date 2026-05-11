package com.example.baqarah.data

import com.squareup.moshi.Json
import com.squareup.moshi.JsonClass

@JsonClass(generateAdapter = false)
data class VersesResponse(
    val verses: List<Verse>,
)

@JsonClass(generateAdapter = false)
data class Verse(
    val id: Int,
    @Json(name = "verse_number") val verseNumber: Int,
    @Json(name = "verse_key") val verseKey: String,
    @Json(name = "page_number") val pageNumber: Int,
    val words: List<Word>,
)

@JsonClass(generateAdapter = false)
data class Word(
    val id: Long,
    val position: Int,
    @Json(name = "code_v2") val codeV2: String?,
    @Json(name = "page_number") val pageNumber: Int,
    @Json(name = "line_number") val lineNumber: Int? = null,
    @Json(name = "char_type_name") val charTypeName: String,
)
