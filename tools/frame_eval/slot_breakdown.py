#!/usr/bin/env python3
"""Per-slot, per-value breakdown of the sweep results.

For each of the six factorised slots in the procedural frame
(`side`, `kiss`, `flour`, `tip`, `band`, `corner`) and each value that
slot can take, show the **best valid** (titleHits=0) seed that uses
that value, alongside the values the other slots took to reach that
best score. Reveals which ornament options actively contribute and
which are dead weight.

Usage:
    ./test_frame --sweep 0 18000 | python3 slot_breakdown.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from score_seeds import parse_target, parse_candidates, score

SLOTS = ["side", "kiss", "flour", "tip", "band", "corner"]


def main():
    target = parse_target()
    # per_slot_value: slots[slot_idx][value] -> best (jaccard, header)
    per = [{} for _ in SLOTS]
    valid_total = 0
    for header, grid in parse_candidates(sys.stdin):
        if header["titleHits"] > 0:
            continue
        valid_total += 1
        s = score(grid, target)
        for i, v in enumerate(header["slots"]):
            cell = per[i].setdefault(v, (-1.0, None, None))
            if s["jaccard"] > cell[0]:
                per[i][v] = (s["jaccard"], header, s)

    print(f"Scored {valid_total} valid candidates "
          f"(target ink = {int(target.sum())}/{target.size}).")
    for i, slot in enumerate(SLOTS):
        print(f"\nslot={slot}")
        for v in sorted(per[i].keys()):
            j, h, s = per[i][v]
            sl = h["slots"]
            others = ", ".join(f"{name}={val}" for name, val in zip(SLOTS, sl)
                                if name != slot)
            print(f"  v={v}  seed={h['seed']:>5}  jacc={j:.3f}  "
                  f"ink={s['candInk']:>4}  others=({others})")


if __name__ == "__main__":
    main()
