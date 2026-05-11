#!/usr/bin/env python3
"""Rasterize the reference Mushaf sura-border SVG into the same
96 x 18 binary grid that the C++ `frame-ascii` debug log emits, so
candidate generator outputs can be diffed against it cheaply.

Usage:
    python3 render_target.py [INPUT.svg [OUTPUT.txt]]

Defaults: ./sura_border.svg -> ./target_grid.txt
"""
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 96, 18
HERE = Path(__file__).parent

def rasterize(svg: Path, coverage_threshold: float = 0.15) -> np.ndarray:
    # Both target and candidate represent *filled regions* (the
    # candidate's grid comes from C++ winding-number rasterisation
    # matching what Vulkan emits). Render the SVG fill at high
    # resolution, downsample with coverage averaging, threshold at
    # 40% coverage — anything denser than that is "solidly inked" in
    # the downscaled grid.
    UP_W, UP_H = W * 8, H * 8
    pgm = subprocess.check_output(
        ["magick", str(svg), "-background", "white", "-flatten",
         "-colorspace", "Gray", "-depth", "8",
         "-resize", f"{UP_W}x{UP_H}!", "PGM:-"]
    )
    img = Image.open(__import__("io").BytesIO(pgm)).convert("L")
    arr = np.asarray(img).astype(np.float32) / 255.0
    ink = 1.0 - arr
    pooled = ink.reshape(H, 8, W, 8).mean(axis=(1, 3))
    return (pooled > coverage_threshold).astype(np.uint8)

def to_ascii(bits: np.ndarray) -> str:
    return "\n".join(
        "|" + "".join("#" if x else " " for x in bits[r]) + "|"
        for r in range(H)
    )

def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "sura_border.svg"
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "target_grid.txt"
    bits = rasterize(src)
    dst.write_text(to_ascii(bits) + "\n")
    print(f"Wrote {dst} ({bits.sum()}/{bits.size} ink cells)")
    print(to_ascii(bits))

if __name__ == "__main__":
    main()
