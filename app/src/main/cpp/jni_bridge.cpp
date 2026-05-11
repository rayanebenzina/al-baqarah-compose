#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <sys/system_properties.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "colr_parser.h"
#include "glyph_extractor.h"
#include "third_party/stb_truetype.h"
#include "vk_renderer.h"

#define LOG_TAG "BaqarahVkJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using baqarah::VkRenderer;

static inline VkRenderer* asRenderer(jlong h) { return reinterpret_cast<VkRenderer*>(h); }

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nCreate(JNIEnv*, jobject) {
    auto* r = new VkRenderer();
    LOGI("nCreate -> %p", r);
    return reinterpret_cast<jlong>(r);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDestroy(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (r) delete r;
    LOGI("nDestroy %p", r);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nAttachSurface(
    JNIEnv* env, jobject, jlong handle, jobject surface) {
    auto* r = asRenderer(handle);
    if (!r || !surface) return JNI_FALSE;
    ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
    if (!w) return JNI_FALSE;
    bool ok = r->attachWindow(w);
    ANativeWindow_release(w);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDetachSurface(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (r) r->detachWindow();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDrawFrame(JNIEnv*, jobject, jlong handle) {
    auto* r = asRenderer(handle);
    if (!r) return JNI_FALSE;
    return r->drawFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nSetScrollY(JNIEnv*, jobject, jlong handle, jfloat y) {
    auto* r = asRenderer(handle);
    if (r) r->setScrollY(y);
}

namespace {

struct FontHandle {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    // Pixels-per-font-unit at fontSizePx=1. Multiply by fontSizePx for the
    // working scale. This is `1 / head.unitsPerEm`, not
    // `1 / (hhea.ascent - hhea.descent)` — those two differ wildly in QPC
    // v4 (e.g. 2500 vs 6460), and getting it wrong shrinks the layout by
    // ~2.5x.
    float scaleFor1em = 0.0f;
    bool ready = false;
};

// Build a band table for one layer's curves. Each layer has its curves
// in `allCurves` starting at `curveOffsetGlobal` (6 floats per curve,
// already normalized to [0,1] UV in TTF Y-up). For each band, append
// the local curve indices (0..curveCount-1) of curves whose v-range
// (convex-hull bound from the 3 control points) touches that band.
//
// bandsArr receives `kNumBands` ivec2 entries (offset, count) into
// curveIndicesArr; curveIndicesArr receives the indices themselves.
void appendLayerBands(const std::vector<float>& allCurves,
                      int curveOffsetGlobal, int curveCount,
                      std::vector<int>& bandsArr,
                      std::vector<int>& curveIndicesArr) {
    constexpr int NB = baqarah::VkRenderer::kNumBands;
    std::array<std::vector<int>, NB> perBand;
    for (int ci = 0; ci < curveCount; ++ci) {
        const float* C = &allCurves[(size_t)(curveOffsetGlobal + ci) * 6];
        const float y0 = C[1], y1 = C[3], y2 = C[5];
        const float yMin = std::min(std::min(y0, y1), y2);
        const float yMax = std::max(std::max(y0, y1), y2);
        int bMin = (int)std::floor(yMin * (float)NB);
        int bMax = (int)std::floor(yMax * (float)NB);
        bMin = std::max(0, std::min(NB - 1, bMin));
        bMax = std::max(0, std::min(NB - 1, bMax));
        for (int b = bMin; b <= bMax; ++b) {
            perBand[(size_t)b].push_back(ci);
        }
    }
    for (int b = 0; b < NB; ++b) {
        bandsArr.push_back((int)curveIndicesArr.size());
        bandsArr.push_back((int)perBand[(size_t)b].size());
        for (int idx : perBand[(size_t)b]) curveIndicesArr.push_back(idx);
    }
}

bool initFontHandle(JNIEnv* env, jbyteArray ba, FontHandle& out) {
    const jsize n = env->GetArrayLength(ba);
    if (n <= 0) return false;
    out.data.assign((size_t)n, 0);
    env->GetByteArrayRegion(ba, 0, n, reinterpret_cast<jbyte*>(out.data.data()));
    int offset = stbtt_GetFontOffsetForIndex(out.data.data(), 0);
    if (offset < 0) return false;
    if (!stbtt_InitFont(&out.info, out.data.data(), offset)) return false;
    out.scaleFor1em = stbtt_ScaleForMappingEmToPixels(&out.info, 1.0f);
    if (out.scaleFor1em <= 0.0f) return false;
    out.ready = true;
    return true;
}

#include "procedural_frame.inl"


// Cached state of the last nUploadColrSurah call, captured after the
// surah glyphs are emitted but *before* the procedural frame is added.
// nUpdateFrameSeed restores from this snapshot, re-emits the frame
// layer with a new seed, and re-uploads — skipping the ~6.9k stbtt
// glyph extractions that dominate a full surah upload.
struct SurahFrameCache {
    bool valid = false;
    // Geometry needed to re-emit the frame.
    float frameX = 0.0f, frameY = 0.0f, frameW = 0.0f, frameH = 0.0f;
    // Renderer handle the cache was built for. Cache is invalidated if
    // the handle changes (e.g. activity rebuilt).
    VkRenderer* renderer = nullptr;
    // Snapshot of arrays at the moment the surah glyphs were complete.
    // emitFrame's curves/layer/bands are *not* in here — they are added
    // anew on every update so the seed can change.
    std::vector<float> allCurves;
    std::vector<float> layerData;
    std::vector<float> layerRects;
    std::vector<int>   bandsArr;
    std::vector<int>   curveIndicesArr;
    int totalCurves = 0;
    int totalLayers = 0;
};
static SurahFrameCache g_surahCache;

}  // namespace

// Multi-font, Mushaf-line RTL layout. Each codepoint picks its TTF by
// fontIndices[i] (parallel to codepoints[]). lineStarts[] partitions
// codepoints into Mushaf lines (length numLines+1; last entry = total
// codepoint count). QPC v4 glyph advances are calibrated for the specific
// Mushaf line a glyph appears on (including stretched kashida variants for
// justification), so each line must be rendered on its own screen row with
// no soft-wrapping. If `firstLineDecorate` is non-zero, the first line is
// centered horizontally (instead of right-aligned RTL) and a procedural
// Mushaf-style frame is drawn around it via `emitFrame`. Returns total
// content height in px, or -1 on error.
extern "C" JNIEXPORT jfloat JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadColrSurah(
    JNIEnv* env, jobject, jlong handle,
    jobjectArray ttfs,
    jintArray codepoints, jintArray fontIndices, jintArray lineStarts,
    jfloat screenWidthPx, jfloat leftMarginPx, jfloat rightMarginPx,
    jfloat topMarginPx, jfloat fontSizePx,
    jfloat lineSpacingPx,
    jboolean firstLineDecorate, jint frameSeed) {
    auto* r = asRenderer(handle);
    if (!r || !ttfs || !codepoints || !fontIndices || !lineStarts) return -1.0f;

    const jsize numFonts = env->GetArrayLength(ttfs);
    if (numFonts <= 0) return -1.0f;
    std::vector<FontHandle> fonts((size_t)numFonts);
    for (jsize i = 0; i < numFonts; ++i) {
        jbyteArray ba = (jbyteArray)env->GetObjectArrayElement(ttfs, i);
        if (!ba || !initFontHandle(env, ba, fonts[(size_t)i])) {
            LOGE("nUploadColrSurah: failed to load font %d", (int)i);
            env->DeleteLocalRef(ba);
            return -1.0f;
        }
        env->DeleteLocalRef(ba);
    }

    const jsize cpCount = env->GetArrayLength(codepoints);
    const jsize fiCount = env->GetArrayLength(fontIndices);
    const jsize lsCount = env->GetArrayLength(lineStarts);
    if (cpCount <= 0 || fiCount != cpCount || lsCount < 2) {
        LOGE("nUploadColrSurah: bad array sizes cp=%d fi=%d ls=%d",
             (int)cpCount, (int)fiCount, (int)lsCount);
        return -1.0f;
    }
    std::vector<int> cps((size_t)cpCount);
    std::vector<int> fis((size_t)fiCount);
    std::vector<int> lss((size_t)lsCount);
    env->GetIntArrayRegion(codepoints, 0, cpCount, cps.data());
    env->GetIntArrayRegion(fontIndices, 0, fiCount, fis.data());
    env->GetIntArrayRegion(lineStarts, 0, lsCount, lss.data());

    std::vector<float> allCurves;
    std::vector<float> layerData;
    std::vector<float> layerRects;
    std::vector<int>   bandsArr;
    std::vector<int>   curveIndicesArr;
    int totalCurves = 0;
    int totalLayers = 0;

    const float minX = leftMarginPx;
    const float maxX = screenWidthPx - rightMarginPx;
    const float usableWidth = maxX - minX;
    float baselineY = topMarginPx;
    const int numLines = (int)lsCount - 1;
    // Baseline of line 0 — captured for the post-loop frame emission.
    const float line0Baseline = baselineY;

    for (int ln = 0; ln < numLines; ++ln) {
        // First pass: compute the line's natural width at fontSizePx, so we
        // can shrink lines wider than the screen and keep narrower lines at
        // the requested size. QPC v4 Mushaf lines are designed to fill one
        // page-width worth of advances, so most lines hit the cap; shorter
        // lines (surah-start fragments, basmala lines) render larger.
        float naturalWidthPx = 0.0f;
        for (int i = lss[ln]; i < lss[ln + 1]; ++i) {
            const int fi = fis[(size_t)i];
            if (fi < 0 || fi >= numFonts) continue;
            FontHandle& font = fonts[(size_t)fi];
            int gid = stbtt_FindGlyphIndex(&font.info, cps[(size_t)i]);
            if (gid == 0) continue;
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font.info, gid, &advance, &lsb);
            naturalWidthPx += (float)advance * font.scaleFor1em * fontSizePx;
        }
        const float lineFs = (naturalWidthPx > usableWidth && naturalWidthPx > 0.0f)
                                 ? fontSizePx * (usableWidth / naturalWidthPx)
                                 : fontSizePx;
        // Line width at the chosen lineFs (advances scale linearly with fs).
        const float scaledWidthPx = naturalWidthPx > 0.0f
                                        ? naturalWidthPx * (lineFs / fontSizePx)
                                        : 0.0f;

        // Line 0 in surah mode (when decorated) is centered, not RTL
        // right-aligned, so the surah-name plate sits in the middle of
        // the frame we'll emit after the glyph(s). Center by the actual
        // ink bbox (x0..x1) rather than the advance box — ornate title
        // plates often have asymmetric side bearings, so advance-box
        // centering leaves them visibly off.
        const bool centerThisLine = (firstLineDecorate && ln == 0);
        float cursorX;
        float lineBaselineY = baselineY;
        if (centerThisLine) {
            float inkLeft = std::numeric_limits<float>::infinity();
            float inkRight = -std::numeric_limits<float>::infinity();
            // Vertical extent in pixels above the baseline (positive = up).
            // inkTopPx = top of ink, inkBotPx = bottom of ink. We collect
            // both so we can shift the baseline and land the ink's vertical
            // center at the frame's vertical center.
            float inkTopPx = -std::numeric_limits<float>::infinity();
            float inkBotPx = std::numeric_limits<float>::infinity();
            float probeX = 0.0f;  // synthetic origin
            for (int i = lss[ln]; i < lss[ln + 1]; ++i) {
                const int fi = fis[(size_t)i];
                if (fi < 0 || fi >= numFonts) continue;
                FontHandle& font = fonts[(size_t)fi];
                const float scale = font.scaleFor1em * lineFs;
                int gid = stbtt_FindGlyphIndex(&font.info, cps[(size_t)i]);
                if (gid == 0) continue;
                int advance = 0, lsb = 0;
                stbtt_GetGlyphHMetrics(&font.info, gid, &advance, &lsb);
                probeX -= (float)advance * scale;
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphBox(&font.info, gid, &x0, &y0, &x1, &y1);
                if (x1 > x0 && y1 > y0) {
                    const float l = probeX + (float)x0 * scale;
                    const float r = probeX + (float)x1 * scale;
                    if (l < inkLeft)  inkLeft  = l;
                    if (r > inkRight) inkRight = r;
                    const float t = (float)y1 * scale;
                    const float b = (float)y0 * scale;
                    if (t > inkTopPx) inkTopPx = t;
                    if (b < inkBotPx) inkBotPx = b;
                }
            }
            const float frameCenterX = (minX + maxX) * 0.5f;
            if (inkRight > inkLeft) {
                const float inkCenterX = (inkLeft + inkRight) * 0.5f;
                cursorX = frameCenterX - inkCenterX;  // shift so ink center = frame center
            } else {
                cursorX = frameCenterX + scaledWidthPx * 0.5f;
            }
            // Frame center Y: frameY = baselineY − 0.7·lineSpacing,
            // frameH = lineSpacing → center = baselineY − 0.2·lineSpacing.
            // Override the baseline so ink center lands there.
            const float frameCenterY = baselineY - lineSpacingPx * 0.2f;
            if (std::isfinite(inkTopPx) && std::isfinite(inkBotPx)) {
                lineBaselineY = frameCenterY + (inkTopPx + inkBotPx) * 0.5f;
            }
        } else {
            cursorX = maxX;
        }

        for (int i = lss[ln]; i < lss[ln + 1]; ++i) {
            const int cp = cps[(size_t)i];
            const int fi = fis[(size_t)i];
            if (fi < 0 || fi >= numFonts) continue;
            FontHandle& font = fonts[(size_t)fi];
            const float scale = font.scaleFor1em * lineFs;

            int gid = stbtt_FindGlyphIndex(&font.info, cp);
            if (gid == 0) continue;

            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font.info, gid, &advance, &lsb);
            const float advancePx = (float)advance * scale;

            // RTL: cursor is the RIGHT edge of the current glyph's advance
            // box. Move it left by the advance before drawing so dstX
            // places the glyph's bbox correctly relative to its origin.
            cursorX -= advancePx;

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBox(&font.info, gid, &x0, &y0, &x1, &y1);
            const float bw = (float)(x1 - x0);
            const float bh = (float)(y1 - y0);

            if (bw > 0.0f && bh > 0.0f) {
                const float dstX = cursorX + (float)x0 * scale;
                const float dstY = lineBaselineY - (float)y1 * scale;
                const float dstW = bw * scale;
                const float dstH = bh * scale;

                std::vector<baqarah::ColrLayer> layers;
                if (!baqarah::parseColrLayersV0(font.data.data(), (int)font.data.size(),
                                                gid, layers)) {
                    layers.push_back({gid, 0xFF281E14u});  // dark brown on cream
                }

                for (const auto& L : layers) {
                    baqarah::GlyphOutline lo;
                    int curveOffset = totalCurves;
                    int curveCountForLayer = 0;
                    if (baqarah::extractGlyphOutlineByIndex(font.data.data(),
                                                            (int)font.data.size(),
                                                            L.glyphId, lo)) {
                        for (size_t k = 0; k < lo.curves.size(); k += 2) {
                            const float u = (lo.curves[k + 0] - (float)x0) / bw;
                            const float vUV = (lo.curves[k + 1] - (float)y0) / bh;
                            allCurves.push_back(u);
                            allCurves.push_back(vUV);
                        }
                        curveCountForLayer = lo.curveCount;
                        totalCurves += curveCountForLayer;
                    }
                    appendLayerBands(allCurves, curveOffset, curveCountForLayer,
                                     bandsArr, curveIndicesArr);
                    layerData.push_back((float)curveOffset);
                    layerData.push_back((float)curveCountForLayer);
                    layerData.push_back(0.0f);
                    layerData.push_back(0.0f);
                    const float a = ((L.rgba >> 24) & 0xFF) / 255.0f;
                    const float rC = ((L.rgba >> 16) & 0xFF) / 255.0f;
                    const float gC = ((L.rgba >> 8) & 0xFF) / 255.0f;
                    const float bC = (L.rgba & 0xFF) / 255.0f;
                    layerData.push_back(rC);
                    layerData.push_back(gC);
                    layerData.push_back(bC);
                    layerData.push_back(a);
                    layerRects.push_back(dstX);
                    layerRects.push_back(dstY);
                    layerRects.push_back(dstW);
                    layerRects.push_back(dstH);
                    ++totalLayers;
                }
            }
        }

        baselineY += lineSpacingPx;
    }

    // Snapshot the post-glyph state for the fast-path style swap. The
    // frame is appended below, so the cache holds the surah-only data
    // exactly as it stood here.
    if (firstLineDecorate) {
        g_surahCache.renderer = r;
        g_surahCache.allCurves = allCurves;
        g_surahCache.layerData = layerData;
        g_surahCache.layerRects = layerRects;
        g_surahCache.bandsArr = bandsArr;
        g_surahCache.curveIndicesArr = curveIndicesArr;
        g_surahCache.totalCurves = totalCurves;
        g_surahCache.totalLayers = totalLayers;
        g_surahCache.frameW = screenWidthPx;
        g_surahCache.frameH = lineSpacingPx;
        g_surahCache.frameX = 0.0f;
        g_surahCache.frameY = line0Baseline - lineSpacingPx * 0.7f;
        g_surahCache.valid = true;
    } else {
        g_surahCache.valid = false;
    }

    // Append the procedural frame as the last layer. (Doing it after the
    // line loop keeps the frame's curves at the tail of the arrays so the
    // surah-only prefix above is reusable.)
    if (firstLineDecorate && numLines > 0) {
        emitFrame(g_surahCache.frameX, g_surahCache.frameY,
                  g_surahCache.frameW, g_surahCache.frameH,
                  0xFF281E14u,
                  allCurves, layerData, layerRects,
                  bandsArr, curveIndicesArr,
                  totalCurves, totalLayers,
                  (int)frameSeed);
    }

    const int bandsCount = totalLayers * baqarah::VkRenderer::kNumBands;
    LOGI("nUploadColrSurah: lines=%d codepoints=%d -> %d layers, %d curves, "
         "%d bands, %zu curveIndices, contentY=%.0f",
         numLines, (int)cpCount, totalLayers, totalCurves,
         bandsCount, curveIndicesArr.size(), baselineY);

    if (!r->setColrGlyphs(allCurves.data(), totalCurves,
                          layerData.data(), layerRects.data(),
                          totalLayers,
                          bandsArr.data(), bandsCount,
                          curveIndicesArr.data(), (int)curveIndicesArr.size())) {
        return -1.0f;
    }
    return baselineY;
}

// Fast path for changing the procedural frame style without re-running
// glyph extraction. Restores the cached post-glyph state from the last
// nUploadColrSurah call, re-emits the frame layer with the new seed,
// and re-uploads. Returns false (and the caller should fall back to a
// full nUploadColrSurah) if the cache is empty, was built for a
// different renderer, or has no frame layer.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUpdateFrameSeed(
    JNIEnv*, jobject, jlong handle, jint frameSeed) {
    auto* r = asRenderer(handle);
    if (!r) return JNI_FALSE;
    if (!g_surahCache.valid || g_surahCache.renderer != r) return JNI_FALSE;

    // Copy snapshot into working vectors so subsequent calls keep
    // working from the same cache.
    std::vector<float> allCurves = g_surahCache.allCurves;
    std::vector<float> layerData = g_surahCache.layerData;
    std::vector<float> layerRects = g_surahCache.layerRects;
    std::vector<int>   bandsArr = g_surahCache.bandsArr;
    std::vector<int>   curveIndicesArr = g_surahCache.curveIndicesArr;
    int totalCurves = g_surahCache.totalCurves;
    int totalLayers = g_surahCache.totalLayers;

    emitFrame(g_surahCache.frameX, g_surahCache.frameY,
              g_surahCache.frameW, g_surahCache.frameH,
              0xFF281E14u,
              allCurves, layerData, layerRects,
              bandsArr, curveIndicesArr,
              totalCurves, totalLayers,
              (int)frameSeed);

    const int bandsCount = totalLayers * baqarah::VkRenderer::kNumBands;
    if (!r->setColrGlyphs(allCurves.data(), totalCurves,
                          layerData.data(), layerRects.data(),
                          totalLayers,
                          bandsArr.data(), bandsCount,
                          curveIndicesArr.data(), (int)curveIndicesArr.size())) {
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

// Debug helper for the frame-eval pipeline: sweep a contiguous range
// of seeds, calling emitFrame on throwaway local vectors so each seed
// dumps its ASCII grid to logcat (assuming debug.baqarah.frame=1) but
// nothing reaches the renderer. Lets us score thousands of seeds
// against the reference target without touching the UI thread per
// seed. No-op if the surah cache is empty.
extern "C" JNIEXPORT jint JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nDumpFrameAsciiSweep(
    JNIEnv*, jobject, jlong handle, jint seedStart, jint seedCount,
    jint stride) {
    auto* r = asRenderer(handle);
    if (!r) return 0;
    if (!g_surahCache.valid || g_surahCache.renderer != r) return 0;
    const int step = stride <= 0 ? 1 : stride;
    int emitted = 0;
    for (int s = 0; s < seedCount; ++s) {
        std::vector<float> ac, ld, lr;
        std::vector<int> bd, ci;
        int tc = 0, tl = 0;
        emitFrame(g_surahCache.frameX, g_surahCache.frameY,
                  g_surahCache.frameW, g_surahCache.frameH,
                  0xFF281E14u, ac, ld, lr, bd, ci, tc, tl,
                  seedStart + s * step);
        ++emitted;
    }
    return emitted;
}

// Lay out an array of codepoints RTL and render each through the
// COLR pipeline. `cursorX, baselineY` give the starting point (right
// edge of the line, baseline y). `fontSizePx` is the desired text size.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadColrLineFromTtf(
    JNIEnv* env, jobject, jlong handle,
    jbyteArray ttfBytes, jintArray codepoints,
    jfloat cursorX, jfloat baselineY, jfloat fontSizePx) {
    auto* r = asRenderer(handle);
    if (!r || !ttfBytes || !codepoints) return JNI_FALSE;

    const jsize ttfSize = env->GetArrayLength(ttfBytes);
    if (ttfSize <= 0) return JNI_FALSE;
    std::vector<uint8_t> ttf((size_t)ttfSize);
    env->GetByteArrayRegion(ttfBytes, 0, ttfSize, reinterpret_cast<jbyte*>(ttf.data()));

    const jsize cpCount = env->GetArrayLength(codepoints);
    if (cpCount <= 0) return JNI_FALSE;
    std::vector<int> cps((size_t)cpCount);
    env->GetIntArrayRegion(codepoints, 0, cpCount, cps.data());

    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset)) return JNI_FALSE;
    // Scale via head.unitsPerEm, not ascent-descent — QPC v4 has those
    // two values diverging by 2-3x.
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, fontSizePx);

    std::vector<float> allCurves;
    std::vector<float> layerData;   // 8 floats per layer (SSBO)
    std::vector<float> layerRects;  // 4 floats per layer (dst rect)
    std::vector<int>   bandsArr;
    std::vector<int>   curveIndicesArr;
    int totalCurves = 0;
    int totalLayers = 0;

    for (int cpIdx = 0; cpIdx < cpCount; ++cpIdx) {
        const int codepoint = cps[cpIdx];
        const int baseGid = baqarah::findGlyphIndex(ttf.data(), ttfSize, codepoint);
        if (baseGid == 0) {
            LOGI("nUploadColrLineFromTtf: skipping U+%04X (no glyph)", codepoint);
            continue;
        }
        baqarah::GlyphOutline base;
        if (!baqarah::extractGlyphOutlineByIndex(ttf.data(), ttfSize, baseGid, base)) {
            continue;
        }

        const float bx0 = (float)base.bboxMinX;
        const float by0 = (float)base.bboxMinY;
        const float bx1 = (float)base.bboxMaxX;
        const float by1 = (float)base.bboxMaxY;
        const float bw = bx1 - bx0;
        const float bh = by1 - by0;
        if (bw <= 0.0f || bh <= 0.0f) {
            // Whitespace-like glyph: just advance the cursor.
            cursorX -= (float)base.advanceX * scale;
            continue;
        }

        // Quad position in screen space: origin (cursorX, baselineY)
        // maps to font-space (0, 0). Apply scale + Y-flip (font Y-up,
        // screen Y-down).
        const float dstX = cursorX + bx0 * scale;
        const float dstY = baselineY - by1 * scale;
        const float dstW = bw * scale;
        const float dstH = bh * scale;

        // Resolve COLR layers (or fall back to single layer = base glyph).
        std::vector<baqarah::ColrLayer> layers;
        if (!baqarah::parseColrLayersV0(ttf.data(), ttfSize, baseGid, layers)) {
            layers.push_back({baseGid, 0xFF281E14u});  // dark brown on cream
        }

        for (const auto& L : layers) {
            baqarah::GlyphOutline lo;
            int curveOffset = totalCurves;
            int curveCountForLayer = 0;
            if (baqarah::extractGlyphOutlineByIndex(ttf.data(), ttfSize, L.glyphId, lo)) {
                for (size_t i = 0; i < lo.curves.size(); i += 2) {
                    const float u = (lo.curves[i + 0] - bx0) / bw;
                    const float v = (lo.curves[i + 1] - by0) / bh;
                    allCurves.push_back(u);
                    allCurves.push_back(v);
                }
                curveCountForLayer = lo.curveCount;
                totalCurves += curveCountForLayer;
            }
            appendLayerBands(allCurves, curveOffset, curveCountForLayer,
                             bandsArr, curveIndicesArr);

            // Per-layer SSBO entry: (offset, count, _, _, r, g, b, a)
            layerData.push_back((float)curveOffset);
            layerData.push_back((float)curveCountForLayer);
            layerData.push_back(0.0f);
            layerData.push_back(0.0f);
            const float a = ((L.rgba >> 24) & 0xFF) / 255.0f;
            const float rC = ((L.rgba >> 16) & 0xFF) / 255.0f;
            const float gC = ((L.rgba >> 8)  & 0xFF) / 255.0f;
            const float bC = ( L.rgba        & 0xFF) / 255.0f;
            layerData.push_back(rC);
            layerData.push_back(gC);
            layerData.push_back(bC);
            layerData.push_back(a);

            // Per-layer dst rect (all of this codepoint's layers share it).
            layerRects.push_back(dstX);
            layerRects.push_back(dstY);
            layerRects.push_back(dstW);
            layerRects.push_back(dstH);
            ++totalLayers;
        }

        cursorX -= (float)base.advanceX * scale;
    }

    const int bandsCount = totalLayers * baqarah::VkRenderer::kNumBands;
    LOGI("nUploadColrLineFromTtf: %d codepoints -> %d layers, %d curves, "
         "%d bands, %zu curveIndices",
         (int)cpCount, totalLayers, totalCurves,
         bandsCount, curveIndicesArr.size());

    return r->setColrGlyphs(allCurves.data(), totalCurves,
                            layerData.data(), layerRects.data(),
                            totalLayers,
                            bandsArr.data(), bandsCount,
                            curveIndicesArr.data(), (int)curveIndicesArr.size())
               ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadColrFromTtf(
    JNIEnv* env, jobject, jlong handle,
    jbyteArray ttfBytes, jint codepoint,
    jfloat dstX, jfloat dstY, jfloat dstW, jfloat dstH) {
    auto* r = asRenderer(handle);
    if (!r || !ttfBytes) return JNI_FALSE;

    const jsize ttfSize = env->GetArrayLength(ttfBytes);
    if (ttfSize <= 0) return JNI_FALSE;
    std::vector<uint8_t> ttf((size_t)ttfSize);
    env->GetByteArrayRegion(ttfBytes, 0, ttfSize, reinterpret_cast<jbyte*>(ttf.data()));

    // Look up base glyph index for codepoint.
    int baseGid = baqarah::findGlyphIndex(ttf.data(), ttfSize, codepoint);
    if (baseGid == 0) {
        LOGE("nUploadColrFromTtf: no glyph for U+%04X", codepoint);
        return JNI_FALSE;
    }

    // Extract the base glyph to get its bbox — all layers normalize to this.
    baqarah::GlyphOutline base;
    if (!baqarah::extractGlyphOutlineByIndex(ttf.data(), ttfSize, baseGid, base)) {
        LOGE("nUploadColrFromTtf: no outline for base gid %d", baseGid);
        return JNI_FALSE;
    }

    // Resolve COLR layers. If absent, fall back to a single layer = base glyph
    // in opaque white.
    std::vector<baqarah::ColrLayer> layers;
    if (!baqarah::parseColrLayersV0(ttf.data(), ttfSize, baseGid, layers)) {
        layers.push_back({baseGid, 0xFFFFFFFFu});
        LOGI("nUploadColrFromTtf: no COLR for gid %d, using base as single layer", baseGid);
    }

    const float bx0 = (float)base.bboxMinX;
    const float by0 = (float)base.bboxMinY;
    const float bx1 = (float)base.bboxMaxX;
    const float by1 = (float)base.bboxMaxY;
    const float bw = bx1 - bx0;
    const float bh = by1 - by0;
    if (bw <= 0.0f || bh <= 0.0f) return JNI_FALSE;

    std::vector<float> allCurves;
    std::vector<float> layerData;  // 8 floats per layer
    std::vector<int>   bandsArr;
    std::vector<int>   curveIndicesArr;
    allCurves.reserve(2048);
    layerData.reserve(layers.size() * 8);

    int totalCurves = 0;
    int emittedLayers = 0;
    for (const auto& L : layers) {
        baqarah::GlyphOutline lo;
        if (!baqarah::extractGlyphOutlineByIndex(ttf.data(), ttfSize, L.glyphId, lo)) {
            // Empty / missing layer — record an empty range so vertex/layer
            // counts stay aligned (the shader will discard via count==0).
            layerData.push_back(0.0f); layerData.push_back(0.0f);
            layerData.push_back(0.0f); layerData.push_back(0.0f);
            const float a = ((L.rgba >> 24) & 0xFF) / 255.0f;
            const float r = ((L.rgba >> 16) & 0xFF) / 255.0f;
            const float g = ((L.rgba >> 8)  & 0xFF) / 255.0f;
            const float b = ( L.rgba        & 0xFF) / 255.0f;
            layerData.push_back(r); layerData.push_back(g);
            layerData.push_back(b); layerData.push_back(a);
            appendLayerBands(allCurves, totalCurves, 0,
                             bandsArr, curveIndicesArr);
            emittedLayers++;
            continue;
        }
        const int offset = totalCurves;
        const int count = lo.curveCount;
        // Append normalized curves (UV in base-glyph bbox). Do NOT flip Y
        // here — the vertex shader will flip the quad UV space instead, so
        // the shader winding test stays consistent with TTF's CCW
        // convention.
        for (size_t i = 0; i < lo.curves.size(); i += 2) {
            const float u = (lo.curves[i + 0] - bx0) / bw;
            const float v = (lo.curves[i + 1] - by0) / bh;
            allCurves.push_back(u);
            allCurves.push_back(v);
        }
        totalCurves += count;
        appendLayerBands(allCurves, offset, count, bandsArr, curveIndicesArr);

        // Per-layer entry (2 vec4 / 8 floats).
        layerData.push_back((float)offset);
        layerData.push_back((float)count);
        layerData.push_back(0.0f); layerData.push_back(0.0f);
        const float a = ((L.rgba >> 24) & 0xFF) / 255.0f;
        const float r = ((L.rgba >> 16) & 0xFF) / 255.0f;
        const float g = ((L.rgba >> 8)  & 0xFF) / 255.0f;
        const float b = ( L.rgba        & 0xFF) / 255.0f;
        layerData.push_back(r); layerData.push_back(g);
        layerData.push_back(b); layerData.push_back(a);
        emittedLayers++;
    }

    const int bandsCount = emittedLayers * baqarah::VkRenderer::kNumBands;
    LOGI("nUploadColrFromTtf U+%04X (gid %d): %d layers, %d curves total, "
         "%d bands, %zu curveIndices",
         codepoint, baseGid, emittedLayers, totalCurves,
         bandsCount, curveIndicesArr.size());

    // Replicate one rect per layer so all layers stack at the same position.
    std::vector<float> rects((size_t)emittedLayers * 4);
    for (int i = 0; i < emittedLayers; ++i) {
        rects[i * 4 + 0] = dstX;
        rects[i * 4 + 1] = dstY;
        rects[i * 4 + 2] = dstW;
        rects[i * 4 + 3] = dstH;
    }

    return r->setColrGlyphs(allCurves.data(), totalCurves,
                            layerData.data(), rects.data(),
                            emittedLayers,
                            bandsArr.data(), bandsCount,
                            curveIndicesArr.data(), (int)curveIndicesArr.size())
               ? JNI_TRUE : JNI_FALSE;
}
