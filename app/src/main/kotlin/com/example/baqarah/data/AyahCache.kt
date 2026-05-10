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
                out.writeInt(plan.quads.size)
                for (q in plan.quads) {
                    out.writeInt(q.atlasIndex)
                    out.writeInt(q.srcX)
                    out.writeInt(q.srcY)
                    out.writeInt(q.w)
                    out.writeInt(q.h)
                    out.writeInt(q.dstX)
                    out.writeInt(q.dstY)
                }
            }
        }
    }

    fun loadPlans(fontSize: Int, widthPx: Int): Map<Int, LayoutPlan>? {
        val file = plansFile(fontSize, widthPx)
        if (!file.exists()) return null
        return runCatching {
            DataInputStream(BufferedInputStream(file.inputStream())).use { inp ->
                val version = inp.readInt()
                if (version != PLANS_VERSION) return null
                val storedWidth = inp.readInt()
                if (storedWidth != widthPx) return null
                val count = inp.readInt()
                val map = HashMap<Int, LayoutPlan>(count)
                repeat(count) {
                    val id = inp.readInt()
                    val totalHeightPx = inp.readFloat()
                    val quadCount = inp.readInt()
                    val quads = ArrayList<GlyphQuad>(quadCount)
                    repeat(quadCount) {
                        quads.add(
                            GlyphQuad(
                                atlasIndex = inp.readInt(),
                                srcX = inp.readInt(),
                                srcY = inp.readInt(),
                                w = inp.readInt(),
                                h = inp.readInt(),
                                dstX = inp.readInt(),
                                dstY = inp.readInt(),
                            )
                        )
                    }
                    map[id] = LayoutPlan(quads, totalHeightPx)
                }
                map
            }
        }.getOrNull()
    }

    companion object {
        private const val PLANS_VERSION = 1
    }
}
