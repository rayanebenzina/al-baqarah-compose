package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.example.baqarah.BaqarahApp
import com.example.baqarah.data.SURAHS
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Single activity that supports two reading modes:
 *
 *  - **Mushaf pages** (one Mushaf page = 15 lines per screen, no vertical
 *    scroll). Swipe left → next page (1..604), swipe right → prev page.
 *  - **Surah by verse** (one surah at a time, vertical scroll inside the
 *    surah, each verse begins on its own line). Swipe left → next surah,
 *    swipe right → prev surah. `MushafLoader.loadSurah` prepends two
 *    decorative lines on top of the verses: the ornate surah-name plate
 *    (one glyph from `quran_titles.ttf`, surah N → codepoint 0xFB8D+N-1
 *    with a 0x21 jump after the 37th), and — for surahs 2..114 except 9
 *    — the basmallah (Al-Fatihah 1:1's words in page-1 QPC v4 glyphs).
 *
 * QPC v4 glyph advances are calibrated for the printed Mushaf line they
 * appear on, so layout always groups codepoints by `(page, line)` and
 * each line renders on its own screen row. In verse mode we add an
 * extra break at every verse boundary so verses don't share rows.
 */
class ReaderActivity : Activity() {

    private enum class Mode { Mushaf, Surah }

    private var mode = Mode.Mushaf
    private var currentPage = 1   // Mushaf mode index, 1..604
    private var currentSurah = 1  // Surah mode index, 1..114
    private var frameSeed = 0     // Surah-title frame design selector; tap title to cycle.

    private lateinit var canvas: VulkanCanvasView
    private lateinit var titleView: TextView
    private lateinit var modeButton: Button
    private lateinit var topBar: LinearLayout
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var loadJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(buildLayout())
        // Recompute layout once the top bar has its real measured height,
        // so the verse canvas starts right below it.
        topBar.viewTreeObserver.addOnGlobalLayoutListener(
            object : ViewTreeObserver.OnGlobalLayoutListener {
                override fun onGlobalLayout() {
                    topBar.viewTreeObserver.removeOnGlobalLayoutListener(this)
                    loadCurrent()
                }
            },
        )
    }

    override fun onDestroy() {
        scope.cancel()
        canvas.release()
        super.onDestroy()
    }

    private fun buildLayout(): View {
        canvas = VulkanCanvasView(this).apply {
            onHorizontalSwipe = { dir -> navigate(dir) }
        }

        topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(BG_TOPBAR)
            setPadding(24, 16, 24, 16)
        }
        modeButton = Button(this).apply {
            text = "Pages"
            setTextColor(INK_DARK)
            setOnClickListener { toggleMode() }
        }
        titleView = TextView(this).apply {
            text = ""
            setTextColor(INK_DARK)
            textSize = 18f
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f,
            )
            // Tap the title to cycle through frame styles (surah mode only).
            setOnClickListener {
                if (mode == Mode.Surah) {
                    frameSeed += 1
                    val s = SURAHS.firstOrNull { it.id == currentSurah }
                    val baseTitle = s?.let { "${it.id}. ${it.nameSimple}" }
                        ?: "Surah $currentSurah"
                    text = "$baseTitle  ·  style ${frameSeed % FRAME_STYLE_COUNT}"
                    // Fast path: the C++ cache lets us swap the frame
                    // layer without re-extracting the surah's glyphs.
                    // Falls back to a full reload only if the cache is
                    // empty (shouldn't happen mid-session).
                    canvas.updateFrameSeed(frameSeed) { ok ->
                        if (!ok) loadCurrent()
                    }
                }
            }
        }
        val prevBtn = Button(this).apply {
            text = "<"
            setTextColor(INK_DARK)
            setOnClickListener { navigate(-1) }
        }
        val nextBtn = Button(this).apply {
            text = ">"
            setTextColor(INK_DARK)
            setOnClickListener { navigate(+1) }
        }
        topBar.addView(modeButton)
        topBar.addView(prevBtn)
        topBar.addView(titleView)
        topBar.addView(nextBtn)

        val root = FrameLayout(this).apply { setBackgroundColor(BG_CREAM) }
        root.addView(
            canvas,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )
        root.addView(
            topBar,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP,
            ),
        )
        return root
    }

    private fun toggleMode() {
        mode = if (mode == Mode.Mushaf) Mode.Surah else Mode.Mushaf
        modeButton.text = if (mode == Mode.Mushaf) "Pages" else "Surah"
        loadCurrent()
    }

    /** dir = +1 for "next" (swipe left, since Arabic reads right→left),
     *  -1 for "previous". */
    private fun navigate(dir: Int) {
        when (mode) {
            Mode.Mushaf -> {
                val next = (currentPage + dir).coerceIn(1, 604)
                if (next == currentPage) return
                currentPage = next
            }
            Mode.Surah -> {
                val next = (currentSurah + dir).coerceIn(1, 114)
                if (next == currentSurah) return
                currentSurah = next
            }
        }
        loadCurrent()
    }

    private fun loadCurrent() {
        loadJob?.cancel()
        val app = application as BaqarahApp
        val screen = resources.displayMetrics
        val screenWidthPx = screen.widthPixels.toFloat()
        val screenHeightPx = screen.heightPixels.toFloat()
        val leftMargin = screenWidthPx * 0.02f
        val rightMargin = screenWidthPx * 0.02f

        val topMargin: Float
        val fontSizePx: Float
        val lineSpacingPx: Float
        val title: String
        val headerHeightPx = topBar.height.takeIf { it > 0 }?.toFloat() ?: 180f
        when (mode) {
            Mode.Mushaf -> {
                // 15 lines should fit between the top bar and a similar
                // bottom reserve. Baseline of line 1 sits one lineSpacing
                // below the bar so the line's ascenders fit cleanly.
                val avail = screenHeightPx - headerHeightPx - 80f
                lineSpacingPx = avail / 15f
                fontSizePx = lineSpacingPx / 2.6f
                topMargin = headerHeightPx + lineSpacingPx * 0.7f
                title = "Page $currentPage"
            }
            Mode.Surah -> {
                fontSizePx = 80f
                lineSpacingPx = fontSizePx * 2.6f
                // Line 0's top edge = baseline − 0.7·lineSpacing. Setting
                // topMargin to that exact gap flushes the procedural
                // frame (height = lineSpacingPx) against the top bar.
                topMargin = headerHeightPx + lineSpacingPx * 0.7f
                val s = SURAHS.firstOrNull { it.id == currentSurah }
                val baseTitle = s?.let { "${it.id}. ${it.nameSimple}" } ?: "Surah $currentSurah"
                title = "$baseTitle  ·  style ${frameSeed % FRAME_STYLE_COUNT}"
            }
        }
        titleView.text = title

        loadJob = scope.launch {
            try {
                val content = when (mode) {
                    Mode.Mushaf -> MushafLoader.loadPage(app, currentPage)
                    Mode.Surah -> MushafLoader.loadSurah(app, currentSurah, LineMode.ByVerse)
                }
                canvas.resetScroll()
                canvas.setColrSurah(
                    ttfs = content.ttfs,
                    codepoints = content.codepoints,
                    fontIndices = content.fontIndices,
                    lineStarts = content.lineStarts,
                    screenWidthPx = screenWidthPx,
                    leftMarginPx = leftMargin,
                    rightMarginPx = rightMargin,
                    topMarginPx = topMargin,
                    fontSizePx = fontSizePx,
                    lineSpacingPx = lineSpacingPx,
                    firstLineDecorate = (mode == Mode.Surah),
                    frameSeed = frameSeed,
                ) { totalHeightPx ->
                    canvas.scrollEnabled = (mode == Mode.Surah)
                    canvas.setContentHeight(totalHeightPx)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "load $mode failed (page=$currentPage surah=$currentSurah)", t)
            }
        }
    }

    companion object {
        private const val TAG = "BaqarahReader"

        private const val BG_CREAM = 0xFFF5EAD2.toInt()        // matches Vulkan clear
        private const val BG_TOPBAR = 0xFFE9DCBE.toInt()       // slightly darker cream
        private const val INK_DARK = 0xFF281E14.toInt()        // matches glyph fallback
        // Keep in sync with NUM_STYLES in jni_bridge.cpp emitFrame switch.
        private const val FRAME_STYLE_COUNT = 31
    }
}
