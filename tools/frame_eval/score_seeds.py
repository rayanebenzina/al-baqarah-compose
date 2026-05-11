#!/usr/bin/env python3
"""Score `frame-ascii` logcat dumps against the reference target grid.

Usage:
    adb logcat -d -s BaqarahVkJNI:I | python3 score_seeds.py [--top N]

Reads stdin; each block of 18 consecutive `frame-ascii |...|` lines
preceded by a header `frame-ascii seed=N slots=(...)` is one candidate
seed. For each, computes:

  - jaccard   = |A∩B| / |A∪B|     (1.0 == identical, 0 == disjoint)
  - matchPct  = sum(A==B) / total (1.0 == every cell agrees)
  - regions   = per-quadrant ink density vs target (gap = abs diff)

Prints the top N seeds ranked by jaccard.
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np

W, H = 96, 18
HERE = Path(__file__).parent

HEADER_RE = re.compile(
    r"frame-ascii seed=(\d+) slots=\(side=(\d+) kiss=(\d+) flour=(\d+) "
    r"tip=(\d+) band=(\d+) corner=(\d+)\) curves=(\d+) ink=(\d+) "
    r"bbox=\(([^)]+)\)-\(([^)]+)\) titleInteriorHits=(\d+)"
)
GRID_RE = re.compile(r"frame-ascii \|(.{96})\|")


def parse_target() -> np.ndarray:
    txt = (HERE / "target_grid.txt").read_text()
    rows = []
    for line in txt.splitlines():
        m = re.match(r"\|(.{96})\|", line)
        if m:
            rows.append([1 if c != " " else 0 for c in m.group(1)])
    assert len(rows) == H, f"target grid has {len(rows)} rows, expected {H}"
    return np.array(rows, dtype=np.uint8)


def parse_candidates(stream):
    """Yields (header_dict, grid) tuples."""
    header = None
    grid_rows = []
    for line in stream:
        line = line.rstrip("\n")
        mh = HEADER_RE.search(line)
        if mh:
            if header and len(grid_rows) == H:
                yield header, np.array(grid_rows, dtype=np.uint8)
            header = {
                "seed": int(mh.group(1)),
                "slots": tuple(int(mh.group(i)) for i in range(2, 8)),
                "curves": int(mh.group(8)),
                "ink": int(mh.group(9)),
                "bbox": (mh.group(10), mh.group(11)),
                "titleHits": int(mh.group(12)),
            }
            grid_rows = []
            continue
        mg = GRID_RE.search(line)
        if mg and header:
            grid_rows.append([1 if c != " " else 0 for c in mg.group(1)])
    if header and len(grid_rows) == H:
        yield header, np.array(grid_rows, dtype=np.uint8)


def score(candidate: np.ndarray, target: np.ndarray) -> dict:
    inter = np.logical_and(candidate, target).sum()
    union = np.logical_or(candidate, target).sum()
    jacc = inter / union if union else 0.0
    match = (candidate == target).mean()
    # Quadrant breakdown: (top-left, top-right, bottom-left, bottom-right).
    qH, qW = H // 2, W // 2
    regions = {}
    for name, (r0, r1, c0, c1) in {
        "TL": (0, qH, 0, qW),
        "TR": (0, qH, qW, W),
        "BL": (qH, H, 0, qW),
        "BR": (qH, H, qW, W),
        "CENTER": (qH - 4, qH + 4, W // 2 - 16, W // 2 + 16),
    }.items():
        cd = candidate[r0:r1, c0:c1].mean()
        td = target[r0:r1, c0:c1].mean()
        regions[name] = (cd, td, abs(cd - td))
    return {"jaccard": jacc, "matchPct": match, "regions": regions,
            "candInk": int(candidate.sum()), "targetInk": int(target.sum())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--show-best", action="store_true",
                    help="Print the best candidate grid alongside the target.")
    args = ap.parse_args()

    target = parse_target()
    results = []
    for header, grid in parse_candidates(sys.stdin):
        s = score(grid, target)
        results.append((header, grid, s))

    if not results:
        print("No frame-ascii blocks parsed from stdin.", file=sys.stderr)
        sys.exit(1)

    # Title-interior invariant is a hard requirement: any candidate
    # that bleeds ink into the title cavity is disqualified, no
    # matter how high its raw jaccard is. Otherwise the optimiser
    # rewards designs that look "dense" by spilling petals across
    # the readable text area.
    def keyfn(r):
        h, g, s = r
        violated = h["titleHits"] > 0
        return (0 if violated else 1, s["jaccard"])

    results.sort(key=keyfn, reverse=True)
    targetInk = int(target.sum())
    valid = sum(1 for h, g, s in results if h["titleHits"] == 0)
    print(f"Parsed {len(results)} candidate grids ({valid} valid, "
          f"{len(results) - valid} disqualified by titleHits>0). "
          f"Target ink: {targetInk}/{W*H}")
    print()
    print(f"{'rank':>4}  {'seed':>6}  side kiss flou tip band corn  curves  jacc  match  ink  titleHits")
    for i, (h, g, s) in enumerate(results[:args.top], 1):
        sl = h["slots"]
        flag = " *DQ" if h["titleHits"] > 0 else ""
        print(f"{i:>4}  {h['seed']:>6}  {sl[0]:>4} {sl[1]:>4} {sl[2]:>4} "
              f"{sl[3]:>3} {sl[4]:>4} {sl[5]:>4}  {h['curves']:>6}  "
              f"{s['jaccard']:.3f}  {s['matchPct']:.3f}  "
              f"{s['candInk']:>4}  {h['titleHits']:>4}{flag}")

    print()
    print("Region density (cand vs target, abs diff):")
    h, g, s = results[0]
    for k, (cd, td, d) in s["regions"].items():
        print(f"  {k:>6}: cand={cd:.2f}  target={td:.2f}  diff={d:.2f}")

    if args.show_best:
        print("\n--- Best candidate (seed=%d) ---" % results[0][0]["seed"])
        for row in results[0][1]:
            print("|" + "".join("#" if x else " " for x in row) + "|")
        print("\n--- Target ---")
        for row in target:
            print("|" + "".join("#" if x else " " for x in row) + "|")


if __name__ == "__main__":
    main()
