"""Brake decomposition on the OT test pad: service brake, engine brake, retarder and the
game's automatic engine brake and retarder, each from 80 km/h, then whether their
decelerations add up.

Phase A keeps the profile's settings (g_motor_brake_auto 1, g_retarder_auto 1) and
records when the game engages the engine brake or the retarder by itself. Phase B turns
both automatics off in the game console and measures coast, engine brake alone (B held),
service brake alone and with the engine brake. Phase R (also with the automatics off)
holds each retarder step with the ; and ' keys, then the top step with the service brake
and with the engine brake. The console is typed with the truck stopped and control
released, because W/A/S/D are driving keys; the settings are restored at the end, also
after a stop. Phase R needs a truck with a retarder (truck_config retarder.steps > 0).

Usage: py -3 brake_test.py [--view] [--phases ABR] [--brakes 0.05,0.1,...] [--combos 0.1,0.3,0.5]
       [--tag keep|up|down]
Logs bridge/recordings/brake-<time>/frames.jsonl (with the brake channels under "x").
"""
import argparse, time

import game_keys as keys
import sysid_drive as sd
from sysid_drive import Session, Step, Stop, ROOT

V0 = 80 / 3.6
STOP_BELOW = 10 / 3.6
CHANNELS = ["truck.brake.motor", "truck.brake.retarder", "truck.brake.parking", "truck.brake.air.pressure",
            "truck.brake.temperature", "truck.input.brake", "truck.input.throttle", "truck.displayed.gear",
            "truck.cruise_control"] + [f"truck.wheel.angular_velocity[{i}]" for i in range(6)]
AUTOMATICS = ("g_motor_brake_auto", "g_retarder_auto")


# --- runs ---
def run(s: Session, label, brake=0.0, engine_brake=False, retarder=0, longest=40.0, auto_off=False):
    """Accelerate to 80 km/h on a straight, then hold the pedals (and B, and the retarder
    step) until 10 km/h."""
    s.ensure_room(2200)
    s.drive(max(8, V0 * 0.8), V0, s.hold(), f"accel:{label}")
    s.drive(5, V0, s.hold(), f"steady:{label}")
    s.thr, s.brk = 0.0, brake
    s.phase = f"brake:{label}"
    lever = keys.Lever(keys.RETARDER_UP, keys.RETARDER_DOWN)
    if engine_brake:
        keys.key(keys.ENGINE_BRAKE, True)
    try:
        end = time.monotonic() + longest
        r = s.frame()
        while time.monotonic() < end and r["speed"] > STOP_BELOW:
            s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), 0.0, brake)
            r = s.frame()
            x = r.get("x") or {}
            lever.update(x.get("truck.brake.retarder"), retarder)
            if auto_off and not engine_brake and x.get("truck.brake.motor"):
                raise Stop("engine brake engaged with g_motor_brake_auto 0 and B released: console setting not applied?")
            if auto_off and not retarder and x.get("truck.brake.retarder"):
                raise Stop("retarder engaged with g_retarder_auto 0: console setting not applied?")
        # Retarder back to 0 before the next acceleration.
        r = s.frame()
        until = time.monotonic() + 5
        while (r.get("x") or {}).get("truck.brake.retarder") and time.monotonic() < until:
            s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), 0.0, brake)
            r = s.frame()
            lever.update((r.get("x") or {}).get("truck.brake.retarder"), 0)
    finally:
        lever.tapper.release()
        if engine_brake:
            keys.key(keys.ENGINE_BRAKE, False)
    s.drive(6, 0.0, s.hold(), f"stop:{label}")


def stop_and_release_control(s: Session):
    s.drive(8, 0.0, s.hold(), "stop for console")
    s.client.request("drive", action="disarm", owner=s.lease, drive_owner=s.lease, epoch=s.epoch)
    s.epoch, s.seen, s.stale = 0, False, 0


def set_automatics(s: Session, on):
    stop_and_release_control(s)
    s.phase = f"console automatics {int(on)}"
    s.status(force=True)
    s.auto_off = not on  # set first: a stop during typing still restores the profile
    keys.console(*(f"{name} {int(on)}" for name in AUTOMATICS), focus=sd.focus_game, tick=s.frame,
                 focused=lambda: sd.game_in_front(s.reader.producer_pid))
    sd.wait_focus_and_arm(s)


def plan(s: Session, brakes, combos, phases="AB", tag="keep", steps_available=0):
    steps = [] if tag == "keep" else [Step(f"lift axle {tag} at standstill", "start", 6, lambda: sd.set_tag(s, tag == "down"))]
    steps.append(Step("launch", "start", 8, lambda: sd.launch(s)))
    per_run = 50
    if "A" in phases:
        steps.append(Step("A coast from 80 km/h (automatics on)", "A: auto on", per_run, lambda: run(s, "A:coast")))
        for b in brakes:
            steps.append(Step(f"A brake {b} from 80 km/h", "A: auto on", per_run, lambda b=b: run(s, f"A:brake:{b}", b)))
    if "B" not in phases and "R" not in phases:
        return steps
    steps.append(Step("console: automatic engine brake and retarder off", "console", 25, lambda: set_automatics(s, False)))
    if "B" in phases:
        steps.append(Step("B coast from 80 km/h", "B: auto off", per_run, lambda: run(s, "B:coast", auto_off=True)))
        steps.append(Step("B engine brake (B held) from 80 km/h", "B: auto off", per_run,
                          lambda: run(s, "B:engine", 0.0, True, auto_off=True)))
        for b in brakes:
            steps.append(Step(f"B service brake {b}", "B: auto off", per_run, lambda b=b: run(s, f"B:brake:{b}", b, auto_off=True)))
        for b in combos:
            steps.append(Step(f"B service brake {b} + engine brake", "B: auto off", per_run,
                              lambda b=b: run(s, f"B:combo:{b}", b, True, auto_off=True)))
    if "R" in phases:
        if not steps_available:
            raise SystemExit("phase R: this truck has no retarder")
        if "B" not in phases:
            steps.append(Step("R coast from 80 km/h", "R: auto off", per_run, lambda: run(s, "B:coast", auto_off=True)))
        top = steps_available
        for n in range(1, top + 1):
            steps.append(Step(f"R retarder step {n}", "R: auto off", per_run,
                              lambda n=n: run(s, f"R:retarder:{n}", retarder=n, auto_off=True)))
        steps.append(Step(f"R retarder {top} + engine brake", "R: auto off", per_run,
                          lambda: run(s, f"R:retarder:{top}+engine", 0.0, True, top, auto_off=True)))
        for b in combos:
            steps.append(Step(f"R retarder {top} + service brake {b}", "R: auto off", per_run,
                              lambda b=b: run(s, f"R:retarder:{top}+brake:{b}", b, retarder=top, auto_off=True)))
    return steps


def retarder_steps(s: Session):
    config = s.client.request("truck_config")
    return next((a["value"] for a in config["attributes"] if a["name"] == "retarder.steps"), 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--brakes", default="0.05,0.1,0.2,0.3,0.5,1.0")
    p.add_argument("--combos", default="0.1,0.3,0.5")
    p.add_argument("--phases", default="ABR",
                   help="A (profile settings), B (automatics off), R (retarder steps, automatics off)")
    p.add_argument("--tag", choices=("keep", "up", "down"), default="keep", help="lift axle position for the run")
    p.add_argument("--view", action="store_true")
    a = p.parse_args()
    values = lambda text: [float(x) for x in text.split(",") if x.strip()]
    out = ROOT / "bridge" / "recordings" / time.strftime("brake-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    s.extra += CHANNELS
    s.auto_off = False
    s.steps = plan(s, values(a.brakes), values(a.combos), a.phases.upper(), a.tag, retarder_steps(s))
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
        s.index, s.state, s.message = len(s.steps), "done", "all runs finished"
    except Stop as e:
        s.state, s.message = "stopped", str(e)
        print("STOPPED:", e, flush=True)
    finally:
        for scan in (keys.ENGINE_BRAKE, keys.RETARDER_UP, keys.RETARDER_DOWN):
            keys.key(scan, False)
        restore = s.auto_off
        sd.release(s)
        if restore:
            # Type only into a stopped truck (letters are driving keys if the console fails).
            speed = None
            with sd.StateReader() as reader:
                until = time.monotonic() + 20
                while time.monotonic() < until:
                    frame = reader.read_latest() or {}
                    speed = ((frame.get("sdk") or {}).get("truck.speed") or {}).get("value")
                    if speed is not None and abs(speed) < 0.5:
                        break
                    time.sleep(0.2)
            if speed is not None and abs(speed) < 0.5:
                keys.console(*(f"{name} 1" for name in AUTOMATICS), focus=sd.focus_game,
                             focused=lambda: sd.game_in_front(reader.producer_pid))
                print("restored", ", ".join(f"{name} 1" for name in AUTOMATICS), flush=True)
            else:
                print("NOT restored: truck still moving; type", " and ".join(f"{name} 1" for name in AUTOMATICS),
                      "in the console", flush=True)
        print("log", out / "frames.jsonl", flush=True)


if __name__ == "__main__":
    main()
