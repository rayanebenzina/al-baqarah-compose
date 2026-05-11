#!/usr/bin/env python3
"""Show the best seed (and its grid stats) for each cornerFill option."""
import sys
import numpy as np
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from score_seeds import parse_target, parse_candidates, score

target = parse_target()
best = {}  # corner -> (jaccard, header, score_dict)
for header, grid in parse_candidates(sys.stdin):
    s = score(grid, target)
    cn = header["slots"][5]
    if cn not in best or s["jaccard"] > best[cn][0]:
        best[cn] = (s["jaccard"], header, s)

print(f"{'corner':>6}  {'seed':>6}  side kiss flou tip band  curves  jacc  match  ink  titleHits")
for cn in sorted(best.keys()):
    j, h, s = best[cn]
    sl = h["slots"]
    print(f"{cn:>6}  {h['seed']:>6}  {sl[0]:>4} {sl[1]:>4} {sl[2]:>4} "
          f"{sl[3]:>3} {sl[4]:>4}  {h['curves']:>6}  "
          f"{j:.3f}  {s['matchPct']:.3f}  {s['candInk']:>4}  {h['titleHits']:>4}")
