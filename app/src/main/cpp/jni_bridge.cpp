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
               int& totalLayers,
               int seed) {
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
    // polystar emits an N-pointed star outline. When `reverse` is true
    // the path is traced the other way around — under non-zero winding
    // that turns the shape into a "hole" cut from any previously-filled
    // region it overlaps. Stacking outer-fill / hole / inner-fill /
    // smaller-hole / center-fill produces visually-distinct concentric
    // rings instead of one blobby filled mass.
    const float PI = 3.14159265f;
    auto polystar = [&](float cxU, float cyV,
                         float ruOut, float rvOut, float ruIn, float rvIn,
                         int points, float phase, bool reverse) {
        const int N = points * 2;
        const float angStep = (reverse ? -1.0f : 1.0f) * PI / (float)points;
        float prevX = 0.0f, prevY = 0.0f;
        for (int i = 0; i <= N; ++i) {
            const float ang = phase + (float)i * angStep;
            const bool isOut = (i & 1) == 0;
            const float ru = isOut ? ruOut : ruIn;
            const float rv = isOut ? rvOut : rvIn;
            const float x = cxU + cosf(ang) * ru;
            const float y = cyV + sinf(ang) * rv;
            if (i > 0) line(prevX, prevY, x, y);
            prevX = x; prevY = y;
        }
    };
    // petal emits an almond / lens shape: two quadratic Béziers, base at
    // `c`, tip at `c + dir*len`, control points offset sideways by
    // `halfWidth`. Forms a single closed teardrop suitable for floral
    // rosettes.
    auto petal = [&](float cU, float cV, float angle,
                     float lenU, float lenV,
                     float halfWU, float halfWV, bool reverse) {
        const float dirU = cosf(angle), dirV = sinf(angle);
        const float perpU = -sinf(angle), perpV = cosf(angle);
        const float baseU = cU, baseV = cV;
        const float tipU = cU + dirU * lenU;
        const float tipV = cV + dirV * lenV;
        const float midU = cU + dirU * lenU * 0.5f;
        const float midV = cV + dirV * lenV * 0.5f;
        const float c1U = midU + perpU * halfWU, c1V = midV + perpV * halfWV;
        const float c2U = midU - perpU * halfWU, c2V = midV - perpV * halfWV;
        if (reverse) {
            curve(baseU, baseV, c2U, c2V, tipU, tipV);
            curve(tipU,  tipV,  c1U, c1V, baseU, baseV);
        } else {
            curve(baseU, baseV, c1U, c1V, tipU, tipV);
            curve(tipU,  tipV,  c2U, c2V, baseU, baseV);
        }
    };

    // -------- Style dispatch --------
    // The seed selects from a hand-tuned set of ornament presets so the
    // host can flip between them to find the look it likes. All styles
    // share the triple band above and emit two end medallions plus a
    // chain along the long edges, but the motifs differ — stars,
    // petals, sunburst rays, or interlaced girih shapes.
    const int NUM_STYLES = 22;
    const int style = ((unsigned)seed) % (unsigned)NUM_STYLES;
    LOGI("emitFrame: seed=%d style=%d", seed, style);

    const float roseOutPx   = minSide * 0.36f;
    const float roseInsetPx = bandPx + roseOutPx + minSide * 0.020f;
    const float cU_left = roseInsetPx / dstW;
    const float cU_right = 1.0f - cU_left;
    const float chainMarginU = (roseInsetPx + roseOutPx + minSide * 0.05f) / dstW;
    const bool drawChain = chainMarginU < 0.45f;

    if (style == 0) {
        // -------- Style 0: layered star burst with petal halo --------
        const float R1o = minSide * 0.36f, R1i = minSide * 0.22f;
        const float R2o = minSide * 0.21f, R2i = minSide * 0.14f;
        const float R3o = minSide * 0.13f, R3i = minSide * 0.070f;
        const float R4o = minSide * 0.065f, R4i = minSide * 0.038f;
        const float R5o = minSide * 0.034f, R5i = minSide * 0.014f;
        const float petalLenPx   = minSide * 0.45f;
        const float petalHalfWPx = minSide * 0.040f;
        auto medallion = [&](float c) {
            for (int p = 0; p < 16; ++p) {
                const float ang = (float)p * 2.0f * PI / 16.0f;
                petal(c, 0.5f, ang,
                      petalLenPx / dstW, petalLenPx / dstH,
                      petalHalfWPx / dstW, petalHalfWPx / dstH, false);
            }
            polystar(c, 0.5f, R1o/dstW, R1o/dstH, R1i/dstW, R1i/dstH, 16, 0.0f,      false);
            polystar(c, 0.5f, R2o/dstW, R2o/dstH, R2i/dstW, R2i/dstH, 12, PI/24.0f, true);
            polystar(c, 0.5f, R3o/dstW, R3o/dstH, R3i/dstW, R3i/dstH,  8, PI/16.0f, false);
            polystar(c, 0.5f, R4o/dstW, R4o/dstH, R4i/dstW, R4i/dstH,  6, 0.0f,      true);
            polystar(c, 0.5f, R5o/dstW, R5o/dstH, R5i/dstW, R5i/dstH,  4, PI/8.0f,  false);
        };
        medallion(cU_left);
        medallion(cU_right);
        if (drawChain) {
            const float chainOutPx = minSide * 0.050f;
            const float chainInPx  = minSide * 0.022f;
            const float aVtop = (bandPx + chainOutPx + minSide * 0.015f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const float petLenU = (chainOutPx * 0.9f) / dstW;
            const float petLenV = (chainOutPx * 0.9f) / dstH;
            const float petHWU  = (chainOutPx * 0.18f) / dstW;
            const float petHWV  = (chainOutPx * 0.18f) / dstH;
            const int N = 7;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                if ((k & 1) == 0) {
                    polystar(uPos, aVtop, chainOutPx/dstW, chainOutPx/dstH,
                             chainInPx/dstW, chainInPx/dstH, 8, 0.0f, false);
                    polystar(uPos, aVbot, chainOutPx/dstW, chainOutPx/dstH,
                             chainInPx/dstW, chainInPx/dstH, 8, 0.0f, false);
                } else {
                    petal(uPos, aVtop, -PI*0.5f, petLenU, petLenV, petHWU, petHWV, false);
                    petal(uPos, aVtop,  PI*0.5f, petLenU, petLenV, petHWU, petHWV, false);
                    petal(uPos, aVbot, -PI*0.5f, petLenU, petLenV, petHWU, petHWV, false);
                    petal(uPos, aVbot,  PI*0.5f, petLenU, petLenV, petHWU, petHWV, false);
                }
            }
        }
    } else if (style == 1) {
        // -------- Style 1: pure floral rosette (nested petal rings) --------
        auto floral = [&](float c) {
            // Outer 16 long thin petals
            const float L1 = minSide * 0.42f, W1 = minSide * 0.030f;
            for (int p = 0; p < 16; ++p) {
                const float ang = (float)p * 2.0f * PI / 16.0f;
                petal(c, 0.5f, ang, L1/dstW, L1/dstH, W1/dstW, W1/dstH, false);
            }
            // Middle 8 fat petals, rotated 22.5° (between outer petals)
            const float L2 = minSide * 0.26f, W2 = minSide * 0.060f;
            for (int p = 0; p < 8; ++p) {
                const float ang = PI/8.0f + (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L2/dstW, L2/dstH, W2/dstW, W2/dstH, false);
            }
            // Inner 6 small fat petals
            const float L3 = minSide * 0.14f, W3 = minSide * 0.048f;
            for (int p = 0; p < 6; ++p) {
                const float ang = (float)p * 2.0f * PI / 6.0f;
                petal(c, 0.5f, ang, L3/dstW, L3/dstH, W3/dstW, W3/dstH, false);
            }
            // Center hole + tiny pip
            polystar(c, 0.5f, (minSide*0.045f)/dstW, (minSide*0.045f)/dstH,
                     (minSide*0.040f)/dstW, (minSide*0.040f)/dstH, 12, 0.0f, true);
            polystar(c, 0.5f, (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                     (minSide*0.016f)/dstW, (minSide*0.016f)/dstH, 8, 0.0f, false);
        };
        floral(cU_left);
        floral(cU_right);
        if (drawChain) {
            // Chain: 4-petal flowers (cross pattern)
            const float L = minSide * 0.045f, W = minSide * 0.014f;
            const float aVtop = (bandPx + L + minSide * 0.010f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                for (int q = 0; q < 4; ++q) {
                    const float ang = (float)q * PI * 0.5f;
                    petal(uPos, aVtop, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                    petal(uPos, aVbot, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                }
            }
        }
    } else if (style == 2) {
        // -------- Style 2: sunburst rays + inner flower --------
        auto sunburst = [&](float c) {
            // 24 thin long radial spikes — outer "polystar" with very small inner radius
            polystar(c, 0.5f,
                     (minSide*0.43f)/dstW, (minSide*0.43f)/dstH,
                     (minSide*0.060f)/dstW, (minSide*0.060f)/dstH,
                     24, 0.0f, false);
            // Ring cutout (polygon hole) at ~0.16
            polystar(c, 0.5f,
                     (minSide*0.16f)/dstW, (minSide*0.16f)/dstH,
                     (minSide*0.15f)/dstW, (minSide*0.15f)/dstH,
                     20, 0.0f, true);
            // 8-petal inner flower
            const float L = minSide * 0.13f, W = minSide * 0.032f;
            for (int p = 0; p < 8; ++p) {
                const float ang = (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
            // Center small star
            polystar(c, 0.5f,
                     (minSide*0.035f)/dstW, (minSide*0.035f)/dstH,
                     (minSide*0.015f)/dstW, (minSide*0.015f)/dstH,
                     6, 0.0f, false);
        };
        sunburst(cU_left);
        sunburst(cU_right);
        if (drawChain) {
            // Chain: small filled circles (polygon w/ matched radii)
            const float R = minSide * 0.026f;
            const float aVtop = (bandPx + R + minSide * 0.018f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                polystar(uPos, aVtop, R/dstW, R/dstH,
                         R*0.85f/dstW, R*0.85f/dstH, 12, 0.0f, false);
                polystar(uPos, aVbot, R/dstW, R/dstH,
                         R*0.85f/dstW, R*0.85f/dstH, 12, 0.0f, false);
            }
        }
    } else if (style == 3) {
        // -------- Style 3: girih (3 overlapping rotated 8-point stars) --------
        auto girih = [&](float c) {
            // 3 overlapping 8-point stars, rotated 15° each = 24-rayed look
            for (int r = 0; r < 3; ++r) {
                const float phase = (float)r * PI / 12.0f;
                polystar(c, 0.5f,
                         (minSide*0.36f)/dstW, (minSide*0.36f)/dstH,
                         (minSide*0.20f)/dstW, (minSide*0.20f)/dstH,
                         8, phase, false);
            }
            // Hexagonal hole at radius ~0.13
            polystar(c, 0.5f,
                     (minSide*0.14f)/dstW, (minSide*0.14f)/dstH,
                     (minSide*0.12f)/dstW, (minSide*0.12f)/dstH,
                     6, 0.0f, true);
            // Inner 12-point star
            polystar(c, 0.5f,
                     (minSide*0.10f)/dstW, (minSide*0.10f)/dstH,
                     (minSide*0.045f)/dstW, (minSide*0.045f)/dstH,
                     12, 0.0f, false);
            // Tiny center hole + pip
            polystar(c, 0.5f,
                     (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                     (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                     6, PI/6.0f, true);
        };
        girih(cU_left);
        girih(cU_right);
        if (drawChain) {
            // Chain: alternating 6-point stars and small diamonds
            const float R = minSide * 0.038f;
            const float aVtop = (bandPx + R + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                const bool isStar = (k & 1) == 0;
                const int pts = isStar ? 6 : 4;
                const float ph = isStar ? 0.0f : PI / 4.0f;
                polystar(uPos, aVtop, R/dstW, R/dstH,
                         R*0.4f/dstW, R*0.4f/dstH, pts, ph, false);
                polystar(uPos, aVbot, R/dstW, R/dstH,
                         R*0.4f/dstW, R*0.4f/dstH, pts, ph, false);
            }
        }
    } else if (style == 4) {
        // -------- Style 4: Lotus — overlapping wide curved petals --------
        // Wide fat petals are drawn at 3 ring sizes with rotation offsets
        // so the outline reads like overlapping lotus petals rather than a
        // pure star.
        auto lotus = [&](float c) {
            // Outer ring — 8 very wide petals
            const float L1 = minSide * 0.40f, W1 = minSide * 0.085f;
            for (int p = 0; p < 8; ++p) {
                const float ang = (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L1/dstW, L1/dstH, W1/dstW, W1/dstH, false);
            }
            // Middle ring — 8 petals rotated 22.5°, slightly shorter
            const float L2 = minSide * 0.30f, W2 = minSide * 0.070f;
            for (int p = 0; p < 8; ++p) {
                const float ang = PI / 8.0f + (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L2/dstW, L2/dstH, W2/dstW, W2/dstH, false);
            }
            // Inner ring — 6 small fat petals
            const float L3 = minSide * 0.15f, W3 = minSide * 0.055f;
            for (int p = 0; p < 6; ++p) {
                const float ang = (float)p * 2.0f * PI / 6.0f;
                petal(c, 0.5f, ang, L3/dstW, L3/dstH, W3/dstW, W3/dstH, false);
            }
            // Hole at the center, then a tiny pip
            polystar(c, 0.5f, (minSide*0.038f)/dstW, (minSide*0.038f)/dstH,
                     (minSide*0.034f)/dstW, (minSide*0.034f)/dstH, 16, 0.0f, true);
            polystar(c, 0.5f, (minSide*0.018f)/dstW, (minSide*0.018f)/dstH,
                     (minSide*0.014f)/dstW, (minSide*0.014f)/dstH, 8, 0.0f, false);
        };
        lotus(cU_left);
        lotus(cU_right);
        if (drawChain) {
            // Chain: tiny lotus petal pairs (vertical + horizontal)
            const float L = minSide * 0.040f, W = minSide * 0.022f;
            const float aVtop = (bandPx + L + minSide * 0.010f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                for (int q = 0; q < 4; ++q) {
                    const float ang = (float)q * PI * 0.5f + PI * 0.25f;
                    petal(uPos, aVtop, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                    petal(uPos, aVbot, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                }
            }
        }
    } else if (style == 5) {
        // -------- Style 5: Spiral mandala — 6 curling arms from center --------
        // Each arm is a chain of three quadratic Béziers following a
        // logarithmic-ish spiral, with a mirrored return curve to close
        // the arm into a filled shape.
        auto spiralArm = [&](float c, float baseAngle, float maxRadiusPx,
                              float startWidthPx, float endWidthPx) {
            // Sample 4 points along the spiral curve. The arm tapers — at
            // r=0 it's wide (startWidthPx), at r=max it's narrow (endWidthPx).
            const int N = 4;
            float lx[N + 1], ly[N + 1];  // left edge
            float rx[N + 1], ry[N + 1];  // right edge
            float midx[N + 1], midy[N + 1];
            for (int i = 0; i <= N; ++i) {
                const float t = (float)i / (float)N;
                const float r = maxRadiusPx * t;
                // Spiral: angle grows with radius. ~135° curl over the arm length.
                const float ang = baseAngle + t * 2.4f;
                const float w = startWidthPx + (endWidthPx - startWidthPx) * t;
                const float ux = cosf(ang), uy = sinf(ang);
                const float px = -uy, py = ux;
                const float mx = (r * ux) / dstW;
                const float my = (r * uy) / dstH;
                midx[i] = c + mx;
                midy[i] = 0.5f + my;
                lx[i] = c + mx + (px * w) / dstW;
                ly[i] = 0.5f + my + (py * w) / dstH;
                rx[i] = c + mx - (px * w) / dstW;
                ry[i] = 0.5f + my - (py * w) / dstH;
            }
            // Draw quadratic Béziers connecting consecutive midpoints,
            // with control points sampled at offsets to give curvature.
            // Outgoing along left edge: l0 -> l1 -> ... -> lN
            // Then around the tip back along right edge.
            for (int i = 0; i < N; ++i) {
                // Control point = midpoint plus slight outward shift
                const float ctrlX = (lx[i] + lx[i + 1]) * 0.5f;
                const float ctrlY = (ly[i] + ly[i + 1]) * 0.5f;
                curve(lx[i], ly[i], ctrlX, ctrlY, lx[i + 1], ly[i + 1]);
            }
            // Tip cap: curve from lN to rN via the midpoint at i=N
            curve(lx[N], ly[N], midx[N], midy[N], rx[N], ry[N]);
            for (int i = N; i > 0; --i) {
                const float ctrlX = (rx[i] + rx[i - 1]) * 0.5f;
                const float ctrlY = (ry[i] + ry[i - 1]) * 0.5f;
                curve(rx[i], ry[i], ctrlX, ctrlY, rx[i - 1], ry[i - 1]);
            }
            // Base cap: curve from r0 back to l0 via center
            curve(rx[0], ry[0], midx[0], midy[0], lx[0], ly[0]);
        };
        auto spiral = [&](float c) {
            const int ARMS = 6;
            const float maxR = minSide * 0.36f;
            const float startW = minSide * 0.022f;
            const float endW   = minSide * 0.012f;
            for (int a = 0; a < ARMS; ++a) {
                const float baseAng = (float)a * 2.0f * PI / (float)ARMS;
                spiralArm(c, baseAng, maxR, startW, endW);
            }
            // Central disc + pip
            polystar(c, 0.5f, (minSide*0.060f)/dstW, (minSide*0.060f)/dstH,
                     (minSide*0.055f)/dstW, (minSide*0.055f)/dstH, 16, 0.0f, false);
            polystar(c, 0.5f, (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                     (minSide*0.026f)/dstW, (minSide*0.026f)/dstH, 12, 0.0f, true);
            polystar(c, 0.5f, (minSide*0.012f)/dstW, (minSide*0.012f)/dstH,
                     (minSide*0.008f)/dstW, (minSide*0.008f)/dstH, 6, 0.0f, false);
        };
        spiral(cU_left);
        spiral(cU_right);
        if (drawChain) {
            // Chain: small comma/curl marks alternating direction
            const float armR = minSide * 0.040f;
            const float aVtop = (bandPx + armR + minSide * 0.015f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                const float baseAng = (k & 1) ? PI * 0.25f : -PI * 0.25f;
                spiralArm(uPos - aVtop * 0.0f, baseAng, armR,
                          minSide * 0.012f, minSide * 0.006f);
                spiralArm(uPos, baseAng + PI, armR,
                          minSide * 0.012f, minSide * 0.006f);
                // Mirror for bottom edge — shift via a fake center by
                // re-using spiralArm at the bottom V band.
                // Hack: call spiralArm twice for bottom positioning.
                // Easier: draw a small dot since spirals get cluttered.
                polystar(uPos, aVbot, armR*0.5f/dstW, armR*0.5f/dstH,
                         armR*0.4f/dstW, armR*0.4f/dstH, 8, 0.0f, false);
            }
        }
    } else if (style == 6) {
        // -------- Style 6: S-scroll cartouche — 4 paired S-curves --------
        // An "S" shape made of two quadratic curves; placed around the
        // medallion center to form a baroque cartouche feel.
        auto sScroll = [&](float c, float baseAngle, float reach, float thickness) {
            // Define 4 points: p0 (start), p1 (first bend), p2 (mid), p3 (end)
            const float dirU = cosf(baseAngle), dirV = sinf(baseAngle);
            const float perpU = -sinf(baseAngle), perpV = cosf(baseAngle);
            const float l = reach;
            // Two ends of the S
            const float p0u = c + (dirU * 0 - perpU * thickness * 0.5f) / dstW;
            const float p0v = 0.5f + (dirV * 0 - perpV * thickness * 0.5f) / dstH;
            const float pEu = c + (dirU * l + perpU * thickness * 0.5f) / dstW;
            const float pEv = 0.5f + (dirV * l + perpV * thickness * 0.5f) / dstH;
            // Control points: bulge OPPOSITE sides for the two halves of the S
            const float qLu = c + (dirU * l * 0.25f + perpU * l * 0.6f) / dstW;
            const float qLv = 0.5f + (dirV * l * 0.25f + perpV * l * 0.6f) / dstH;
            const float qRu = c + (dirU * l * 0.75f - perpU * l * 0.6f) / dstW;
            const float qRv = 0.5f + (dirV * l * 0.75f - perpV * l * 0.6f) / dstH;
            // Lower edge (outgoing) and upper edge (return) make a closed strip
            const float w = thickness * 0.5f;
            const float p0uL = c + (dirU * 0 - perpU * w) / dstW;
            const float p0vL = 0.5f + (dirV * 0 - perpV * w) / dstH;
            const float p0uR = c + (dirU * 0 + perpU * w) / dstW;
            const float p0vR = 0.5f + (dirV * 0 + perpV * w) / dstH;
            const float pEuL = c + (dirU * l - perpU * w) / dstW;
            const float pEvL = 0.5f + (dirV * l - perpV * w) / dstH;
            const float pEuR = c + (dirU * l + perpU * w) / dstW;
            const float pEvR = 0.5f + (dirV * l + perpV * w) / dstH;
            // Two-curve S as a closed thin strip: bottom curve out, top curve back
            curve(p0uL, p0vL, qLu, qLv, pEuL, pEvL);
            curve(pEuL, pEvL, qRu, qRv, pEuR, pEvR);  // tip turn
            curve(pEuR, pEvR, qRu, qRv, p0uR, p0vR);  // wait — closing path
            curve(p0uR, p0vR, qLu, qLv, p0uL, p0vL);
            // Suppress unused vars warning
            (void)p0u; (void)p0v; (void)pEu; (void)pEv;
        };
        auto cartouche = [&](float c) {
            // 4 S-scrolls arranged in a plus pattern, each curling outward
            const float reach = minSide * 0.32f;
            const float thick = minSide * 0.050f;
            for (int q = 0; q < 4; ++q) {
                const float ang = (float)q * PI * 0.5f;
                sScroll(c, ang, reach, thick);
            }
            // 4 smaller S-scrolls at 45° angles
            for (int q = 0; q < 4; ++q) {
                const float ang = (float)q * PI * 0.5f + PI * 0.25f;
                sScroll(c, ang, reach * 0.6f, thick * 0.7f);
            }
            // Center: layered star
            polystar(c, 0.5f, (minSide*0.080f)/dstW, (minSide*0.080f)/dstH,
                     (minSide*0.040f)/dstW, (minSide*0.040f)/dstH, 8, 0.0f, false);
            polystar(c, 0.5f, (minSide*0.035f)/dstW, (minSide*0.035f)/dstH,
                     (minSide*0.028f)/dstW, (minSide*0.028f)/dstH, 12, 0.0f, true);
            polystar(c, 0.5f, (minSide*0.018f)/dstW, (minSide*0.018f)/dstH,
                     (minSide*0.012f)/dstW, (minSide*0.012f)/dstH, 6, 0.0f, false);
        };
        cartouche(cU_left);
        cartouche(cU_right);
        if (drawChain) {
            // Chain: alternating small S-scroll and dot
            const float reach = minSide * 0.045f;
            const float thick = minSide * 0.014f;
            const float aVtop = (bandPx + thick + minSide * 0.020f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                if ((k & 1) == 0) {
                    sScroll(uPos, 0.0f, reach, thick);
                    sScroll(uPos, PI, reach, thick);
                } else {
                    polystar(uPos, aVtop,
                             (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                             (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                             8, 0.0f, false);
                    polystar(uPos, aVbot,
                             (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                             (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                             8, 0.0f, false);
                }
            }
        }
    } else if (style == 7) {
        // -------- Style 7: Fractal flower — petals with sub-flowers at tips --------
        auto subFlower = [&](float cU_, float cV_, float petalLenPx, float petalWPx, int n) {
            for (int p = 0; p < n; ++p) {
                const float ang = (float)p * 2.0f * PI / (float)n;
                petal(cU_, cV_, ang,
                      petalLenPx/dstW, petalLenPx/dstH,
                      petalWPx/dstW, petalWPx/dstH, false);
            }
        };
        auto fractalFlower = [&](float c) {
            // 8 main petals
            const float L1 = minSide * 0.36f, W1 = minSide * 0.040f;
            for (int p = 0; p < 8; ++p) {
                const float ang = (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L1/dstW, L1/dstH, W1/dstW, W1/dstH, false);
                // Sub-flower at each tip
                const float tipU = c + cosf(ang) * (L1 * 0.92f) / dstW;
                const float tipV = 0.5f + sinf(ang) * (L1 * 0.92f) / dstH;
                subFlower(tipU, tipV, minSide * 0.040f, minSide * 0.014f, 5);
            }
            // 6 secondary petals filling the gaps, rotated 22.5° (between main petals)
            const float L2 = minSide * 0.20f, W2 = minSide * 0.050f;
            for (int p = 0; p < 8; ++p) {
                const float ang = PI / 8.0f + (float)p * 2.0f * PI / 8.0f;
                petal(c, 0.5f, ang, L2/dstW, L2/dstH, W2/dstW, W2/dstH, false);
            }
            // Center flower (4-petal)
            subFlower(c, 0.5f, minSide * 0.060f, minSide * 0.022f, 4);
            // Tiny pip
            polystar(c, 0.5f, (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                     (minSide*0.010f)/dstW, (minSide*0.010f)/dstH, 6, 0.0f, false);
        };
        fractalFlower(cU_left);
        fractalFlower(cU_right);
        if (drawChain) {
            // Chain: small 5-petal fractal-style flowers
            const float aVtop = (bandPx + minSide * 0.040f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                subFlower(uPos, aVtop, minSide * 0.030f, minSide * 0.010f, 5);
                subFlower(uPos, aVbot, minSide * 0.030f, minSide * 0.010f, 5);
            }
        }
    } else if (style == 8) {
        // -------- Style 8: Cardioid heart ring --------
        // A heart shape made of two mirrored Béziers: from base point up
        // to apex via outward lobe controls. Arranged radially in a ring
        // around the medallion center.
        auto heart = [&](float cU_, float cV_, float angle, float sizePx) {
            const float dirU = cosf(angle), dirV = sinf(angle);
            const float perpU = -sinf(angle), perpV = cosf(angle);
            // Heart oriented so tip points OUTWARD from center.
            const float lenU = sizePx / dstW, lenV = sizePx / dstH;
            const float baseU = cU_, baseV = cV_;  // not used, just inner anchor
            (void)baseU; (void)baseV;
            // The heart has TIP at distance `sizePx` outward; the lobes are
            // at the inner side. Two Bezier curves trace base→tip→base.
            const float tipU = cU_ + dirU * lenU;
            const float tipV = cV_ + dirV * lenV;
            // Mid offset point on each side
            const float bulgeU = sizePx * 0.55f;
            const float bulgeV = sizePx * 0.55f;
            const float lobeAU = cU_ + (-dirU * lenU * 0.25f + perpU * bulgeU) / dstW;
            const float lobeAV = cV_ + (-dirV * lenV * 0.25f + perpV * bulgeV) / dstH;
            const float lobeBU = cU_ + (-dirU * lenU * 0.25f - perpU * bulgeU) / dstW;
            const float lobeBV = cV_ + (-dirV * lenV * 0.25f - perpV * bulgeV) / dstH;
            // Inner pinch point — the cusp of the heart, slightly inside center
            const float pinchU = cU_ - (dirU * sizePx * 0.05f) / dstW;
            const float pinchV = cV_ - (dirV * sizePx * 0.05f) / dstH;
            curve(pinchU, pinchV, lobeAU, lobeAV, tipU, tipV);
            curve(tipU,   tipV,   lobeBU, lobeBV, pinchU, pinchV);
        };
        auto hearts = [&](float c) {
            const int N = 10;
            const float sz1 = minSide * 0.32f;
            for (int p = 0; p < N; ++p) {
                const float ang = (float)p * 2.0f * PI / (float)N;
                heart(c, 0.5f, ang, sz1);
            }
            // Inner smaller ring of 8 hearts rotated half-step
            const int N2 = 8;
            const float sz2 = minSide * 0.15f;
            for (int p = 0; p < N2; ++p) {
                const float ang = PI / (float)N2 + (float)p * 2.0f * PI / (float)N2;
                heart(c, 0.5f, ang, sz2);
            }
            // Center small flower
            for (int p = 0; p < 6; ++p) {
                const float ang = (float)p * 2.0f * PI / 6.0f;
                petal(c, 0.5f, ang, (minSide*0.060f)/dstW, (minSide*0.060f)/dstH,
                      (minSide*0.020f)/dstW, (minSide*0.020f)/dstH, false);
            }
            polystar(c, 0.5f, (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                     (minSide*0.010f)/dstW, (minSide*0.010f)/dstH, 6, 0.0f, false);
        };
        hearts(cU_left);
        hearts(cU_right);
        if (drawChain) {
            const float aVtop = (bandPx + minSide * 0.045f + minSide * 0.008f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                // Heart pointing up at top edge, down at bottom edge
                heart(uPos, aVtop, -PI * 0.5f, minSide * 0.040f);
                heart(uPos, aVbot,  PI * 0.5f, minSide * 0.040f);
            }
        }
    } else if (style == 9) {
        // -------- Style 9: Vortex — sweeping curved arms --------
        // Long pinwheel arms that taper from a thick base to a thin tip;
        // similar to spiral but with deeper curl and chunkier base.
        auto vortexArm = [&](float c, float baseAngle, float maxRadiusPx,
                              float startWidthPx, float endWidthPx,
                              float curl) {
            const int N = 6;
            float lx[N + 1], ly[N + 1];
            float rx[N + 1], ry[N + 1];
            float mx[N + 1], my[N + 1];
            for (int i = 0; i <= N; ++i) {
                const float t = (float)i / (float)N;
                const float r = maxRadiusPx * t;
                const float ang = baseAngle + t * curl;
                const float w = startWidthPx + (endWidthPx - startWidthPx) * t;
                const float ux = cosf(ang), uy = sinf(ang);
                const float px = -uy, py = ux;
                mx[i] = c + (r * ux) / dstW;
                my[i] = 0.5f + (r * uy) / dstH;
                lx[i] = mx[i] + (px * w) / dstW;
                ly[i] = my[i] + (py * w) / dstH;
                rx[i] = mx[i] - (px * w) / dstW;
                ry[i] = my[i] - (py * w) / dstH;
            }
            for (int i = 0; i < N; ++i) {
                curve(lx[i], ly[i],
                      (lx[i] + lx[i + 1]) * 0.5f, (ly[i] + ly[i + 1]) * 0.5f,
                      lx[i + 1], ly[i + 1]);
            }
            curve(lx[N], ly[N], mx[N], my[N], rx[N], ry[N]);
            for (int i = N; i > 0; --i) {
                curve(rx[i], ry[i],
                      (rx[i] + rx[i - 1]) * 0.5f, (ry[i] + ry[i - 1]) * 0.5f,
                      rx[i - 1], ry[i - 1]);
            }
            curve(rx[0], ry[0], mx[0], my[0], lx[0], ly[0]);
        };
        auto vortex = [&](float c) {
            const int ARMS = 5;
            for (int a = 0; a < ARMS; ++a) {
                const float baseAng = (float)a * 2.0f * PI / (float)ARMS;
                vortexArm(c, baseAng, minSide * 0.38f,
                          minSide * 0.045f, minSide * 0.005f, 3.2f);
            }
            // Inner counter-rotating arms (smaller, opposite curl)
            for (int a = 0; a < ARMS; ++a) {
                const float baseAng = PI / 5.0f + (float)a * 2.0f * PI / (float)ARMS;
                vortexArm(c, baseAng, minSide * 0.16f,
                          minSide * 0.025f, minSide * 0.003f, -2.0f);
            }
            // Center pip + ring
            polystar(c, 0.5f, (minSide*0.060f)/dstW, (minSide*0.060f)/dstH,
                     (minSide*0.052f)/dstW, (minSide*0.052f)/dstH, 16, 0.0f, false);
            polystar(c, 0.5f, (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                     (minSide*0.024f)/dstW, (minSide*0.024f)/dstH, 12, 0.0f, true);
            polystar(c, 0.5f, (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                     (minSide*0.010f)/dstW, (minSide*0.010f)/dstH, 6, 0.0f, false);
        };
        vortex(cU_left);
        vortex(cU_right);
        if (drawChain) {
            // Chain: small twin vortex arms forming wave-like motifs
            const float aVtop = (bandPx + minSide * 0.040f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                // Place mini vortex arms; using a hack: re-center the
                // "0.5f" by passing uPos as c (since vortexArm uses 0.5f
                // hard-coded for the v-center). For chain we instead just
                // draw small petal-pairs for simplicity.
                petal(uPos, aVtop, -PI * 0.5f,
                      (minSide*0.040f)/dstW, (minSide*0.040f)/dstH,
                      (minSide*0.016f)/dstW, (minSide*0.016f)/dstH, false);
                petal(uPos, aVtop,  PI * 0.5f,
                      (minSide*0.040f)/dstW, (minSide*0.040f)/dstH,
                      (minSide*0.016f)/dstW, (minSide*0.016f)/dstH, false);
                petal(uPos, aVbot, -PI * 0.5f,
                      (minSide*0.040f)/dstW, (minSide*0.040f)/dstH,
                      (minSide*0.016f)/dstW, (minSide*0.016f)/dstH, false);
                petal(uPos, aVbot,  PI * 0.5f,
                      (minSide*0.040f)/dstW, (minSide*0.040f)/dstH,
                      (minSide*0.016f)/dstW, (minSide*0.016f)/dstH, false);
            }
        }
    } else if (style == 10) {
        // -------- Style 10: Paisley — curled teardrop with hooked tail --------
        // A paisley/buta motif: a body with a long curled tail. Modeled
        // as a closed path of two quadratic Béziers: an outer "tail
        // hook" curve and an inner return curve. Tip = curl, base = body.
        auto paisley = [&](float cU_, float cV_, float angle, float sizePx) {
            const float dirU = cosf(angle), dirV = sinf(angle);
            const float perpU = -sinf(angle), perpV = cosf(angle);
            // base of body — near center
            const float bU = cU_, bV = cV_;
            // outer body apex (fat part)
            const float bodyR = sizePx * 0.55f;
            const float bodyU = cU_ + (dirU * sizePx * 0.5f + perpU * bodyR) / dstW;
            const float bodyV = cV_ + (dirV * sizePx * 0.5f + perpV * bodyR) / dstH;
            // tip (curls back toward center)
            const float tipU = cU_ + (dirU * sizePx - perpU * sizePx * 0.05f) / dstW;
            const float tipV = cV_ + (dirV * sizePx - perpV * sizePx * 0.05f) / dstH;
            // inner control near base
            const float innerU = cU_ + (dirU * sizePx * 0.4f - perpU * sizePx * 0.15f) / dstW;
            const float innerV = cV_ + (dirV * sizePx * 0.4f - perpV * sizePx * 0.15f) / dstH;
            // outer curve from base → body → tip
            curve(bU, bV, bodyU, bodyV, tipU, tipV);
            // inner curve tip → inner → base (closes the path)
            curve(tipU, tipV, innerU, innerV, bU, bV);
        };
        auto paisleyMandala = [&](float c) {
            // Outer ring of 8 paisleys, all curling counter-clockwise
            const float sz1 = minSide * 0.36f;
            for (int p = 0; p < 8; ++p) {
                const float ang = (float)p * 2.0f * PI / 8.0f;
                paisley(c, 0.5f, ang, sz1);
            }
            // Inner ring of 6 smaller paisleys, opposite curl (rotate -PI/6 phase)
            const float sz2 = minSide * 0.17f;
            for (int p = 0; p < 6; ++p) {
                const float ang = PI / 6.0f + (float)p * 2.0f * PI / 6.0f;
                paisley(c, 0.5f, ang, sz2);
            }
            // Center small flower
            for (int p = 0; p < 5; ++p) {
                const float ang = (float)p * 2.0f * PI / 5.0f;
                petal(c, 0.5f, ang,
                      (minSide*0.040f)/dstW, (minSide*0.040f)/dstH,
                      (minSide*0.015f)/dstW, (minSide*0.015f)/dstH, false);
            }
        };
        paisleyMandala(cU_left);
        paisleyMandala(cU_right);
        if (drawChain) {
            // Chain: small paisleys alternating direction
            const float aVtop = (bandPx + minSide * 0.045f + minSide * 0.008f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                const float dir = (k & 1) ? 1.0f : -1.0f;
                paisley(uPos, aVtop, dir * PI * 0.5f, minSide * 0.045f);
                paisley(uPos, aVbot, dir * -PI * 0.5f, minSide * 0.045f);
            }
        }
    } else if (style == 11) {
        // -------- Style 11: Wave / ripple — concentric wavy rings --------
        // Each ring is a polystar with many points and a small in/out
        // radius oscillation — looks like a circular sine wave.
        auto wavyRing = [&](float c, float radiusPx, float amplitudePx,
                              int waves, float phase, bool reverse) {
            polystar(c, 0.5f,
                     (radiusPx + amplitudePx) / dstW,
                     (radiusPx + amplitudePx) / dstH,
                     (radiusPx - amplitudePx) / dstW,
                     (radiusPx - amplitudePx) / dstH,
                     waves, phase, reverse);
        };
        auto ripple = [&](float c) {
            // 5 concentric wavy rings, alternating phase for a quilted look
            wavyRing(c, minSide * 0.34f, minSide * 0.025f, 20, 0.0f,       false);
            wavyRing(c, minSide * 0.30f, minSide * 0.020f, 18, PI / 18.0f, true);
            wavyRing(c, minSide * 0.24f, minSide * 0.025f, 16, 0.0f,       false);
            wavyRing(c, minSide * 0.18f, minSide * 0.020f, 14, PI / 14.0f, true);
            wavyRing(c, minSide * 0.12f, minSide * 0.025f, 12, 0.0f,       false);
            // Center 6-petal flower
            for (int p = 0; p < 6; ++p) {
                const float ang = (float)p * 2.0f * PI / 6.0f;
                petal(c, 0.5f, ang,
                      (minSide*0.055f)/dstW, (minSide*0.055f)/dstH,
                      (minSide*0.022f)/dstW, (minSide*0.022f)/dstH, false);
            }
            polystar(c, 0.5f, (minSide*0.015f)/dstW, (minSide*0.015f)/dstH,
                     (minSide*0.011f)/dstW, (minSide*0.011f)/dstH, 8, 0.0f, false);
        };
        ripple(cU_left);
        ripple(cU_right);
        if (drawChain) {
            // Chain: small wavy circles (rings drawn as a thin band)
            const float aVtop = (bandPx + minSide * 0.030f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                polystar(uPos, aVtop,
                         (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                         (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                         10, 0.0f, false);
                polystar(uPos, aVbot,
                         (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                         (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                         10, 0.0f, false);
            }
        }
    } else if (style == 12) {
        // -------- Style 12: Sunflower — fibonacci spiral packing --------
        // Many small petals/seeds placed on a golden-angle spiral. The
        // golden angle is ~137.508° = pi * (3 - sqrt(5)).
        const float goldenAngle = 2.39996323f;  // pi * (3 - sqrt 5)
        auto sunflower = [&](float c) {
            const int SEEDS = 80;
            const float maxR = minSide * 0.38f;
            for (int i = 0; i < SEEDS; ++i) {
                const float t = (float)i / (float)(SEEDS - 1);
                const float r = sqrtf(t) * maxR;
                const float ang = (float)i * goldenAngle;
                const float uPos = c + (r * cosf(ang)) / dstW;
                const float vPos = 0.5f + (r * sinf(ang)) / dstH;
                // Each seed: small oriented petal pointing outward
                const float petL = minSide * 0.030f + r * 0.05f;
                const float petW = minSide * 0.011f;
                petal(uPos, vPos, ang,
                      petL/dstW, petL/dstH, petW/dstW, petW/dstH, false);
            }
            // Center cap so the densely packed spirals join a clean disc
            polystar(c, 0.5f,
                     (minSide*0.055f)/dstW, (minSide*0.055f)/dstH,
                     (minSide*0.050f)/dstW, (minSide*0.050f)/dstH,
                     16, 0.0f, false);
            polystar(c, 0.5f,
                     (minSide*0.025f)/dstW, (minSide*0.025f)/dstH,
                     (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                     12, 0.0f, true);
        };
        sunflower(cU_left);
        sunflower(cU_right);
        if (drawChain) {
            // Chain: mini sunflower seeds (small petals on a tiny golden spiral)
            const float aVtop = (bandPx + minSide * 0.035f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                for (int i = 0; i < 6; ++i) {
                    const float r = sqrtf((float)i / 5.0f) * minSide * 0.030f;
                    const float ang = (float)i * goldenAngle;
                    const float sU = uPos + (r * cosf(ang)) / dstW;
                    const float sVt = aVtop + (r * sinf(ang)) / dstH;
                    const float sVb = aVbot + (r * sinf(ang)) / dstH;
                    petal(sU, sVt, ang,
                          (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                          (minSide*0.006f)/dstW, (minSide*0.006f)/dstH, false);
                    petal(sU, sVb, ang,
                          (minSide*0.014f)/dstW, (minSide*0.014f)/dstH,
                          (minSide*0.006f)/dstW, (minSide*0.006f)/dstH, false);
                }
            }
        }
    } else if (style == 13) {
        // -------- Style 13: Rose curve (rhodonea) --------
        // r = R * |cos(k * theta)| traces a rose with 2k petals (for even
        // k). Sampled densely and emitted as a polyline ring. Two nested
        // roses with different k counts create a bouquet effect.
        auto rose = [&](float cU, float cV, float R, int k, float phase,
                        bool reverse) {
            const int STEPS = 240;
            float prevU = 0.0f, prevV = 0.0f;
            for (int i = 0; i <= STEPS; ++i) {
                const float t = (float)i / (float)STEPS;
                const float theta = phase + t * 2.0f * PI * (reverse ? -1.0f : 1.0f);
                const float r = R * fabsf(cosf((float)k * theta));
                const float u = cU + (r * cosf(theta)) / dstW;
                const float v = cV + (r * sinf(theta)) / dstH;
                if (i > 0) line(prevU, prevV, u, v);
                prevU = u; prevV = v;
            }
        };
        auto roseGroup = [&](float c) {
            // Outer rose (8 petals, k=4)
            rose(c, 0.5f, minSide * 0.38f, 4, 0.0f, false);
            // Middle rose (6 petals, k=3), rotated
            rose(c, 0.5f, minSide * 0.24f, 3, PI / 6.0f, true);
            // Inner rose (4 petals, k=2)
            rose(c, 0.5f, minSide * 0.13f, 2, 0.0f, false);
            // Center dot
            polystar(c, 0.5f,
                     (minSide*0.025f)/dstW, (minSide*0.025f)/dstH,
                     (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                     12, 0.0f, true);
        };
        roseGroup(cU_left);
        roseGroup(cU_right);
        if (drawChain) {
            // Chain: tiny 4-petal roses
            const float aVtop = (bandPx + minSide * 0.035f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 7;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                rose(uPos, aVtop, minSide * 0.033f, 2, 0.0f, false);
                rose(uPos, aVbot, minSide * 0.033f, 2, 0.0f, true);
            }
        }
    } else if (style == 14) {
        // -------- Style 14: Peacock feather fan --------
        // Long curved feathers radiating outward, each ending in an "eye"
        // (a small floral cluster). Reads like a peacock's tail when seen
        // from the medallion centre.
        auto peacock = [&](float c) {
            const int RAYS = 12;
            const float featherLen = minSide * 0.42f;
            const float featherHalfW = minSide * 0.040f;
            const float eyeR = minSide * 0.060f;
            for (int i = 0; i < RAYS; ++i) {
                const float ang = (float)i * 2.0f * PI / (float)RAYS;
                // Main feather body (long thin petal)
                petal(c, 0.5f, ang,
                      featherLen / dstW, featherLen / dstH,
                      featherHalfW / dstW, featherHalfW / dstH, false);
                // Eye at the tip
                const float tipU = c + cosf(ang) * (featherLen * 0.88f) / dstW;
                const float tipV = 0.5f + sinf(ang) * (featherLen * 0.88f) / dstH;
                polystar(tipU, tipV,
                         eyeR / dstW, eyeR / dstH,
                         (eyeR * 0.45f) / dstW, (eyeR * 0.45f) / dstH,
                         8, ang, false);
                polystar(tipU, tipV,
                         (eyeR * 0.45f) / dstW, (eyeR * 0.45f) / dstH,
                         (eyeR * 0.25f) / dstW, (eyeR * 0.25f) / dstH,
                         6, ang + PI / 6.0f, true);
                polystar(tipU, tipV,
                         (eyeR * 0.20f) / dstW, (eyeR * 0.20f) / dstH,
                         (eyeR * 0.10f) / dstW, (eyeR * 0.10f) / dstH,
                         4, ang, false);
            }
            // Central rosette
            polystar(c, 0.5f,
                     (minSide * 0.10f) / dstW, (minSide * 0.10f) / dstH,
                     (minSide * 0.060f) / dstW, (minSide * 0.060f) / dstH,
                     12, 0.0f, false);
            polystar(c, 0.5f,
                     (minSide * 0.055f) / dstW, (minSide * 0.055f) / dstH,
                     (minSide * 0.030f) / dstW, (minSide * 0.030f) / dstH,
                     8, PI / 8.0f, true);
            polystar(c, 0.5f,
                     (minSide * 0.025f) / dstW, (minSide * 0.025f) / dstH,
                     (minSide * 0.012f) / dstW, (minSide * 0.012f) / dstH,
                     6, 0.0f, false);
        };
        peacock(cU_left);
        peacock(cU_right);
        if (drawChain) {
            const float aVtop = (bandPx + minSide * 0.030f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                // Mini 6-ray feather burst
                for (int r = 0; r < 6; ++r) {
                    const float ang = (float)r * 2.0f * PI / 6.0f;
                    petal(uPos, aVtop, ang,
                          (minSide*0.024f)/dstW, (minSide*0.024f)/dstH,
                          (minSide*0.006f)/dstW, (minSide*0.006f)/dstH, false);
                    petal(uPos, aVbot, ang,
                          (minSide*0.024f)/dstW, (minSide*0.024f)/dstH,
                          (minSide*0.006f)/dstW, (minSide*0.006f)/dstH, false);
                }
            }
        }
    } else if (style == 15) {
        // -------- Style 15: Celtic interlace knot --------
        // Three offset rings woven together via alternating in/out
        // winding rules — gives a chained-loop / trefoil ornament feel.
        // Each "ring" is approximated by a wavy polystar with many points.
        auto interlace = [&](float c) {
            const int RING_PTS = 60;
            // Ring 1 (large, even ring at 0°)
            polystar(c, 0.5f,
                     (minSide * 0.36f) / dstW, (minSide * 0.36f) / dstH,
                     (minSide * 0.31f) / dstW, (minSide * 0.31f) / dstH,
                     RING_PTS, 0.0f, false);
            polystar(c, 0.5f,
                     (minSide * 0.34f) / dstW, (minSide * 0.34f) / dstH,
                     (minSide * 0.30f) / dstW, (minSide * 0.30f) / dstH,
                     RING_PTS, PI / (float)RING_PTS, true);
            // Three trefoil loops offset around the centre, each a
            // smaller wavy ring.
            for (int t = 0; t < 3; ++t) {
                const float ang = (float)t * 2.0f * PI / 3.0f - PI / 2.0f;
                const float offsetR = minSide * 0.16f;
                const float ringR   = minSide * 0.18f;
                const float lU = c + cosf(ang) * offsetR / dstW;
                const float lV = 0.5f + sinf(ang) * offsetR / dstH;
                polystar(lU, lV,
                         (ringR) / dstW, (ringR) / dstH,
                         (ringR - minSide * 0.022f) / dstW,
                         (ringR - minSide * 0.022f) / dstH,
                         36, ang, false);
                polystar(lU, lV,
                         (ringR - minSide * 0.022f) / dstW,
                         (ringR - minSide * 0.022f) / dstH,
                         (ringR - minSide * 0.044f) / dstW,
                         (ringR - minSide * 0.044f) / dstH,
                         36, ang + PI / 36.0f, true);
            }
            // Centre boss
            polystar(c, 0.5f,
                     (minSide * 0.045f) / dstW, (minSide * 0.045f) / dstH,
                     (minSide * 0.025f) / dstW, (minSide * 0.025f) / dstH,
                     12, 0.0f, false);
        };
        interlace(cU_left);
        interlace(cU_right);
        if (drawChain) {
            const float aVtop = (bandPx + minSide * 0.035f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 7;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                // Trefoil at each anchor
                for (int j = 0; j < 3; ++j) {
                    const float ang = (float)j * 2.0f * PI / 3.0f - PI / 2.0f;
                    const float lU = uPos + cosf(ang) * (minSide * 0.020f) / dstW;
                    polystar(lU, aVtop + sinf(ang) * (minSide * 0.020f) / dstH,
                             (minSide*0.018f)/dstW, (minSide*0.018f)/dstH,
                             (minSide*0.012f)/dstW, (minSide*0.012f)/dstH,
                             12, ang, false);
                    polystar(lU, aVbot + sinf(ang) * (minSide * 0.020f) / dstH,
                             (minSide*0.018f)/dstW, (minSide*0.018f)/dstH,
                             (minSide*0.012f)/dstW, (minSide*0.012f)/dstH,
                             12, ang, false);
                }
            }
        }
    } else if (style == 16) {
        // -------- Style 16: Fractal mandala (3-level recursive petals) --------
        // Each main petal hosts a 4-petal sub-flower at its mid-point, and
        // each sub-flower's center has a tiny 3-petal micro-flower. Three
        // levels of self-similar floral ornament.
        auto microFlower = [&](float cU, float cV, float baseAng, float scale) {
            for (int i = 0; i < 4; ++i) {
                const float ang = baseAng + (float)i * PI / 2.0f;
                petal(cU, cV, ang,
                      (minSide * 0.035f * scale) / dstW,
                      (minSide * 0.035f * scale) / dstH,
                      (minSide * 0.010f * scale) / dstW,
                      (minSide * 0.010f * scale) / dstH, false);
            }
        };
        auto mandala = [&](float c) {
            const int MAIN = 8;
            for (int i = 0; i < MAIN; ++i) {
                const float ang = (float)i * 2.0f * PI / (float)MAIN;
                // Long main petal
                petal(c, 0.5f, ang,
                      (minSide * 0.42f) / dstW, (minSide * 0.42f) / dstH,
                      (minSide * 0.055f) / dstW, (minSide * 0.055f) / dstH, false);
                // Sub-flower at petal mid-point (4 small petals)
                const float midR = minSide * 0.24f;
                const float subU = c + cosf(ang) * midR / dstW;
                const float subV = 0.5f + sinf(ang) * midR / dstH;
                for (int j = 0; j < 4; ++j) {
                    const float subAng = ang + (float)j * PI / 2.0f + PI / 4.0f;
                    petal(subU, subV, subAng,
                          (minSide * 0.060f) / dstW, (minSide * 0.060f) / dstH,
                          (minSide * 0.014f) / dstW, (minSide * 0.014f) / dstH, false);
                }
                // Micro-flower (3-petal) at sub-flower center punched as hole
                polystar(subU, subV,
                         (minSide*0.018f)/dstW, (minSide*0.018f)/dstH,
                         (minSide*0.010f)/dstW, (minSide*0.010f)/dstH,
                         6, ang, true);
            }
            // Centre punch + ring
            polystar(c, 0.5f,
                     (minSide*0.080f)/dstW, (minSide*0.080f)/dstH,
                     (minSide*0.060f)/dstW, (minSide*0.060f)/dstH,
                     16, 0.0f, false);
            polystar(c, 0.5f,
                     (minSide*0.045f)/dstW, (minSide*0.045f)/dstH,
                     (minSide*0.025f)/dstW, (minSide*0.025f)/dstH,
                     12, PI/12.0f, true);
            microFlower(c, 0.5f, 0.0f, 0.7f);
        };
        mandala(cU_left);
        mandala(cU_right);
        if (drawChain) {
            const float aVtop = (bandPx + minSide * 0.030f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 8;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                microFlower(uPos, aVtop, (float)k * PI / 8.0f, 0.7f);
                microFlower(uPos, aVbot, (float)k * PI / 8.0f, 0.7f);
            }
        }
    } else if (style == 17) {
        // -------- Style 17: Climbing-vine arabesque --------
        // A sinuous S-curve stem winding through the medallion, with
        // alternating leaves along its length. Both medallions and the
        // chain share this organic flowing motif.
        auto vine = [&](float c) {
            const int SEG = 12;
            const float stemR = minSide * 0.34f;
            // Master spiral approximated as polyline; petals branch off
            // each anchor point alternating sides.
            float prevU = 0.0f, prevV = 0.0f;
            for (int i = 0; i <= SEG; ++i) {
                const float t = (float)i / (float)SEG;
                // Logarithmic-ish spiral
                const float ang = t * 3.0f * PI;
                const float r = stemR * (1.0f - 0.6f * t);
                const float u = c + cosf(ang) * r / dstW;
                const float v = 0.5f + sinf(ang) * r / dstH;
                if (i > 0) {
                    // Thicken stem via two parallel curves
                    const float perpU = -sinf(ang) * (minSide * 0.012f) / dstW;
                    const float perpV =  cosf(ang) * (minSide * 0.012f) / dstH;
                    line(prevU + perpU, prevV + perpV, u + perpU, v + perpV);
                    line(u - perpU, v - perpV, prevU - perpU, prevV - perpV);
                }
                if (i > 0 && i < SEG) {
                    // Leaf branching outward (perpendicular to stem)
                    const float leafAng = ang + (((i & 1) == 0) ? PI/2.0f : -PI/2.0f);
                    petal(u, v, leafAng,
                          (minSide * 0.075f) / dstW, (minSide * 0.075f) / dstH,
                          (minSide * 0.025f) / dstW, (minSide * 0.025f) / dstH, false);
                    // Secondary smaller leaf opposite
                    petal(u, v, leafAng + PI,
                          (minSide * 0.035f) / dstW, (minSide * 0.035f) / dstH,
                          (minSide * 0.011f) / dstW, (minSide * 0.011f) / dstH, false);
                }
                prevU = u; prevV = v;
            }
            // Bud at spiral centre
            polystar(c, 0.5f,
                     (minSide*0.045f)/dstW, (minSide*0.045f)/dstH,
                     (minSide*0.025f)/dstW, (minSide*0.025f)/dstH,
                     8, 0.0f, false);
            polystar(c, 0.5f,
                     (minSide*0.020f)/dstW, (minSide*0.020f)/dstH,
                     (minSide*0.010f)/dstW, (minSide*0.010f)/dstH,
                     6, PI/6.0f, true);
        };
        vine(cU_left);
        vine(cU_right);
        if (drawChain) {
            // Chain: undulating leaf vine along top + bottom
            const float aVtop = (bandPx + minSide * 0.030f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 22;
            const float u0 = chainMarginU;
            const float u1 = 1.0f - chainMarginU;
            for (int k = 0; k <= N; ++k) {
                const float tt = (float)k / (float)N;
                const float u = u0 + tt * (u1 - u0);
                const float phaseTop = sinf(tt * 8.0f * PI) * (minSide * 0.012f) / dstH;
                const float phaseBot = sinf(tt * 8.0f * PI + PI) * (minSide * 0.012f) / dstH;
                if ((k & 1) == 0) {
                    petal(u, aVtop + phaseTop, ((k & 2) == 0) ? PI*0.5f : -PI*0.5f,
                          (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                          (minSide*0.008f)/dstW, (minSide*0.008f)/dstH, false);
                    petal(u, aVbot + phaseBot, ((k & 2) == 0) ? -PI*0.5f : PI*0.5f,
                          (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                          (minSide*0.008f)/dstW, (minSide*0.008f)/dstH, false);
                }
            }
        }
    } else if (style == 18) {
        // -------- Style 18: Crescent rosette --------
        // 8 overlapping crescent shapes (built from two arcs) arranged in a
        // ring, like a stylized moon-petal flower. Each crescent: an outer
        // arc CCW + an inner arc CW with offset center, creating a curved
        // sliver.
        auto crescent = [&](float cU, float cV, float ang, float scale) {
            const int ARC = 14;
            const float R   = minSide * 0.10f * scale;
            const float rIn = minSide * 0.078f * scale;
            const float offset = minSide * 0.030f * scale;
            // Center of outer arc
            const float ocU = cU;
            const float ocV = cV;
            // Center of inner arc shifted along ang
            const float icU = cU + cosf(ang) * offset / dstW;
            const float icV = cV + sinf(ang) * offset / dstH;
            // Outer arc CCW from ang+PI/2 → ang-PI/2 (180°)
            float prevU = 0.0f, prevV = 0.0f;
            for (int i = 0; i <= ARC; ++i) {
                const float t = (float)i / (float)ARC;
                const float a = ang + PI * 0.5f + t * PI; // CCW outer
                const float u = ocU + cosf(a) * R / dstW;
                const float v = ocV + sinf(a) * R / dstH;
                if (i > 0) line(prevU, prevV, u, v);
                prevU = u; prevV = v;
            }
            // Inner arc CW (returning), from ang-PI/2 → ang+PI/2
            for (int i = 0; i <= ARC; ++i) {
                const float t = (float)i / (float)ARC;
                const float a = ang - PI * 0.5f - t * PI; // sweep back
                const float u = icU + cosf(a) * rIn / dstW;
                const float v = icV + sinf(a) * rIn / dstH;
                if (i > 0) line(prevU, prevV, u, v);
                else       line(prevU, prevV, u, v);
                prevU = u; prevV = v;
            }
        };
        auto crescentRose = [&](float c) {
            const int N = 8;
            const float ringR = minSide * 0.24f;
            for (int i = 0; i < N; ++i) {
                const float ang = (float)i * 2.0f * PI / (float)N;
                const float cxU = c + cosf(ang) * ringR / dstW;
                const float cyV = 0.5f + sinf(ang) * ringR / dstH;
                crescent(cxU, cyV, ang, 1.0f);
            }
            // Inner ring of 4 smaller crescents
            const float innerR = minSide * 0.085f;
            for (int i = 0; i < 4; ++i) {
                const float ang = (float)i * PI / 2.0f + PI / 4.0f;
                const float cxU = c + cosf(ang) * innerR / dstW;
                const float cyV = 0.5f + sinf(ang) * innerR / dstH;
                crescent(cxU, cyV, ang, 0.55f);
            }
            // Centre boss
            polystar(c, 0.5f,
                     (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                     (minSide*0.022f)/dstW, (minSide*0.022f)/dstH,
                     8, 0.0f, false);
        };
        crescentRose(cU_left);
        crescentRose(cU_right);
        if (drawChain) {
            const float aVtop = (bandPx + minSide * 0.035f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                const float ang = (float)k * PI / 4.0f;
                crescent(uPos, aVtop, ang, 0.50f);
                crescent(uPos, aVbot, -ang, 0.50f);
            }
        }
    } else if (style == 19) {
        // -------- Style 19: Hexagonal tessellation field --------
        // No end medallions. Instead, fill the interior with a regular
        // tessellation of small hex stars. Different layout regime → very
        // distinctive look.
        const float cellPx = minSide * 0.085f;
        const float cellU = cellPx / dstW;
        const float cellV = cellPx / dstH;
        const float interiorU0 = bandPx / dstW + cellU;
        const float interiorU1 = 1.0f - interiorU0;
        const float interiorV0 = bandPx / dstH + cellV;
        const float interiorV1 = 1.0f - interiorV0;
        const float rowStep = cellV * 1.55f;
        const float colStep = cellU * 1.80f;
        int row = 0;
        for (float v = interiorV0; v <= interiorV1; v += rowStep) {
            const float xOff = ((row & 1) == 0) ? 0.0f : colStep * 0.5f;
            for (float u = interiorU0 + xOff; u <= interiorU1; u += colStep) {
                polystar(u, v,
                         (cellPx * 0.45f) / dstW, (cellPx * 0.45f) / dstH,
                         (cellPx * 0.22f) / dstW, (cellPx * 0.22f) / dstH,
                         6, (row & 1 ? PI/6.0f : 0.0f), false);
                polystar(u, v,
                         (cellPx * 0.18f) / dstW, (cellPx * 0.18f) / dstH,
                         (cellPx * 0.09f) / dstW, (cellPx * 0.09f) / dstH,
                         6, (row & 1 ? 0.0f : PI/6.0f), true);
            }
            ++row;
        }
    } else if (style == 20) {
        // -------- Style 20: Corner cartouches --------
        // No end medallions. Instead, four ornate triangular corner
        // cartouches with arcing inner edge — like the corners of an
        // illuminated manuscript page.
        const float cornerU = (bandPx + minSide * 0.015f) / dstW;
        const float cornerV = (bandPx + minSide * 0.015f) / dstH;
        const float cornerSize = minSide * 0.34f;
        auto cornerPiece = [&](float anchorU, float anchorV, float dirU, float dirV) {
            // Triangular corner from anchor extending inward by (dirU, dirV)
            const float farU = anchorU + dirU * cornerSize / dstW;
            const float farV = anchorV + dirV * cornerSize / dstH;
            // Filled fan with 5 petals radiating inward
            for (int p = 0; p < 5; ++p) {
                const float t = (float)p / 4.0f;  // 0..1
                const float ang = atan2f(dirV, dirU) + (t - 0.5f) * (PI * 0.6f);
                petal(anchorU, anchorV, ang,
                      (cornerSize * 0.80f) / dstW, (cornerSize * 0.80f) / dstH,
                      (minSide * 0.025f) / dstW, (minSide * 0.025f) / dstH, false);
            }
            // Rosette boss near the far point
            polystar(farU * 0.4f + anchorU * 0.6f, farV * 0.4f + anchorV * 0.6f,
                     (minSide * 0.040f) / dstW, (minSide * 0.040f) / dstH,
                     (minSide * 0.024f) / dstW, (minSide * 0.024f) / dstH,
                     8, 0.0f, false);
            polystar(farU * 0.4f + anchorU * 0.6f, farV * 0.4f + anchorV * 0.6f,
                     (minSide * 0.022f) / dstW, (minSide * 0.022f) / dstH,
                     (minSide * 0.012f) / dstW, (minSide * 0.012f) / dstH,
                     6, PI/6.0f, true);
        };
        cornerPiece(cornerU,        cornerV,         +1.0f, +1.0f);
        cornerPiece(1.0f-cornerU,   cornerV,         -1.0f, +1.0f);
        cornerPiece(cornerU,        1.0f-cornerV,    +1.0f, -1.0f);
        cornerPiece(1.0f-cornerU,   1.0f-cornerV,    -1.0f, -1.0f);
        // Central decoration: a horizontal flowing chain of small
        // ornaments spanning across the band.
        const float aV = 0.5f;
        const int N = 7;
        for (int k = 0; k < N; ++k) {
            const float t = (float)(k + 1) / (float)(N + 1);
            const float uPos = 0.35f + t * 0.30f;
            if ((k & 1) == 0) {
                polystar(uPos, aV,
                         (minSide * 0.022f) / dstW, (minSide * 0.022f) / dstH,
                         (minSide * 0.011f) / dstW, (minSide * 0.011f) / dstH,
                         8, 0.0f, false);
            } else {
                petal(uPos, aV, 0.0f,
                      (minSide * 0.030f) / dstW, (minSide * 0.030f) / dstH,
                      (minSide * 0.008f) / dstW, (minSide * 0.008f) / dstH, false);
                petal(uPos, aV, PI,
                      (minSide * 0.030f) / dstW, (minSide * 0.030f) / dstH,
                      (minSide * 0.008f) / dstW, (minSide * 0.008f) / dstH, false);
            }
        }
    } else if (style == 21) {
        // -------- Style 21: Bold calligraphic flourish --------
        // A single dramatic asymmetric swirl per medallion: thick
        // S-curve with a bulb at one end, like a Tughra calligraphic
        // mark or a stylized leaf. Much bolder than the symmetric
        // mandalas.
        auto flourish = [&](float c, bool mirror) {
            const float sgn = mirror ? -1.0f : 1.0f;
            const float SCALE = minSide * 0.36f;
            // Thick S-curve: two arcs concatenated with offset.
            // Approximated as polyline.
            const int SEG = 24;
            float prevUO = 0.0f, prevVO = 0.0f, prevUI = 0.0f, prevVI = 0.0f;
            for (int i = 0; i <= SEG; ++i) {
                const float t = (float)i / (float)SEG;
                // S-curve parameter: x = sgn*SCALE*(t*2-1), y = SCALE*sin(t*2*PI)*0.6
                const float xC = sgn * SCALE * (t * 2.0f - 1.0f);
                const float yC = SCALE * sinf(t * 2.0f * PI) * 0.45f;
                // Tangent direction
                const float dx = sgn * SCALE * 2.0f;
                const float dy = SCALE * cosf(t * 2.0f * PI) * 2.0f * PI * 0.45f;
                const float dlen = sqrtf(dx*dx + dy*dy);
                const float perpX = -dy / dlen;
                const float perpY =  dx / dlen;
                const float thickness = minSide * 0.045f * (1.0f - 0.5f * fabsf(t * 2.0f - 1.0f));
                const float uO = c + (xC + perpX * thickness) / dstW;
                const float vO = 0.5f + (yC + perpY * thickness) / dstH;
                const float uI = c + (xC - perpX * thickness) / dstW;
                const float vI = 0.5f + (yC - perpY * thickness) / dstH;
                if (i > 0) {
                    line(prevUO, prevVO, uO, vO);
                    line(uI, vI, prevUI, prevVI);
                }
                prevUO = uO; prevVO = vO;
                prevUI = uI; prevVI = vI;
            }
            // Bulb at start
            polystar(c + sgn * (-SCALE) / dstW, 0.5f,
                     (minSide * 0.055f) / dstW, (minSide * 0.055f) / dstH,
                     (minSide * 0.040f) / dstW, (minSide * 0.040f) / dstH,
                     12, 0.0f, false);
            // Pointed tip at end (petal pointing outward)
            petal(c + sgn * (SCALE * 0.9f) / dstW, 0.5f,
                  sgn > 0 ? 0.3f : PI - 0.3f,
                  (minSide * 0.075f) / dstW, (minSide * 0.075f) / dstH,
                  (minSide * 0.015f) / dstW, (minSide * 0.015f) / dstH, false);
            // Accent dots above and below the centre
            polystar(c, 0.5f - (minSide * 0.12f) / dstH,
                     (minSide * 0.020f) / dstW, (minSide * 0.020f) / dstH,
                     (minSide * 0.010f) / dstW, (minSide * 0.010f) / dstH,
                     6, 0.0f, false);
            polystar(c, 0.5f + (minSide * 0.12f) / dstH,
                     (minSide * 0.020f) / dstW, (minSide * 0.020f) / dstH,
                     (minSide * 0.010f) / dstW, (minSide * 0.010f) / dstH,
                     6, 0.0f, false);
        };
        flourish(cU_left, false);
        flourish(cU_right, true);
        if (drawChain) {
            // Tiny mirror flourishes along the band
            const float aVtop = (bandPx + minSide * 0.035f + minSide * 0.012f) / dstH;
            const float aVbot = 1.0f - aVtop;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float uPos = chainMarginU + t * (1.0f - 2.0f * chainMarginU);
                petal(uPos, aVtop, (k & 1 ? PI*0.25f : PI*0.75f),
                      (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                      (minSide*0.007f)/dstW, (minSide*0.007f)/dstH, false);
                petal(uPos, aVbot, (k & 1 ? -PI*0.25f : -PI*0.75f),
                      (minSide*0.030f)/dstW, (minSide*0.030f)/dstH,
                      (minSide*0.007f)/dstW, (minSide*0.007f)/dstH, false);
            }
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
