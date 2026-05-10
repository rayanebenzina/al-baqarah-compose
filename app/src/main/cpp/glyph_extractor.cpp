#include "glyph_extractor.h"

#include <android/log.h>

#include <algorithm>

#include "third_party/stb_truetype.h"

#define LOG_TAG "BaqarahGlyph"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace baqarah {

bool extractGlyphOutline(const uint8_t* ttfData, int ttfSize,
                         int codepoint, GlyphOutline& out) {
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

    int glyphIndex = stbtt_FindGlyphIndex(&font, codepoint);
    if (glyphIndex == 0) {
        LOGE("no glyph for codepoint U+%04X", codepoint);
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

    // unitsPerEm from head table — stbtt doesn't expose directly, but
    // ascent - descent is typically close to the em size for most fonts.
    // QPC v4 fonts use 2048 units/em; we'll detect via the head table
    // version stbtt parses indirectly.
    int unitsPerEm = ascent - descent;  // approximate
    if (unitsPerEm <= 0) unitsPerEm = 1024;

    // Extract glyph shape: a list of stbtt_vertex describing the outline.
    // Each vertex is either a vmove, vline, vcurve (quadratic) or vcubic.
    stbtt_vertex* vertices = nullptr;
    int vertexCount = stbtt_GetGlyphShape(&font, glyphIndex, &vertices);
    if (vertexCount == 0 || vertices == nullptr) {
        LOGE("no outline for glyph %d", glyphIndex);
        if (vertices) stbtt_FreeShape(&font, vertices);
        return false;
    }

    // Walk vertices and emit quadratic-Bezier curves. TTF outlines are
    // natively quadratic. Straight lines become degenerate quadratics
    // with the control point at the segment midpoint.
    out.curves.clear();
    out.curves.reserve((size_t)vertexCount * 6);

    float cx = 0.0f, cy = 0.0f;  // current pen position
    for (int i = 0; i < vertexCount; ++i) {
        const stbtt_vertex& v = vertices[i];
        switch (v.type) {
            case STBTT_vmove:
                cx = (float)v.x;
                cy = (float)v.y;
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
                break;
            }
            case STBTT_vcubic: {
                // Cubic Bezier — approximate as two quadratics. For QPC v4
                // (TrueType native), cubics only appear via stbtt's
                // emission for OpenType OTF — should be rare. Use a simple
                // midpoint split for now.
                float nx = (float)v.x;
                float ny = (float)v.y;
                float c1x = (float)v.cx;
                float c1y = (float)v.cy;
                float c2x = (float)v.cx1;
                float c2y = (float)v.cy1;
                // Midpoint of cubic: easy quadratic approximation
                float qcx = (c1x + c2x) * 0.5f;
                float qcy = (c1y + c2y) * 0.5f;
                out.curves.push_back(cx);  out.curves.push_back(cy);
                out.curves.push_back(qcx); out.curves.push_back(qcy);
                out.curves.push_back(nx);  out.curves.push_back(ny);
                cx = nx; cy = ny;
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

    LOGI("extracted glyph U+%04X: %d curves, bbox (%d,%d)..(%d,%d), advance %d, unitsPerEm %d",
         codepoint, out.curveCount, x0, y0, x1, y1, advance, unitsPerEm);
    return true;
}

}  // namespace baqarah
