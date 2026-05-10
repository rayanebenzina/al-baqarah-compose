#pragma once

#include <cstdint>
#include <vector>

namespace baqarah {

// Outline of a single glyph as a flat array of quadratic Beziers
// (3 vec2 per curve, normalized to [0, 1]^2 with the glyph's bounding
// box mapped onto the unit square). `advanceFrac` and `originXFrac` /
// `originYFrac` describe the glyph's metrics in the same normalized
// space so the layouter can position quads correctly.
struct GlyphOutline {
    std::vector<float> curves;  // 6 floats per curve: p0.xy, p1.xy, p2.xy
    int curveCount = 0;
    // Bounding box in font units (no scaling applied)
    int bboxMinX = 0;
    int bboxMinY = 0;
    int bboxMaxX = 0;
    int bboxMaxY = 0;
    int advanceX = 0;  // horizontal advance in font units
    int unitsPerEm = 1024;  // font's em size, for scaling
};

// Extract a single glyph's outline. `ttfData` points at the entire TTF
// file. `codepoint` is the Unicode codepoint to look up via cmap.
// Returns false if the glyph isn't found or has no outline.
bool extractGlyphOutline(const uint8_t* ttfData, int ttfSize,
                         int codepoint, GlyphOutline& out);

// Same, but takes a font-internal glyph index instead of a codepoint.
// Used for COLR layer glyphs which aren't cmap-mapped.
bool extractGlyphOutlineByIndex(const uint8_t* ttfData, int ttfSize,
                                int glyphIndex, GlyphOutline& out);

// Look up the cmap glyph index for a codepoint without extracting the
// outline. Returns 0 if not present.
int findGlyphIndex(const uint8_t* ttfData, int ttfSize, int codepoint);

}  // namespace baqarah
