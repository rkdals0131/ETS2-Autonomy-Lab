"""Lift axle on the OT test pad: does the U key lower and raise it at standstill and at
speed, how long the travel takes, and how the truck reacts while the wheels touch down.

Steps: at standstill lower and raise; then at 30, 60 and 85 km/h on a straight lower,
hold 6 s, raise, hold 6 s. A refused key press is recorded and the run goes on. The
lift axle is left raised at the end.

Usage: py -3 lift_test.py [--view]
Logs bridge/recordings/lift-<time>/frames.jsonl (lift channels under "x") and prints a
summary per toggle.
"""
import argparse, json, time

import sysid_drive as sd
from sysid_drive import Session, Step, Stop, ROOT, LiftRefused

CHANNELS = ["truck.brake.retarder", "truck.displayed.gear"] + [
    f"truck.wheel.{k}[{i}]" for i in range(6) for k in ("suspension.deflection", "angular_velocity")]
results = []


def toggle(s: Session, down, kmh):
    start, v0 = time.monotonic(), s.last["speed"] if s.last else 0.0
    try:
        sd.set_tag(s, down, kmh / 3.6)
        results.append({"kmh": kmh, "down": down, "ok": True, "seconds": round(time.monotonic() - start, 2),
                        "speed_kmh": round(v0 * 3.6, 1)})
    except LiftRefused as e:
        results.append({"kmh": kmh, "down": down, "ok": False, "error": str(e)})
        print("refused:", e, flush=True)
    s.drive(6, kmh / 3.6, s.hold() if kmh else (lambda r, t: sd.steer_to(r, 0.0)), f"hold:{'down' if down else 'up'}:{kmh}")


def at_speed(s: Session, kmh):
    s.ensure_room(kmh / 3.6 * 40 + 300)
    s.drive(max(8, kmh / 3.6 * 0.8), kmh / 3.6, s.hold(), f"accel:{kmh}")
    s.drive(3, kmh / 3.6, s.hold(), f"steady:{kmh}")
    toggle(s, True, kmh)
    toggle(s, False, kmh)


def plan(s: Session):
    steps = [Step("raise first if lowered", "standstill", 8, lambda: sd.set_tag(s, False)),
             Step("lower at standstill", "standstill", 12, lambda: toggle(s, True, 0)),
             Step("raise at standstill", "standstill", 12, lambda: toggle(s, False, 0)),
             Step("launch", "start", 8, lambda: sd.launch(s))]
    for kmh in (30, 60, 85):
        steps.append(Step(f"lower and raise at {kmh} km/h", f"{kmh} km/h", 40, lambda k=kmh: at_speed(s, k)))
    return steps


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--view", action="store_true")
    a = p.parse_args()
    out = ROOT / "bridge" / "recordings" / time.strftime("lift-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    if not s.geometry.liftable:
        raise SystemExit("this truck has no lift axle")
    s.extra += CHANNELS
    s.steps = plan(s)
    s.status(force=True)
    if a.view:
        sd.open_viewer(out / "status.json")
    print("lease", s.lease, "->", out, flush=True)
    s.client.request("lease", action="claim", owner=s.lease)
    s.hb = time.monotonic()
    try:
        sd.wait_focus_and_arm(s)
        for i, step in enumerate(s.steps):
            s.index, s.step_start = i, time.monotonic()
            s.status(force=True)
            sd.run_with_rearm(s, step.run)
        s.index, s.state, s.message = len(s.steps), "done", "all steps finished"
    except Stop as e:
        s.state, s.message = "stopped", str(e)
        print("STOPPED:", e, flush=True)
    finally:
        sd.release(s)
        (out / "summary.json").write_text(json.dumps(results, indent=1))
        for r in results:
            print(r, flush=True)
        print("log", out / "frames.jsonl", flush=True)


if __name__ == "__main__":
    main()
