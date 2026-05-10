package com.example.baqarah.data

import android.content.Context
import android.graphics.Typeface
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.Dispatcher
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File

class FontRepository(context: Context) {

    private val cacheDir = File(context.filesDir, "qpc-v4").apply { mkdirs() }
    private val client = OkHttpClient.Builder()
        .dispatcher(Dispatcher().apply {
            maxRequests = 32
            maxRequestsPerHost = 16
        })
        .build()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val cache = HashMap<Int, Deferred<Typeface>>()
    private val cacheLock = Mutex()

    suspend fun typefaceForPage(page: Int): Typeface {
        val deferred = cacheLock.withLock {
            cache.getOrPut(page) {
                scope.async {
                    val file = File(cacheDir, "p$page.ttf")
                    if (!file.exists() || file.length() == 0L) downloadFont(page, file)
                    Typeface.createFromFile(file)
                }
            }
        }
        return deferred.await()
    }

    private fun downloadFont(page: Int, dest: File) {
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
