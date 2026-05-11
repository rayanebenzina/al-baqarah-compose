// Shared procedural-frame implementation. Include from `jni_bridge.cpp`
// (Android target — gets `LOGI` / `__system_property_get` / a real
// `appendLayerBands`) AND from the host-side test harness at
// `tools/frame_eval/test_frame.cpp` (which provides printf-based
// `LOGI` and a no-op `appendLayerBands`). Same source means the
// laptop tooling can never drift from on-device behaviour.

// Read at most once per emitFrame call; cheap on Android since the
// property server caches values in shared memory.
static bool isFrameAsciiDebugEnabled() {
    char buf[PROP_VALUE_MAX] = {0};
    const int n = __system_property_get("debug.baqarah.frame", buf);
    return n > 0 && buf[0] == '1';
}

// ASCII rasterizer for the curves just emitted by emitFrame. Computes
// the actual filled region (non-zero winding rule) per 96×18 cell —
// matching what Vulkan ultimately rasterises — and dumps the grid to
// logcat alongside an ink-cell count, bounding box, and a count of
// cells that fall inside the title-glyph ellipse (= readability
// violations, marked 'X'). Gated by the `debug.baqarah.frame` Android
// system property so it costs nothing in normal runs:
//   adb shell setprop debug.baqarah.frame 1
//   adb logcat -s BaqarahVkJNI:I
//
// Cost per call: 96 * 18 = 1728 cells x ~1000 curves x O(1) ray-cast =
// ~1.7M ops. Comfortably runs in single-digit ms.
static void logFrameAsciiPreview(const std::vector<float>& allCurves,
                                  int curveStart, int curveCount,
                                  int seed,
                                  int sideOpt, int kissOpt, int flourishOpt,
                                  int tipOpt, int bandOpt, int cornerOpt) {
    if (!isFrameAsciiDebugEnabled()) return;

    constexpr int W = 96;
    constexpr int H = 18;
    char grid[H][W + 1];

    constexpr float interiorMargin = 0.85f;
    constexpr float midHalfWUV = 0.28f, midHalfHUV = 0.40f;
    auto insideTitle = [&](float u, float v) {
        const float du = (u - 0.5f) / midHalfWUV;
        const float dv = (v - 0.5f) / midHalfHUV;
        return (du * du + dv * dv) < (interiorMargin * interiorMargin);
    };

    // Winding number at (px,py): for every quadratic curve, find roots
    // of y(t) = py, then count +1/-1 based on dy/dt direction whenever
    // x(t) > px (rightward horizontal ray cast).
    auto winding = [&](float px, float py) -> int {
        int wn = 0;
        for (int i = 0; i < curveCount; ++i) {
            const float x0 = allCurves[(curveStart + i) * 6 + 0];
            const float y0 = allCurves[(curveStart + i) * 6 + 1];
            const float x1 = allCurves[(curveStart + i) * 6 + 2];
            const float y1 = allCurves[(curveStart + i) * 6 + 3];
            const float x2 = allCurves[(curveStart + i) * 6 + 4];
            const float y2 = allCurves[(curveStart + i) * 6 + 5];
            const float a = y0 - 2.0f * y1 + y2;
            const float b = 2.0f * (y1 - y0);
            const float c = y0 - py;
            float roots[2];
            int n = 0;
            // 1e-5 (not 1e-9): the `line` lambda emits "degenerate
            // quadratics" with control y = (y0+y2)/2, which in float
            // gives a ≈ 5e-9 — non-zero but tiny. Treating that as a
            // genuine quadratic squeezes one root to ~0 and blows the
            // other to ~1e8, missing the real linear root near 0.5
            // and silently dropping crossings. Petals have |a|>0.01,
            // so they still take the quadratic branch.
            if (std::fabs(a) < 1e-5f) {
                if (std::fabs(b) > 1e-9f) {
                    const float t = -c / b;
                    if (t > 0.0f && t < 1.0f) roots[n++] = t;
                }
            } else {
                const float disc = b * b - 4.0f * a * c;
                if (disc >= 0.0f) {
                    const float sq = std::sqrt(disc);
                    const float tA = (-b - sq) / (2.0f * a);
                    const float tB = (-b + sq) / (2.0f * a);
                    if (tA > 0.0f && tA < 1.0f) roots[n++] = tA;
                    if (tB > 0.0f && tB < 1.0f) roots[n++] = tB;
                }
            }
            for (int k = 0; k < n; ++k) {
                const float t  = roots[k];
                const float mt = 1.0f - t;
                const float xt = mt*mt*x0 + 2.0f*mt*t*x1 + t*t*x2;
                if (xt > px) {
                    const float dydt = b + 2.0f * a * t;
                    if      (dydt > 0.0f) ++wn;
                    else if (dydt < 0.0f) --wn;
                }
            }
        }
        return wn;
    };

    float uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
    int interiorHits = 0;
    int inkCells = 0;
    // Jitter the ray's v by a tiny irrational so it never aligns with
    // a shape's horizontal axis (e.g., the middle ellipse's v=0.5).
    constexpr float V_JITTER = 0.000173f;
    for (int r = 0; r < H; ++r) {
        const float v = (r + 0.5f) / (float)H + V_JITTER;
        for (int c = 0; c < W; ++c) {
            const float u = (c + 0.5f) / (float)W;
            const bool filled = (winding(u, v) != 0);
            if (filled) {
                ++inkCells;
                uMin = std::min(uMin, u); uMax = std::max(uMax, u);
                vMin = std::min(vMin, v); vMax = std::max(vMax, v);
                if (insideTitle(u, v)) {
                    grid[r][c] = 'X';
                    ++interiorHits;
                } else {
                    grid[r][c] = '#';
                }
            } else {
                grid[r][c] = ' ';
            }
        }
        grid[r][W] = '\0';
    }
    if (inkCells == 0) { uMin = vMin = 0.0f; uMax = vMax = 0.0f; }

    LOGI("frame-ascii seed=%d slots=(side=%d kiss=%d flour=%d tip=%d band=%d corner=%d) "
         "curves=%d ink=%d bbox=(%.2f,%.2f)-(%.2f,%.2f) titleInteriorHits=%d",
         seed, sideOpt, kissOpt, flourishOpt, tipOpt, bandOpt, cornerOpt, curveCount,
         inkCells, uMin, vMin, uMax, vMax, interiorHits);
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
    // the frame; the seed factorises into six independent ornament
    // slots so every value produces a unique mix of motifs drawn from
    // the earlier hand-tuned styles. Total combinations = 6·5·6·5·4·5.
    const int NUM_STYLES = 18000;
    unsigned int seedU = (unsigned)seed;
    const int sideInteriorOpt = (int)(seedU % 6u); seedU /= 6u;
    const int kissOpt         = (int)(seedU % 5u); seedU /= 5u;
    const int flourishOpt     = (int)(seedU % 6u); seedU /= 6u;
    const int tipOpt          = (int)(seedU % 5u); seedU /= 5u;
    const int bandOpt         = (int)(seedU % 4u); seedU /= 4u;
    const int cornerFillOpt   = (int)(seedU % 5u);
    (void)NUM_STYLES;
    LOGI("emitFrame: seed=%d (mix: side=%d kiss=%d flourish=%d tip=%d band=%d corner=%d)",
         seed, sideInteriorOpt, kissOpt, flourishOpt, tipOpt, bandOpt, cornerFillOpt);

    // ---- Triple-loop backbone ----
    // Middle oval wide enough to clear the centred surah-title plate;
    // two circular side loops kiss it tangentially at u = 0.5 ±
    // midHalfWU. Adjacent loop pairs share a 6-point kiss star.
    const float midHalfWPx  = dstW * 0.28f;
    const float sideHalfPx  = dstH * 0.30f;
    const float thicknessPx = minSide * 0.022f;

    // filledRing — closed annulus, winding +1 between outer and inner
    // circles. Emits the outer ellipse CCW and the inner ellipse CW
    // as two *independent* closed sequences (separate prev-tracking)
    // so there's no spurious connector line between them.
    auto filledRing = [&](float cU, float cV,
                          float rUout, float rVout, float rUin, float rVin) {
        const int SEG = 64;
        {
            float pu = cU + rUout, pv = cV;
            for (int i = 1; i <= SEG; ++i) {
                const float t = (float)i * 2.0f * PI / (float)SEG;
                const float u = cU + cosf(t) * rUout;
                const float v = cV + sinf(t) * rVout;
                line(pu, pv, u, v);
                pu = u; pv = v;
            }
        }
        {
            float pu = cU + rUin, pv = cV;
            for (int i = 1; i <= SEG; ++i) {
                const float t = -(float)i * 2.0f * PI / (float)SEG;  // CW
                const float u = cU + cosf(t) * rUin;
                const float v = cV + sinf(t) * rVin;
                line(pu, pv, u, v);
                pu = u; pv = v;
            }
        }
    };

    // Helper: emit a closed rounded rectangle outline with the given
    // winding (`ccw=true` for CCW, false for CW reverse-trace). Each
    // rounded corner uses CORNER_SEG arc segments.
    auto traceRoundedRect = [&](float a0, float b0, float a1, float b1,
                                 float r, bool ccw) {
        const int CORNER_SEG = 8;
        // Build vertex list along the path in CCW order, then walk
        // forward or reverse based on `ccw`.
        std::vector<float> verts;
        auto add = [&](float u, float v) { verts.push_back(u); verts.push_back(v); };
        // Top edge → TR corner → right edge → BR corner → bottom edge
        // → BL corner → left edge → TL corner → close.
        add(a0 + r, b0);
        add(a1 - r, b0);
        for (int i = 1; i <= CORNER_SEG; ++i) {
            const float t = (float)i * PI / (2.0f * CORNER_SEG);
            add(a1 - r + sinf(t) * r, b0 + r - cosf(t) * r);
        }
        add(a1, b1 - r);
        for (int i = 1; i <= CORNER_SEG; ++i) {
            const float t = (float)i * PI / (2.0f * CORNER_SEG);
            add(a1 - r + cosf(t) * r, b1 - r + sinf(t) * r);
        }
        add(a0 + r, b1);
        for (int i = 1; i <= CORNER_SEG; ++i) {
            const float t = (float)i * PI / (2.0f * CORNER_SEG);
            add(a0 + r - sinf(t) * r, b1 - r + cosf(t) * r);
        }
        add(a0, b0 + r);
        for (int i = 1; i <= CORNER_SEG; ++i) {
            const float t = (float)i * PI / (2.0f * CORNER_SEG);
            add(a0 + r - cosf(t) * r, b0 + r - sinf(t) * r);
        }
        const int N = (int)(verts.size() / 2);
        if (ccw) {
            for (int i = 0; i < N - 1; ++i) {
                line(verts[i*2], verts[i*2+1], verts[(i+1)*2], verts[(i+1)*2+1]);
            }
            line(verts[(N-1)*2], verts[(N-1)*2+1], verts[0], verts[1]);
        } else {
            for (int i = N - 1; i > 0; --i) {
                line(verts[i*2], verts[i*2+1], verts[(i-1)*2], verts[(i-1)*2+1]);
            }
            line(verts[0], verts[1], verts[(N-1)*2], verts[(N-1)*2+1]);
        }
    };

    // roundedRectCartouche — filled annular band: CCW outer rounded
    // rect + CW inner rounded rect. The annulus shows as a thick
    // rounded-rectangle outline at 96×18.
    auto roundedRectCartouche = [&](float u0, float v0, float u1, float v1,
                                     float cornerR, float thicknessU, float thicknessV) {
        traceRoundedRect(u0, v0, u1, v1, cornerR, true);
        traceRoundedRect(u0 + thicknessU, v0 + thicknessV,
                         u1 - thicknessU, v1 - thicknessV,
                         std::max(0.0f, cornerR - 0.5f * (thicknessU + thicknessV)),
                         false);
    };

    // scroll — a long curving bezier path from `start` to `end` with a
    // perpendicular bulge of magnitude `bulge`. Branches off `leaves`
    // small almond-petals at evenly-spaced points along the curve.
    // Used to fill corner regions with flowing organic shapes rather
    // than regimented petal grids.
    auto scroll = [&](float u0, float v0, float u1, float v1,
                      float bulge, int leaves, float leafLen, float leafW) {
        const float midU = (u0 + u1) * 0.5f;
        const float midV = (v0 + v1) * 0.5f;
        const float dirU = u1 - u0, dirV = v1 - v0;
        const float dlen = std::max(1e-6f, sqrtf(dirU*dirU + dirV*dirV));
        const float perpU = -dirV / dlen;
        const float perpV =  dirU / dlen;
        const float ctrlU = midU + perpU * bulge;
        const float ctrlV = midV + perpV * bulge;
        curve(u0, v0, ctrlU, ctrlV, u1, v1);
        // Sprout perpendicular leaves at evenly-spaced positions
        // along the curve.
        for (int k = 1; k <= leaves; ++k) {
            const float t = (float)k / (float)(leaves + 1);
            const float mt = 1.0f - t;
            const float bu = mt*mt*u0 + 2.0f*mt*t*ctrlU + t*t*u1;
            const float bv = mt*mt*v0 + 2.0f*mt*t*ctrlV + t*t*v1;
            const float tx = 2.0f*mt*(ctrlU - u0) + 2.0f*t*(u1 - ctrlU);
            const float ty = 2.0f*mt*(ctrlV - v0) + 2.0f*t*(v1 - ctrlV);
            const float tlen = std::max(1e-6f, sqrtf(tx*tx + ty*ty));
            const float lpu = -ty / tlen;
            const float lpv =  tx / tlen;
            const float ang = atan2f(lpv, lpu);
            petal(bu, bv, ang,         leafLen, leafLen, leafW, leafW, false);
            petal(bu, bv, ang + PI,    leafLen, leafLen, leafW, leafW, false);
        }
    };

    // ---- Triple-loop backbone ----
    // Replaced: middle ribbon → rounded-rectangle cartouche bounding
    // the title cavity; side ribbons → filled circular medallions.
    const float sideOffsetU = (midHalfWPx + sideHalfPx) / dstW;
    const float sideCLU = 0.50f - sideOffsetU;
    const float sideCRU = 0.50f + sideOffsetU;

    // Filled medallion rings — annulus thick enough (≥1 grid cell) to
    // read as a solid outline at 96×18. Inner radius = 65% of outer
    // gives a chunky ring in v (the limiting dimension at this
    // aspect).
    const float medOuterU = sideHalfPx / dstW;
    const float medOuterV = sideHalfPx / dstH;
    const float medInnerU = medOuterU * 0.65f;
    const float medInnerV = medOuterV * 0.65f;
    filledRing(sideCLU, 0.5f, medOuterU, medOuterV, medInnerU, medInnerV);
    filledRing(sideCRU, 0.5f, medOuterU, medOuterV, medInnerU, medInnerV);

    // Rounded-rectangle cartouche around the title cavity. Its top
    // and bottom edges deliberately CROSS the inner band, producing
    // the structural "notches" visible in the reference.
    const float cartU0 = 0.50f - midHalfWPx / dstW;
    const float cartU1 = 0.50f + midHalfWPx / dstW;
    const float cartV0 = (bandPx * 0.3f) / dstH;             // poke into band
    const float cartV1 = 1.0f - cartV0;
    const float cartCornerR = std::min(midHalfWPx * 0.10f / dstW,
                                       (cartV1 - cartV0) * 0.20f);
    const float cartThU = thicknessPx * 1.2f / dstW;
    const float cartThV = thicknessPx * 1.2f / dstH;
    roundedRectCartouche(cartU0, cartV0, cartU1, cartV1,
                         cartCornerR, cartThU, cartThV);

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
    // When corner-fill is active, the medallion interior is carved
    // out by drawCornerFill and re-decorated there — drawing the
    // side-interior ornament here would land beneath the cutout and
    // leak +1 windings the single cutout can't cancel. Suppress.
    if (cornerFillOpt == 0) {
        drawSideInterior(sideCLU, sideInteriorOpt);
        drawSideInterior(sideCRU, sideInteriorOpt);
    }

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

    // ---- Slot 6: dense side-block filigree ----
    // The reference Mushaf border fills the *entire* left- and right-
    // hand side regions (between the band edge and the title cavity)
    // with dense floral interlace, with a circular medallion cutout
    // around each side loop. This slot reproduces that pattern: one
    // big block per side (full vertical extent of the band area),
    // each option drawing a different ornament style. A CW circle
    // around the side-loop centre carves the medallion cutout.
    auto drawCornerFill = [&](int opt) {
        if (opt == 0) return;
        const float uMinL = (bandPx + minSide * 0.006f) / dstW;
        // Block right edge sits just past the medallion's outer rim
        // (the side-loop's right edge) so the CW medallion-cutout
        // disc stays fully inside the block. The block stays left of
        // the title interior margin (~0.5 − 0.85·midHalfW/dstW).
        const float uMaxL = sideCLU + sideHalfPx / dstW + minSide * 0.004f / dstW;
        const float uMinR = 1.0f - uMaxL;
        const float uMaxR = 1.0f - uMinL;
        const float vMin  = (bandPx + minSide * 0.006f) / dstH;
        const float vMax  = 1.0f - vMin;

        auto fillBlock = [&](float u0, float u1, float v0, float v1,
                             float medU, int o) {
            const float spanU = u1 - u0;
            const float spanV = v1 - v0;
            if (spanU <= 0.0f || spanV <= 0.0f) return;
            if (o == 1) {
                // Flowing scrollwork: long bezier vines curving along
                // the block's long axis, with small leaves branching
                // off — directly evokes the reference's filigree.
                const float bulge = std::min(spanU, spanV) * 0.18f;
                const float leafL = std::min(spanU, spanV) * 0.10f;
                const float leafW = leafL * 0.45f;
                const int leaves = 6;
                // Two overlapping S-scrolls in opposite directions.
                scroll(u0 + spanU * 0.05f, v0 + spanV * 0.25f,
                       u0 + spanU * 0.95f, v0 + spanV * 0.75f,
                        bulge, leaves, leafL, leafW);
                scroll(u0 + spanU * 0.05f, v0 + spanV * 0.75f,
                       u0 + spanU * 0.95f, v0 + spanV * 0.25f,
                       -bulge, leaves, leafL, leafW);
                // A horizontal vine across the middle.
                scroll(u0 + spanU * 0.05f, v0 + spanV * 0.50f,
                       u0 + spanU * 0.95f, v0 + spanV * 0.50f,
                        bulge * 0.7f, leaves, leafL, leafW);
            } else if (o == 2) {
                // Tight 6-star grid (8 cols × 3 rows).
                const float r = std::min(spanU / 16.0f, spanV / 6.0f);
                for (int rr = 0; rr < 3; ++rr) {
                    for (int cc = 0; cc < 8; ++cc) {
                        const float u = u0 + spanU * ((float)(cc + 0.5f)) / 8.0f;
                        const float v = v0 + spanV * ((float)(rr + 0.5f)) / 3.0f;
                        polystar(u, v, r * 2.0f, r * 2.0f,
                                 r * 1.0f, r * 1.0f, 6, 0.0f, false);
                    }
                }
            } else if (o == 3) {
                // Damascus interlace clipped to the block: 12×5 grid
                // of overlapping petals at alternating angles, with
                // petal size capped to min(cellU, cellV)·overlap so
                // no petal spills past the block edge into the title
                // cavity. Slightly less dense than the unclipped
                // variant but invariant-safe.
                const int NU = 12, NV = 5;
                const float cellU = spanU / (float)NU;
                const float cellV = spanV / (float)NV;
                const float pL = std::min(cellU, cellV) * 1.30f;
                const float pW = pL * 0.55f;
                for (int rr = 0; rr < NV; ++rr) {
                    for (int cc = 0; cc < NU; ++cc) {
                        const float u = u0 + cellU * ((float)cc + 0.5f);
                        const float v = v0 + cellV * ((float)rr + 0.5f);
                        const float ang = ((cc + rr) & 1) ? PI * 0.25f : -PI * 0.25f;
                        petal(u, v, ang,         pL, pL, pW, pW, false);
                        petal(u, v, ang + PI,    pL, pL, pW, pW, false);
                    }
                }
            } else { // o == 4
                // Saturated filled block: a CCW outer rectangle fills
                // the entire region with winding +1, then a CW
                // medallion circle (drawn at the end of this lambda)
                // carves the cutout. Highest possible coverage.
                line(u0, v0, u1, v0);
                line(u1, v0, u1, v1);
                line(u1, v1, u0, v1);
                line(u0, v1, u0, v0);
            }
            // Carve the medallion cutout: a CW-traced circle around
            // the side-loop centre. Radius matches the medallion's
            // outer rim exactly so the cutout stays strictly inside
            // the block (no leftover negative-winding crescent past
            // the block edge).
            const float rU = sideHalfPx / dstW;
            const float rV = sideHalfPx / dstH;
            const int SEG = 64;
            float prevU = medU + rU, prevV = 0.5f;
            for (int i = 1; i <= SEG; ++i) {
                const float t = -(float)i * 2.0f * PI / (float)SEG;  // CW
                const float uu = medU + cosf(t) * rU;
                const float vv = 0.5f + sinf(t) * rV;
                line(prevU, prevV, uu, vv);
                prevU = uu; prevV = vv;
            }
        };
        fillBlock(uMinL, uMaxL, vMin, vMax, sideCLU, opt);
        fillBlock(uMinR, uMaxR, vMin, vMax, sideCRU, opt);
    };
    drawCornerFill(cornerFillOpt);

    const int curveCount = totalCurves - curveStart;

    logFrameAsciiPreview(allCurves, curveStart, curveCount, seed,
                         sideInteriorOpt, kissOpt, flourishOpt, tipOpt,
                         bandOpt, cornerFillOpt);

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
