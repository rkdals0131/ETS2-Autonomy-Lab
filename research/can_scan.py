"""Find CAN-like engine and driveline values inside the player vehicle (read-only).

Opens the game with PROCESS_VM_READ | PROCESS_QUERY_INFORMATION only (no injection,
writes or remote calls). Resolves the player vehicle through the pinned schema
(controller -> player actor -> vehicle), reads the vehicle object and every object it
points to (one level, optionally two), and samples them with the SDK state from the ot
shared memory while the truck drives (run apps/sysid/brake_test.py or any drive at the
same time). Each 4-byte slot is read as float32 and correlated online with targets:
rpm, effective throttle, expected engine torque (def torque curve x throttle), engine
brake flag, retarder step, effective brake, speed, gear.

Usage: py -3 research/can_scan.py --seconds 600 [--hz 10] [--depth 2] [--out research/live/<dir>]
       py -3 research/can_scan.py --report research/live/<dir>/scan.npz
"""
import argparse, ctypes as C, json, struct, sys, time
from ctypes import wintypes as W
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ot" / "py"))
from otpy.client import StateReader  # noqa: E402

K = C.WinDLL("kernel32", use_last_error=True)
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
K.ReadProcessMemory.restype = W.BOOL
PSAPI = C.WinDLL("psapi", use_last_error=True)
PSAPI.EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(W.HMODULE), W.DWORD, C.POINTER(W.DWORD), W.DWORD]

# Scania DC16 520 (def/vehicle/truck/scania.s_2016/engine/dc16_520.sii), base game.
TORQUE_NM = 2700.0
TORQUE_CURVE = [(300, 0.1), (440, 0.4), (1000, 1), (1300, 1), (1600, 0.82), (1900, 0.72), (2300, 0.4), (2500, 0.1)]
TARGETS = ["rpm", "throttle", "torque_expected", "engine_brake", "retarder", "brake", "speed", "gear", "rpm_rad"]


def curve(rpm):
    xs, ys = zip(*TORQUE_CURVE)
    return float(np.interp(rpm, xs, ys))


class Process:
    def __init__(self, pid):
        self.h = K.OpenProcess(0x0410, False, pid)
        if not self.h:
            raise C.WinError(C.get_last_error())
        mods, need = (W.HMODULE * 1)(), W.DWORD()
        if not PSAPI.EnumProcessModulesEx(self.h, mods, C.sizeof(mods), C.byref(need), 0x02):
            raise C.WinError(C.get_last_error())
        self.base = mods[0]

    def read(self, address, size):
        buf, done = C.create_string_buffer(size), C.c_size_t()
        if not address or not K.ReadProcessMemory(self.h, address, buf, size, C.byref(done)) or done.value != size:
            return None
        return buf.raw

    def ptr(self, address):
        b = self.read(address, 8)
        return struct.unpack("<Q", b)[0] if b else 0


def schema():
    game = json.loads((ROOT / "ot" / "schema" / "1.61.1.9" / "fields.json").read_text(encoding="utf-8"))
    return game["controller_pointer_rva"], game["player_offset"], game["vehicle_physics"]["actor_vehicle_offset"]


def vehicle_address(proc):
    controller_rva, player, vehicle_off = schema()
    actor = proc.ptr(proc.ptr(proc.base + controller_rva) + player)
    return proc.ptr(actor + vehicle_off) if actor else 0


def pointers(blob):
    out = []
    for off in range(0, len(blob) - 7, 8):
        (v,) = struct.unpack_from("<Q", blob, off)
        if 0x10000 < v < 0x7FFF00000000 and v % 8 == 0:
            out.append((off, v))
    return out


def layout(proc, vehicle, size, child, depth):
    """Regions to sample: (path, address, size). Children of the vehicle and, with depth 2,
    their children; module-image pointers (vtables, globals) are skipped."""
    image_end = proc.base + 0x8000000
    regions, seen = [("vehicle", vehicle, size)], {vehicle}
    frontier = [("vehicle", vehicle, size)]
    for level in range(depth):
        nxt = []
        for path, address, length in frontier:
            blob = proc.read(address, length)
            if not blob:
                continue
            for off, target in pointers(blob):
                if proc.base <= target < image_end or target in seen:
                    continue
                if proc.read(target, child) is None:
                    continue
                seen.add(target)
                region = (f"{path}+{off:#x}", target, child)
                regions.append(region)
                nxt.append(region)
        frontier = nxt
    return regions


def sdk_targets(frame):
    sdk = frame["sdk"]
    v = lambda k: (sdk.get(k) or {}).get("value") if (sdk.get(k) or {}).get("available") else None
    rpm, thr = v("truck.engine.rpm") or 0.0, v("truck.effective.throttle") or 0.0
    return {"rpm": rpm, "throttle": thr, "torque_expected": TORQUE_NM * curve(rpm) * thr,
            "engine_brake": float(bool(v("truck.brake.motor"))), "retarder": float(v("truck.brake.retarder") or 0),
            "brake": v("truck.effective.brake") or 0.0, "speed": v("truck.speed") or 0.0,
            "gear": float(v("truck.engine.gear") or 0), "rpm_rad": rpm * 2 * np.pi / 60}


def scan(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    reader = StateReader()
    proc = Process(reader.producer_pid)
    vehicle = vehicle_address(proc)
    if not vehicle:
        raise SystemExit("player vehicle not resolved (schema pointers)")
    regions = layout(proc, vehicle, a.size, a.child, a.depth)
    total = sum(r[2] for r in regions)
    n_float = total // 4
    print(f"vehicle {vehicle:#x}, {len(regions)} regions, {total / 1e6:.1f} MB per sample", flush=True)
    T = len(TARGETS)
    s_x, s_xx, s_y, s_yy = np.zeros(n_float), np.zeros(n_float), np.zeros(T), np.zeros(T)
    s_xy = np.zeros((n_float, T))
    lo, hi = np.full(n_float, np.inf), np.full(n_float, -np.inf)
    bad = np.zeros(n_float, bool)
    kept, kept_y, n = [], [], 0
    end, period = time.monotonic() + a.seconds, 1.0 / a.hz
    next_t = time.monotonic()
    while time.monotonic() < end:
        frame = reader.read_latest()
        if not frame:
            time.sleep(0.002)
            continue
        if frame["paused"]:
            time.sleep(0.05)
            continue
        parts = [proc.read(addr, size) for _, addr, size in regions]
        if any(p is None for p in parts):
            print("region unreadable; stopping (truck changed?)", flush=True)
            break
        x = np.frombuffer(b"".join(parts), np.float32).astype(np.float64)
        bad |= ~np.isfinite(x) | (np.abs(x) > 1e7)
        x = np.where(bad, 0.0, x)
        t = sdk_targets(frame)
        y = np.array([t[k] for k in TARGETS])
        s_x += x; s_xx += x * x; s_y += y; s_yy += y * y; s_xy += np.outer(x, y)
        np.minimum(lo, x, out=lo); np.maximum(hi, x, out=hi)
        n += 1
        if n % a.keep_every == 0:
            kept.append(x.astype(np.float32)); kept_y.append(y)
        if n % (a.hz * 30) == 0:
            print(f"{n} samples", flush=True)
        next_t += period
        time.sleep(max(0.0, next_t - time.monotonic()))
    np.savez_compressed(out / "scan.npz", n=n, s_x=s_x, s_xx=s_xx, s_y=s_y, s_yy=s_yy, s_xy=s_xy, lo=lo, hi=hi, bad=bad,
                        kept=np.array(kept), kept_y=np.array(kept_y), targets=np.array(TARGETS))
    (out / "regions.json").write_text(json.dumps([{"path": p, "address": hex(ad), "size": s} for p, ad, s in regions], indent=0))
    print(f"{n} samples -> {out / 'scan.npz'}", flush=True)


def report(path, top):
    d = np.load(path)
    regions = json.loads((Path(path).parent / "regions.json").read_text())
    starts, acc = [], 0
    for r in regions:
        starts.append(acc); acc += r["size"]
    def where(i):
        byte = i * 4
        k = max(j for j, s in enumerate(starts) if s <= byte)
        return f"{regions[k]['path']} +{byte - starts[k]:#x}"
    n = float(d["n"])
    mx, my = d["s_x"] / n, d["s_y"] / n
    vx = d["s_xx"] / n - mx ** 2
    vy = d["s_yy"] / n - my ** 2
    cov = d["s_xy"] / n - np.outer(mx, my)
    r = cov / np.sqrt(np.maximum(vx, 1e-30)[:, None] * np.maximum(vy, 1e-30)[None, :])
    usable = ~d["bad"] & (vx > 1e-12)
    targets = list(d["targets"])
    print(f"{int(n)} samples, {usable.sum()} varying slots of {len(vx)}")
    for j, name in enumerate(targets):
        order = np.argsort(-np.abs(np.where(usable, r[:, j], 0)))[:top]
        print(f"\n== {name}")
        for i in order:
            others = " ".join(f"{t[:6]}={r[i, k]:+.2f}" for k, t in enumerate(targets) if k != j)
            print(f"  r={r[i, j]:+.3f}  {where(i):40s} range {d['lo'][i]:+.4g}..{d['hi'][i]:+.4g}  | {others}")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--seconds", type=float, default=600)
    p.add_argument("--hz", type=int, default=10)
    p.add_argument("--size", type=lambda v: int(v, 0), default=0x2000, help="bytes of the vehicle object")
    p.add_argument("--child", type=lambda v: int(v, 0), default=0x400, help="bytes per pointed object")
    p.add_argument("--depth", type=int, default=1)
    p.add_argument("--keep-every", type=int, default=10)
    p.add_argument("--out", default=str(ROOT / "research" / "live" / time.strftime("%Y-%m-%d-can-scan-%H%M%S")))
    p.add_argument("--report")
    p.add_argument("--top", type=int, default=12)
    a = p.parse_args()
    if a.report:
        report(a.report, a.top)
    else:
        scan(a)


if __name__ == "__main__":
    main()
