#include "colr_parser.h"

#include <android/log.h>

#include <cstring>

#define LOG_TAG "BaqarahColr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace baqarah {
namespace {

inline uint16_t rd_u16(const uint8_t* p, size_t off) {
    return ((uint16_t)p[off] << 8) | (uint16_t)p[off + 1];
}
inline uint32_t rd_u32(const uint8_t* p, size_t off) {
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
           ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
}

// Look up a TTF top-level table by 4-byte tag. Returns absolute offset
// into `ttf` or 0 if not found.
uint32_t findTable(const uint8_t* ttf, int ttfSize, const char tag[4]) {
    if (ttfSize < 12) return 0;
    uint16_t numTables = rd_u16(ttf, 4);
    const int recBase = 12;
    if (ttfSize < recBase + numTables * 16) return 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t* rec = ttf + recBase + i * 16;
        if (rec[0] == (uint8_t)tag[0] && rec[1] == (uint8_t)tag[1] &&
            rec[2] == (uint8_t)tag[2] && rec[3] == (uint8_t)tag[3]) {
            return rd_u32(rec, 8);
        }
    }
    return 0;
}

}  // namespace

bool parseColrLayersV0(const uint8_t* ttf, int ttfSize, int baseGlyphId,
                      std::vector<ColrLayer>& layers) {
    layers.clear();

    const uint32_t colrOff = findTable(ttf, ttfSize, "COLR");
    if (colrOff == 0) return false;
    if ((int)colrOff + 14 > ttfSize) return false;

    const uint16_t version = rd_u16(ttf, colrOff + 0);
    if (version != 0) {
        LOGE("COLR v%u not supported (only v0)", (unsigned)version);
        return false;
    }
    const uint16_t numBaseRecs = rd_u16(ttf, colrOff + 2);
    const uint32_t baseRecsOff = colrOff + rd_u32(ttf, colrOff + 4);
    const uint32_t layerRecsOff = colrOff + rd_u32(ttf, colrOff + 8);
    const uint16_t numLayerRecs = rd_u16(ttf, colrOff + 12);

    // Binary search baseGlyphRecords (sorted by glyphID, 6 bytes each).
    int lo = 0, hi = (int)numBaseRecs - 1;
    int idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t gid = rd_u16(ttf, baseRecsOff + (size_t)mid * 6);
        if (gid == baseGlyphId) { idx = mid; break; }
        if (gid < baseGlyphId) lo = mid + 1;
        else                   hi = mid - 1;
    }
    if (idx < 0) return false;  // no COLR composite for this glyph

    const uint16_t firstLayer = rd_u16(ttf, baseRecsOff + (size_t)idx * 6 + 2);
    const uint16_t numLayers = rd_u16(ttf, baseRecsOff + (size_t)idx * 6 + 4);
    if ((uint32_t)firstLayer + numLayers > numLayerRecs) {
        LOGE("COLR layer range OOB: first=%u num=%u total=%u",
             (unsigned)firstLayer, (unsigned)numLayers, (unsigned)numLayerRecs);
        return false;
    }

    // Locate CPAL palette 0 for colour lookup.
    const uint32_t cpalOff = findTable(ttf, ttfSize, "CPAL");
    if (cpalOff == 0) {
        LOGE("CPAL not found");
        return false;
    }
    const uint16_t cpalVersion = rd_u16(ttf, cpalOff + 0);
    (void)cpalVersion;
    const uint16_t numPaletteEntries = rd_u16(ttf, cpalOff + 2);
    const uint16_t numPalettes = rd_u16(ttf, cpalOff + 4);
    const uint32_t colorRecsOff = cpalOff + rd_u32(ttf, cpalOff + 8);
    const uint16_t pal0Start = rd_u16(ttf, cpalOff + 12 + 0 * 2);
    (void)numPaletteEntries; (void)numPalettes;

    layers.reserve(numLayers);
    for (uint16_t i = 0; i < numLayers; ++i) {
        const uint32_t recOff = layerRecsOff + (uint32_t)(firstLayer + i) * 4;
        const uint16_t layerGid = rd_u16(ttf, recOff + 0);
        const uint16_t palIdx = rd_u16(ttf, recOff + 2);

        uint32_t rgba;
        // QPC v4 CPAL convention: palette index 0 is the "main text colour"
        // (black for light backgrounds). 0xFFFF is the spec-defined
        // foreground placeholder. Both get substituted with the app's text
        // colour — dark brown, for our cream Mushaf reader. Other palette
        // indices are tajweed-rule colours and pass through unchanged.
        if (palIdx == 0 || palIdx == 0xFFFF) {
            rgba = 0xFF281E14u;  // ARGB: dark warm brown (40, 30, 20)
        } else {
            const uint32_t cidx = (uint32_t)pal0Start + (uint32_t)palIdx;
            const uint32_t cOff = colorRecsOff + cidx * 4;
            const uint8_t b = ttf[cOff + 0];
            const uint8_t g = ttf[cOff + 1];
            const uint8_t r = ttf[cOff + 2];
            const uint8_t a = ttf[cOff + 3];
            rgba = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | (uint32_t)b;
        }
        layers.push_back({(int)layerGid, rgba});
    }
    LOGI("COLR for base gid %d: %u layers", baseGlyphId, (unsigned)numLayers);
    return true;
}

}  // namespace baqarah
