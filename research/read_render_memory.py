"""Read CPU metadata for ETS2 1.61.1.1's DX11 render graph.

No GPU pixels, COM calls, injection, input or writes. Reuses the external reader.
Offsets were traced in this installed EXE and Windows d3d11.dll; see docs/12.
Graph lists are rebuilt during each frame. Samples can be empty or inconsistent
across reads and must not be treated as synchronized camera frames.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import struct
import time

from read_live_memory import Reader


class RenderReader(Reader):
    def __init__(self, pid, base, d3d_base):
        super().__init__(pid, base)
        self.d3d_base = d3d_base
        self.texture_vtables = set()

    def string(self, address):
        return self.read(address, 384).split(b"\0", 1)[0].decode("utf-8", "replace") if address else ""

    def texture2d(self, address):
        raw = self.read(address, 0x124)
        vtable = struct.unpack_from("<Q", raw)[0]
        if vtable not in self.texture_vtables:
            # This decoder follows the field-copy GetDesc in the observed DLL.
            # A different implementation needs its own layout, not these offsets.
            getter = self.ptr(vtable + 10 * 8)
            if getter - self.d3d_base != 0x59CE0:
                raise ValueError(f"unmapped Texture2D GetDesc implementation {getter:#x}")
            self.texture_vtables.add(vtable)
        bind = struct.unpack_from("<H", raw, 0xE4)[0]
        misc = struct.unpack_from("<I", raw, 0xEC)[0] & 0x7EFBF7
        if raw[0xE0] & 8:
            bind &= 0xFFFFFF77
            misc = (misc & 0xFFFFFEFD) | 0x8000
        return {"resource": hex(address), "width": struct.unpack_from("<I", raw, 0x118)[0],
                "height": struct.unpack_from("<I", raw, 0x11C)[0], "mip_levels": raw[0xE8],
                "array_size": struct.unpack_from("<H", raw, 0x122)[0],
                "format": struct.unpack_from("<h", raw, 0xD4)[0],
                "sample_count_quality": list(struct.unpack_from("<2I", raw, 0xD8)),
                "usage": struct.unpack_from("<b", raw, 0xCA)[0], "bind_flags": hex(bind),
                "cpu_access_flags": hex(struct.unpack_from("<H", raw, 0xE2)[0] << 16),
                "misc_flags": hex(misc)}

    def graph(self):
        renderer = self.ptr(self.base + 0x3681860)
        _, image_data, image_size, image_capacity = struct.unpack("<4Q", self.read(renderer + 0x363FE58, 32))
        if image_size > image_capacity or not image_data:
            raise ValueError("inconsistent external DX11 image pool")
        result = {"renderer": hex(renderer), "buffers": []}
        result["alias_array"], raw = self.array(self.base + 0x2D2B938, stride=16, limit=256)
        result["aliases"] = [{"texture_id": hex(struct.unpack_from("<H", raw, i)[0]),
                              "graph_id": struct.unpack_from("<I", raw, i + 4)[0],
                              "mask": hex(struct.unpack_from("<Q", raw, i + 8)[0])}
                             for i in range(0, len(raw), 16)]
        ctrl = self.ptr(self.base + 0x36AE6D8)
        result["submission_array"], raw = self.array(ctrl + 0x3AC0, stride=0x98, limit=9)
        result["submitted_mirror_indices"] = [struct.unpack_from("<I", raw, i + 0x10)[0]
                                               for i in range(0, len(raw), 0x98)]
        result["current_buffer_before_read"] = struct.unpack("<I", self.read(self.base + 0x3050448, 4))[0]
        for buffer in range(3):
            info, raw = self.array(self.base + 0x304FC50 + 0x2A8 * buffer, stride=0x7F0, limit=512)
            entries = []
            for offset in range(0, len(raw), 0x7F0):
                item = {"index": offset // 0x7F0}
                try:
                    item["name"] = self.string(struct.unpack_from("<Q", raw, offset + 8)[0])
                    item["namespace"] = self.string(struct.unpack_from("<Q", raw, offset + 0xA8)[0])
                    item["graph_format"] = raw[offset + 0x1E4]
                    item["graph_dimensions"] = list(struct.unpack_from("<2I", raw, offset + 0x1EC))
                    image_id = struct.unpack_from("<H", raw, offset + 0x740)[0]
                    item["image_id"] = image_id
                    if image_id != 0xFFFF and image_id < image_size:
                        resource = self.ptr(image_data + image_id * 0xA8 + 0x58)
                        if resource:
                            item["dx11"] = self.texture2d(resource)
                except (OSError, ValueError) as exc:
                    item["read_error"] = str(exc)
                entries.append(item)
            result["buffers"].append({"buffer": buffer, "array": info, "entries": entries})
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--base", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--d3d-base", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--hz", type=float, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.seconds <= 30 or not 0 < args.hz <= 10:
        parser.error("observation budget: 0 < seconds <= 30 and 0 < hz <= 10")
    reader = RenderReader(args.pid, args.base, args.d3d_base)
    print(json.dumps({"observer_pid": os.getpid(), "target_pid": args.pid}), flush=True)
    try:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps({"type": "metadata", "utc": datetime.now(timezone.utc).isoformat(),
                "target_pid": args.pid, "module_base": hex(args.base), "d3d_base": hex(args.d3d_base),
                "access_mask": "0x1010", "coherent_frame": False, "gpu_pixels": False,
                "build_scope": "ETS2 1.61.1.1; local d3d11.dll GetDesc RVA 0x59CE0"}) + "\n")
            start = time.monotonic()
            count = 0
            while time.monotonic() - start < args.seconds:
                row = {"type": "sample", "elapsed_s": time.monotonic() - start}
                try:
                    row["graph"] = reader.graph()
                except (OSError, ValueError) as exc:
                    row["read_error"] = str(exc)
                row["read_duration_ms"] = (time.monotonic() - start - row["elapsed_s"]) * 1000
                stream.write(json.dumps(row) + "\n")
                count += 1
                time.sleep(max(0, min(start + count / args.hz, start + args.seconds) - time.monotonic()))
            print(json.dumps({"samples": count, "bytes_read": reader.bytes_read, "read_calls": reader.calls}), flush=True)
    finally:
        reader.close()
        print("Read-only process handle closed.", flush=True)


if __name__ == "__main__":
    main()
