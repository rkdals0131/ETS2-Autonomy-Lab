"""Find the trailer's per-wheel brake torque (read-only, plus the trailer brake key).

The truck's wheel brake torques sit at vehicle+0x150 -> +0x260 -> float[wheels]
(docs/30). If the trailer is a vehicle of the same kind, an object reachable from the
truck vehicle has the same shape. This lists every reachable object (two levels) whose
+0x150 -> +0x260 leads to `count` finite floats, then reads them with the trailer brake
key (numpad 0 in the ETS_SysID profile) released and pressed at standstill.

Usage: py -3 research/trailer_brake_find.py [--count 6]
"""
import argparse, struct, sys, time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "apps" / "sysid"))
from can_scan import Process, StateReader, vehicle_address, layout  # noqa: E402
import game_keys as keys  # noqa: E402
import sysid_drive as sd  # noqa: E402

TRAILER_BRAKE = 0x52  # numpad 0


def candidates(proc, vehicle, count):
    found = []
    for path, address, _ in layout(proc, vehicle, 0x2000, 0x400, 2):
        physics = proc.ptr(address + 0x150)
        if not physics:
            continue
        array = proc.ptr(physics + 0x260)
        blob = proc.read(array, 4 * count) if array else None
        if not blob:
            continue
        values = struct.unpack(f"<{count}f", blob)
        if all(np.isfinite(values)) and all(abs(v) < 1e6 for v in values):
            found.append((path, address, array))
    return found


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--count", type=int, default=6)
    a = p.parse_args()
    reader = StateReader()
    proc = Process(reader.producer_pid)
    vehicle = vehicle_address(proc)
    found = candidates(proc, vehicle, a.count)
    print(f"truck vehicle {vehicle:#x}; {len(found)} vehicle-shaped objects")
    read = lambda arr: np.array(struct.unpack(f"<{a.count}f", proc.read(arr, 4 * a.count)))
    before = {path: read(arr) for path, _, arr in found}
    sd.focus_game()
    time.sleep(1.0)
    keys.key(TRAILER_BRAKE, True)
    try:
        time.sleep(2.0)
        during = {path: read(arr) for path, _, arr in found}
    finally:
        keys.key(TRAILER_BRAKE, False)
    time.sleep(1.5)
    after = {path: read(arr) for path, _, arr in found}
    for path, address, arr in found:
        changed = np.abs(during[path] - before[path]).max()
        print(f"{path:40s} object {address:#x} array {arr:#x}  before {np.round(before[path]).tolist()}  "
              f"pressed {np.round(during[path]).tolist()}  after {np.round(after[path]).tolist()}  {'<== changes' if changed > 1 else ''}")


if __name__ == "__main__":
    main()
