#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
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

// Procedural Mushaf-style frame around a title glyph, emitted as one
// extra COLR layer through the existing Slug outline pipeline.
//
// Curves are quadratic Béziers in UV [0,1]² (Y-up to match TTF
// convention; the vertex shader flips quad.v so this still maps to a
// Y-down screen rect). Three shapes share the layer:
//
//   - **Outer rounded rectangle, CCW**: contributes +1 winding inside.
//   - **Inner rounded rectangle, CW**: contributes −1, cutting a hole
//     to leave only the stroke band filled.
//   - **Four corner diamonds**: small filled lozenges sitting inside the
//     inner rect, near the corners. Their winding either reinforces or
//     cancels the inner hole — non-zero / even-odd both render them as
//     filled either way.
//
// Mushaf-style surah-title frame: triple stroke (outer thick, middle,
// inner thin) with twelve-pointed corner stars, eight-pointed end
// medallions, and small six-pointed accents along the long edges. All
// curve sizes are derived from the short dimension so the design stays
// visually consistent at any aspect ratio.
void emitFrame(float dstX, float dstY, float dstW, float dstH,
               uint32_t color,
               std::vector<float>& allCurves,
               std::vector<float>& layerData,
               std::vector<float>& layerRects,
               std::vector<int>& bandsArr,
               std::vector<int>& curveIndicesArr,
               int& totalCurves,
               int& totalLayers) {
    const float minSide  = std::min(dstW, dstH);

    // Triple band: outer thick + gap + middle + gap + inner thin.
    const float strokeOuterPx = minSide * 0.045f;
    const float gapOMPx       = minSide * 0.030f;
    const float strokeMidPx   = minSide * 0.020f;
    const float gapMIPx       = minSide * 0.015f;
    const float strokeInnerPx = minSide * 0.010f;
    const float bandPx = strokeOuterPx + gapOMPx + strokeMidPx +
                         gapMIPx + strokeInnerPx;

    const int curveStart = totalCurves;

    auto curve = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        allCurves.push_back(x0); allCurves.push_back(y0);
        allCurves.push_back(x1); allCurves.push_back(y1);
        allCurves.push_back(x2); allCurves.push_back(y2);
        ++totalCurves;
    };
    auto line = [&](float x0, float y0, float x1, float y1) {
        curve(x0, y0, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, x1, y1);
    };
    // CCW outer outline (in UV space; with v=0 at top, this traces
    // top → right → bottom → left and produces a filled body under
    // non-zero winding).
    auto rectOuter = [&](float u0, float v0, float u1, float v1) {
        line(u0, v0, u1, v0);
        line(u1, v0, u1, v1);
        line(u1, v1, u0, v1);
        line(u0, v1, u0, v0);
    };
    // Opposite winding — cuts a hole inside the surrounding outer rect.
    auto rectHole = [&](float u0, float v0, float u1, float v1) {
        line(u1, v0, u0, v0);
        line(u0, v0, u0, v1);
        line(u0, v1, u1, v1);
        line(u1, v1, u1, v0);
    };

    // -------- Triple stroke bands --------
    {
        const float sU = strokeOuterPx / dstW, sV = strokeOuterPx / dstH;
        rectOuter(0.0f, 0.0f, 1.0f, 1.0f);
        rectHole(sU, sV, 1.0f - sU, 1.0f - sV);
    }
    {
        const float offPx = strokeOuterPx + gapOMPx;
        const float oU = offPx / dstW, oV = offPx / dstH;
        const float sU = strokeMidPx / dstW, sV = strokeMidPx / dstH;
        rectOuter(oU, oV, 1.0f - oU, 1.0f - oV);
        rectHole(oU + sU, oV + sV, 1.0f - oU - sU, 1.0f - oV - sV);
    }
    {
        const float offPx = strokeOuterPx + gapOMPx + strokeMidPx + gapMIPx;
        const float oU = offPx / dstW, oV = offPx / dstH;
        const float sU = strokeInnerPx / dstW, sV = strokeInnerPx / dstH;
        rectOuter(oU, oV, 1.0f - oU, 1.0f - oV);
        rectHole(oU + sU, oV + sV, 1.0f - oU - sU, 1.0f - oV - sV);
    }

    // -------- Ornaments --------
    // polystar lays down N*2 line segments alternating between an outer
    // and an inner radius, producing a filled N-pointed star.
    auto polystar = [&](float cxU, float cyV,
                         float ruOut, float rvOut, float ruIn, float rvIn,
                         int points, float phase) {
        const int N = points * 2;
        float prevX = 0.0f, prevY = 0.0f;
        for (int i = 0; i <= N; ++i) {
            const float ang = phase + (float)i * 3.14159265f / (float)points;
            const bool isOut = (i & 1) == 0;
            const float ru = isOut ? ruOut : ruIn;
            const float rv = isOut ? rvOut : rvIn;
            const float x = cxU + cosf(ang) * ru;
            const float y = cyV + sinf(ang) * rv;
            if (i > 0) line(prevX, prevY, x, y);
            prevX = x; prevY = y;
        }
    };

    // Four corner 12-pointed stars sit just inside the innermost band.
    const float cornerOutPx = minSide * 0.13f;
    const float cornerInPx  = minSide * 0.052f;
    const float cornerInsetPx = bandPx + cornerOutPx + minSide * 0.015f;
    {
        const float roU = cornerOutPx / dstW, roV = cornerOutPx / dstH;
        const float riU = cornerInPx  / dstW, riV = cornerInPx  / dstH;
        const float cU  = cornerInsetPx / dstW, cV  = cornerInsetPx / dstH;
        const float phase = 3.14159265f / 12.0f;  // a tip points up
        polystar(cU,        cV,        roU, roV, riU, riV, 12, phase);
        polystar(1.0f - cU, cV,        roU, roV, riU, riV, 12, phase);
        polystar(1.0f - cU, 1.0f - cV, roU, roV, riU, riV, 12, phase);
        polystar(cU,        1.0f - cV, roU, roV, riU, riV, 12, phase);
    }

    // Two end medallions at the vertical centers of the short edges.
    // 8-pointed star + smaller 8-pointed star rotated 22.5° on top —
    // overlapping CCW stars stay filled under non-zero winding and
    // produce a 16-rayed look.
    const float medOutPx  = minSide * 0.18f;
    const float medMidPx  = minSide * 0.075f;
    const float medCorePx = minSide * 0.085f;
    const float medCoreInPx = minSide * 0.040f;
    const float medInsetPxU = bandPx + medOutPx + minSide * 0.030f;
    {
        const float roU = medOutPx / dstW,  roV = medOutPx / dstH;
        const float riU = medMidPx / dstW,  riV = medMidPx / dstH;
        const float coreOutU = medCorePx / dstW, coreOutV = medCorePx / dstH;
        const float coreInU  = medCoreInPx / dstW, coreInV = medCoreInPx / dstH;
        const float cU = medInsetPxU / dstW;
        polystar(cU,        0.5f, roU, roV, riU, riV, 8, 0.0f);
        polystar(cU,        0.5f, roU, roV, riU, riV, 8, 3.14159265f / 8.0f);
        polystar(cU,        0.5f, coreOutU, coreOutV, coreInU, coreInV, 6, 0.0f);
        polystar(1.0f - cU, 0.5f, roU, roV, riU, riV, 8, 0.0f);
        polystar(1.0f - cU, 0.5f, roU, roV, riU, riV, 8, 3.14159265f / 8.0f);
        polystar(1.0f - cU, 0.5f, coreOutU, coreOutV, coreInU, coreInV, 6, 0.0f);
    }

    // Small 6-pointed accents along the long top/bottom edges, between
    // the corner ornaments. Skipped on near-square frames where they'd
    // crowd the corners.
    const float accentOutPx = minSide * 0.045f;
    const float accentInPx  = minSide * 0.020f;
    const float accentInsetVPx = bandPx + accentOutPx + minSide * 0.010f;
    const float topOpenUPx = dstW * 0.5f -
                             (cornerInsetPx + cornerOutPx) -
                             (medInsetPxU + medOutPx);
    if (topOpenUPx > minSide * 0.20f) {
        const float roU = accentOutPx / dstW, roV = accentOutPx / dstH;
        const float riU = accentInPx  / dstW, riV = accentInPx  / dstH;
        const float aV = accentInsetVPx / dstH;
        // Two accents between left medallion and corner top/bottom, two
        // between right medallion and corner — total 4 per long edge.
        const float leftSpanLowU  = (cornerInsetPx + cornerOutPx) / dstW;
        const float leftSpanHighU = (medInsetPxU - medOutPx) / dstW;
        const float rightSpanLowU  = 1.0f - (medInsetPxU - medOutPx) / dstW;
        const float rightSpanHighU = 1.0f - (cornerInsetPx + cornerOutPx) / dstW;
        for (int k = 0; k < 2; ++k) {
            const float tL = (float)(k + 1) / 3.0f;
            const float uL = leftSpanLowU + tL * (leftSpanHighU - leftSpanLowU);
            const float uR = rightSpanLowU + tL * (rightSpanHighU - rightSpanLowU);
            polystar(uL, aV,        roU, roV, riU, riV, 6, 0.0f);
            polystar(uR, aV,        roU, roV, riU, riV, 6, 0.0f);
            polystar(uL, 1.0f - aV, roU, roV, riU, riV, 6, 0.0f);
            polystar(uR, 1.0f - aV, roU, roV, riU, riV, 6, 0.0f);
        }
    }

    const int curveCount = totalCurves - curveStart;

    // Layer header: curveOffset, curveCount, _, _, r, g, b, a (8 floats).
    layerData.push_back((float)curveStart);
    layerData.push_back((float)curveCount);
    layerData.push_back(0.0f); layerData.push_back(0.0f);
    const float a  = ((color >> 24) & 0xFF) / 255.0f;
    const float rC = ((color >> 16) & 0xFF) / 255.0f;
    const float gC = ((color >> 8)  & 0xFF) / 255.0f;
    const float bC = ( color        & 0xFF) / 255.0f;
    layerData.push_back(rC);
    layerData.push_back(gC);
    layerData.push_back(bC);
    layerData.push_back(a);

    appendLayerBands(allCurves, curveStart, curveCount, bandsArr, curveIndicesArr);

    layerRects.push_back(dstX);
    layerRects.push_back(dstY);
    layerRects.push_back(dstW);
    layerRects.push_back(dstH);

    ++totalLayers;
}

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
    jboolean firstLineDecorate) {
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

        // After the title glyph(s) are laid out, draw a Mushaf-style
        // frame around line 0. The frame fills the full screen width and
        // occupies exactly one line-spacing vertically — flush against
        // the top bar (line 0's top edge = top of canvas viewport), and
        // its bottom is the baseline of line 1 minus the same gap.
        if (centerThisLine) {
            const float frameHeightPx = lineSpacingPx;
            const float frameX = 0.0f;
            const float frameWidthPx = screenWidthPx;
            // Line 0's top edge sits ~0.7 line-heights above the
            // baseline (matches the topMargin set by the host activity).
            const float frameY = baselineY - lineSpacingPx * 0.7f;
            emitFrame(frameX, frameY, frameWidthPx, frameHeightPx,
                      0xFF281E14u,  // dark warm brown, matches glyph fallback
                      allCurves, layerData, layerRects,
                      bandsArr, curveIndicesArr,
                      totalCurves, totalLayers);
        }

        baselineY += lineSpacingPx;
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
