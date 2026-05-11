# Procedural frame eval pipeline

Cheap iteration loop for the surah-title frame generator: no Android,
no adb, no taps. The C++ frame code lives in
`app/src/main/cpp/procedural_frame.inl` and is `#include`d by both the
Android JNI bridge and the host-side test harness, so the laptop tool
can never drift from on-device behaviour.

## Build

    make            # compiles tools/frame_eval/test_frame

## Score a single seed against the reference

    ./test_frame --seed 14400 | python3 diff_grids.py

## Sweep the full 18 000-combination design space

    ./test_frame --sweep 0 18000 > /tmp/sweep.log
    python3 score_seeds.py --top 20 < /tmp/sweep.log
    python3 per_corner_best.py < /tmp/sweep.log

A full sweep takes ~80 s on a laptop. Each grid is rasterised under
the same non-zero-winding rule Vulkan uses on-device, so the host
score predicts what the rendered frame will look like.

## Files

- `test_frame.cpp` — host harness; `#include`s the shared `.inl`.
- `Makefile` — `make test_frame`, `make sweep` (build + run sweep).
- `sura_border.svg` — reference Wikimedia border (the target shape).
- `render_target.py` — rasterises the SVG to `target_grid.txt` at
  96×18, the same resolution as the candidate grids.
- `target_grid.txt` — checked-in 96×18 target grid.
- `score_seeds.py` — parses stdin, computes Jaccard / match% / quadrant
  density vs the target. Disqualifies candidates with
  `titleInteriorHits > 0` (readability invariant).
- `per_corner_best.py` — best valid seed per `cornerFill` option.
- `diff_grids.py` — cell-by-cell diff (`#` both, `:` target only,
  `+` candidate only) to spot structural gaps.

## Adding a new ornament

1. Edit `app/src/main/cpp/procedural_frame.inl`.
2. `make` → runs in ~1 second.
3. `./test_frame --seed N | python3 diff_grids.py` for a quick read.
4. `./test_frame --sweep 0 18000 | python3 score_seeds.py` for the
   full design space.
5. Once happy, the Android library picks up the same `.inl` on the
   next `./gradlew assembleDebug`.
