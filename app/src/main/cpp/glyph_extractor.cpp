#include "glyph_extractor.h"

#include <android/log.h>

#include <algorithm>

#include "third_party/stb_truetype.h"

#define LOG_TAG "BaqarahGlyph"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace baqarah {

int findGlyphIndex(const uint8_t* ttfData, int ttfSize, int codepoint) {
    (void)ttfSize;
    stbtt_fontinfo font;
    int offset = stbtt_GetFontOffsetForIndex(ttfData, 0);
    if (offset < 0) return 0;
    if (!stbtt_InitFont(&font, ttfData, offset)) return 0;
    return stbtt_FindGlyphIndex(&font, codepoint);
}

bool extractGlyphOutline(const uint8_t* ttfData, int ttfSize,
                         int codepoint, GlyphOutline& out) {
    int glyphIndex = findGlyphIndex(ttfData, ttfSize, codepoint);
    if (glyphIndex == 0) {
        LOGE("no glyph for codepoint U+%04X", codepoint);
        return false;
    }
    return extractGlyphOutlineByIndex(ttfData, ttfSize, glyphIndex, out);
}

bool extractGlyphOutlineByIndex(const uint8_t* ttfData, int ttfSize,
                                int glyphIndex, GlyphOutline& out) {
    (void)ttfSize;
    stbtt_fontinfo font;
    int offset = stbtt_GetFontOffsetForIndex(ttfData, 0);
    if (offset < 0) {
        LOGE("stbtt_GetFontOffsetForIndex failed");
        return false;
    }
    if (!stbtt_InitFont(&font, ttfData, offset)) {
        LOGE("stbtt_InitFont failed");
        return false;
    }

    // Glyph metrics in font units (no scale applied)
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&font, glyphIndex, &advance, &lsb);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBox(&font, glyphIndex, &x0, &y0, &x1, &y1);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    (void)lineGap;

    int unitsPerEm = ascent - descent;
    if (unitsPerEm <= 0) unitsPerEm = 1024;

    // Extract glyph shape: a list of stbtt_vertex describing the outline.
    stbtt_vertex* vertices = nullptr;
    int vertexCount = stbtt_GetGlyphShape(&font, glyphIndex, &vertices);
    if (vertexCount == 0 || vertices == nullptr) {
        if (vertices) stbtt_FreeShape(&font, vertices);
        return false;
    }

    // Walk vertices and emit quadratic-Bezier curves. TTF outlines are
    // natively quadratic. Straight lines become degenerate quadratics
    // with the control point at the segment midpoint.
    out.curves.clear();
    out.curves.reserve((size_t)vertexCount * 6);

    int nMove = 0, nLine = 0, nCurve = 0, nCubic = 0;
    float cx = 0.0f, cy = 0.0f;  // current pen position
    float startX = 0.0f, startY = 0.0f;  // first point of current contour
    for (int i = 0; i < vertexCount; ++i) {
        const stbtt_vertex& v = vertices[i];
        switch (v.type) {
            case STBTT_vmove:
                cx = (float)v.x;
                cy = (float)v.y;
                startX = cx;
                startY = cy;
                ++nMove;
                break;
            case STBTT_vline: {
                float nx = (float)v.x;
                float ny = (float)v.y;
                float mx = (cx + nx) * 0.5f;
                float my = (cy + ny) * 0.5f;
                // Skip zero-length segments (can happen at contour seams).
                if (cx != nx || cy != ny) {
                    out.curves.push_back(cx); out.curves.push_back(cy);
                    out.curves.push_back(mx); out.curves.push_back(my);
                    out.curves.push_back(nx); out.curves.push_back(ny);
                }
                cx = nx; cy = ny;
                ++nLine;
                break;
            }
            case STBTT_vcurve: {
                float nx = (float)v.x;
                float ny = (float)v.y;
                float ctrlX = (float)v.cx;
                float ctrlY = (float)v.cy;
                out.curves.push_back(cx);    out.curves.push_back(cy);
                out.curves.push_back(ctrlX); out.curves.push_back(ctrlY);
                out.curves.push_back(nx);    out.curves.push_back(ny);
                cx = nx; cy = ny;
                ++nCurve;
                break;
            }
            case STBTT_vcubic: {
                // Cubic Bezier — approximate as two quadratics using a
                // single mid-split. For better accuracy, use the average
                // of the two cubic control points as the quadratic control,
                // which exactly matches the cubic's mid-tangent direction.
                float nx = (float)v.x;
                float ny = (float)v.y;
                float c1x = (float)v.cx;
                float c1y = (float)v.cy;
                float c2x = (float)v.cx1;
                float c2y = (float)v.cy1;
                // Midpoint of cubic at t=0.5: B(0.5) = (p0 + 3c1 + 3c2 + p3) / 8
                float midX = (cx + 3.0f * c1x + 3.0f * c2x + nx) * 0.125f;
                float midY = (cy + 3.0f * c1y + 3.0f * c2y + ny) * 0.125f;
                // First quadratic: from (cx,cy) through midpoint, control
                // chosen to match cubic's tangent at t=0 (which is in
                // direction c1 - p0).
                // Tangent-extension control for a quadratic A->M with start tangent T:
                //   control = A + T * something — for simplest approximation,
                //   put control halfway along the cubic's first segment.
                float q1cx = (cx + c1x) * 0.5f;
                float q1cy = (cy + c1y) * 0.5f;
                out.curves.push_back(cx);   out.curves.push_back(cy);
                out.curves.push_back(q1cx); out.curves.push_back(q1cy);
                out.curves.push_back(midX); out.curves.push_back(midY);
                // Second quadratic: from midpoint to (nx,ny), control on
                // cubic's second segment.
                float q2cx = (c2x + nx) * 0.5f;
                float q2cy = (c2y + ny) * 0.5f;
                out.curves.push_back(midX); out.curves.push_back(midY);
                out.curves.push_back(q2cx); out.curves.push_back(q2cy);
                out.curves.push_back(nx);   out.curves.push_back(ny);
                cx = nx; cy = ny;
                ++nCubic;
                break;
            }
        }
    }

    stbtt_FreeShape(&font, vertices);

    out.curveCount = (int)(out.curves.size() / 6);
    out.bboxMinX = x0; out.bboxMinY = y0;
    out.bboxMaxX = x1; out.bboxMaxY = y1;
    out.advanceX = advance;
    out.unitsPerEm = unitsPerEm;

    LOGI("extracted gid %d: %d curves (move=%d line=%d quad=%d cubic=%d), bbox (%d,%d)..(%d,%d), advance %d",
         glyphIndex, out.curveCount, nMove, nLine, nCurve, nCubic, x0, y0, x1, y1, advance);
    return true;
}

}  // namespace baqarah
