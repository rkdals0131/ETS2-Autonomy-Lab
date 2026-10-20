"""Summary of an acc_test.py run.

Usage: py -3 analyze_acc.py bridge/recordings/acc-<time> [--mass-kg 20300]
  stops     per brake input and start speed: deceleration plateau (median between 85 %
            and 25 % of the start speed), peak, time from the brake command to 63 % of the
            plateau, stopping distance and time; the truck's and the trailer's wheel brake
            force (sum of torque / radius) against mass x deceleration; locked wheels; how
            often the game's automatic engine brake and retarder were on
  standstill  largest speed and distance moved on the brake and after its release
  launch    time to 10 / 30 / 50 km/h, clutch slip time (engine speed against output shaft
            speed x ratio), shifts and the drive torque gap during each
  tips      after each tip-in and tip-out: output torque overshoot over its final value,
            the oscillation frequency and damping ratio of engine minus output shaft speed
Writes summary.json in the run directory.
"""
import argparse, json
from collections import defaultdict
from pathlib import Path

import numpy as np

from sysid_drive import final_attempts

TRUCK_RADIUS = 0.506
MASS_KG = 20300.0  # truck with the empty trailer, docs/31


def series(rows, key):
    return np.array([key(r) if key(r) is not None else np.nan for r in rows], dtype=float)


def time_of(rows):
    t = np.array([r["sim_us"] for r in rows]) / 1e6
    return t - t[0]


def decel(t, v, window=0.2):
    """-dv/dt from a centred difference over `window` seconds."""
    out = np.full(len(v), np.nan)
    for i in range(len(v)):
        lo, hi = np.searchsorted(t, t[i] - window / 2), np.searchsorted(t, t[i] + window / 2) - 1
        if hi > lo and t[hi] > t[lo]:
            out[i] = -(v[hi] - v[lo]) / (t[hi] - t[lo])
    return out


def wheel_forces(rows, trailer_radius):
    ft = np.array([sum(r["dt"]["wheel_brake_torque_nm"]) / TRUCK_RADIUS if "dt" in r else np.nan for r in rows])
    fr = np.array([sum(r["dt"].get("trailer_wheel_brake_torque_nm") or []) / trailer_radius if "dt" in r else np.nan for r in rows])
    return ft, fr


def stop_summary(name, rows, trailer_radius, mass):
    t, v = time_of(rows), series(rows, lambda r: r["speed"])
    d = decel(t, v)
    v0 = v[0]
    plateau = (v < 0.85 * v0) & (v > 0.25 * v0)
    a_plat = float(np.nanmedian(d[plateau])) if plateau.any() else float("nan")
    rise = next((t[i] for i in range(len(t)) if d[i] >= 0.63 * a_plat), None)
    ft, fr = wheel_forces(rows, trailer_radius)
    x = rows[0].get("x") or {}
    locked = []
    for i in range(6):
        for prefix, radius in (("truck", TRUCK_RADIUS), ("trailer.0", trailer_radius)):
            w = series(rows, lambda r, k=f"{prefix}.wheel.angular_velocity[{i}]": (r.get("x") or {}).get(k))
            if np.isfinite(w).any() and np.nanmax(np.abs(w)) > 0.1:  # a raised lift axle does not turn
                slip = 1 - np.abs(w) * 2 * np.pi * radius / np.maximum(v, 0.1)
                if np.nanmax(np.where(v > 2, slip, np.nan)) > 0.9:
                    locked.append(f"{prefix}[{i}]")
    flag = lambda k: float(np.nanmean(series(rows, lambda r: 1.0 if (r.get("x") or {}).get(k) else 0.0)))
    out = {"v0_kmh": round(v0 * 3.6, 1), "plateau_mps2": round(a_plat, 3), "peak_mps2": round(float(np.nanmax(d)), 3),
           "t63_s": None if rise is None else round(float(rise), 3), "stop_time_s": round(float(t[-1]), 2),
           "stop_distance_m": round(float(np.trapezoid(v, t)), 1),
           "truck_brake_force_kn": round(float(np.nanmedian(ft[plateau])) / 1000, 1) if plateau.any() else None,
           "trailer_brake_force_kn": round(float(np.nanmedian(fr[plateau])) / 1000, 1) if plateau.any() else None,
           "m_times_a_kn": round(mass * a_plat / 1000, 1), "locked_wheels": locked,
           "engine_brake_on": round(flag("truck.brake.motor"), 2),
           "retarder_on": round(flag("truck.brake.retarder"), 2),
           "wipers": bool(x.get("truck.wipers"))}
    print(f"== {name}: {out}")
    return out


def standstill_summary(name, rows):
    t, v = time_of(rows), series(rows, lambda r: r["speed"])
    out = {"max_speed_mps": round(float(np.nanmax(np.abs(v))), 3), "distance_m": round(float(np.trapezoid(np.abs(v), t)), 3)}
    print(f"   {name}: {out}")
    return out


def launch_summary(name, rows):
    t, v = time_of(rows), series(rows, lambda r: r["speed"])
    reach = {k: next((round(float(t[i]), 2) for i in range(len(v)) if v[i] >= k / 3.6), None) for k in (10, 30, 50)}
    dt = [r.get("dt") or {} for r in rows]
    we = np.array([d.get("engine_speed_radps", np.nan) for d in dt])
    wo = np.array([d.get("output_shaft_speed_radps", np.nan) * d.get("gear_ratio", np.nan) for d in dt])
    slip = np.abs(we - wo) / np.maximum(we, 1.0)
    first_lock = next((t[i] for i in range(len(t)) if t[i] > 0.2 and slip[i] < 0.02 and wo[i] > 10), None)
    gears = series(rows, lambda r: r["gear"])
    shifts = []
    to = np.array([d.get("output_torque_nm", np.nan) for d in dt])
    for i in range(1, len(gears)):
        if gears[i] != gears[i - 1] and gears[i] > 0 and gears[i - 1] > 0:
            before = np.nanmedian(to[max(0, i - 30):max(1, i - 10)])
            window = (t > t[i] - 1.0) & (t < t[i] + 2.0)
            low = window & (to < 0.2 * before)
            shifts.append({"t_s": round(float(t[i]), 2), "from": int(gears[i - 1]), "to": int(gears[i]),
                           "torque_gap_s": round(float(low.sum() * np.median(np.diff(t))), 2)})
    out = {"reach_s": reach, "clutch_slip_s": None if first_lock is None else round(float(first_lock), 2), "shifts": shifts}
    print(f"== {name}: {out}")
    return out


def tip_summary(name, rows):
    t = time_of(rows)
    dt = [r.get("dt") or {} for r in rows]
    to = np.array([d.get("output_torque_nm", np.nan) for d in dt])
    we = np.array([d.get("engine_speed_radps", np.nan) for d in dt])
    wo = np.array([d.get("output_shaft_speed_radps", np.nan) * d.get("gear_ratio", np.nan) for d in dt])
    final = float(np.nanmedian(to[-max(3, len(to) // 4):]))
    extreme = float(to[np.nanargmax(np.abs(to - to[0]))])
    overshoot = (extreme - final) / (final - to[0]) if abs(final - to[0]) > 1 else float("nan")
    wind = we - wo
    wind = wind - np.nanmean(wind)
    peaks = [i for i in range(1, len(wind) - 1) if abs(wind[i]) >= abs(wind[i - 1]) and abs(wind[i]) > abs(wind[i + 1])
             and abs(wind[i]) > 0.5]
    freq = zeta = None
    if len(peaks) >= 3:
        half = np.diff(t[peaks])
        freq = 1 / (2 * float(np.median(half)))
        amps = np.abs(wind[peaks])
        dec = np.log(amps[:-1] / amps[1:])
        dec = dec[np.isfinite(dec) & (dec > 0)]
        if len(dec):
            delta = 2 * float(np.median(dec))  # successive half cycles, so twice per full cycle
            zeta = delta / np.sqrt(4 * np.pi ** 2 + delta ** 2)
    out = {"from_nm": round(float(to[0])), "final_nm": round(final), "overshoot": None if not np.isfinite(overshoot) else round(overshoot, 2),
           "windup_freq_hz": None if freq is None else round(freq, 2), "windup_zeta": None if zeta is None else round(zeta, 3),
           "windup_peak_radps": round(float(np.nanmax(np.abs(wind))), 2), "duration_s": round(float(t[-1]), 2)}
    print(f"   {name}: {out}")
    return out


def ladder_summary(rows):
    """Per retarder step: time from the SDK step change to 63 % of the change in the
    retarder level (Tier 1) and in the output torque, and the deceleration over the step."""
    out = {}
    t = time_of(rows)
    v = series(rows, lambda r: r["speed"])
    d = decel(t, v, 0.5)
    sdk = series(rows, lambda r: (r.get("x") or {}).get("truck.brake.retarder"))
    level = series(rows, lambda r: (r.get("dt") or {}).get("retarder_level"))
    to = series(rows, lambda r: (r.get("dt") or {}).get("output_torque_nm"))
    changes = [i for i in range(1, len(sdk)) if sdk[i] != sdk[i - 1] and np.isfinite(sdk[i])]
    for n, i in enumerate(changes):
        j_end = changes[n + 1] if n + 1 < len(changes) else len(rows)
        seg = slice(i, j_end)
        def t63(x):
            x0, x1 = np.nanmedian(x[max(0, i - 10):i]), np.nanmedian(x[max(i, j_end - 30):j_end])
            if not np.isfinite(x0) or not np.isfinite(x1) or abs(x1 - x0) < 1e-6:
                return None
            k = next((k for k in range(i, j_end) if (x[k] - x0) / (x1 - x0) >= 0.63), None)
            return None if k is None else round(float(t[k] - t[i]), 3)
        key = f"{int(sdk[i - 1])}->{int(sdk[i])}@{v[i] * 3.6:.0f}kmh"
        out[key] = {"level_t63_s": t63(level), "torque_t63_s": t63(to),
                    "level": round(float(np.nanmedian(level[seg][-30:])), 3),
                    "decel_mps2": round(float(np.nanmedian(d[seg][len(d[seg]) // 2:])), 3)}
        print(f"   ladder {key}: {out[key]}")
    return out


def blend_summary(name, rows):
    t, v = time_of(rows), series(rows, lambda r: r["speed"])
    d = decel(t, v, 0.5)
    window = (v < 65 / 3.6) & (v > 35 / 3.6)
    out = {"decel_mps2": round(float(np.nanmedian(d[window])), 3) if window.any() else None,
           "retarder_level": round(float(np.nanmedian(series(rows, lambda r: (r.get("dt") or {}).get("retarder_level"))[window])), 2) if window.any() else None,
           "engine_brake_on": round(float(np.nanmean(series(rows, lambda r: 1.0 if (r.get("x") or {}).get("truck.brake.motor") else 0.0))), 2),
           "gears": sorted({int(g) for g in series(rows, lambda r: r["gear"])[window] if np.isfinite(g)})}
    print(f"== {name}: {out}")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("run")
    p.add_argument("--mass-kg", type=float, default=MASS_KG)
    a = p.parse_args()
    run = Path(a.run)
    rows = final_attempts([json.loads(line) for line in (run / "frames.jsonl").open()])
    trailer_radius = TRUCK_RADIUS
    configs = json.loads((run / "configs.json").read_text())
    radii = [x["value"] for x in (configs.get("trailer.0") or {}).get("attributes", []) if x["name"] == "wheel.radius"]
    if radii:
        trailer_radius = float(radii[0])
    phases = defaultdict(list)
    for r in rows:
        phases[r["phase"]].append(r)
    summary = {"stops": {}, "standstill": {}, "launch": {}, "tips": {}, "ladder": {}, "blend": {}}
    ladder = [r for r in rows if r["phase"].startswith("ladder:")]
    if ladder:
        summary["ladder"] = ladder_summary(ladder)
    for name, g in phases.items():
        kind = name.split(":")[0]
        if kind == "blend":
            summary["blend"][name] = blend_summary(name, g)
        if kind == "stop":
            summary["stops"][name] = stop_summary(name, g, trailer_radius, a.mass_kg)
        elif kind in ("standstill", "released"):
            summary["standstill"][name] = standstill_summary(name, g)
        elif kind == "launch":
            summary["launch"][name] = launch_summary(name, g)
        elif kind in ("tipin", "tipout"):
            summary["tips"][name] = tip_summary(name, g)
    (run / "summary.json").write_text(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
