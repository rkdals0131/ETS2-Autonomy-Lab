"""Summarize a sysid_drive.py run: steering plant, yaw response and longitudinal steps.

Usage: py -3 analyze_sysid.py bridge/recordings/sysid-<time>
- Steering plant per speed band: least squares of d(eff)/dt = a*u(t-d) - c*eff over
  frame delays d = 0..3, keeping the best fit.
- Yaw: SDK angular velocity is in turns/s (checked against the heading rate). For the
  sines, gain and phase of yaw rate over the mean front wheel angle against the
  kinematic v/L; for the steady turns, the wheelbase that makes v*delta/r match
  (L_eff) and the understeer gradient against L. L is the harness's kinematic
  wheelbase (front axle to the weighted rear reference, sysid_drive.Geometry), from
  truck_config.json (written by the run) and the lift-axle contact channel; without
  them the Scania S 4x2's 3.734 m.
- Longitudinal: coast-down, brake steps (deceleration, time to the first -0.3 m/s),
  throttle steps (time to 20/40/60 km/h).
Frames of aborted step attempts and frames taken while the game window was in the
background are left out. Writes summary.json next to frames.jsonl.
"""
import json, math, sys
from collections import OrderedDict
from pathlib import Path

import numpy as np

from sysid_drive import Geometry, final_attempts, lift_down

BANDS = ((4, 7, 20), (10, 12.5, 40), (15.5, 18, 60), (21, 23, 80), (24, 26, 90), (26.8, 29, 100))


def main():
    run = Path(sys.argv[1])
    rows = final_attempts([json.loads(line) for line in (run / "frames.jsonl").open()])
    rows = [r for r in rows if r["speed"] is not None and r["eff_steer"] is not None and r["v_ang"] is not None]
    config = run / "truck_config.json"
    geometry = Geometry(json.loads(config.read_text())) if config.exists() else None
    wheelbase = lambda r: geometry.rear_z(bool(lift_down(r))) - geometry.front if geometry else 3.734
    out = {"frames": len(rows), "plant": {}, "sines": [], "turns": [], "longitudinal": []}

    t = (np.array([r["sim_us"] for r in rows]) - rows[0]["sim_us"]) / 1e6
    e = np.array([r["eff_steer"] for r in rows])
    u = np.array([r["cmd_steer"] for r in rows])
    v = np.array([r["speed"] for r in rows])
    de, dt = np.diff(e), np.diff(t)
    ok = dt > 0.005
    print("== steering plant d(eff)/dt = a*u(t-d) - c*eff")
    for lo, hi, kmh in BANDS:
        best = None
        for d in range(4):
            idx = np.arange(d, len(de))
            m = ok[idx] & (v[idx] > lo) & (v[idx] < hi)
            if m.sum() < 100:
                continue
            y = de[idx][m] / dt[idx][m]
            X = np.column_stack([u[idx - d][m], -e[idx][m]])
            coef = np.linalg.lstsq(X, y, rcond=None)[0]
            r2 = 1 - np.sum((X @ coef - y) ** 2) / np.sum((y - y.mean()) ** 2)
            if best is None or r2 > best["r2"]:
                best = {"delay_frames": d, "a": round(coef[0], 4), "c": round(coef[1], 4), "r2": round(r2, 4), "n": int(m.sum())}
        if best:
            out["plant"][kmh] = best
            print(f"  ~{kmh:3d} km/h: delay {best['delay_frames']}  a={best['a']:.3f}  c={best['c']:.3f}  R2={best['r2']:.3f}  n={best['n']}")

    phases = OrderedDict()
    for r in rows:
        phases.setdefault(r["phase"], []).append(r)
    tt = lambda g: (np.array([x["sim_us"] for x in g]) - g[0]["sim_us"]) / 1e6
    wheel = lambda g: np.array([(x["w0"] + x["w1"]) / 2 * 2 * math.pi for x in g])
    yaw = lambda g: np.array([x["v_ang"][1] for x in g]) * 2 * math.pi
    speed = lambda g: np.array([x["speed"] for x in g])
    length = lambda g: float(np.mean([wheelbase(x) for x in g]))

    placed = [r for r in rows if r["rot"]]
    h = np.unwrap(np.array([r["rot"][0] * 2 * math.pi for r in placed]))
    rate = np.gradient(h, tt(placed))
    raw = np.array([r["v_ang"][1] for r in placed])
    sel = np.abs(rate) > 0.05
    if sel.sum() > 20:
        out["angular_velocity_scale"] = round(float(np.median(rate[sel] / raw[sel])), 3)
        print(f"\nheading rate / v_ang[1] = {out['angular_velocity_scale']} (2*pi: turns/s)")

    print("\n== sines: yaw rate / wheel angle against v/L")
    for name, g in phases.items():
        if not name.startswith("sine"):
            continue
        _, kmh, f, _ = name.split(":")
        f, x = float(f), tt(g)
        X = np.column_stack([np.sin(2 * math.pi * f * x), np.cos(2 * math.pi * f * x), np.ones_like(x), x])
        cd, cr = np.linalg.lstsq(X, wheel(g), rcond=None)[0], np.linalg.lstsq(X, yaw(g), rcond=None)[0]
        gain = math.hypot(cr[0], cr[1]) / math.hypot(cd[0], cd[1])
        phase = math.degrees(math.remainder(math.atan2(cr[1], cr[0]) - math.atan2(cd[1], cd[0]), 2 * math.pi))
        vm, L = float(np.mean(speed(g))), length(g)
        item = {"phase": name, "kmh": round(vm * 3.6, 1), "hz": f, "L": round(L, 3), "gain": round(gain, 4),
                "kinematic": round(vm / L, 4), "ratio": round(gain / (vm / L), 3), "phase_deg": round(phase, 1),
                "L_eff": round(vm / gain, 3)}
        out["sines"].append(item)
        print(f"  {name:24s} v={item['kmh']:5.1f}  L={L:.3f}  |r/delta|/(v/L)={item['ratio']:.3f}  "
              f"phase={item['phase_deg']:+5.1f} deg  L_eff={item['L_eff']:.3f}")

    print("\n== steady turns")
    for name, g in phases.items():
        if not name.startswith("turn"):
            continue
        g = g[len(g) // 2:]
        vm, r, d, L = float(np.mean(speed(g))), float(np.mean(yaw(g))), float(np.mean(wheel(g))), length(g)
        ay = vm * r
        item = {"phase": name, "kmh": round(vm * 3.6, 1), "L": round(L, 3), "delta": round(d, 5), "ay": round(ay, 3),
                "L_eff": round(vm * d / r, 3) if abs(r) > 1e-4 else None,
                "understeer": round((d - L * r / vm) / ay, 5) if abs(ay) > 0.2 else None}
        out["turns"].append(item)
        print(f"  {name:20s} v={item['kmh']:5.1f}  delta={d:+.4f}  ay={ay:+.2f}  L={L:.3f}  L_eff={item['L_eff']}  "
              f"K={item['understeer']} rad/(m/s^2)")

    print("\n== longitudinal")
    for name, g in phases.items():
        x, s = tt(g), speed(g)
        if name.startswith("brake_step"):
            moving = [i for i in range(len(g)) if s[i] > 1.0]
            if len(moving) < 10:
                continue
            k = len(moving) // 5
            a = np.polyfit(x[moving][k:], s[moving][k:], 1)[0]
            lag = next((x[i] for i in moving if s[0] - s[i] > 0.3), None)
            item = {"phase": name, "from_kmh": round(s[0] * 3.6, 1), "decel": round(-a, 3), "lag_s": lag and round(lag, 3)}
        elif name.startswith("throttle_step"):
            item = {"phase": name}
            for target in (20, 40, 60):
                k = next((i for i in range(len(g)) if s[i] * 3.6 >= target), None)
                item[f"to_{target}"] = round(float(x[k]), 2) if k is not None else None
        elif name == "coast":
            item = {"phase": name, "from_kmh": round(s[0] * 3.6, 1), "to_kmh": round(s[-1] * 3.6, 1),
                    "decel": round(-np.polyfit(x, s, 1)[0], 3)}
        else:
            continue
        out["longitudinal"].append(item)
        print("  " + "  ".join(f"{k}={v}" for k, v in item.items()))
    (run / "summary.json").write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
