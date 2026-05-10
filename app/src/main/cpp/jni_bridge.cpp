#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdio>
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
    int unitsPerEm = 1000;
    bool ready = false;
};

bool initFontHandle(JNIEnv* env, jbyteArray ba, FontHandle& out) {
    const jsize n = env->GetArrayLength(ba);
    if (n <= 0) return false;
    out.data.assign((size_t)n, 0);
    env->GetByteArrayRegion(ba, 0, n, reinterpret_cast<jbyte*>(out.data.data()));
    int offset = stbtt_GetFontOffsetForIndex(out.data.data(), 0);
    if (offset < 0) return false;
    if (!stbtt_InitFont(&out.info, out.data.data(), offset)) return false;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&out.info, &ascent, &descent, &lineGap);
    out.unitsPerEm = (ascent - descent > 0) ? (ascent - descent) : 1000;
    out.ready = true;
    return true;
}

}  // namespace

// Multi-font, multi-verse RTL layout. Each codepoint picks its TTF by
// fontIndices[i] (parallel to codepoints[]). verseStarts[] partitions
// codepoints into verses (length numVerses+1; last entry = total
// codepoint count). Returns total content height in px, or -1 on error.
extern "C" JNIEXPORT jfloat JNICALL
Java_com_example_baqarah_vk_NativeRenderer_nUploadColrSurah(
    JNIEnv* env, jobject, jlong handle,
    jobjectArray ttfs,
    jintArray codepoints, jintArray fontIndices, jintArray verseStarts,
    jfloat screenWidthPx, jfloat leftMarginPx, jfloat rightMarginPx,
    jfloat topMarginPx, jfloat fontSizePx,
    jfloat lineSpacingPx, jfloat ayahSpacingPx) {
    auto* r = asRenderer(handle);
    if (!r || !ttfs || !codepoints || !fontIndices || !verseStarts) return -1.0f;

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
    const jsize vsCount = env->GetArrayLength(verseStarts);
    if (cpCount <= 0 || fiCount != cpCount || vsCount < 2) {
        LOGE("nUploadColrSurah: bad array sizes cp=%d fi=%d vs=%d",
             (int)cpCount, (int)fiCount, (int)vsCount);
        return -1.0f;
    }
    std::vector<int> cps((size_t)cpCount);
    std::vector<int> fis((size_t)fiCount);
    std::vector<int> vss((size_t)vsCount);
    env->GetIntArrayRegion(codepoints, 0, cpCount, cps.data());
    env->GetIntArrayRegion(fontIndices, 0, fiCount, fis.data());
    env->GetIntArrayRegion(verseStarts, 0, vsCount, vss.data());

    std::vector<float> allCurves;
    std::vector<float> layerData;
    std::vector<float> layerRects;
    int totalCurves = 0;
    int totalLayers = 0;

    const float minX = leftMarginPx;
    const float maxX = screenWidthPx - rightMarginPx;
    float baselineY = topMarginPx;
    const int numVerses = (int)vsCount - 1;

    for (int v = 0; v < numVerses; ++v) {
        // Begin a new verse on a new line. baselineY already points at the
        // first baseline (caller passes topMarginPx for verse 0; subsequent
        // verses get += lineSpacing + ayahSpacing at the end of the prior
        // verse loop body).
        float cursorX = maxX;

        for (int i = vss[v]; i < vss[v + 1]; ++i) {
            const int cp = cps[(size_t)i];
            const int fi = fis[(size_t)i];
            if (fi < 0 || fi >= numFonts) continue;
            FontHandle& font = fonts[(size_t)fi];
            const float scale = fontSizePx / (float)font.unitsPerEm;

            int gid = stbtt_FindGlyphIndex(&font.info, cp);
            if (gid == 0) continue;

            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font.info, gid, &advance, &lsb);
            const float advancePx = (float)advance * scale;

            // Wrap to next line if this glyph wouldn't fit.
            if (cursorX - advancePx < minX) {
                baselineY += lineSpacingPx;
                cursorX = maxX;
            }

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBox(&font.info, gid, &x0, &y0, &x1, &y1);
            const float bw = (float)(x1 - x0);
            const float bh = (float)(y1 - y0);

            if (bw > 0.0f && bh > 0.0f) {
                const float dstX = cursorX + (float)x0 * scale;
                const float dstY = baselineY - (float)y1 * scale;
                const float dstW = bw * scale;
                const float dstH = bh * scale;

                std::vector<baqarah::ColrLayer> layers;
                if (!baqarah::parseColrLayersV0(font.data.data(), (int)font.data.size(),
                                                gid, layers)) {
                    layers.push_back({gid, 0xFFFAEBC8u});
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

            cursorX -= advancePx;
        }

        // End of verse: advance to the next verse's baseline.
        baselineY += lineSpacingPx + ayahSpacingPx;
    }

    LOGI("nUploadColrSurah: verses=%d codepoints=%d -> %d layers, %d curves, contentY=%.0f",
         numVerses, (int)cpCount, totalLayers, totalCurves, baselineY);

    if (!r->setColrGlyphs(allCurves.data(), totalCurves,
                          layerData.data(), layerRects.data(),
                          totalLayers)) {
        return -1.0f;
    }
    return baselineY;
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

    // Resolve em-size scale from the font's vertical metrics so the
    // requested fontSizePx matches Skia's interpretation: scale =
    // fontSizePx / unitsPerEm. We don't have unitsPerEm directly from
    // stbtt, but ascent - descent is what stbtt itself uses.
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset)) return JNI_FALSE;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    const int unitsPerEm = (ascent - descent > 0) ? (ascent - descent) : 1000;
    const float scale = fontSizePx / (float)unitsPerEm;

    std::vector<float> allCurves;
    std::vector<float> layerData;   // 8 floats per layer (SSBO)
    std::vector<float> layerRects;  // 4 floats per layer (dst rect)
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
            layers.push_back({baseGid, 0xFFFAEBC8u});  // parchment fallback
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

    LOGI("nUploadColrLineFromTtf: %d codepoints -> %d layers, %d curves",
         (int)cpCount, totalLayers, totalCurves);

    return r->setColrGlyphs(allCurves.data(), totalCurves,
                            layerData.data(), layerRects.data(),
                            totalLayers) ? JNI_TRUE : JNI_FALSE;
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

    LOGI("nUploadColrFromTtf U+%04X (gid %d): %d layers, %d curves total",
         codepoint, baseGid, emittedLayers, totalCurves);

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
                            emittedLayers) ? JNI_TRUE : JNI_FALSE;
}
