#include "sdf_gen.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace baqarah {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Felzenszwalb & Huttenlocher 2012 — 1D Euclidean distance transform.
// f[0..n) is the input. d[0..n) receives the squared distance to the
// nearest source location.
void edt1d(const float* f, int n, float* d, int* v, float* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float fq = f[q];
        if (fq == kInf) {
            // Skip — no parabola contributed from here.
            // Loop body below handles this implicitly via the divide check.
        }
        float s;
        while (true) {
            int vk = v[k];
            float denom = 2.0f * (float)(q - vk);
            if (denom == 0.0f) {
                s = -kInf;
            } else {
                s = ((fq + (float)(q * q)) - (f[vk] + (float)(vk * vk))) / denom;
            }
            if (s > z[k]) break;
            --k;
            if (k < 0) {
                k = 0;
                break;
            }
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < (float)q) ++k;
        int dq = q - v[k];
        d[q] = (float)(dq * dq) + f[v[k]];
    }
}

void edt2d(const float* f, int w, int h, float* d) {
    const int maxDim = std::max(w, h);
    std::vector<int> v(maxDim);
    std::vector<float> z(maxDim + 1);
    std::vector<float> col_in(h);
    std::vector<float> col_out(h);

    // Row pass
    for (int y = 0; y < h; ++y) {
        edt1d(&f[y * w], w, &d[y * w], v.data(), z.data());
    }
    // Column pass — input is the row-pass result.
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) col_in[y] = d[y * w + x];
        edt1d(col_in.data(), h, col_out.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y) d[y * w + x] = col_out[y];
    }
}

}  // namespace

void computeSdf(const uint8_t* alpha, int w, int h, int spread,
                uint8_t threshold, uint8_t* out) {
    const int n = w * h;
    std::vector<float> f_outside(n);
    std::vector<float> f_inside(n);
    for (int i = 0; i < n; ++i) {
        bool inside = alpha[i] >= threshold;
        f_outside[i] = inside ? kInf : 0.0f;
        f_inside[i] = inside ? 0.0f : kInf;
    }

    std::vector<float> dist_outside(n);
    std::vector<float> dist_inside(n);
    edt2d(f_outside.data(), w, h, dist_outside.data());
    edt2d(f_inside.data(), w, h, dist_inside.data());

    const float invSpread = 1.0f / (float)spread;
    for (int i = 0; i < n; ++i) {
        float signed_dist;
        if (alpha[i] >= threshold) {
            // Inside: distance to nearest outside pixel, positive
            signed_dist = std::sqrt(dist_outside[i]);
        } else {
            // Outside: distance to nearest inside pixel, negated
            signed_dist = -std::sqrt(dist_inside[i]);
        }
        float n01 = 0.5f + (signed_dist * invSpread) * 0.5f;
        n01 = std::max(0.0f, std::min(1.0f, n01));
        out[i] = (uint8_t)(n01 * 255.0f + 0.5f);
    }
}

}  // namespace baqarah
