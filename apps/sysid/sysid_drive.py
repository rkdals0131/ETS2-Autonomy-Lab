"""Reference-truck identification drive on the OT test pad (steering + pedals).

Runs a fixed list of steps (per speed: steering rate pulses, steering sines, a
steady turn; then coast-down, brake and throttle steps), logs every SDK frame to
bridge/recordings/sysid-<time>/frames.jsonl and writes status.json for
sysid_view.py. --view opens the viewer in a new PowerShell window.
Manual input, pause, F11 or leaving the pad margin releases control.
"""
import argparse, ctypes, json, math, os, subprocess, sys, time, uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ot" / "py"))
from otpy.client import Client, StateReader  # noqa: E402
import game_keys as keys  # noqa: E402

PAD = (-28550.0, -50000.0, -23450.0, -42000.0)  # x0, z0, x1, z1 (SCS), bridge/testground_map defaults
MARGIN = 250.0
HARD_EDGE = 100.0  # m from the pad edge that always stops a run
# Front axle to the middle of the grounded rear axles, and that middle's SDK z (behind the
# truck origin). Session reads them from the truck configuration and keeps them current
# while a lift axle goes up or down; these defaults are the Scania S 4x2.
WHEELBASE = 3.734
REAR_Z = 2.094
ANGLE_PER_UNIT = 0.70   # front wheel rad per unit of applied steering at speed
ticks = ctypes.windll.kernel32.GetTickCount64
ticks.restype = ctypes.c_uint64


class Stop(Exception):
    pass


class Expired(Stop):
    """The core released control because no command arrived inside its 200 ms window
    (a game hitch such as an autosave). Test scripts may re-arm once; manual input,
    pause and F11 releases stay final."""


class LiftRefused(Stop):
    """The lift axle did not react to its key (the game may refuse it in some states)."""


class FocusLost(Stop):
    """The game window was in the background (our commands had no effect) and is back;
    run_with_rearm recovers the speed and runs the step again."""


FOCUS_WAIT_S = 600


@dataclass
class Step:
    label: str
    group: str
    seconds: float
    run: Callable[[], None]


class Geometry:
    """Axles from the SDK truck configuration (z grows backwards, metres)."""

    def __init__(self, config):
        wheels = {}
        for a in config["attributes"]:
            if a["name"].startswith("wheel."):
                wheels.setdefault(a["index"], {})[a["name"][6:]] = a["value"]
        self.front = mean({w["position"][2] for w in wheels.values() if w["steerable"] and not w["liftable"]})
        self.fixed = sorted({w["position"][2] for w in wheels.values() if not w["steerable"] and not w["liftable"]})
        self.lift_z = sorted({w["position"][2] for w in wheels.values() if w["liftable"]})
        self.liftable = sorted(i for i, w in wheels.items() if w["liftable"])

    def rear_z(self, lift_down):
        return mean(self.fixed + (self.lift_z if lift_down else []))

    def channels(self):
        return (["truck.lift_axle"] + [f"truck.wheel.{k}[{i}]" for i in self.liftable for k in ("lift", "on_ground")]
                if self.liftable else [])


def mean(values):
    values = list(values)
    return sum(values) / len(values)


def lift_down(r):
    """True when the liftable wheels touch the ground (None without a lift axle)."""
    grounded = [v for k, v in (r.get("x") or {}).items() if k.startswith("truck.wheel.on_ground[")]
    return any(grounded) if grounded else None


class Session:
    def __init__(self, out_dir):
        self.out = out_dir
        self.log = (out_dir / "frames.jsonl").open("w")
        self.client = Client(timeout=1.0)
        self.reader = StateReader()
        self.lease = "sysid-" + uuid.uuid4().hex[:8]
        self.epoch = self.seq = self.stale = 0
        self.seen = False
        self.steer = self.thr = self.brk = 0.0
        self.phase, self.hb, self.vi, self.last = "init", 0.0, 0.0, None
        self.goal = 0.0
        self.extra: list[str] = []  # more SDK channels to log per frame, under "x"
        self.steps: list[Step] = []
        self.index, self.step_start, self.state, self.message = -1, 0.0, "waiting", ""
        self.status_time = 0.0
        self.unfocused_speed = 0.0
        self.edge = None
        self.focus_losses: list[dict] = []
        self.attempt = 0  # increases on every step retry; frames carry it so analyses can drop aborted attempts
        config =self.client.request("truck_config")
        (out_dir / "truck_config.json").write_text(json.dumps(config, indent=1))
        self.geometry = Geometry(config)
        # Wipers: the profile runs the rain sensor (g_rain_sensor 1), so they mark rain.
        self.extra += self.geometry.channels() + ["truck.brake.temperature", "truck.wipers"]

    # status for the viewer, at most 10 Hz
    def status(self, force=False):
        now = time.monotonic()
        if not force and now - self.status_time < 0.1:
            return
        self.status_time = now
        r = self.last or {}
        pos = r.get("pos")
        data = {"run_dir": str(self.out), "updated": time.time(), "state": self.state, "message": self.message, "phase": self.phase,
                "index": self.index, "step_elapsed": now - self.step_start if self.index >= 0 else 0.0,
                "steps": [{"label": s.label, "group": s.group, "seconds": s.seconds} for s in self.steps],
                "live": {"speed_kmh": (r.get("speed") or 0) * 3.6, "eff_steer": r.get("eff_steer"), "cmd_steer": self.steer,
                         "throttle": self.thr, "brake": self.brk, "armed": r.get("armed"), "gear": r.get("gear"),
                         "edge_m": edge_distance(pos) if pos else None, "frames": self.seq}}
        tmp = self.out / "status.json.tmp"
        tmp.write_text(json.dumps(data))
        # The viewer may hold status.json open for a moment; Windows then refuses the replace.
        for attempt in range(5 if force else 1):
            try:
                os.replace(tmp, self.out / "status.json")
                return
            except PermissionError:
                time.sleep(0.01)

    def frame(self):
        """Next SDK frame (logged). While we hold control and the game window is in the
        background, the game ignores our commands: wait here, keeping the lease and the
        command window alive, so step code never acts on frames it does not control. When
        the window comes back after more than 0.5 s, raise FocusLost (run_with_rearm then
        recovers the speed and runs the step again)."""
        r = self._frame()
        if not (self.epoch and self.seen) or r["focus"]:
            return r
        start, speed, phase = time.monotonic(), r["speed"] or 0.0, self.phase
        self.message = "game window in the background: commands have no effect, waiting for it"
        print(f"focus lost in {phase} at {speed * 3.6:.0f} km/h; waiting", flush=True)
        self.status(force=True)
        while not r["focus"]:
            if time.monotonic() - start > FOCUS_WAIT_S:
                raise Stop(f"game window in the background for {FOCUS_WAIT_S} s")
            self.send()
            r = self._frame()
        away = time.monotonic() - start
        self.message = "hands off; F11 stops"
        if away <= 0.5:
            return r
        self.unfocused_speed = speed
        self.focus_losses.append({"phase": phase, "seconds": round(away, 2)})
        raise FocusLost(f"game window was in the background for {away:.1f} s in {phase}")

    def _frame(self):
        now = time.monotonic()
        if now - self.hb > 1.0:
            self.client.request("lease", action="heartbeat", owner=self.lease)
            self.hb = now
        deadline = now + 0.5
        while True:
            s = self.reader.read_latest()
            if s:
                break
            if time.monotonic() > deadline:
                raise Stop("no SDK frame for 500 ms")
            time.sleep(0.001)
        d = s["drive"]
        v = lambda k: (s["sdk"].get(k) or {}).get("value") if (s["sdk"].get(k) or {}).get("available") else None
        pl = v("truck.world.placement")
        r = {"t": time.monotonic(), "sim_us": s["simulation_time_us"], "frame": s["frame_id"], "phase": self.phase,
             "cmd_steer": self.steer, "cmd_thr": self.thr, "cmd_brk": self.brk, "armed": d["armed"], "reason": d["reason"],
             "core_steer": d["steering"], "manual_steer": d["manual_steering"], "input_steer": v("truck.input.steering"),
             "eff_steer": v("truck.effective.steering"), "eff_thr": v("truck.effective.throttle"), "eff_brk": v("truck.effective.brake"),
             "w0": v("truck.wheel.steering[0]"), "w1": v("truck.wheel.steering[1]"), "speed": v("truck.speed"),
             "gear": v("truck.engine.gear"), "rpm": v("truck.engine.rpm"), "v_lin": v("truck.local.velocity.linear"),
             "v_ang": v("truck.local.velocity.angular"), "a_lin": v("truck.local.acceleration.linear"),
             "pos": pl["position_m"] if pl else None, "rot": pl["euler_rotations"] if pl else None, "paused": s["paused"],
             "focus": game_in_front(self.reader.producer_pid), "attempt": self.attempt}
        if self.extra:
            r["x"] = {k: v(k) for k in self.extra}
        self.log.write(json.dumps(r) + "\n")
        self.last = r
        global WHEELBASE, REAR_Z
        REAR_Z = self.geometry.rear_z(bool(lift_down(r)))
        WHEELBASE = REAR_Z - self.geometry.front
        if self.epoch and d["epoch"] == self.epoch:
            if d["armed"]:
                self.seen = True
            elif self.seen:
                if d["reason"] == "expired":
                    raise Expired("released by game side: expired")
                raise Stop(f"released by game side: {d['reason']}")
        if self.epoch and not self.seen:
            self.stale += 1
            if self.stale > 30:
                raise Stop(f"arm never appeared: {d['reason']}")
        # Inside the margin only moving further out stops the run (driving back in, and the
        # slow turn toward the centre, are allowed); closer than HARD_EDGE to the pad edge
        # always stops it.
        if r["pos"] and self.phase not in ("init", "stop"):
            edge, before = edge_distance(r["pos"]), self.edge
            self.edge = edge
            outward = edge < 0 and before is not None and edge < before - 0.01 and self.phase != "turn inward"
            if edge < HARD_EDGE - MARGIN or outward:
                raise Stop(f"left the pad margin at x={r['pos'][0]:.0f} z={r['pos'][2]:.0f}")
        self.status()
        return r

    def send(self, steer=None, thr=None, brk=None):
        if steer is not None:
            self.steer = max(-1.0, min(1.0, steer))
        if thr is not None:
            self.thr = max(0.0, min(1.0, thr))
        if brk is not None:
            self.brk = max(0.0, min(1.0, brk))
        self.seq += 1
        r = self.client.request("drive", action="command", owner=self.lease, drive_owner=self.lease, epoch=self.epoch,
                                sequence=self.seq, deadline_ms=int(ticks()) + 190, axes=3,
                                steering=self.steer, throttle=self.thr, brake=self.brk)
        if not r.get("accepted"):
            raise Stop(f"command rejected: {r.get('reason')}")

    # building blocks
    def pedals(self, r, v_target):
        e = v_target - r["speed"]
        if v_target <= 0.1:
            self.vi = 0.0
            return 0.0, min(1.0, 0.3 + 0.1 * r["speed"])
        self.vi = max(-5.0, min(5.0, self.vi + e / 60.0))
        u = 0.12 * e + 0.04 * self.vi
        return (max(0.0, u), 0.0) if u >= -0.05 else (0.0, min(1.0, -0.15 * e))

    def drive(self, seconds, v_target, steer_fn, phase):
        """steer_fn(r, t) -> steering command; v_target None keeps the pedals as set."""
        self.phase = phase
        end = time.monotonic() + seconds
        r = self.frame()
        t0 = time.monotonic()
        while time.monotonic() < end:
            thr, brk = self.pedals(r, v_target) if v_target is not None else (self.thr, self.brk)
            self.send(steer_fn(r, time.monotonic() - t0), thr, brk)
            r = self.frame()
        return r

    def hold(self):
        g = self.goal
        return lambda r, t: steer_to(r, heading_hold(r, g))

    def ensure_room(self, need_m):
        """Turn around on a 60 m radius at 30 km/h when the straight ahead is short."""
        if room_ahead(self.last["pos"], self.goal) >= need_m:
            return
        self.drive(12, 8.3, self.hold(), "uturn: slow down")
        self.goal = math.remainder(self.goal + math.pi, 2 * math.pi)
        eff = math.atan(WHEELBASE / 60.0) / ANGLE_PER_UNIT
        self.phase = "uturn: turning"
        r = self.frame()
        while abs(math.remainder(self.goal - heading(r), 2 * math.pi)) > 0.15:
            thr, brk = self.pedals(r, 8.3)
            self.send(steer_to(r, eff), thr, brk)
            r = self.frame()
        self.drive(4, 8.3, self.hold(), "uturn: settle")


def heading(r):
    # SCS heading in turns, counterclockwise from -Z; forward = (-sin, -cos).
    return r["rot"][0] * 2 * math.pi


def steer_to(r, eff_target, kp=8.0):
    return kp * (eff_target - r["eff_steer"])


def heading_hold(r, goal):
    err = math.remainder(goal - heading(r), 2 * math.pi)
    kappa = max(-0.02, min(0.02, 2.0 * err / max(20.0, 1.5 * max(3.0, r["speed"]))))
    return math.atan(WHEELBASE * kappa) / ANGLE_PER_UNIT


def edge_distance(pos):
    x, _, z = pos
    return min(x - PAD[0], PAD[2] - x, z - PAD[1], PAD[3] - z) - MARGIN


def room_ahead(pos, goal):
    x, _, z = pos
    fx, fz = -math.sin(goal), -math.cos(goal)
    d = []
    if fx > 1e-6: d.append((PAD[2] - MARGIN - x) / fx)
    if fx < -1e-6: d.append((PAD[0] + MARGIN - x) / fx)
    if fz > 1e-6: d.append((PAD[3] - MARGIN - z) / fz)
    if fz < -1e-6: d.append((PAD[1] + MARGIN - z) / fz)
    return min(d) if d else 1e9


def plan(s: Session, speeds_kmh, brakes=(0.2, 0.5, 1.0), throttles=(0.3, 0.6, 1.0), coast_down=True, tag="keep"):
    steps = [] if tag == "keep" else [Step(f"lift axle {tag} at standstill", "start", 6, lambda: set_tag(s, tag == "down"))]
    steps.append(Step("launch at 18 km/h", "start", 8, lambda: launch(s)))
    for kmh in speeds_kmh:
        v = kmh / 3.6
        steps.append(Step(f"accelerate to {kmh} km/h", f"{kmh} km/h", max(8, v * 0.8) + 4, lambda v=v, k=kmh: speed_up(s, v, k)))
        pulse = min(1.0, math.atan(WHEELBASE * 2.0 / v ** 2) / ANGLE_PER_UNIT / (0.65 * 0.3))
        for sign in (1, -1):
            steps.append(Step(f"rate pulse {sign * 0.3:+.1f} for {pulse:.2f} s, then hold u=0", f"{kmh} km/h", pulse + 6.5,
                              lambda v=v, k=kmh, sg=sign, p=pulse: rate_pulse(s, v, k, sg, p)))
        amp = min(0.15, math.atan(WHEELBASE * 1.5 / max(v, 4) ** 2) / ANGLE_PER_UNIT)
        for f in (0.2, 0.5, 1.0):
            steps.append(Step(f"steering sine {f} Hz x3, amplitude {amp:.3f}", f"{kmh} km/h", 3 / f + 3,
                              lambda v=v, k=kmh, f=f, a=amp: sine(s, v, k, f, a)))
        eff = math.atan(WHEELBASE * 2.0 / max(v, 4) ** 2) / ANGLE_PER_UNIT
        steps.append(Step(f"steady turn 2 m/s^2 (eff {eff:.3f})", f"{kmh} km/h", 12, lambda v=v, k=kmh, e=eff: turn(s, v, k, e)))
    if coast_down:
        steps.append(Step("coast-down 15 s from top speed", "longitudinal", 15, lambda: coast(s)))
    for b in brakes:
        steps.append(Step(f"brake step {b} from 60 km/h", "longitudinal", 24, lambda b=b: brake_step(s, b)))
    for a in throttles:
        steps.append(Step(f"throttle step {a} from standstill", "longitudinal", 23, lambda a=a: throttle_step(s, a)))
    return steps


def launch(s):
    r = s.drive(1.0, 0.0, lambda r, t: steer_to(r, 0.0), "settle")
    if r["pos"] is None:
        raise Stop("no placement channel")
    s.goal = heading(r)
    if room_ahead(r["pos"], s.goal) < 300:
        turn_inward(s)
    r = s.drive(8, 5.0, s.hold(), "launch")
    if r["speed"] < 2.0:
        raise Stop("truck did not move under throttle (parking brake? gearbox?)")


def turn_inward(s, radius=15.0):
    """At 10 km/h, turn toward the pad centre (a run that stopped near the margin)."""
    cx, cz = (PAD[0] + PAD[2]) / 2, (PAD[1] + PAD[3]) / 2
    x, _, z = s.last["pos"]
    s.goal = math.atan2(-(cx - x), -(cz - z))  # forward is (-sin h, -cos h)
    s.phase = "turn inward"
    r = s.frame()
    while abs(err := math.remainder(s.goal - heading(r), 2 * math.pi)) > 0.1:
        thr, brk = s.pedals(r, 2.8)
        s.send(steer_to(r, math.copysign(math.atan(WHEELBASE / radius) / ANGLE_PER_UNIT, err)), thr, brk)
        r = s.frame()


def speed_up(s, v, kmh):
    s.ensure_room(v * 75 + 400)
    s.drive(max(8, v * 0.8), v, s.hold(), f"accel:{kmh}")
    s.drive(4, v, s.hold(), f"steady:{kmh}")


def rate_pulse(s, v, kmh, sign, pulse):
    s.drive(pulse, v, lambda r, t: 0.3 * sign, f"rate_pulse:{kmh}:{sign:+d}:{pulse:.3f}")
    s.drive(2.5, v, lambda r, t: 0.0, f"rate_hold:{kmh}:{sign:+d}")
    s.drive(4, v, s.hold(), f"recover:{kmh}")


def sine(s, v, kmh, f, amp):
    g = s.goal
    s.drive(3 / f, v, lambda r, t: steer_to(r, heading_hold(r, g) + amp * math.sin(2 * math.pi * f * t)), f"sine:{kmh}:{f}:{amp:.3f}")
    s.drive(3, v, s.hold(), f"recover:{kmh}")


def turn(s, v, kmh, eff):
    s.drive(8, v, lambda r, t: steer_to(r, eff), f"turn:{kmh}:{eff:.3f}")
    s.goal = heading(s.last)
    s.drive(4, v, s.hold(), f"recover:{kmh}")


def coast(s):
    s.ensure_room(2500)
    s.thr, s.brk = 0.0, 0.0
    s.drive(15, None, s.hold(), "coast")


def brake_step(s, b):
    s.ensure_room(1500)
    s.drive(12, 60 / 3.6, s.hold(), f"pre_brake:{b}")
    s.thr, s.brk = 0.0, b
    s.drive(10, None, s.hold(), f"brake_step:{b}")
    s.drive(2, 0.0, s.hold(), "stopped")


def set_tag(s, down, v_target=0.0):
    """Lower (down=True) or raise the lift axle with its key while holding v_target, then
    wait until the wheels have reached the end of travel (about 5 s)."""
    if not s.geometry.liftable:
        raise Stop("this truck has no lift axle")
    lift = lambda r: (r.get("x") or {}).get(f"truck.wheel.lift[{s.geometry.liftable[0]}]")
    target = 0.0 if down else 1.0
    r = s.frame()
    if lift(r) == target:
        return
    s.phase = f"tag:{'down' if down else 'up'}:{v_target * 3.6:.0f}"
    tapper = keys.Tapper()
    tapper.press(keys.LIFT_AXLE)
    start = time.monotonic()
    try:
        while lift(r) != target:
            thr, brk = s.pedals(r, v_target)
            steer = steer_to(r, heading_hold(r, s.goal) if r["speed"] > 0.5 else 0.0)
            s.send(steer, thr, brk)
            r = s.frame()
            tapper.update()
            if time.monotonic() - start > 2.0 and (r["x"] or {}).get("truck.lift_axle") == down:
                raise LiftRefused(f"lift axle key had no effect at {r['speed'] * 3.6:.0f} km/h")
            if time.monotonic() - start > 15.0:
                raise Stop(f"lift axle stuck at {lift(r)}")
    finally:
        tapper.release()


def throttle_step(s, a):
    s.ensure_room(1500)
    s.drive(3, 0.0, s.hold(), "standstill")
    s.thr, s.brk = a, 0.0
    s.drive(12, None, s.hold(), f"throttle_step:{a}")
    s.drive(8, 0.0, s.hold(), "brake_to_stop")


def open_viewer(status_path):
    """New PowerShell window shown without activation, so the game keeps the focus it needs for input."""
    viewer = Path(__file__).with_name("sysid_view.py")
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 4  # SW_SHOWNOACTIVATE
    # conhost honors SW_SHOWNOACTIVATE; a default Windows Terminal host would take the focus.
    subprocess.Popen(["conhost.exe", "powershell", "-NoProfile", "-Command",
                      f"$Host.UI.RawUI.WindowTitle='OT sysid'; py -3 '{viewer}' '{status_path}'"],
                     creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=info)
    time.sleep(1.0)
    focus_game()


def focus_game():
    """Bring the game window back to the front (Alt tap lifts the foreground lock for this process)."""
    user32 = ctypes.windll.user32
    game_pid = StateReader._producer_pid() if hasattr(StateReader, "_producer_pid") else None
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(hwnd, _):
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if user32.IsWindowVisible(hwnd) and pid.value == (game_pid or _game_pid()):
            found.append(hwnd)
        return True
    user32.EnumWindows(each, 0)
    if found:
        user32.keybd_event(0x12, 0, 0, 0)       # VK_MENU down
        user32.keybd_event(0x12, 0, 2, 0)       # VK_MENU up
        user32.SetForegroundWindow(found[0])


def game_in_front(game_pid):
    user32 = ctypes.windll.user32
    pid = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
    return pid.value == game_pid


def _game_pid():
    with StateReader() as r:
        return r.producer_pid


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--speeds", default="20,40,60,80", help="comma-separated km/h for the lateral blocks ('' for none)")
    p.add_argument("--brakes", default="0.2,0.5,1.0", help="brake step inputs from 60 km/h ('' for none)")
    p.add_argument("--throttles", default="0.3,0.6,1.0", help="throttle step inputs from standstill ('' for none)")
    p.add_argument("--no-coast", action="store_true", help="skip the coast-down")
    p.add_argument("--tag", choices=("keep", "up", "down"), default="keep", help="lift axle position for the run")
    p.add_argument("--view", action="store_true", help="open the progress viewer in a new PowerShell window")
    a = p.parse_args()
    out = ROOT / "bridge" / "recordings" / time.strftime("sysid-%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    values = lambda text, kind: [kind(x) for x in text.split(",") if x.strip()]
    s.steps = plan(s, values(a.speeds, int), values(a.brakes, float), values(a.throttles, float), not a.no_coast, a.tag)
    s.status(force=True)
    if a.view:
        open_viewer(out / "status.json")
    print("lease", s.lease, "->", out, flush=True)
    s.client.request("lease", action="claim", owner=s.lease)
    s.hb = time.monotonic()
    try:
        wait_focus_and_arm(s)
        for i, step in enumerate(s.steps):
            s.index, s.step_start = i, time.monotonic()
            s.status(force=True)
            run_with_rearm(s, step.run)
        s.index = len(s.steps)
        s.state, s.message = "done", "all steps finished"
        print("done", flush=True)
    except Stop as e:
        s.state, s.message = "stopped", str(e)
        print("STOPPED:", e, flush=True)
    finally:
        release(s)
        print("log", out / "frames.jsonl", flush=True)


def wait_focus_and_arm(s):
    """Wait up to 180 s for the game window to stay in front for 2 s, then arm steering and pedals."""
    s.message = "click the game window; arming 2 s after it has focus"
    user32 = ctypes.windll.user32
    until, since = time.monotonic() + 180, None
    while True:
        s.frame()
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(pid))
        if pid.value == s.reader.producer_pid:
            since = since or time.monotonic()
            if time.monotonic() - since > 2.0:
                break
        else:
            since = None
        if time.monotonic() > until:
            raise Stop("game window never came to the foreground")
        time.sleep(0.05)
    until = time.monotonic() + 30
    while True:
        s.frame()
        r = s.client.request("drive", action="arm", owner=s.lease, drive_owner=s.lease, axes=3)
        if r.get("accepted"):
            s.epoch = r["epoch"]
            break
        if time.monotonic() > until:
            raise Stop(f"arm not accepted: {r.get('reason')}")
        time.sleep(0.5)
    s.state, s.message = "running", "hands off; F11 stops"
    print("armed", s.epoch, flush=True)


def rearm(s):
    """Arm again after an Expired release (same lease); the caller retries its step."""
    # No epoch while waiting: frame() would otherwise count the released old epoch as an
    # arm that never appeared and stop after 30 frames.
    s.epoch, s.seen, s.stale = 0, False, 0
    # Wait for the hitch to pass: 1 s of frames no more than 100 ms apart.
    calm_since, last = time.monotonic(), s.frame()["t"]
    until = time.monotonic() + 20
    while time.monotonic() - calm_since < 1.0:
        t = s.frame()["t"]
        if t - last > 0.1:
            calm_since = t
        last = t
        if time.monotonic() > until:
            raise Stop("game frames did not settle after the expiry")
    until = time.monotonic() + 10
    while True:
        r = s.client.request("drive", action="arm", owner=s.lease, drive_owner=s.lease, axes=3)
        if r.get("accepted"):
            s.epoch = r["epoch"]
            break
        if time.monotonic() > until:
            raise Stop(f"re-arm after expiry not accepted: {r.get('reason')}")
        s.frame()
    print("re-armed after expiry", s.epoch, flush=True)


def run_with_rearm(s, run, attempts=3, focus_retries=5):
    """Run a step; after an Expired release re-arm and run it again (up to `attempts`
    times). When the game window went to the background during the step, wait for it to
    come back, recover the speed the truck had and run the step again (up to
    `focus_retries` times)."""
    expired = lost = 0
    while True:
        try:
            return run()
        except Expired:
            expired += 1
            if expired >= attempts:
                raise
            s.attempt += 1
            rearm(s)
            continue
        except FocusLost as e:
            print(e, flush=True)
        while True:
            lost += 1
            if lost > focus_retries:
                raise Stop(f"game window went to the background {lost} times in one step")
            s.attempt += 1
            try:
                resume(s)
                break
            except FocusLost as e:
                print(e, flush=True)


def resume(s):
    """Bring the truck back to the speed it had when the window went to the background."""
    v = s.unfocused_speed
    if v > 1.0:
        s.ensure_room(v * 20 + 300)
        s.drive(max(6.0, v * 0.8), v, s.hold(), "resume after focus loss")
    else:
        s.drive(2.0, 0.0, lambda r, t: steer_to(r, 0.0), "resume after focus loss")


def final_attempts(rows):
    """Frames of the last attempt of each phase, without frames taken in the background
    (a step that was retried logs its phases more than once)."""
    last = {}
    for r in rows:
        last[r["phase"]] = max(last.get(r["phase"], 0), r.get("attempt", 0))
    return [r for r in rows if r.get("attempt", 0) == last[r["phase"]] and r.get("focus", True)]


def release(s):
    """Brake to a stop if still in control, disarm, release the lease, close logs."""
    try:
        if s.epoch:
            s.phase = "stop"
            if s.seen and s.last and s.last["armed"]:
                try:
                    end = time.monotonic() + 15
                    r = s.last
                    while time.monotonic() < end and r["speed"] > 0.3:
                        s.send(steer_to(r, 0.0), 0.0, 0.6)
                        r = s._frame()  # no waiting for the window here
                except Exception:
                    pass
            s.client.request("drive", action="disarm", owner=s.lease, drive_owner=s.lease, epoch=s.epoch)
    except Exception as e:
        print("disarm:", e)
    try:
        s.client.request("lease", action="release", owner=s.lease)
    except Exception as e:
        print("lease release:", e)
    if s.focus_losses:
        (s.out / "focus_losses.json").write_text(json.dumps(s.focus_losses, indent=1))
        print("game window in the background:", s.focus_losses, flush=True)
    s.status(force=True)
    s.log.close()
    s.reader.close()


if __name__ == "__main__":
    main()
