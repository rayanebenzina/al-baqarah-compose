package com.example.baqarah.data

import android.content.Context
import com.squareup.moshi.Moshi
import com.squareup.moshi.Types
import com.squareup.moshi.kotlin.reflect.KotlinJsonAdapterFactory
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File

class AyahCache(context: Context) {

    private val baseDir = File(context.filesDir, "ayah_cache").apply { mkdirs() }
    private val versesFile = File(baseDir, "verses.json")

    private val moshi = Moshi.Builder().add(KotlinJsonAdapterFactory()).build()
    private val verseListType = Types.newParameterizedType(List::class.java, Verse::class.java)
    private val verseListAdapter = moshi.adapter<List<Verse>>(verseListType)

    fun atlasDir(fontSize: Int): File =
        File(baseDir, "fs_$fontSize/atlas").apply { mkdirs() }

    private fun plansFile(fontSize: Int, widthPx: Int): File =
        File(baseDir, "fs_$fontSize/plans_w$widthPx.bin")

    fun saveVerses(verses: List<Verse>) {
        versesFile.outputStream().use { it.write(verseListAdapter.toJson(verses).toByteArray()) }
    }

    fun loadVerses(): List<Verse>? {
        if (!versesFile.exists()) return null
        return runCatching {
            versesFile.inputStream().use { verseListAdapter.fromJson(it.bufferedReader().readText()) }
        }.getOrNull()
    }

    fun savePlans(fontSize: Int, widthPx: Int, plans: Map<Int, LayoutPlan>) {
        val file = plansFile(fontSize, widthPx)
        file.parentFile?.mkdirs()
        DataOutputStream(BufferedOutputStream(file.outputStream())).use { out ->
            out.writeInt(PLANS_VERSION)
            out.writeInt(widthPx)
            out.writeInt(plans.size)
            for ((id, plan) in plans) {
                out.writeInt(id)
                out.writeFloat(plan.totalHeightPx)
                out.writeInt(plan.quadData.size)
                for (v in plan.quadData) out.writeInt(v)
            }
        }
    }

    fun loadPlans(fontSize: Int, widthPx: Int): Map<Int, LayoutPlan>? {
        val file = plansFile(fontSize, widthPx)
        if (!file.exists()) return null
        return runCatching {
            val raw = file.readBytes()
            val bb = java.nio.ByteBuffer.wrap(raw).order(java.nio.ByteOrder.BIG_ENDIAN)
            if (bb.int != PLANS_VERSION) return null
            if (bb.int != widthPx) return null
            val count = bb.int
            val map = HashMap<Int, LayoutPlan>(count)
            repeat(count) {
                val id = bb.int
                val totalHeightPx = bb.float
                val len = bb.int
                val data = IntArray(len)
                val ib = bb.asIntBuffer()
                ib.get(data)
                bb.position(bb.position() + len * 4)
                map[id] = LayoutPlan(data, totalHeightPx)
            }
            map
        }.getOrNull()
    }

    companion object {
        private const val PLANS_VERSION = 2
    }
}
