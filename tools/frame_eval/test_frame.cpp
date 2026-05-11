// Host-side test harness for the procedural frame generator. Compiles
// the same `procedural_frame.inl` that ships in the Android library
// (so it can never drift from on-device behaviour) but with printf-
// based logging and a no-op `appendLayerBands` stub. Lets us sweep
// thousands of seeds and pipe the ASCII output through
// `score_seeds.py` on the laptop — no adb, no APK install, no taps.
//
// Build:
//   g++ -O2 -std=c++17 -I. test_frame.cpp -o test_frame
// Or:
//   make -C tools/frame_eval test_frame
//
// Usage:
//   ./test_frame --seed 0
//   ./test_frame --sweep START COUNT [STRIDE]
//   ./test_frame --grid 1080 208 --sweep 0 18000 | python3 score_seeds.py --top 20

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

#define LOGI(fmt, ...) do { std::printf(fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) do { std::fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)

// The .inl calls `__system_property_get` to decide whether to dump
// ASCII. On the host we always want ASCII — return "1".
static int __system_property_get(const char* /*name*/, char* value) {
    value[0] = '1';
    value[1] = '\0';
    return 1;
}

// No-op stub: the renderer's band index is GPU-only; the ASCII raster
// walks `allCurves` directly, so we don't need a real implementation.
static void appendLayerBands(const std::vector<float>& /*allCurves*/,
                              int /*curveStart*/, int /*curveCount*/,
                              std::vector<int>& /*bandsArr*/,
                              std::vector<int>& /*curveIndicesArr*/) {}

#include "../../app/src/main/cpp/procedural_frame.inl"

static void emitOne(int seed, float dstW, float dstH) {
    std::vector<float> allCurves, layerData, layerRects;
    std::vector<int>   bandsArr, curveIndicesArr;
    int totalCurves = 0, totalLayers = 0;
    emitFrame(0.0f, 0.0f, dstW, dstH, 0xFF281E14u,
              allCurves, layerData, layerRects,
              bandsArr, curveIndicesArr,
              totalCurves, totalLayers, seed);
}

int main(int argc, char** argv) {
    float dstW = 1080.0f, dstH = 208.0f;  // matches the real surah-mode frame
    int seedStart = 0, seedCount = 1, stride = 1;
    bool sweep = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seedStart = std::atoi(argv[++i]);
            seedCount = 1; stride = 1; sweep = false;
        } else if (std::strcmp(argv[i], "--sweep") == 0 && i + 2 < argc) {
            seedStart = std::atoi(argv[++i]);
            seedCount = std::atoi(argv[++i]);
            sweep = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                stride = std::atoi(argv[++i]);
            }
        } else if (std::strcmp(argv[i], "--grid") == 0 && i + 2 < argc) {
            dstW = (float)std::atof(argv[++i]);
            dstH = (float)std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr,
                "Usage: %s [--seed N | --sweep START COUNT [STRIDE]] "
                "[--grid dstW dstH]\n", argv[0]);
            return 0;
        }
    }
    if (sweep) {
        for (int s = 0; s < seedCount; ++s) {
            emitOne(seedStart + s * stride, dstW, dstH);
        }
    } else {
        emitOne(seedStart, dstW, dstH);
    }
    return 0;
}
