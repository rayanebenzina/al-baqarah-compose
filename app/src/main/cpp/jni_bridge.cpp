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

// Read at most once per emitFrame call; cheap on Android since the
// property server caches values in shared memory.
static bool isFrameAsciiDebugEnabled() {
    char buf[PROP_VALUE_MAX] = {0};
    const int n = __system_property_get("debug.baqarah.frame", buf);
    return n > 0 && buf[0] == '1';
}

// ASCII rasterizer for the curves just emitted by emitFrame. Samples
// each quadratic Bézier in UV space and prints a 96×18 grid to logcat,
// plus the bounding box and a count of cells that fall inside the
// title-glyph ellipse (= invariant violations — those cells are marked
// 'X' instead of '#'). Gated by the `debug.baqarah.frame` system
// property so it costs nothing in normal runs:
//   adb shell setprop debug.baqarah.frame 1
//   adb logcat -s BaqarahVkJNI:I
static void logFrameAsciiPreview(const std::vector<float>& allCurves,
                                  int curveStart, int curveCount,
                                  int seed,
                                  int sideOpt, int kissOpt, int flourishOpt,
                                  int tipOpt, int bandOpt) {
    if (!isFrameAsciiDebugEnabled()) return;

    constexpr int W = 96;
    constexpr int H = 18;
    char grid[H][W + 1];
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) grid[r][c] = ' ';
        grid[r][W] = '\0';
    }

    // Middle-loop ellipse: cx=0.5, cy=0.5, half-axes (0.28, 0.40) in
    // UV. The title plate sits inside it — any non-backbone curve
    // landing strictly inside (margin 0.85 to allow the loop ribbon
    // itself) violates the readability invariant.
    constexpr float interiorMargin = 0.85f;
    constexpr float midHalfWUV = 0.28f, midHalfHUV = 0.40f;
    auto insideTitle = [&](float u, float v) {
        const float du = (u - 0.5f) / midHalfWUV;
        const float dv = (v - 0.5f) / midHalfHUV;
        return (du * du + dv * dv) < (interiorMargin * interiorMargin);
    };

    float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
    int interiorHits = 0;

    constexpr int SAMPLES = 12;
    for (int i = 0; i < curveCount; ++i) {
        const float x0 = allCurves[(curveStart + i) * 6 + 0];
        const float y0 = allCurves[(curveStart + i) * 6 + 1];
        const float x1 = allCurves[(curveStart + i) * 6 + 2];
        const float y1 = allCurves[(curveStart + i) * 6 + 3];
        const float x2 = allCurves[(curveStart + i) * 6 + 4];
        const float y2 = allCurves[(curveStart + i) * 6 + 5];
        for (int s = 0; s <= SAMPLES; ++s) {
            const float t  = (float)s / (float)SAMPLES;
            const float mt = 1.0f - t;
            const float u  = mt*mt*x0 + 2.0f*mt*t*x1 + t*t*x2;
            const float v  = mt*mt*y0 + 2.0f*mt*t*y1 + t*t*y2;
            uMin = std::min(uMin, u); uMax = std::max(uMax, u);
            vMin = std::min(vMin, v); vMax = std::max(vMax, v);
            const int col = (int)(u * (float)W);
            const int row = (int)(v * (float)H);
            if (col >= 0 && col < W && row >= 0 && row < H) {
                if (insideTitle(u, v)) {
                    grid[row][col] = 'X';
                    ++interiorHits;
                } else if (grid[row][col] != 'X') {
                    grid[row][col] = '#';
                }
            }
        }
    }

    LOGI("frame-ascii seed=%d slots=(side=%d kiss=%d flour=%d tip=%d band=%d) "
         "curves=%d bbox=(%.2f,%.2f)-(%.2f,%.2f) titleInteriorHits=%d",
         seed, sideOpt, kissOpt, flourishOpt, tipOpt, bandOpt, curveCount,
         uMin, vMin, uMax, vMax, interiorHits);
    for (int r = 0; r < H; ++r) {
        LOGI("frame-ascii |%s|", grid[r]);
    }
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

    // -------- Mixed-mode compositional dispatch --------
    // The triple chained-loop is the always-on structural backbone of
    // the frame; the seed factorises into five independent ornament
    // slots so every value produces a unique mix of motifs drawn from
    // the earlier hand-tuned styles. Total combinations = 6·5·6·5·4.
    const int NUM_STYLES = 3600;
    unsigned int seedU = (unsigned)seed;
    const int sideInteriorOpt = (int)(seedU % 6u); seedU /= 6u;
    const int kissOpt         = (int)(seedU % 5u); seedU /= 5u;
    const int flourishOpt     = (int)(seedU % 6u); seedU /= 6u;
    const int tipOpt          = (int)(seedU % 5u); seedU /= 5u;
    const int bandOpt         = (int)(seedU % 4u);
    (void)NUM_STYLES;
    LOGI("emitFrame: seed=%d (mix: side=%d kiss=%d flourish=%d tip=%d band=%d)",
         seed, sideInteriorOpt, kissOpt, flourishOpt, tipOpt, bandOpt);

    // ---- Triple-loop backbone ----
    // Middle oval wide enough to clear the centred surah-title plate;
    // two circular side loops kiss it tangentially at u = 0.5 ±
    // midHalfWU. Adjacent loop pairs share a 6-point kiss star.
    const float midHalfWPx  = dstW * 0.28f;
    const float midHalfHPx  = dstH * 0.40f;
    const float sideHalfPx  = dstH * 0.30f;
    const float thicknessPx = minSide * 0.022f;

    auto ovalLoop = [&](float cU, float halfWPx, float halfHPx, float thPx) {
        const int SEG = 96;
        float prevUO = 0, prevVO = 0, prevUI = 0, prevVI = 0;
        for (int i = 0; i <= SEG; ++i) {
            const float t = (float)i * 2.0f * 3.14159265f / (float)SEG;
            const float xC = halfWPx * cosf(t);
            const float yC = halfHPx * sinf(t);
            const float tx = -halfWPx * sinf(t);
            const float ty =  halfHPx * cosf(t);
            const float tlen = std::max(1e-6f, sqrtf(tx*tx + ty*ty));
            const float perpX = -ty / tlen;
            const float perpY =  tx / tlen;
            const float uO = cU + (xC + perpX * thPx) / dstW;
            const float vO = 0.5f + (yC + perpY * thPx) / dstH;
            const float uI = cU + (xC - perpX * thPx) / dstW;
            const float vI = 0.5f + (yC - perpY * thPx) / dstH;
            if (i > 0) {
                line(prevUO, prevVO, uO, vO);
                line(uI, vI, prevUI, prevVI);
            }
            prevUO = uO; prevVO = vO;
            prevUI = uI; prevVI = vI;
        }
    };

    ovalLoop(0.50f, midHalfWPx, midHalfHPx, thicknessPx);
    const float sideOffsetU = (midHalfWPx + sideHalfPx) / dstW;
    const float sideCLU = 0.50f - sideOffsetU;
    const float sideCRU = 0.50f + sideOffsetU;
    ovalLoop(sideCLU, sideHalfPx, sideHalfPx, thicknessPx * 0.9f);
    ovalLoop(sideCRU, sideHalfPx, sideHalfPx, thicknessPx * 0.9f);

    // ---- Slot 1: side-loop interior ornament ----
    auto drawSideInterior = [&](float cU, int opt) {
        const float r = sideHalfPx;
        if (opt == 0) {
            // empty — the loop stands on its own
        } else if (opt == 1) {
            // 8-pointed star
            polystar(cU, 0.5f,
                     (r*0.55f)/dstW, (r*0.55f)/dstH,
                     (r*0.28f)/dstW, (r*0.28f)/dstH,
                     8, 0.0f, false);
        } else if (opt == 2) {
            // 6-petal floret
            const float L = r * 0.55f, W = r * 0.18f;
            for (int p = 0; p < 6; ++p) {
                const float ang = (float)p * 2.0f * PI / 6.0f;
                petal(cU, 0.5f, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        } else if (opt == 3) {
            // Concentric polygon rings
            polystar(cU, 0.5f,
                     (r*0.62f)/dstW, (r*0.62f)/dstH,
                     (r*0.50f)/dstW, (r*0.50f)/dstH,
                     20, 0.0f, false);
            polystar(cU, 0.5f,
                     (r*0.34f)/dstW, (r*0.34f)/dstH,
                     (r*0.20f)/dstW, (r*0.20f)/dstH,
                     12, 0.0f, true);
        } else if (opt == 4) {
            // Mini peony — two petal rings + centre pip
            const float L1 = r*0.58f, W1 = r*0.20f;
            for (int p = 0; p < 10; ++p) {
                const float ang = (float)p * 2.0f * PI / 10.0f;
                petal(cU, 0.5f, ang, L1/dstW, L1/dstH, W1/dstW, W1/dstH, false);
            }
            const float L2 = r*0.32f, W2 = r*0.16f;
            for (int p = 0; p < 7; ++p) {
                const float ang = (PI/7.0f) + (float)p * 2.0f * PI / 7.0f;
                petal(cU, 0.5f, ang, L2/dstW, L2/dstH, W2/dstW, W2/dstH, false);
            }
            polystar(cU, 0.5f,
                     (r*0.13f)/dstW, (r*0.13f)/dstH,
                     (r*0.07f)/dstW, (r*0.07f)/dstH,
                     6, 0.0f, false);
        } else { // opt == 5
            // 6-point star + petal halo
            polystar(cU, 0.5f,
                     (r*0.38f)/dstW, (r*0.38f)/dstH,
                     (r*0.18f)/dstW, (r*0.18f)/dstH,
                     6, 0.0f, false);
            const float L = r * 0.58f, W = r * 0.09f;
            for (int p = 0; p < 6; ++p) {
                const float ang = (PI/6.0f) + (float)p * 2.0f * PI / 6.0f;
                petal(cU, 0.5f, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        }
    };
    drawSideInterior(sideCLU, sideInteriorOpt);
    drawSideInterior(sideCRU, sideInteriorOpt);

    // ---- Slot 2: kiss-point ornament (tangent join of middle & side loop) ----
    const float kissLU = 0.50f - midHalfWPx / dstW;
    const float kissRU = 0.50f + midHalfWPx / dstW;
    auto drawKiss = [&](float u, int opt) {
        const float r = minSide * 0.030f;
        if (opt == 0) {
            polystar(u, 0.5f, r/dstW, r/dstH,
                     (r*0.55f)/dstW, (r*0.55f)/dstH, 6, 0.0f, false);
        } else if (opt == 1) {
            polystar(u, 0.5f, r/dstW, r/dstH,
                     (r*0.30f)/dstW, (r*0.30f)/dstH, 4, PI/4.0f, false);
        } else if (opt == 2) {
            const float L = r * 0.85f, W = r * 0.25f;
            for (int q = 0; q < 4; ++q) {
                const float ang = (float)q * PI * 0.5f;
                petal(u, 0.5f, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        } else if (opt == 3) {
            polystar(u, 0.5f, r/dstW, r/dstH,
                     (r*0.85f)/dstW, (r*0.85f)/dstH, 16, 0.0f, false);
            polystar(u, 0.5f, (r*0.55f)/dstW, (r*0.55f)/dstH,
                     (r*0.40f)/dstW, (r*0.40f)/dstH, 12, 0.0f, true);
        } else { // opt == 4
            polystar(u, 0.5f, r/dstW, r/dstH,
                     (r*0.42f)/dstW, (r*0.42f)/dstH, 8, 0.0f, false);
        }
    };
    drawKiss(kissLU, kissOpt);
    drawKiss(kissRU, kissOpt);

    // ---- Slot 3: flourish in the band above/below the loops ----
    auto drawFlourish = [&](int opt) {
        if (opt == 0) return;
        const float aVtop = (bandPx + minSide * 0.012f) / dstH;
        const float aVbot = 1.0f - aVtop;
        const float uMin = (bandPx + minSide * 0.020f) / dstW;
        const float uMax = 1.0f - uMin;
        if (opt == 1) {
            // Sine waveline ribbon
            const int SEG = 96;
            const float amp = minSide * 0.011f;
            const float thicknessPx2 = minSide * 0.003f;
            for (int row = 0; row < 2; ++row) {
                const float vCenter = (row == 0) ? aVtop : aVbot;
                float prevUO = 0, prevVO = 0, prevUI = 0, prevVI = 0;
                for (int i = 0; i <= SEG; ++i) {
                    const float t = (float)i / (float)SEG;
                    const float u = uMin + t * (uMax - uMin);
                    const float yC = amp * sinf(t * 8.0f * PI);
                    const float vO = vCenter + (yC + thicknessPx2) / dstH;
                    const float vI = vCenter + (yC - thicknessPx2) / dstH;
                    if (i > 0) {
                        line(prevUO, prevVO, u, vO);
                        line(u, vI, prevUI, prevVI);
                    }
                    prevUO = u; prevVO = vO;
                    prevUI = u; prevVI = vI;
                }
            }
        } else if (opt == 2) {
            // Trefoil leaf chain
            const float L = minSide * 0.016f, W = L * 0.30f;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                for (int q = 0; q < 3; ++q) {
                    const float ang = (float)q * 2.0f * PI / 3.0f - PI*0.5f;
                    petal(u, aVtop, ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                    petal(u, aVbot, -ang, L/dstW, L/dstH, W/dstW, W/dstH, false);
                }
            }
        } else if (opt == 3) {
            // 8-pointed star chain
            const float r = minSide * 0.011f;
            const int N = 11;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                polystar(u, aVtop, r/dstW, r/dstH,
                         (r*0.45f)/dstW, (r*0.45f)/dstH, 8, 0.0f, false);
                polystar(u, aVbot, r/dstW, r/dstH,
                         (r*0.45f)/dstW, (r*0.45f)/dstH, 8, 0.0f, false);
            }
        } else if (opt == 4) {
            // S-scroll filigree petals
            const float L = minSide * 0.022f, W = L * 0.22f;
            const int N = 9;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                petal(u, aVtop, -PI*0.25f, L/dstW, L/dstH, W/dstW, W/dstH, false);
                petal(u, aVtop, -PI*0.75f, L/dstW, L/dstH, W/dstW, W/dstH, false);
                petal(u, aVbot,  PI*0.25f, L/dstW, L/dstH, W/dstW, W/dstH, false);
                petal(u, aVbot,  PI*0.75f, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        } else { // opt == 5
            // Tiny-diamond stipple
            const float r = minSide * 0.005f;
            const int N = 21;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                polystar(u, aVtop, r/dstW, r/dstH,
                         (r*0.25f)/dstW, (r*0.25f)/dstH, 4, PI/4.0f, false);
                polystar(u, aVbot, r/dstW, r/dstH,
                         (r*0.25f)/dstW, (r*0.25f)/dstH, 4, PI/4.0f, false);
            }
        }
    };
    drawFlourish(flourishOpt);

    // ---- Slot 4: outer-tip terminator on each side loop ----
    const float farLeftU  = sideCLU - sideHalfPx / dstW;
    const float farRightU = sideCRU + sideHalfPx / dstW;
    auto drawTip = [&](float u, bool leftward, int opt) {
        const float facing = leftward ? PI : 0.0f;
        const float r = sideHalfPx * 0.55f;
        if (opt == 0) {
            const float L = r * 0.85f, W = r * 0.22f;
            petal(u, 0.5f, facing, L/dstW, L/dstH, W/dstW, W/dstH, false);
        } else if (opt == 1) {
            // Cardioid bud pointing outward
            const float a = r * 0.40f;
            const float sign = leftward ? -1.0f : 1.0f;
            const int SEG = 48;
            float prevU = 0, prevV = 0;
            for (int i = 0; i <= SEG; ++i) {
                const float th = (float)i * 2.0f * PI / (float)SEG;
                const float rr = a * (1.0f - cosf(th));
                const float xC = sign * sinf(th) * rr;
                const float yC = cosf(th) * rr - a;
                const float uu = u + xC / dstW;
                const float vv = 0.5f + yC / dstH;
                if (i > 0) line(prevU, prevV, uu, vv);
                prevU = uu; prevV = vv;
            }
        } else if (opt == 2) {
            polystar(u, 0.5f,
                     (r*0.70f)/dstW, (r*0.70f)/dstH,
                     (r*0.30f)/dstW, (r*0.30f)/dstH,
                     5, facing, false);
        } else if (opt == 3) {
            // Three-petal fan
            const float L = r * 0.80f, W = r * 0.18f;
            for (int q = -1; q <= 1; ++q) {
                const float a2 = facing + (float)q * 0.45f;
                petal(u, 0.5f, a2, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        } else { // opt == 4
            // Spiral curl
            const float sign = leftward ? -1.0f : 1.0f;
            const int SEG = 40;
            float prevU = 0, prevV = 0;
            for (int i = 0; i <= SEG; ++i) {
                const float t = (float)i / (float)SEG;
                const float th = t * 3.0f * PI;
                const float rr = r * (1.0f - t * 0.8f);
                const float xC = sign * cosf(th) * rr;
                const float yC = sinf(th) * rr;
                const float uu = u + xC / dstW;
                const float vv = 0.5f + yC / dstH;
                if (i > 0) line(prevU, prevV, uu, vv);
                prevU = uu; prevV = vv;
            }
        }
    };
    drawTip(farLeftU, true, tipOpt);
    drawTip(farRightU, false, tipOpt);

    // ---- Slot 5: band-edge micro-stipple ----
    auto drawBand = [&](int opt) {
        if (opt == 0) return;
        const float aVtop = (bandPx + minSide * 0.004f) / dstH;
        const float aVbot = 1.0f - aVtop;
        const float uMin = (bandPx + minSide * 0.008f) / dstW;
        const float uMax = 1.0f - uMin;
        if (opt == 1) {
            // Tiny dotted line
            const float r = minSide * 0.0035f;
            const int N = 31;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                polystar(u, aVtop, r/dstW, r/dstH,
                         (r*0.45f)/dstW, (r*0.45f)/dstH, 8, 0.0f, false);
                polystar(u, aVbot, r/dstW, r/dstH,
                         (r*0.45f)/dstW, (r*0.45f)/dstH, 8, 0.0f, false);
            }
        } else if (opt == 2) {
            // Alternating 6-star / 4-diamond
            const float r = minSide * 0.006f;
            const int N = 21;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                if (k & 1) {
                    polystar(u, aVtop, r/dstW, r/dstH,
                             (r*0.35f)/dstW, (r*0.35f)/dstH, 6, 0.0f, false);
                    polystar(u, aVbot, r/dstW, r/dstH,
                             (r*0.35f)/dstW, (r*0.35f)/dstH, 6, 0.0f, false);
                } else {
                    polystar(u, aVtop, (r*0.55f)/dstW, (r*0.55f)/dstH,
                             (r*0.25f)/dstW, (r*0.25f)/dstH, 4, PI/4.0f, false);
                    polystar(u, aVbot, (r*0.55f)/dstW, (r*0.55f)/dstH,
                             (r*0.25f)/dstW, (r*0.25f)/dstH, 4, PI/4.0f, false);
                }
            }
        } else { // opt == 3
            // Tiny upright petals
            const float L = minSide * 0.010f, W = L * 0.35f;
            const int N = 17;
            for (int k = 0; k < N; ++k) {
                const float t = (float)(k + 1) / (float)(N + 1);
                const float u = uMin + t * (uMax - uMin);
                petal(u, aVtop,  PI*0.5f, L/dstW, L/dstH, W/dstW, W/dstH, false);
                petal(u, aVbot, -PI*0.5f, L/dstW, L/dstH, W/dstW, W/dstH, false);
            }
        }
    };
    drawBand(bandOpt);

    const int curveCount = totalCurves - curveStart;

    logFrameAsciiPreview(allCurves, curveStart, curveCount, seed,
                         sideInteriorOpt, kissOpt, flourishOpt, tipOpt, bandOpt);

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
