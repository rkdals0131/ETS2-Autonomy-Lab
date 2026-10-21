"""HUD trajectory decal (docs/24): inject a world path into Local\\OT_Overlay.

The relay normally writes the block from /ets2/overlay/path; this module is the
game-test path without ROS (T2): it holds a bridge lease, switches the core's
overlay on and keeps the file's path ahead of the truck in the shared block.
"""
import ctypes as C
import json
import math
import time
import uuid
from pathlib import Path

from .client import _close, _map, _open_mapping, _unmap

_BLOCK_BYTES = 4416  # sizeof(ot::OverlayBlock), ABI 2 (strokes)
_MAX_POINTS = 256


class OverlayBlock:
    """Writer view of Local\\OT_Overlay through ot_ipc.dll (seqlock in C++)."""

    def __init__(self, helper=None):
        self._handle = self._view = self._native = None
        try:
            path = Path(helper) if helper else Path(__file__).resolve().parents[2] / "dist" / "ot_ipc.dll"
            self._native = C.WinDLL(str(path.resolve()))
            try:
                self._write = self._native.ot_overlay_write
            except AttributeError:
                raise RuntimeError("ot/dist/ot_ipc.dll predates the overlay writer; rebuild with ot\\build.cmd") from None
            self._write.argtypes = [C.c_void_p, C.POINTER(C.c_double), C.c_uint32, C.c_uint64, C.c_uint64,
                                    C.c_float, C.POINTER(C.c_float), C.c_double, C.c_int]
            self._write.restype = C.c_int
            self._handle = _open_mapping(0xF001F, False, r"Local\OT_Overlay")  # FILE_MAP_ALL_ACCESS
            if not self._handle:
                raise RuntimeError("Local\\OT_Overlay is absent; the core creates it on overlay enable (core 0.43.0)")
            self._view = _map(self._handle, 0xF001F, 0, 0, _BLOCK_BYTES)
            if not self._view:
                raise C.WinError(C.get_last_error())
            if C.string_at(self._view, 8) != b"OTOVL02\0":
                raise ValueError("Unsupported Local\\OT_Overlay ABI")
        except BaseException:
            self.close()
            raise

    def write(self, xz, stamp_us=0, ttl_ms=0, width_m=3.5, rgba=(1.0, 1.0, 1.0, 0.22), ground_y=math.nan, valid=True):
        count = min(len(xz), _MAX_POINTS)
        points = (C.c_double * (2 * max(count, 1)))(*[v for p in xz[:count] for v in p])
        color = (C.c_float * 4)(*rgba)
        if self._write(self._view, points, count, int(stamp_us), int(ttl_ms), float(width_m), color, float(ground_y), int(valid)):
            raise ValueError("ot_overlay_write rejected the path")

    def clear(self):
        self.write([], valid=False)

    def close(self):
        if self._view:
            _unmap(self._view)
            self._view = None
        if self._handle:
            _close(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def load_world_path(file):
    """Provider/GT JSON: frame_id "world", points [[x, y], ...] in ROS world (= SCS X, -Z)."""
    data = json.loads(Path(file).read_text(encoding="utf-8"))
    if data.get("frame_id", "world") != "world":
        raise ValueError("Only frame_id world files are injected here; base_link paths go through the relay")
    points = [(float(p[0]), float(p[1])) for p in data["points"]]
    if len(points) < 2 or not all(math.isfinite(v) for p in points for v in p):
        raise ValueError("The path needs at least two finite points")
    return [(x, -y) for x, y in points]  # SCS X, Z


def _rotation(q):
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]


def ego_ground(snapshot, origin):
    """SCS base_link point (nominal ground under the drive axle) from the Tier 1 pose."""
    vehicle = (snapshot.get("engine") or {}).get("vehicle") or {}
    if not vehicle.get("available"):
        return None
    pose = vehicle["pose_physics"]
    r = _rotation(pose["quaternion_wxyz"])
    p = pose["position_m"]
    return [p[i] + sum(r[i][j] * origin[j] for j in range(3)) for i in range(3)]


def ahead(xz, ego, max_length=200.0):
    """The path from the point nearest the truck forward, at most 256 points / 200 m."""
    start = min(range(len(xz)), key=lambda i: (xz[i][0] - ego[0]) ** 2 + (xz[i][1] - ego[2]) ** 2)
    out, length = [xz[start]], 0.0
    for i in range(start + 1, len(xz)):
        length += math.dist(xz[i], xz[i - 1])
        if length > max_length or len(out) == _MAX_POINTS:
            break
        out.append(xz[i])
    return start, out


def show(client, file, width=3.5, alpha=0.22, duration=None, options=None, report=print):
    xz = load_world_path(file)
    owner = uuid.uuid4().hex

    def owned(command, **arguments):
        return client.request(command, owner=owner, **arguments)

    owned("lease", action="claim")
    try:
        owned("tier", value=1)
        status = owned("overlay", action="enable", **(options or {}))
        report(json.dumps({k: status[k] for k in ("enabled", "render_mode")}, ensure_ascii=False))
        # base_link origin of the current truck: the core's mounting authority with no views.
        origin = client.request("resolve_layout", rig={"views": []})["base_origin"]
        deadline = None if duration is None else time.monotonic() + duration
        with OverlayBlock() as block:
            last = None
            while deadline is None or time.monotonic() < deadline:
                snapshot = client.snapshot()
                ego = ego_ground(snapshot, origin)
                if ego is not None:
                    start, points = ahead(xz, ego)
                    key = (start, round(ego[1], 2))
                    if key != last:
                        # ttl 0: this process refreshes the block; the lease ends it.
                        block.write(points, stamp_us=snapshot.get("paused_simulation_time_us", 0), ttl_ms=0,
                                    width_m=width, rgba=(1.0, 1.0, 1.0, alpha), ground_y=ego[1], valid=len(points) >= 2)
                        if last is None or key[0] != last[0]:
                            report(f"path from point {start}: {len(points)} points, ground Y {ego[1]:.2f}")
                        last = key
                time.sleep(1)
                owned("lease", action="heartbeat")
            block.clear()
    finally:
        try:
            owned("overlay", action="disable")
        except Exception as error:
            report(f"overlay disable: {error}")
        try:
            owned("lease", action="release")
        except Exception as error:
            report(f"Lease release: {error}")
    return 0


def clear():
    with OverlayBlock() as block:
        block.clear()
    return {"cleared": True}


def _png(frame, output):
    import numpy as np
    from PIL import Image
    width, height, fmt = frame["width"], frame["height"], frame["dxgi_format"]
    raw = Path(frame["file"]).read_bytes()
    if fmt in (28, 29, 87, 91):  # R8G8B8A8 / B8G8R8A8 (UNORM, SRGB)
        pixels = np.frombuffer(raw, np.uint8).reshape(height, width, 4)[..., :3]
        if fmt in (87, 91):
            pixels = pixels[..., ::-1]
    elif fmt == 24:  # R10G10B10A2_UNORM
        v = np.frombuffer(raw, "<u4").reshape(height, width)
        pixels = np.stack([(v >> s) & 1023 for s in (0, 10, 20)], -1) >> 2
    elif fmt == 10:  # R16G16B16A16_FLOAT, clamped
        pixels = np.clip(np.frombuffer(raw, "<f2").reshape(height, width, 4)[..., :3], 0, 1) * 255
    else:
        raise ValueError(f"No PNG conversion for DXGI format {fmt}")
    Image.fromarray(np.ascontiguousarray(pixels.astype(np.uint8))).save(output)


def capture(client, directory=None, png=False):
    """Two consecutive drawn frames of main0/composition_out after the decal (T3)."""
    arguments = {"action": "capture"}
    if directory:
        arguments["directory"] = str(Path(directory).resolve())
    result = client.request("overlay", **arguments)
    if png:
        for frame in result["frames"]:
            output = Path(frame["file"]).with_suffix(".png")
            _png(frame, output)
            frame["png"] = str(output)
    return result
