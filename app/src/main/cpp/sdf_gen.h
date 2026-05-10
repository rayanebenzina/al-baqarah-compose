#pragma once

#include <cstdint>

namespace baqarah {

// Compute a single-channel signed distance field from a binary alpha mask.
//
// `alpha` is a `w*h` row-major byte array; values >= `threshold` count as
// inside the shape, otherwise outside. The SDF is encoded into `out` (same
// size as the input) such that 0x80 represents the shape boundary, values
// above 0x80 are inside (up to 0xFF at `+spread` pixels inward), and values
// below 0x80 are outside (down to 0x00 at `-spread` pixels outward).
//
// Distances are computed with a 2-pass O(N) Felzenszwalb–Huttenlocher
// Euclidean distance transform, run separately on the inside and outside
// pixels and then combined.
void computeSdf(const uint8_t* alpha, int w, int h, int spread,
                uint8_t threshold, uint8_t* out);

}  // namespace baqarah
