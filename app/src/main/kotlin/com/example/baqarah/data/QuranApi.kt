package com.example.baqarah.data

import com.squareup.moshi.Moshi
import com.squareup.moshi.kotlin.reflect.KotlinJsonAdapterFactory
import retrofit2.Retrofit
import retrofit2.converter.moshi.MoshiConverterFactory
import retrofit2.http.GET
import retrofit2.http.Path
import retrofit2.http.Query

interface QuranApi {
    @GET("api/v4/verses/by_chapter/{chapter}")
    suspend fun versesByChapter(
        @Path("chapter") chapter: Int,
        @Query("words") words: Boolean = true,
        @Query("word_fields") wordFields: String = "code_v2,page_number,char_type_name",
        @Query("mushaf") mushaf: Int = 19,
        @Query("per_page") perPage: Int = 300,
    ): VersesResponse

    @GET("api/v4/verses/by_page/{page}")
    suspend fun versesByPage(
        @Path("page") page: Int,
        @Query("words") words: Boolean = true,
        @Query("word_fields") wordFields: String = "code_v2,page_number,char_type_name",
        @Query("mushaf") mushaf: Int = 19,
        @Query("per_page") perPage: Int = 300,
    ): VersesResponse

    companion object {
        private const val BASE_URL = "https://api.quran.com/"

        fun create(): QuranApi {
            val moshi = Moshi.Builder().add(KotlinJsonAdapterFactory()).build()
            return Retrofit.Builder()
                .baseUrl(BASE_URL)
                .addConverterFactory(MoshiConverterFactory.create(moshi))
                .build()
                .create(QuranApi::class.java)
        }
    }
}
