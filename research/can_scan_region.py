"""List every varying float slot of one scanned region with its range and correlations.

Usage: py -3 research/can_scan_region.py research/live/<dir>/scan.npz "vehicle+0x150" [more paths]
"""
import json, sys
from pathlib import Path
import numpy as np

d = np.load(sys.argv[1])
regions = json.loads((Path(sys.argv[1]).parent / "regions.json").read_text())
starts, acc = {}, 0
for r in regions:
    starts[r["path"]] = (acc, r["size"]); acc += r["size"]
n = float(d["n"]); targets = list(d["targets"])
mx, my = d["s_x"] / n, d["s_y"] / n
vx, vy = d["s_xx"] / n - mx ** 2, d["s_yy"] / n - my ** 2
cov = d["s_xy"] / n - np.outer(mx, my)
r = cov / np.sqrt(np.maximum(vx, 1e-30)[:, None] * np.maximum(vy, 1e-30)[None, :])
for path in sys.argv[2:]:
    start, size = starts[path]
    print(f"== {path} ({size:#x} bytes)")
    for i in range(start // 4, (start + size) // 4):
        if d["bad"][i] or vx[i] <= 1e-12:
            continue
        best = sorted(range(len(targets)), key=lambda k: -abs(r[i, k]))[:3]
        print(f"  +{i * 4 - start:#05x}  {d['lo'][i]:+11.4g} .. {d['hi'][i]:+11.4g}  mean {mx[i]:+10.4g}  "
              + "  ".join(f"{targets[k]}={r[i, k]:+.2f}" for k in best))
