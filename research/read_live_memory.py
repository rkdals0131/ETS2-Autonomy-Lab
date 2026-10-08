"""Bounded external observation of this lab's ETS2 1.61.1.1 build.

Uses PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION only. No injection,
debug attach, remote calls, input, or process-memory writes. Offsets come from
the pinned ETS2LA source and findings/ets2la_resolved_offsets.json. The mirror
owner array was traced in this local EXE; see live/2026-10-08-idle/.
Reads are asynchronous observations, not a coherent game-frame snapshot.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import struct
import time


K = C.WinDLL("kernel32", use_last_error=True)
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
K.ReadProcessMemory.restype = W.BOOL
K.CloseHandle.argtypes = [W.HANDLE]
K.CloseHandle.restype = W.BOOL
K.OpenFileMappingW.argtypes = [W.DWORD, W.BOOL, W.LPCWSTR]
K.OpenFileMappingW.restype = W.HANDLE


def placement(data, offset):
    x, y, z, cx, cz, qw, qx, qy, qz = struct.unpack_from("<3f2h4f", data, offset)
    return {"local_xyz": [x, y, z], "cell_xz": [cx, cz],
            "world_xyz": [x + 512 * cx, y, z + 512 * cz], "quaternion_wxyz": [qw, qx, qy, qz]}


class Reader:
    def __init__(self, pid, base):
        self.base = base
        self.handle = K.OpenProcess(0x1010, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.bytes_read = 0
        self.calls = 0
        self.unit_names = {}
        self.traffic_types = {}

    def close(self):
        if self.handle:
            if not K.CloseHandle(self.handle):
                raise C.WinError(C.get_last_error())
            self.handle = None

    def read(self, address, size):
        if not address:
            raise ValueError("null source pointer")
        buf = C.create_string_buffer(size)
        done = C.c_size_t()
        self.calls += 1
        if not K.ReadProcessMemory(self.handle, address, buf, size, C.byref(done)) or done.value != size:
            raise OSError(C.get_last_error(), f"ReadProcessMemory at {address:#x}, {size} bytes, read {done.value}")
        self.bytes_read += size
        return buf.raw

    def ptr(self, address):
        return struct.unpack("<Q", self.read(address, 8))[0]

    def unit_name(self, address):
        vtable = self.ptr(address)
        if vtable not in self.unit_names:
            getter = self.ptr(vtable + 5 * 8)
            code = self.read(getter, 8)
            # Decode the observed `lea rax,[rip+disp32]; ret` getter, never call it.
            if code[:3] != b"\x48\x8d\x05" or code[7] != 0xC3:
                raise ValueError(f"unsupported unit descriptor getter at {getter:#x}")
            descriptor = getter + 7 + struct.unpack_from("<i", code, 3)[0]
            name = self.read(self.ptr(self.ptr(descriptor)), 96).split(b"\0", 1)[0].decode("ascii", "replace")
            self.unit_names[vtable] = name
        return self.unit_names[vtable]

    def array(self, address, stride=8, limit=64):
        _, pointer, size, capacity = struct.unpack("<4Q", self.read(address, 32))
        info = {"address": hex(address), "data": hex(pointer), "size": size, "capacity": capacity}
        if size > capacity or (size and not pointer):
            raise ValueError(f"inconsistent external array: {info}")
        count = min(size, limit)
        info["read_count"] = count
        info["truncated_for_read_budget"] = size > count
        return info, self.read(pointer, count * stride) if count else b""

    def traffic_type(self, address):
        vtable = self.ptr(address)
        if vtable not in self.traffic_types:
            getter = self.ptr(vtable + 8)
            code = self.read(getter, 6)
            # Observed get_type implementations: mov eax,imm32; ret.
            if code[0] != 0xB8 or code[5] != 0xC3:
                raise ValueError(f"unsupported traffic type getter at {getter:#x}: {code.hex()}")
            self.traffic_types[vtable] = struct.unpack_from("<I", code, 1)[0]
        return self.traffic_types[vtable]

    def world(self):
        """Nearby prefab signals and traffic actors from the pinned source paths."""
        ctrl = self.ptr(self.base + 0x36AE6D8)
        traffic = self.ptr(self.base + 0x36AE728)
        objects, raw = self.array(traffic + 0x1E0, limit=512)
        candidates = {p: [{"list": "traffic_objects_1"}] for (p,) in struct.iter_unpack("<Q", raw) if p}
        kdops, raw = self.array(ctrl + 0x650, limit=1024)
        item_types = {}
        prefabs = []
        errors = []
        for (ptr,) in struct.iter_unpack("<Q", raw):
            if not ptr:
                continue
            try:
                item = self.read(ptr, 0x50)
                kind = item[0xA]
                item_types[str(kind)] = item_types.get(str(kind), 0) + 1
                if kind != 4:
                    continue
                uid = f"{struct.unpack_from('<Q', item, 0x40)[0]:016x}"
                segment = struct.unpack_from("<Q", item, 0x48)[0]
                prefab = {"address": hex(ptr), "uid": uid, "segment": hex(segment)}
                if segment:
                    info, instances = self.array(segment + 0x80, stride=16, limit=128)
                    prefab["semaphore_array"] = info
                    for identifier, _, actor in struct.iter_unpack("<IIQ", instances):
                        if actor:
                            candidates.setdefault(actor, []).append({"prefab_uid": uid, "semaphore_id": identifier})
                prefabs.append(prefab)
            except (OSError, ValueError) as exc:
                errors.append({"address": hex(ptr), "read_error": str(exc)})
        actors = []
        for ptr, origins in candidates.items():
            actor = {"address": hex(ptr), "origins": origins}
            try:
                kind = self.traffic_type(ptr)
                actor["traffic_type"] = kind
                if kind in (1, 2, 3, 4, 5, 6, 7):
                    body = self.read(ptr + 0x28, 0x38)
                    actor.update(placement=placement(body, 0), aabb_raw=list(struct.unpack_from("<6f", body, 0x20)))
                if kind in (5, 6):
                    actor["slave_trailer_raw_pointer"] = hex(self.ptr(ptr + 0x88))
                if kind == 7:
                    rule = self.ptr(ptr + 0x100)
                    actor["rule"] = hex(rule)
                    rule_type = self.traffic_type(rule) if rule else None
                    actor["rule_type"] = rule_type
                    if rule_type == 0x2000:
                        actor["signal_state"] = struct.unpack("<I", self.read(rule + 0x70, 4))[0]
                        actor["state_time_remaining_raw"] = struct.unpack("<f", self.read(rule + 0xB8, 4))[0]
                    else:
                        actor["gate_state_raw"] = struct.unpack("<I", self.read(ptr + 0x108, 4))[0]
                        actor["gate_animation_mode"] = self.read(ptr + 0x136, 1)[0]
                        actor["gate_open_time_remaining_raw"] = struct.unpack("<f", self.read(ptr + 0x11C, 4))[0]
                        actor["gate_animation_time_elapsed_raw"] = struct.unpack("<f", self.read(ptr + 0x13C, 4))[0]
            except (OSError, ValueError) as exc:
                actor["read_error"] = str(exc)
            actors.append(actor)
        return {"traffic_objects_array": objects, "nearby_kdop_array": kdops,
                "nearby_item_type_counts": item_types, "prefabs": prefabs, "actors": actors, "item_errors": errors}

    def camera(self, ptr):
        raw = self.read(ptr, 0xA0)
        return {"vtable": hex(struct.unpack_from("<Q", raw)[0]),
                "unit_class": self.unit_name(ptr),
                "fov_near_far_raw": list(struct.unpack_from("<3f", raw, 0x20)),
                "h_v_fov_raw": list(struct.unpack_from("<2f", raw, 0x38)),
                "placement": placement(raw, 0x40),
                "projection_raw": list(struct.unpack_from("<16f", raw, 0x60))}

    def mirrors(self):
        ctrl = self.ptr(self.base + 0x36AE6D8)
        actor = self.ptr(ctrl + 0x31B0)
        if not actor:
            return {"unavailable": "no player actor"}
        interior = self.ptr(actor + 0x20)
        if not interior:
            return {"unavailable": "no visual interior"}
        kind = self.unit_name(interior)
        if kind != "visual_interior":
            return {"unavailable": "owner is not visual_interior", "owner_class": kind}
        info, pointers = self.array(interior + 0x13A8, limit=32)
        result = {"owner": hex(interior), "owner_class": kind, "array": info, "entries": []}
        for index, (ptr,) in enumerate(struct.iter_unpack("<Q", pointers)):
            item = {"index": index, "address": hex(ptr)}
            if ptr:
                try:
                    item.update(self.camera(ptr))
                except (OSError, ValueError) as exc:
                    item["read_error"] = str(exc)
            result["entries"].append(item)
        return result

    def cameras(self):
        manager = self.ptr(self.base + 0x36AE740)
        current, field14 = struct.unpack("<2I", self.read(manager + 0x10, 8))
        info, pointers = self.array(manager + 0x30)
        result = {"manager": hex(manager), "current_index": current, "unknown_manager_0x14": field14,
                  "array": info, "entries": []}
        for index, (ptr,) in enumerate(struct.iter_unpack("<Q", pointers)):
            item = {"index": index, "address": hex(ptr), "active": index == current}
            if ptr:
                try:
                    item.update(self.camera(ptr))
                except (OSError, ValueError) as exc:
                    item["read_error"] = str(exc)
            result["entries"].append(item)
        return result

    def player(self):
        ctrl = self.ptr(self.base + 0x36AE6D8)
        actor = self.ptr(ctrl + 0x31B0)
        vehicle = self.ptr(actor + 0x18) if actor else 0
        result = {"controller": hex(ctrl), "actor": hex(actor), "vehicle": hex(vehicle)}
        result["controller_class"] = self.unit_name(ctrl)
        if vehicle:
            result["aabb_raw"] = list(struct.unpack("<6f", self.read(vehicle + 0x18, 24)))
            result["steering_raw"] = struct.unpack("<f", self.read(vehicle + 0x690, 4))[0]
            result["trailer_actor"] = hex(self.ptr(vehicle + 0x1580))
        return result

    def traffic(self):
        manager = self.ptr(self.base + 0x36AE728)
        result = {"manager": hex(manager), "lists": {}}
        for name, offset, stride in (("spawned_vehicles_1", 0xF0, 16), ("spawned_vehicles_2", 0x118, 16),
                                     ("player_vehicles_1", 0x208, 8)):
            info, pointers = self.array(manager + offset, stride=stride, limit=128)
            items = []
            for off in range(0, len(pointers), stride):
                ptr = struct.unpack_from("<Q", pointers, off)[0]
                item = {"address": hex(ptr)}
                if ptr:
                    try:
                        raw = self.read(ptr + 0x28, 0x38)
                        item.update(placement=placement(raw, 0), aabb_raw=list(struct.unpack_from("<6f", raw, 0x20)))
                        if name.startswith("spawned"):
                            physics = self.ptr(ptr + 0x238)
                            item["physics"] = hex(physics)
                            if physics:
                                item["speed_acceleration_raw"] = list(struct.unpack("<2f", self.read(physics + 0x70, 8)))
                        else:
                            item["speed_acceleration_raw"] = list(struct.unpack("<2f", self.read(ptr + 0x218, 8)))
                            wrapper = self.ptr(ptr + 0x208)
                            item["player_vehicle_object"] = hex(self.ptr(wrapper + 8)) if wrapper else None
                    except (OSError, ValueError) as exc:
                        item["read_error"] = str(exc)
                items.append(item)
            result["lists"][name] = {"array": info, "entries": items}
        return result

    def route(self, limit=16):
        ctrl = self.ptr(self.base + 0x36AE6D8)
        if self.unit_name(ctrl) != "game_ctrl":
            return {"unavailable": "base controller is not game_ctrl"}
        gps = ctrl + 0x4128
        # gps_manager has its own vptr; embedded simple_route_source starts at +8.
        task = self.ptr(gps + 8 + 0x20)
        result = {"gps_candidate": hex(gps), "task": hex(task),
                  "trip_distance_time_checkpoint_raw": list(struct.unpack("<4f", self.read(gps + 0x274, 16)))}
        if task:
            info, raw = self.array(task + 0x50, stride=0x20, limit=limit)
            result["physical_route_array"] = info
            result["physical_route_prefix"] = []
            for off in range(0, len(raw), 0x20):
                node = struct.unpack_from("<Q", raw, off)[0]
                item = {"node": hex(node), "distance_time_totals_raw": list(struct.unpack_from("<4f", raw, off + 0xC))}
                try:
                    node_data = self.read(node, 0x38)
                    item["node_coords_int_raw"] = list(struct.unpack_from("<3i", node_data))
                    item["node_uid_hex"] = f"{struct.unpack_from('<Q', node_data, 0x30)[0]:016x}"
                except (OSError, ValueError) as exc:
                    item["read_error"] = str(exc)
                result["physical_route_prefix"].append(item)
        return result


def mapping_presence():
    result = {}
    for name in ("SCSTelemetry", "ETS2LAPluginStatus", "ETS2LACameraProps", "ETS2LATraffic",
                 "ETS2LARoute", "ETS2LA_Capture_Control", "ETS2LA_Controller"):
        handle = K.OpenFileMappingW(4, False, "Local\\" + name)
        if handle:
            result[name] = {"opened_read_only": True}
            K.CloseHandle(handle)
        else:
            result[name] = {"opened_read_only": False, "winerror": C.get_last_error()}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--base", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--hz", type=float, default=2)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--world", action="store_true", help="also read nearby prefab signals and parked actors")
    parser.add_argument("--mirrors", action="store_true", help="also read cameras owned by visual_interior")
    args = parser.parse_args()
    if not 0 < args.seconds <= 60 or not 0 < args.hz <= 5:
        parser.error("observation budget: 0 < seconds <= 60 and 0 < hz <= 5")
    reader = Reader(args.pid, args.base)
    print(json.dumps({"observer_pid": os.getpid(), "target_pid": args.pid, "seconds": args.seconds}), flush=True)
    try:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps({"type": "metadata", "utc": datetime.now(timezone.utc).isoformat(),
                "target_pid": args.pid, "module_base": hex(args.base), "access_mask": "0x1010",
                "layout_source": "ETS2LA/plugin@3b01d90b5be2469c0d94fcfdee4b354184ddeb02 + local mirror-array disassembly",
                "build_scope": "ETS2 1.61.1.1, previously analyzed local EXE",
                "coherent_frame": False, "mappings": mapping_presence()}) + "\n")
            start = time.monotonic()
            count = 0
            while time.monotonic() - start < args.seconds:
                row = {"type": "sample", "reader_elapsed_s": time.monotonic() - start}
                for name in ("cameras", "player", "traffic", "route") + (("world",) if args.world else ()) + (("mirrors",) if args.mirrors else ()):
                    try:
                        row[name] = getattr(reader, name)()
                    except (OSError, ValueError) as exc:
                        row[name] = {"read_error": str(exc)}
                row["read_duration_ms"] = (time.monotonic() - start - row["reader_elapsed_s"]) * 1000
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                count += 1
                time.sleep(max(0, min(start + count / args.hz, start + args.seconds) - time.monotonic()))
            print(json.dumps({"samples": count, "read_calls": reader.calls, "bytes_read": reader.bytes_read,
                              "output": str(args.output)}), flush=True)
    finally:
        reader.close()
        print("Read-only process handle closed.", flush=True)


if __name__ == "__main__":
    main()
