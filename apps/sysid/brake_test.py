"""Brake decomposition on the OT test pad: service brake, engine brake and the game's
automatic engine brake, each from 80 km/h, then whether their decelerations add up.

Phase A keeps the profile's settings (g_motor_brake_auto 1) and records when the game
engages the engine brake by itself. Phase B turns the automatic engine brake off in
the game console and measures coast, engine brake alone (B held), service brake alone
and both together. The console is typed with the truck stopped and control released,
because W/A/S/D are driving keys; the setting is restored at the end, also after a
stop. The Scania S on the pad has no retarder (retarder.steps 0), so none is tested.

Usage: py -3 brake_test.py [--view] [--brakes 0.05,0.1,...] [--combos 0.1,0.3,0.5]
Logs bridge/recordings/brake-<time>/frames.jsonl (with the brake channels under "x").
"""
import argparse, ctypes, math, time
from ctypes import wintypes
from pathlib import Path

import sysid_drive as sd
from sysid_drive import Session, Step, Stop, ROOT

V0 = 80 / 3.6
STOP_BELOW = 10 / 3.6
CHANNELS = ["truck.brake.motor", "truck.brake.retarder", "truck.brake.parking", "truck.brake.air.pressure",
            "truck.brake.temperature", "truck.input.brake", "truck.input.throttle", "truck.displayed.gear",
            "truck.cruise_control"] + [f"truck.wheel.angular_velocity[{i}]" for i in range(4)]

# --- keyboard through SendInput scan codes (DirectInput sees them like real keys) ---
SCAN = {}
for start, row in ((0x10, "qwertyuiop"), (0x1E, "asdfghjkl"), (0x2C, "zxcvbnm")):
    for i, c in enumerate(row):
        SCAN[c] = start + i
for i, c in enumerate("1234567890"):
    SCAN[c] = 0x02 + i
SCAN[" "], SCAN["-"] = 0x39, 0x0C
ENTER, GRAVE, SHIFT, KEY_B = 0x1C, 0x29, 0x2A, 0x30


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def key(scan, down):
    event = INPUT(type=1)  # INPUT_KEYBOARD
    event.ki = KEYBDINPUT(0, scan, 0x0008 | (0 if down else 0x0002), 0, 0)  # SCANCODE, KEYUP
    ctypes.windll.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))


def tap(scan, hold=0.05):
    key(scan, True); time.sleep(hold); key(scan, False); time.sleep(0.05)


def type_text(text, tick=lambda: None):
    for c in text:
        if c == "_":
            key(SHIFT, True); tap(SCAN["-"]); key(SHIFT, False)
        else:
            tap(SCAN[c])
        tick()


def console(*commands, tick=lambda: None):
    """Open the game console, run the commands, close it. The game must have focus.
    tick() runs between keys (a lease heartbeat: typing takes longer than its 5 s)."""
    sd.focus_game()
    time.sleep(0.8)
    tap(GRAVE)
    time.sleep(0.6)
    for command in commands:
        type_text(command, tick)
        tap(ENTER)
        time.sleep(0.3)
        tick()
    tap(GRAVE)
    time.sleep(0.6)
    tick()


# --- runs ---
def run(s: Session, label, brake=0.0, engine_brake=False, longest=30.0, auto_off=False):
    """Accelerate to 80 km/h on a straight, then hold the pedals (and B) until 10 km/h."""
    s.ensure_room(2000)
    s.drive(max(8, V0 * 0.8), V0, s.hold(), f"accel:{label}")
    s.drive(5, V0, s.hold(), f"steady:{label}")
    s.thr, s.brk = 0.0, brake
    s.phase = f"brake:{label}"
    if engine_brake:
        key(KEY_B, True)
    try:
        end = time.monotonic() + longest
        r = s.frame()
        while time.monotonic() < end and r["speed"] > STOP_BELOW:
            s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), 0.0, brake)
            r = s.frame()
            if auto_off and not engine_brake and (r.get("x") or {}).get("truck.brake.motor"):
                raise Stop("engine brake engaged with g_motor_brake_auto 0 and B released: console setting not applied?")
    finally:
        if engine_brake:
            key(KEY_B, False)
    s.drive(6, 0.0, s.hold(), f"stop:{label}")


def stop_and_release_control(s: Session):
    s.drive(8, 0.0, s.hold(), "stop for console")
    s.client.request("drive", action="disarm", owner=s.lease, drive_owner=s.lease, epoch=s.epoch)
    s.epoch, s.seen, s.stale = 0, False, 0


def set_auto_engine_brake(s: Session, on):
    stop_and_release_control(s)
    s.phase = f"console g_motor_brake_auto {int(on)}"
    s.status(force=True)
    s.auto_off = not on  # set first: a stop during typing still restores the profile
    console(f"g_motor_brake_auto {int(on)}", tick=s.frame)
    sd.wait_focus_and_arm(s)


def plan(s: Session, brakes, combos, phases="AB"):
    steps = [Step("launch", "start", 8, lambda: sd.launch(s))]
    per_run = 45
    if "A" in phases:
        steps.append(Step("A coast from 80 km/h (auto engine brake on)", "A: auto on", per_run, lambda: run(s, "A:coast")))
        for b in brakes:
            steps.append(Step(f"A brake {b} from 80 km/h", "A: auto on", per_run, lambda b=b: run(s, f"A:brake:{b}", b)))
    if "B" not in phases:
        return steps
    steps.append(Step("console: g_motor_brake_auto 0", "console", 20, lambda: set_auto_engine_brake(s, False)))
    steps.append(Step("B coast from 80 km/h", "B: auto off", per_run, lambda: run(s, "B:coast", auto_off=True)))
    steps.append(Step("B engine brake (B held) from 80 km/h", "B: auto off", per_run,
                      lambda: run(s, "B:engine", 0.0, True, auto_off=True)))
    for b in brakes:
        steps.append(Step(f"B service brake {b}", "B: auto off", per_run, lambda b=b: run(s, f"B:brake:{b}", b, auto_off=True)))
    for b in combos:
        steps.append(Step(f"B service brake {b} + engine brake", "B: auto off", per_run,
                          lambda b=b: run(s, f"B:combo:{b}", b, True, auto_off=True)))
    return steps


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--brakes", default="0.05,0.1,0.2,0.3,0.5,1.0")
    p.add_argument("--combos", default="0.1,0.3,0.5")
    p.add_argument("--phases", default="AB", help="A (profile settings), B (automatic engine brake off), or both")
    p.add_argument("--view", action="store_true")
    a = p.parse_args()
    values = lambda text: [float(x) for x in text.split(",") if x.strip()]
    out = ROOT / "bridge" / "recordings" / time.strftime("brake-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    s.extra = CHANNELS
    s.auto_off = False
    s.steps = plan(s, values(a.brakes), values(a.combos), a.phases.upper())
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
        key(KEY_B, False)
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
                console("g_motor_brake_auto 1")
                print("restored g_motor_brake_auto 1", flush=True)
            else:
                print("NOT restored: truck still moving; type g_motor_brake_auto 1 in the console", flush=True)
        print("log", out / "frames.jsonl", flush=True)


if __name__ == "__main__":
    main()
