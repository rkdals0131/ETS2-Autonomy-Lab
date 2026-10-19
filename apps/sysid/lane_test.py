"""Closed-loop lane keeping on the OT test pad with the production lateral controller.

Each scenario builds an exact lane polyline (straight with an initial offset, arcs,
S-curves, a 3.5 m lane shift) from the truck's current pose, then steers with
LateralController + VirtualEps from bridge/build/lateral_capi (apps/sysid/build_lateral.cmd)
over the ot pipe, using the SDK placement as ground truth. Speed is held by a pedal PI.
Logs frames.jsonl (SDK) and lateral.jsonl (controller) and prints a summary.
"""
import argparse, ctypes, json, math, time
from pathlib import Path

import sysid_drive as sd
from sysid_drive import Session, Step, Stop, heading, heading_hold, steer_to, WHEELBASE, PAD, MARGIN

ANGLE_PER_UNIT = 0.70
REAR_AXLE_BEHIND_ORIGIN = 2.094   # SDK wheel z of the drive axle (forward is -z)
LEAD_IN = 60.0
DLL = sd.ROOT / "bridge" / "build" / "lateral_capi" / "lateral_capi.dll"


class Lateral:
    def __init__(self):
        self.dll = ctypes.CDLL(str(DLL))
        self.dll.lat_create.restype = ctypes.c_void_p
        self.dll.lat_create.argtypes = [ctypes.c_char_p]
        self.dll.lat_last_error.restype = ctypes.c_char_p
        self.dll.lat_destroy.argtypes = [ctypes.c_void_p]
        self.dll.lat_step.argtypes = [ctypes.c_void_p] + [ctypes.c_double] * 11 + [ctypes.POINTER(ctypes.c_double)]
        self.out = (ctypes.c_double * 10)()

    def create(self, path):
        h = self.dll.lat_create(str(path).encode())
        if not h:
            raise Stop("lane load failed: " + self.dll.lat_last_error().decode())
        return h

    def step(self, h, x, y, yaw, v, applied, dt):
        if self.dll.lat_step(h, x, y, yaw, v, applied, dt, WHEELBASE, 6.0, 0.7, ANGLE_PER_UNIT, 6.5, self.out):
            raise Stop("controller: " + self.dll.lat_last_error().decode())
        o = self.out
        return {"cmd": o[0], "target": o[1], "rate": o[2], "wheel": o[3], "offset": o[4], "heading_error": o[5],
                "distance": o[6], "remaining": o[7], "path_curvature": o[8], "curvature": o[9]}


def ros_pose(r):
    """Rear-axle pose in ROS world: x = SCS X, y = -SCS Z, yaw CCW from +x."""
    X, _, Z = r["pos"]
    yaw = heading(r) + math.pi / 2
    return X - REAR_AXLE_BEHIND_ORIGIN * math.cos(yaw), -Z - REAR_AXLE_BEHIND_ORIGIN * math.sin(yaw), yaw


def build_lane(x, y, yaw, segments, shift=0.0, ds=1.5):
    """segments: [(length_m, curvature)], starting with a LEAD_IN straight; shift moves the lane left."""
    pts, h = [], yaw
    px, py = x - shift * math.sin(yaw), y + shift * math.cos(yaw)
    pts.append((px, py))
    for length, k in [(LEAD_IN, 0.0)] + segments:
        n = max(1, round(length / ds))
        for _ in range(n):
            step = length / n
            if k:
                h2 = h + k * step
                px += (math.sin(h2) - math.sin(h)) / k
                py -= (math.cos(h2) - math.cos(h)) / k
                h = h2
            else:
                px += step * math.cos(h)
                py += step * math.sin(h)
            pts.append((px, py))
    return pts


def inside(pts):
    return all(PAD[0] + MARGIN < x < PAD[2] - MARGIN and PAD[1] + MARGIN < -y < PAD[3] - MARGIN for x, y in pts)


def lane_shift(width, length):
    """Two opposite arcs (angle a, radius R) shifting the lane by `width` over `length`:
    forward 2R sin a = length, lateral 2R(1 - cos a) = length tan(a/2) = width."""
    a = 2 * math.atan(width / length)
    R = length / (2 * math.sin(a))
    return [(R * a, 1 / R), (R * a, -1 / R)]


SCENARIOS = [
    # name, km/h, segments, shift (lane start left of truck, m)
    ("straight, lane 1 m left, 30 km/h", 30, [(500, 0)], 1.0),
    ("straight, lane 1 m left, 80 km/h", 80, [(1400, 0)], 1.0),
    ("arc R400 180 deg, 60 km/h", 60, [(400 * math.pi, 1 / 400), (150, 0)], 0.0),
    ("arc R400 180 deg, 80 km/h", 80, [(400 * math.pi, 1 / 400), (150, 0)], 0.0),
    ("arc R200 180 deg, 50 km/h", 50, [(200 * math.pi, 1 / 200), (120, 0)], 0.0),
    ("S-curve R300 3x90 deg, 60 km/h", 60, [(300 * math.pi / 2, 1 / 300), (300 * math.pi / 2, -1 / 300), (300 * math.pi / 2, 1 / 300), (150, 0)], 0.0),
    ("S-curve R300 3x90 deg, 80 km/h", 80, [(300 * math.pi / 2, 1 / 300), (300 * math.pi / 2, -1 / 300), (300 * math.pi / 2, 1 / 300), (150, 0)], 0.0),
    ("3.5 m lane shift over 120 m, 80 km/h", 80, [(300, 0)] + lane_shift(3.5, 120) + [(400, 0)], 0.0),
]


def run_scenario(s: Session, lat: Lateral, out_dir: Path, log, name, kmh, segments, shift, idx):
    v = kmh / 3.6
    straight_need = sum(L for L, _ in segments) + LEAD_IN + v * 25 + 300
    s.ensure_room(min(straight_need, 2500))
    s.drive(max(6, v * 0.9), v, s.hold(), f"lane:{idx}:speed_up")
    r = s.last
    x, y, yaw = ros_pose(r)
    pts = None
    for mirror in (1, -1):
        cand = build_lane(x, y, yaw, [(L, mirror * k) for L, k in segments], shift)
        if inside(cand):
            pts = cand
            break
    if pts is None:
        s.ensure_room(1e9)  # turn around and retry once
        r = s.drive(6, v, s.hold(), f"lane:{idx}:speed_up")
        x, y, yaw = ros_pose(r)
        for mirror in (1, -1):
            cand = build_lane(x, y, yaw, [(L, mirror * k) for L, k in segments], shift)
            if inside(cand):
                pts = cand
                break
    if pts is None:
        return {"name": name, "skipped": "lane does not fit on the pad from this pose"}
    lane_file = out_dir / f"lane_{idx:02d}.json"
    lane_file.write_text(json.dumps({"frame_id": "world", "lane_width_m": 3.5, "points": pts, "end_reason": "test"}))
    h = lat.create(lane_file)
    s.phase = f"lane:{idx}:{name}"
    rows, prev, result = [], None, {"name": name, "kmh": kmh}
    try:
        r = s.frame()
        while True:
            x, y, yaw = ros_pose(r)
            dt = 0.0 if prev is None else (r["sim_us"] - prev) / 1e6
            prev = r["sim_us"]
            c = lat.step(h, x, y, yaw, max(0.0, r["speed"]), r["eff_steer"], dt)
            row = {"t": r["sim_us"], "scenario": idx, "speed": r["speed"], "applied": r["eff_steer"], **c}
            rows.append(row)
            log.write(json.dumps(row) + "\n")
            if len(rows) % 12 == 0:
                s.message = f"{name}: offset {c['offset']:+.3f} m, heading err {math.degrees(c['heading_error']):+.2f} deg, remaining {c['remaining']:.0f} m"
            if c["distance"] > 2.5:
                result["aborted"] = f"lane lost: {c['distance']:.2f} m"
                break
            if c["remaining"] < 5:
                break
            thr, brk = s.pedals(r, v)
            s.send(c["cmd"], thr, brk)
            r = s.frame()
    finally:
        lat.dll.lat_destroy(h)
    s.goal = heading(s.last)
    s.drive(3, v, s.hold(), f"lane:{idx}:exit")
    result.update(summarize(rows))
    return result


def summarize(rows):
    if not rows:
        return {}
    total = rows[0]["remaining"]
    travelled = lambda r: total - r["remaining"]
    first = [r for r in rows if travelled(r) <= 150]
    settle = [r for r in rows if travelled(r) > 150]
    curved = [r for r in settle if abs(r["path_curvature"]) > 1e-4]
    off = [abs(r["offset"]) for r in settle] or [0.0]
    cmd = [r["cmd"] for r in rows]
    return {"frames": len(rows),
            "max_offset_first150m": max((abs(r["offset"]) for r in first), default=None),
            "max_offset_after150m": max(off), "rms_offset_after150m": math.sqrt(sum(o * o for o in off) / len(off)),
            "mean_offset_in_curves": sum(r["offset"] for r in curved) / len(curved) if curved else None,
            "max_heading_error_deg_after150m": max((abs(math.degrees(r["heading_error"])) for r in settle), default=None),
            "cmd_sign_reversals": sum(1 for a, b in zip(cmd, cmd[1:]) if a * b < 0 and abs(a - b) > 0.05),
            "saturated_frames": sum(1 for c in cmd if abs(c) >= 0.999)}


def with_retry(s, run):
    """Retry a scenario once if a game hitch let the command window expire."""
    try:
        return run()
    except sd.Expired:
        sd.rearm(s)
        result = run()
        result["retried_after_expiry"] = True
        return result


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--view", action="store_true")
    p.add_argument("--only", default="", help="comma-separated scenario indices")
    a = p.parse_args()
    out = sd.ROOT / "bridge" / "recordings" / time.strftime("lanetest-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    lat = Lateral()
    log = (out / "lateral.jsonl").open("w")
    results = []
    chosen = [i for i in range(len(SCENARIOS)) if not a.only or str(i) in a.only.split(",")]
    s.steps = [Step("launch at 18 km/h", "start", 8, lambda: sd.launch(s))]
    for i in chosen:
        name, kmh, segs, shift = SCENARIOS[i]
        est = (sum(L for L, _ in segs) + LEAD_IN) / (kmh / 3.6) + 12
        s.steps.append(Step(name, "lane keeping", est,
                            lambda i=i, n=name, k=kmh, g=segs, sh=shift: results.append(with_retry(s, lambda: run_scenario(s, lat, out, log, n, k, g, sh, i)))))
    s.steps.append(Step("stop", "end", 10, lambda: s.drive(10, 0.0, s.hold(), "stop")))
    s.status(force=True)
    if a.view:
        sd.open_viewer(out / "status.json")
    s.client.request("lease", action="claim", owner=s.lease)
    s.hb = time.monotonic()
    try:
        sd.wait_focus_and_arm(s)
        for i, step in enumerate(s.steps):
            s.index, s.step_start = i, time.monotonic()
            s.status(force=True)
            step.run()
        s.index, s.state, s.message = len(s.steps), "done", "all scenarios finished"
    except Stop as e:
        s.state, s.message = "stopped", str(e)
        print("STOPPED:", e, flush=True)
    finally:
        sd.release(s)
        log.close()
        (out / "summary.json").write_text(json.dumps(results, indent=1))
        print(json.dumps(results, indent=1))
        print("log", out, flush=True)


if __name__ == "__main__":
    main()
