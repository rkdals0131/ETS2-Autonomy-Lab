"""Fixed-gear engine and retarder maps on the OT test pad (sequential gearbox).

The automatic gearbox holds about 2,000-2,200 rpm whenever the engine brake works, so the
engine brake, friction and full-load torque are only seen at a few speeds. With the
gearbox set to sequential (SDK controls shifter.type "manual") the harness holds one
gear and sweeps the engine speed: per gear it drives to the speed where the engine runs
near HIGH_RPM, holds the gear and runs, each between LOW_RPM and HIGH_RPM,
  eb      throttle 0 with the engine brake (B held) down to LOW_RPM
  full    full throttle up to HIGH_RPM (full-load torque and its response)
  coast   throttle 0, no engine brake, down to LOW_RPM (friction torque)
  half    throttle 0.5 up to HIGH_RPM
  ret     retarder step 3 down to LOW_RPM
Core 0.39.0+ logs the actual engine and output torque under "dt" (Tier 1).

Usage: py -3 gear_test.py [--gears 6,8,10,12] [--view]
"""
import argparse, math, time

import game_keys as keys
import sysid_drive as sd
from sysid_drive import Session, Step, Stop, ROOT

LOW_RPM, HIGH_RPM, SPEED_LIMIT = 900.0, 2250.0, 85 / 3.6
RATIOS = [20.81, 16.16, 12.57, 9.76, 7.56, 5.87, 4.55, 3.53, 2.77, 2.15, 1.66, 1.29, 1.0, 0.78]
FINAL, RADIUS = 2.59, 0.506


def rpm_per_mps(gear):
    return RATIOS[gear - 1] * FINAL / RADIUS * 60 / (2 * math.pi)


def until_rpm(s, label, thr, brk, stop, longest=40.0, engine_brake=False, retarder=0):
    """Hold the pedals (and B / the retarder step) in the fixed gear until stop(rpm) or `longest`."""
    s.phase = label
    s.thr, s.brk = thr, brk
    lever = keys.Lever(keys.RETARDER_UP, keys.RETARDER_DOWN)
    if engine_brake:
        keys.key(keys.ENGINE_BRAKE, True)
    try:
        end = time.monotonic() + longest
        r = s.frame()
        while time.monotonic() < end and not stop(r["rpm"] or 0.0) and r["speed"] < SPEED_LIMIT + 2:
            s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), thr, brk)
            r = s.frame()
            lever.update((r.get("x") or {}).get("truck.brake.retarder"), retarder)
        r = s.frame()
        until = time.monotonic() + 5
        while (r.get("x") or {}).get("truck.brake.retarder") and time.monotonic() < until:
            s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), 0.0, 0.0)
            r = s.frame()
            lever.update((r.get("x") or {}).get("truck.brake.retarder"), 0)
    finally:
        lever.tapper.release()
        if engine_brake:
            keys.key(keys.ENGINE_BRAKE, False)


def hold_gear(s, gear, seconds=8.0):
    """Shift to `gear` and keep the speed until it is engaged."""
    s.shifter.fixed = gear
    s.phase = f"shift:{gear}"
    end = time.monotonic() + seconds
    r = s.frame()
    while time.monotonic() < end and r["gear"] != gear:
        thr, brk = s.pedals(r, r["speed"])
        s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), thr, brk)
        r = s.frame()
    if r["gear"] != gear:
        raise Stop(f"gear {gear} not engaged (at {r['gear']})")


def sweep(s, gear):
    k = rpm_per_mps(gear)
    v_high = min(SPEED_LIMIT, HIGH_RPM / k)
    high = min(HIGH_RPM, v_high * k)
    s.ensure_room(v_high * 120 + 600)
    s.shifter.fixed = None
    s.drive(max(15, v_high * 2.0), v_high, s.hold(), f"approach:{gear}")
    hold_gear(s, gear)
    try:
        until_rpm(s, f"eb:{gear}", 0.0, 0.0, lambda rpm: rpm < LOW_RPM, engine_brake=True)
        until_rpm(s, f"full:{gear}", 1.0, 0.0, lambda rpm: rpm > high)
        until_rpm(s, f"coast:{gear}", 0.0, 0.0, lambda rpm: rpm < LOW_RPM, longest=60)
        until_rpm(s, f"half:{gear}", 0.5, 0.0, lambda rpm: rpm > high)
        until_rpm(s, f"ret:{gear}", 0.0, 0.0, lambda rpm: rpm < LOW_RPM, retarder=3)
    finally:
        s.shifter.fixed = None


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--gears", default="6,8,10,12")
    p.add_argument("--view", action="store_true")
    a = p.parse_args()
    out = ROOT / "bridge" / "recordings" / time.strftime("gear-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    if not s.shifter:
        raise SystemExit("set the gearbox to sequential (SDK controls shifter.type is not 'manual')")
    s.extra += ["truck.brake.motor", "truck.brake.retarder", "truck.effective.clutch", "truck.input.throttle"]
    gears = [int(g) for g in a.gears.split(",") if g.strip()]
    s.steps = [Step("launch", "start", 10, lambda: sd.launch(s))]
    for g in gears:
        s.steps.append(Step(f"gear {g}: engine brake, full load, coast, half throttle, retarder 3", f"gear {g}", 90,
                            lambda g=g: sweep(s, g)))
    s.steps.append(Step("stop", "end", 15, lambda: s.drive(15, 0.0, s.hold(), "stop")))
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
        s.index, s.state, s.message = len(s.steps), "done", "all gears finished"
    except Stop as e:
        s.state, s.message = "stopped", str(e)
        print("STOPPED:", e, flush=True)
    finally:
        for scan in (keys.ENGINE_BRAKE, keys.RETARDER_UP, keys.RETARDER_DOWN):
            keys.key(scan, False)
        sd.release(s)
        print("log", out / "frames.jsonl", flush=True)


if __name__ == "__main__":
    main()
