"""Trailer brake torque search while driving (read-only memory, trailer brake key).

Drives at 40 km/h on the test pad with the sysid harness, then presses the trailer
brake (numpad 0) for 3 s, rolls 4 s, applies the service brake 0.2 for 3 s. Every frame
it reads the per-wheel float arrays of all vehicle-shaped objects reachable from the
truck vehicle (see trailer_brake_find.py) and prints which arrays respond to which brake.

Usage: py -3 research/trailer_brake_drive.py
"""
import json, struct, sys, time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "apps" / "sysid"))
from can_scan import Process, vehicle_address  # noqa: E402
from trailer_brake_find import candidates, TRAILER_BRAKE  # noqa: E402
import game_keys as keys  # noqa: E402
import sysid_drive as sd  # noqa: E402
from sysid_drive import Session, Step, Stop, ROOT  # noqa: E402


def main():
    out = ROOT / "research" / "live" / time.strftime("%Y-%m-%d-trailer-brake-%H%M%S")
    out.mkdir(parents=True)
    s = Session(out)
    proc = Process(s.reader.producer_pid)
    found = candidates(proc, vehicle_address(proc), 6)
    samples = []

    def sample(phase):
        row = {"phase": phase, "speed": s.last["speed"] if s.last else None,
               "arrays": {path: list(struct.unpack("<6f", proc.read(arr, 24) or b"\0" * 24)) for path, _, arr in found}}
        samples.append(row)

    def hold(seconds, v, phase, key=None, brake=None):
        s.phase = phase
        if key:
            keys.key(key, True)
        try:
            end = time.monotonic() + seconds
            r = s.frame()
            while time.monotonic() < end:
                thr, brk = s.pedals(r, v) if brake is None else (0.0, brake)
                s.send(sd.steer_to(r, sd.heading_hold(r, s.goal)), thr, brk)
                r = s.frame()
                sample(phase)
        finally:
            if key:
                keys.key(key, False)

    def run():
        s.ensure_room(1500)
        s.drive(25, 40 / 3.6, s.hold(), "accel")
        hold(3, 40 / 3.6, "cruise")
        hold(3, None, "trailer_brake", key=TRAILER_BRAKE, brake=0.0)
        hold(4, 40 / 3.6, "recover")
        hold(3, None, "service_0.2", brake=0.2)
        s.drive(8, 0.0, s.hold(), "stop")

    s.steps = [Step("launch", "start", 10, lambda: sd.launch(s)), Step("trailer brake test", "test", 45, run)]
    s.client.request("lease", action="claim", owner=s.lease)
    s.hb = time.monotonic()
    try:
        sd.wait_focus_and_arm(s)
        for i, step in enumerate(s.steps):
            s.index = i
            sd.run_with_rearm(s, step.run)
    except Stop as e:
        print("STOPPED:", e)
    finally:
        keys.key(TRAILER_BRAKE, False)
        sd.release(s)
    (out / "arrays.json").write_text(json.dumps(samples))
    by = {}
    for row in samples:
        by.setdefault(row["phase"], []).append(row)
    print("phase mean speed:", {k: round(np.mean([r["speed"] for r in v]) * 3.6, 1) for k, v in by.items()})
    for path, _, _ in found:
        means = {k: np.mean([r["arrays"][path] for r in v], axis=0) for k, v in by.items()}
        if max(np.abs(m).max() for m in means.values()) < 1:
            continue
        span = max(np.abs(means[k] - means["cruise"]).max() for k in ("trailer_brake", "service_0.2"))
        if span > 50:
            print(path, {k: np.round(m).tolist() for k, m in means.items()})


if __name__ == "__main__":
    main()
