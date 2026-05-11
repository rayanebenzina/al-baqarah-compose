#!/usr/bin/env python3
"""Cell-by-cell diff between a candidate and the target grid.
Print three side-by-side grids and a legend so structural gaps jump out.

Usage:
    grep -A 19 "seed=11554 slots" /tmp/sweep.log | python3 diff_grids.py
    ./test_frame --seed 11554 | python3 diff_grids.py
"""
import re
import sys
from pathlib import Path

W, H = 96, 18
HERE = Path(__file__).parent

GRID_RE = re.compile(r"frame-ascii \|(.{96})\|")
TARGET_RE = re.compile(r"\|(.{96})\|")


def parse_grid_from_stream(stream):
    rows = []
    for line in stream:
        m = GRID_RE.search(line)
        if m:
            rows.append([1 if c != " " else 0 for c in m.group(1)])
    return rows


def parse_target():
    rows = []
    for line in (HERE / "target_grid.txt").read_text().splitlines():
        m = TARGET_RE.match(line)
        if m:
            rows.append([1 if c != " " else 0 for c in m.group(1)])
    return rows


def render(cand, target):
    # 4 states per cell:
    #   '#' both filled
    #   ':' only target (we MISSED — structural under-fill, the gap to close)
    #   '+' only candidate (over-fill, ornament hitting outside target)
    #   ' ' both empty
    out = []
    misses = excess = matches = 0
    for r in range(H):
        line = []
        for c in range(W):
            t = target[r][c]
            v = cand[r][c]
            if t and v:
                line.append("#"); matches += 1
            elif t and not v:
                line.append(":"); misses += 1
            elif not t and v:
                line.append("+"); excess += 1
            else:
                line.append(" ")
        out.append("".join(line))
    return out, matches, misses, excess


def main():
    cand = parse_grid_from_stream(sys.stdin)
    if len(cand) != H:
        print(f"ERROR: expected {H} grid rows, got {len(cand)}", file=sys.stderr)
        sys.exit(1)
    target = parse_target()

    diff, matches, misses, excess = render(cand, target)
    total = H * W
    print(f"target_ink={sum(sum(r) for r in target)}  "
          f"cand_ink={sum(sum(r) for r in cand)}  "
          f"matches={matches}  misses={misses}  excess={excess}  "
          f"jaccard={matches/(matches+misses+excess):.3f}")
    print()
    print("Legend: '#' both, ':' target only (MISSED), '+' candidate only (EXCESS), ' ' empty")
    print()
    print("DIFF:")
    for line in diff:
        print(f"|{line}|")
    print()
    print("CANDIDATE:")
    for r in range(H):
        print("|" + "".join("#" if v else " " for v in cand[r]) + "|")
    print()
    print("TARGET:")
    for r in range(H):
        print("|" + "".join("#" if v else " " for v in target[r]) + "|")


if __name__ == "__main__":
    main()
