"""What an ACC needs from the longitudinal plant, on the OT test pad.

  launch    standstill, then throttle 0.2 / 0.4 / 0.7 / 1.0 up to 50 km/h (clutch slip at
            pull-away, the first shifts)
  lowstop   from 20 km/h, brake 0.1 / 0.2 / 0.4 to a stop (the stronger brake below about
            5 rad/s wheel speed), then 3 s at standstill on the brake and 4 s released
            (does the truck creep or roll?)
  tips      sequential gearbox only: in gears 4, 6 and 8 from about 1,150 rpm, throttle
            0.8 until 1,900 rpm then 0 until 1,000 rpm, three times (driveline wind-up and
            shuffle after tip-in and tip-out)
  stops     from 80 km/h, brake 0.3 / 0.5 / 0.75 / 1.0 to a stop, then standstill as above
            (deceleration limit; with the truck's and the trailer's wheel brake torques the
            forces can be balanced)

The game's automatic engine brake and retarder keep the profile's settings, as an ACC
would meet them. Every frame logs the drivetrain under "dt" (Tier 1, core 0.41.0: also
the trailer's wheel brake torques).

Usage: py -3 acc_test.py [--blocks launch,lowstop,tips,stops] [--view]
"""
import argparse, time

import game_keys as keys
import gear_test as gt
import sysid_drive as sd
from sysid_drive import Session, Step, Stop, ROOT

CHANNELS = ["truck.brake.motor", "truck.brake.retarder", "truck.brake.parking", "truck.brake.air.pressure",
            "truck.input.brake", "truck.input.throttle", "truck.effective.clutch", "truck.displayed.gear"] \
    + [f"truck.wheel.angular_velocity[{i}]" for i in range(6)] \
    + [f"trailer.0.wheel.angular_velocity[{i}]" for i in range(6)]
TIP_LOW, TIP_HIGH, TIP_START = 1000.0, 1900.0, 1150.0


def until(s, label, thr, brk, stop, longest):
    """Hold the pedals on the heading until stop(r) or `longest` seconds."""
    s.phase = label
    s.thr, s.brk = thr, brk
    end = time.monotonic() + longest
    r = s.frame()
    while time.monotonic() < end and not stop(r):
        s.send(sd.steer_to(r, sd.heading_hold(r, s.goal) if r["speed"] > 0.5 else 0.0), thr, brk)
        r = s.frame()
    return r


def standstill(s, tag, brake):
    until(s, f"standstill:{tag}", 0.0, max(brake, 0.2), lambda r: False, 3.0)
    until(s, f"released:{tag}", 0.0, 0.0, lambda r: False, 4.0)


def stop_from(s, kmh, brake):
    v = kmh / 3.6
    s.ensure_room(v * 40 + 400 + v * v / (2 * max(0.3, 8 * brake)))
    s.drive(max(8, v * 0.8), v, s.hold(), f"accel:{kmh}:{brake}")
    # With the trailer the truck needs longer than the plain drive time to reach 80 km/h.
    s.phase = f"accel:{kmh}:{brake}"
    r, end = s.frame(), time.monotonic() + 60
    while r["speed"] < v - 0.5 and time.monotonic() < end:
        s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), 1.0, 0.0)
        r = s.frame()
    s.drive(4, v, s.hold(), f"steady:{kmh}:{brake}")
    r = until(s, f"stop:{kmh}:{brake}", 0.0, brake, lambda r: r["speed"] < 0.05, 90)
    if r["speed"] >= 0.05:
        raise Stop(f"brake {brake} from {kmh} km/h did not stop the truck")
    standstill(s, f"{kmh}:{brake}", brake)


def launch_step(s, thr):
    s.ensure_room(900)
    s.drive(8, 0.0, s.hold(), f"to_standstill:{thr}")
    if s.shifter:
        # A sequential gearbox steps down one gear per press: wait for the start gear.
        start = s.shifter.START_GEAR
        r = until(s, f"downshift:{thr}", 0.0, 0.3, lambda r: s.shifter.selected(r) == start, 25)
        if s.shifter.selected(r) != start:
            raise Stop(f"start gear not reached (in {s.shifter.selected(r)})")
    until(s, f"pre_launch:{thr}", 0.0, 0.3, lambda r: False, 2.0)
    until(s, f"launch:{thr}", thr, 0.0, lambda r: r["speed"] > 50 / 3.6, 25)
    s.drive(10, 0.0, s.hold(), f"brake_to_stop:{thr}")


def tips(s, gear):
    v = TIP_START / gt.rpm_per_mps(gear)
    s.ensure_room(v * 60 + 600)
    s.shifter.fixed = None
    s.drive(max(10, v * 2), v, s.hold(), f"approach:{gear}")
    gt.hold_gear(s, gear)
    try:
        for i in range(3):
            s.drive(4, v, s.hold(), f"settle:{gear}:{i}")
            until(s, f"tipin:{gear}:{i}", 0.8, 0.0, lambda r: (r["rpm"] or 0) > TIP_HIGH, 6)
            until(s, f"tipout:{gear}:{i}", 0.0, 0.0, lambda r: (r["rpm"] or 0) < TIP_LOW, 8)
    finally:
        s.shifter.fixed = None
    s.drive(10, 0.0, s.hold(), f"brake_to_stop:{gear}")


def plan(s, blocks):
    steps = [Step("launch", "start", 10, lambda: sd.launch(s))]
    if "launch" in blocks:
        for thr in (0.2, 0.4, 0.7, 1.0):
            steps.append(Step(f"launch at throttle {thr} to 50 km/h", "launch", 40, lambda t=thr: launch_step(s, t)))
    if "lowstop" in blocks:
        for b in (0.1, 0.2, 0.4):
            steps.append(Step(f"brake {b} from 20 km/h to a stop", "low-speed stop", 30, lambda b=b: stop_from(s, 20, b)))
    if "tips" in blocks and s.shifter:
        for g in (4, 6, 8):
            steps.append(Step(f"gear {g}: throttle tip-in and tip-out x3", "driveline", 60, lambda g=g: tips(s, g)))
    if "stops" in blocks:
        for b in (0.3, 0.5, 0.75, 1.0):
            steps.append(Step(f"brake {b} from 80 km/h to a stop", "stop", 70, lambda b=b: stop_from(s, 80, b)))
    steps.append(Step("stop", "end", 15, lambda: s.drive(15, 0.0, s.hold(), "stop")))
    return steps


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--blocks", default="launch,lowstop,tips,stops")
    p.add_argument("--view", action="store_true")
    a = p.parse_args()
    out = ROOT / "bridge" / "recordings" / time.strftime("acc-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    s.extra += CHANNELS
    blocks = {b.strip() for b in a.blocks.split(",") if b.strip()}
    if "tips" in blocks and not s.shifter:
        print("gearbox is not sequential: skipping the tip-in block", flush=True)
    s.steps = plan(s, blocks)
    s.status(force=True)
    if a.view:
        sd.open_viewer(out / "status.json")
    print("lease", s.lease, "->", out, flush=True)
    s.client.request("lease", action="claim", owner=s.lease)
    s.hb = time.monotonic()
    try:
        sd.focus_game()
        sd.wait_focus_and_arm(s)
        for i, step in enumerate(s.steps):
            s.index, s.step_start = i, time.monotonic()
            s.status(force=True)
            print("step", i, step.label, flush=True)
            sd.run_with_rearm(s, step.run)
        s.index, s.state, s.message = len(s.steps), "done", "all steps finished"
        print("done", flush=True)
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
