"""Engine and retarder maps from a gear_test.py run (needs the "dt" drivetrain log).

Usage: py -3 analyze_gear.py bridge/recordings/gear-<time>
Per phase kind (eb, full, coast, half, ret) and gear: median actual engine torque and
transmission output torque in 100 rpm bins, with the def full-load curve beside the full
throttle bins. For each full and half throttle step: the time from the throttle command
to 63 % and 90 % of the torque the step reaches (engine torque response). For the
retarder: torque at the output shaft beyond the engine's share (output torque minus
engine torque x gear ratio x 0.905) by output shaft speed. Writes summary.json.
"""
import json, sys
from collections import defaultdict
from pathlib import Path

import numpy as np

from sysid_drive import final_attempts

TORQUE_NM = 2700.0
CURVE = [(300, .1), (440, .4), (1000, 1), (1300, 1), (1600, .82), (1900, .72), (2300, .4), (2500, .1)]
EFFICIENCY = 0.905  # output / (engine x ratio) when coasting, docs/30


def main():
    run = Path(sys.argv[1])
    rows = [r for r in final_attempts([json.loads(l) for l in (run / "frames.jsonl").open()]) if "dt" in r]
    phases = defaultdict(list)
    for r in rows:
        kind = r["phase"].split(":")[0]
        if kind in ("eb", "full", "coast", "half", "ret"):
            phases[r["phase"]].append(r)
    summary = {"maps": {}, "response": [], "retarder": {}}
    for name, g in sorted(phases.items(), key=lambda kv: (kv[0].split(":")[0], int(kv[0].split(":")[1]))):
        kind, gear = name.split(":")
        rpm = np.array([r["dt"]["engine_speed_radps"] * 60 / (2 * np.pi) for r in g])
        eng = np.array([r["dt"]["engine_torque_nm"] for r in g])
        out = np.array([r["dt"]["output_torque_nm"] for r in g])
        ratio = np.array([r["dt"]["gear_ratio"] for r in g])
        bins = {}
        for lo in range(800, 2400, 100):
            m = (rpm >= lo) & (rpm < lo + 100) & (ratio > 0.5)
            if m.sum() >= 5:
                bins[lo] = {"engine_nm": round(float(np.median(eng[m])), 1), "output_nm": round(float(np.median(out[m])), 1),
                            "n": int(m.sum())}
                if kind == "full":
                    bins[lo]["def_nm"] = round(TORQUE_NM * float(np.interp(lo + 50, *zip(*CURVE))), 1)
        summary["maps"][name] = bins
        print(f"== {name}  ({len(g)} frames, rpm {rpm.min():.0f}-{rpm.max():.0f})")
        for lo, b in bins.items():
            extra = f"  def {b['def_nm']:7.0f}" if "def_nm" in b else ""
            print(f"   {lo:4d}-{lo + 100:<4d} engine {b['engine_nm']:8.0f}  output {b['output_nm']:8.0f}{extra}  n={b['n']}")
        if kind in ("full", "half"):
            t = np.array([r["sim_us"] for r in g]) / 1e6
            t -= t[0]
            final = np.median(eng[-max(5, len(eng) // 5):])
            start = eng[0]
            rise = lambda f: next((t[i] for i in range(len(t)) if (eng[i] - start) >= f * (final - start)), None)
            item = {"phase": name, "from_nm": round(float(start)), "to_nm": round(float(final)),
                    "t63_s": rise(0.63), "t90_s": rise(0.9)}
            summary["response"].append(item)
            print(f"   response {item}")
        if kind == "ret":
            w = np.array([r["dt"]["output_shaft_speed_radps"] for r in g])
            rt = out - eng * ratio * EFFICIENCY
            ret = {}
            for lo in range(0, 140, 20):
                m = (w >= lo) & (w < lo + 20) & (ratio > 0.5)
                if m.sum() >= 5:
                    ret[lo] = round(float(np.median(rt[m])), 1)
            summary["retarder"][name] = ret
            print(f"   retarder torque at the output shaft by output speed (rad/s): {ret}")
    (run / "summary.json").write_text(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
