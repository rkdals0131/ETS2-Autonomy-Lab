"""Sample chosen vehicle sub-objects as raw bytes together with every SDK channel
(read-only, same access as research/can_scan.py), for labelling the slots it found.

Regions are paths from the player vehicle: "0x150" is the object the vehicle points to
at +0x150, "0x1820/0x2b0" goes one level further. Each sample stores the bytes of every
region and the scalar SDK channels with the simulation time.

Usage: py -3 research/can_probe.py --seconds 600 --hz 30 --region 0x150:0x1000 --region 0x1820:0x800 --out <dir>
       py -3 research/can_probe.py --label <dir>/probe.npz [--top 3]
"""
import argparse, json, struct, sys, time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from can_scan import Process, StateReader, vehicle_address  # noqa: E402


def resolve(proc, vehicle, path):
    address = vehicle
    for hop in path.split("/"):
        address = proc.ptr(address + int(hop, 16))
        if not address:
            return 0
    return address


def scalars(frame):
    out = {"sim_s": frame["simulation_time_us"] / 1e6}
    for k, v in frame["sdk"].items():
        if not v.get("available"):
            continue
        val = v.get("value")
        if isinstance(val, bool):
            out[k] = float(val)
        elif isinstance(val, (int, float)):
            out[k] = float(val)
        elif isinstance(val, list) and len(val) == 3 and all(isinstance(x, (int, float)) for x in val):
            for i, x in enumerate(val):
                out[f"{k}[{i}]"] = float(x)
    return out


def probe(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    reader = StateReader()
    proc = Process(reader.producer_pid)
    vehicle = vehicle_address(proc)
    regions = []
    for spec in a.region:
        path, size = spec.split(":")
        regions.append((path, resolve(proc, vehicle, path), int(size, 16)))
    print("regions", [(p, hex(ad), hex(s)) for p, ad, s in regions], flush=True)
    raw, rows, last = [], [], None
    end, period, next_t = time.monotonic() + a.seconds, 1.0 / a.hz, time.monotonic()
    while time.monotonic() < end:
        frame = reader.read_latest()
        if not frame or frame["paused"] or frame["frame_id"] == last:
            time.sleep(0.002)
            continue
        last = frame["frame_id"]
        parts = [proc.read(ad, s) for _, ad, s in regions]
        if any(p is None for p in parts):
            print("region unreadable; stopping", flush=True)
            break
        raw.append(np.frombuffer(b"".join(parts), np.uint8))
        rows.append(scalars(frame))
        next_t += period
        time.sleep(max(0.0, next_t - time.monotonic()))
    keys = sorted({k for r in rows for k in r})
    sdk = np.array([[r.get(k, np.nan) for k in keys] for r in rows])
    np.savez_compressed(out / "probe.npz", raw=np.array(raw), sdk=sdk, keys=np.array(keys),
                        regions=np.array([f"{p}:{s:#x}" for p, _, s in regions]))
    print(f"{len(rows)} samples -> {out / 'probe.npz'}", flush=True)


def label(path, top, only):
    d = np.load(path)
    raw, sdk, keys = d["raw"], d["sdk"], list(d["keys"])
    floats = raw.view(np.float32).astype(np.float64)
    starts, acc = [], 0
    for r in d["regions"]:
        p, s = r.split(":")
        starts.append((p, acc, int(s, 16))); acc += int(s, 16)
    ok_sdk = [j for j in range(len(keys)) if np.nanstd(sdk[:, j]) > 1e-9 and not np.isnan(sdk[:, j]).any()]
    S = sdk[:, ok_sdk]
    S = (S - S.mean(0)) / S.std(0)
    for p, start, size in starts:
        if only and p not in only:
            continue
        print(f"== {p}")
        for slot in range(start // 4, (start + size) // 4):
            x = floats[:, slot]
            if not np.isfinite(x).all() or np.abs(x).max() > 1e7 or x.std() < 1e-9:
                continue
            z = (x - x.mean()) / x.std()
            r = z @ S / len(z)
            best = np.argsort(-np.abs(r))[:top]
            print(f"  +{slot * 4 - start:#05x} {x.min():+11.4g}..{x.max():+11.4g}  "
                  + "  ".join(f"{keys[ok_sdk[k]]}={r[k]:+.3f}" for k in best))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--seconds", type=float, default=600)
    p.add_argument("--hz", type=int, default=30)
    p.add_argument("--region", action="append", default=[])
    p.add_argument("--out", default="research/live/can-probe")
    p.add_argument("--label")
    p.add_argument("--only", action="append", default=[])
    p.add_argument("--top", type=int, default=3)
    a = p.parse_args()
    if a.label:
        label(a.label, a.top, a.only)
    else:
        probe(a)


if __name__ == "__main__":
    main()
