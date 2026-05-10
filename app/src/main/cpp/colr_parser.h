#pragma once

#include <cstdint>
#include <vector>

namespace baqarah {

struct ColrLayer {
    int glyphId;
    // RGBA8 packed (alpha in high byte). 0xFFFFFFFF = white opaque.
    uint32_t rgba;
};

// Look up the COLRv0 layers for `baseGlyphId` in the given TTF. CPAL palette
// 0 is used. Returns false if the font has no COLR table or no record for
// this glyph (i.e. it's not a colour composite — render the glyph itself).
bool parseColrLayersV0(const uint8_t* ttf, int ttfSize, int baseGlyphId,
                      std::vector<ColrLayer>& layers);

}  // namespace baqarah
