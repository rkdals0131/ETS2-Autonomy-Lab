"""Summarize a brake_test.py run: deceleration per run and whether contributions add up.

Usage: py -3 analyze_brake.py bridge/recordings/brake-<time>
Per run (frames whose phase is "brake:<label>"): mean deceleration between 70 and 20
km/h from a least-squares fit of speed over simulation time, deceleration per 10 km/h
band, the share of frames with the engine brake flag and the retarder on, gears seen,
the lowest air pressure and the hottest brake. Superposition: for each combined run
("B:combo:b", "R:retarder:n+engine", "R:retarder:n+brake:b"), the extra deceleration
over the coast compared with the sum of its parts' extras, per speed band.
Aborted step attempts and frames taken with the game window in the background are left
out. Writes summary.json next to frames.jsonl.
"""
import json, sys
from collections import defaultdict
from pathlib import Path

from sysid_drive import final_attempts

BANDS = [(70, 60), (60, 50), (50, 40), (40, 30), (30, 20)]


def slope(points):
    """Least-squares d(speed)/dt in m/s^2 over (t, v) points, or None."""
    if len(points) < 10:
        return None
    n = len(points)
    mt = sum(t for t, _ in points) / n
    mv = sum(v for _, v in points) / n
    den = sum((t - mt) ** 2 for t, _ in points)
    return sum((t - mt) * (v - mv) for t, v in points) / den if den else None


def parts(label):
    """Runs whose extra decelerations should add up to this combined run's, or []."""
    if label.startswith("B:combo:"):
        return [f"B:brake:{label.split(':')[2]}", "B:engine"]
    if label.startswith("R:retarder:") and "+" in label:
        retarder, other = label.split("+")
        return [retarder, "B:engine" if other == "engine" else f"B:{other}"]
    return []


def main():
    run = Path(sys.argv[1])
    rows = defaultdict(list)
    for r in final_attempts([json.loads(line) for line in (run / "frames.jsonl").open()]):
        if r["phase"].startswith("brake:") and r["speed"] is not None:
            rows[r["phase"][6:]].append(r)
    summary = {}
    for label, frames in rows.items():
        pts = [(f["sim_us"] / 1e6, f["speed"]) for f in frames]
        x = [f.get("x") or {} for f in frames]
        def band(hi, lo):
            sel = [(t, v) for t, v in pts if lo / 3.6 <= v <= hi / 3.6]
            a = slope(sel)
            return None if a is None else round(-a, 3)
        summary[label] = {
            "decel_70_20": band(70, 20),
            "bands": {f"{hi}-{lo}": band(hi, lo) for hi, lo in BANDS},
            "engine_brake_share": round(sum(bool(v.get("truck.brake.motor")) for v in x) / max(1, len(x)), 3),
            "retarder_share": round(sum(bool(v.get("truck.brake.retarder")) for v in x) / max(1, len(x)), 3),
            "retarder_max": max((v.get("truck.brake.retarder") or 0 for v in x), default=0),
            "gears": sorted({f["gear"] for f in frames if f["gear"] is not None}),
            "min_air_pressure": min((v.get("truck.brake.air.pressure") for v in x
                                     if v.get("truck.brake.air.pressure") is not None), default=None),
            "max_brake_temperature": max((v.get("truck.brake.temperature") for v in x
                                          if v.get("truck.brake.temperature") is not None), default=None),
            "effective_brake": round(sum(f["eff_brk"] or 0 for f in frames) / len(frames), 3),
            "frames": len(frames)}
    superposition = {}
    coast = summary.get("B:coast")
    for label, s in summary.items():
        components = [summary.get(c) for c in parts(label)]
        if not coast or not components or None in components:
            continue
        per_band = {}
        for key in ["decel_70_20"] + [f"bands.{hi}-{lo}" for hi, lo in BANDS]:
            get = (lambda d: d["decel_70_20"]) if key == "decel_70_20" else (lambda d, k=key[6:]: d["bands"][k])
            values = [get(d) for d in [s, coast] + components]
            if None in values:
                continue
            combo, c0, *alone = values
            predicted = sum(a - c0 for a in alone)
            per_band[key] = {"measured_extra": round(combo - c0, 3), "sum_of_extras": round(predicted, 3),
                             "error": round(combo - c0 - predicted, 3)}
        superposition[label] = per_band
    out = {"runs": summary, "superposition": superposition}
    (run / "summary.json").write_text(json.dumps(out, indent=1))
    print(f"{'run':26} {'70-20':>6} " + " ".join(f"{hi}-{lo:<3}" for hi, lo in BANDS) + "  EB-share R-share R-max T-max gears  air_min")
    for label in sorted(summary):
        s = summary[label]
        fmt = lambda v: f"{v:6.2f}" if v is not None else "     -"
        print(f"{label:26} {fmt(s['decel_70_20'])} " + " ".join(fmt(s['bands'][f'{hi}-{lo}']) for hi, lo in BANDS)
              + f"  {s['engine_brake_share']:6.2f}  {s['retarder_share']:6.2f}  {s['retarder_max']}  "
              + f"{s['max_brake_temperature'] or 0:5.0f}  {s['gears']}  {s['min_air_pressure'] or 0:.0f}")
    for label, per_band in superposition.items():
        print(f"superposition {label} = " + " + ".join(parts(label)) + ":")
        for k, v in per_band.items():
            print(f"  {k:12} measured extra {v['measured_extra']:6.2f}  sum {v['sum_of_extras']:6.2f}  error {v['error']:+.2f} m/s^2")


if __name__ == "__main__":
    main()
