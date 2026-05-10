package com.example.baqarah.data

import android.content.Context
import android.graphics.Typeface
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File

class FontRepository(context: Context) {

    private val cacheDir = File(context.filesDir, "qpc-v4").apply { mkdirs() }
    private val client = OkHttpClient()
    private val mutex = Mutex()
    private val cache = mutableMapOf<Int, Typeface>()

    suspend fun typefaceForPage(page: Int): Typeface {
        cache[page]?.let { return it }
        return mutex.withLock {
            cache.getOrPut(page) {
                val file = File(cacheDir, "p$page.ttf")
                if (!file.exists() || file.length() == 0L) downloadFont(page, file)
                Typeface.createFromFile(file)
            }
        }
    }

    private suspend fun downloadFont(page: Int, dest: File) = withContext(Dispatchers.IO) {
        val url = FONT_URL_TEMPLATE.format(page)
        val req = Request.Builder().url(url).build()
        client.newCall(req).execute().use { resp ->
            check(resp.isSuccessful) { "Font download failed for page $page: HTTP ${resp.code}" }
            val body = resp.body ?: error("Empty body for page $page")
            dest.outputStream().use { out -> body.byteStream().copyTo(out) }
        }
    }

    companion object {
        const val FONT_URL_TEMPLATE =
            "https://verses.quran.foundation/fonts/quran/hafs/v4/colrv1/ttf/p%d.ttf"
    }
}
