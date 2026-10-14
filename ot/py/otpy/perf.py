"""One labeled performance sample of the running game, and a table of several.

A sample is PresentMon frame times plus what explains them: core counter deltas
(sensor and native mirror renders, hook and SDK callback time, stream drops),
traffic around the truck, CPU of the game and helper processes, whether the game
window stayed in front, CPU per game thread, and optionally a main-thread
profile with call stacks (saved beside the sample as <label>.stacks.json for
`ot.cmd hotspots`). Everything here
only reads; conditions are changed by the caller between samples, alternating
A/B so that scene drift (traffic, time of day) shows up as spread.
"""
import csv
import ctypes as C
import json
import math
import statistics
import subprocess
import threading
import time
from ctypes import wintypes as W
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAME = "eurotrucks2.exe"
HELPERS = ("Foxglove.exe", "ets2_relay.exe", "foxglove_bridge.exe", "pythonw.exe")
K = C.WinDLL("kernel32", use_last_error=True)
U = C.WinDLL("user32", use_last_error=True)
K.OpenProcess.restype = K.CreateToolhelp32Snapshot.restype = W.HANDLE
U.GetForegroundWindow.restype = W.HWND


class _Process(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("pid", W.DWORD), ("heap", C.c_size_t),
                ("module", W.DWORD), ("threads", W.DWORD), ("parent", W.DWORD), ("base", W.LONG),
                ("flags", W.DWORD), ("image", W.WCHAR * 260)]


def processes():
    """{image name: [pid, ...]} for every process."""
    snapshot = K.CreateToolhelp32Snapshot(0x2, 0)
    entry, found = _Process(size=C.sizeof(_Process)), {}
    more = K.Process32FirstW(snapshot, C.byref(entry))
    while more:
        found.setdefault(entry.image.lower(), []).append(entry.pid)
        more = K.Process32NextW(snapshot, C.byref(entry))
    K.CloseHandle(snapshot)
    return found


def cpu_seconds(pid):
    handle = K.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    times = [W.FILETIME() for _ in range(4)]
    ok = K.GetProcessTimes(handle, *[C.byref(t) for t in times])
    K.CloseHandle(handle)
    if not ok:
        return None
    return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7


def helper_cpu(names):
    table = processes()
    return {name: {pid: cpu_seconds(pid) for pid in table.get(name.lower(), [])} for name in names}


class _Thread(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("tid", W.DWORD), ("pid", W.DWORD),
                ("base_priority", W.LONG), ("delta_priority", W.LONG), ("flags", W.DWORD)]


class _Module(C.Structure):
    _fields_ = [("size", W.DWORD), ("module_id", W.DWORD), ("pid", W.DWORD), ("global_usage", W.DWORD),
                ("process_usage", W.DWORD), ("base", C.c_void_p), ("bytes", W.DWORD), ("handle", W.HMODULE),
                ("name", W.WCHAR * 256), ("path", W.WCHAR * 260)]


K.OpenThread.restype = W.HANDLE
K.LocalFree.argtypes = [C.c_void_p]
_NT = C.WinDLL("ntdll")
_NT.NtQueryInformationThread.argtypes = [W.HANDLE, W.ULONG, C.c_void_p, W.ULONG, C.POINTER(W.ULONG)]
_NT.NtQueryInformationThread.restype = W.LONG
_describe = getattr(K, "GetThreadDescription", None)
if _describe:
    _describe.argtypes = [W.HANDLE, C.POINTER(C.c_void_p)]
    _describe.restype = W.LONG


def _modules(pid):
    for _ in range(5):  # ERROR_BAD_LENGTH while the module list changes; retry.
        snapshot = K.CreateToolhelp32Snapshot(0x18, pid)  # MODULE | MODULE32
        if snapshot and snapshot != W.HANDLE(-1).value:
            break
        time.sleep(0.01)
    else:
        return []
    entry, found = _Module(size=C.sizeof(_Module)), []
    more = K.Module32FirstW(snapshot, C.byref(entry))
    while more:
        found.append((entry.base or 0, entry.bytes, entry.name))
        more = K.Module32NextW(snapshot, C.byref(entry))
    K.CloseHandle(snapshot)
    return found


def threads(pid):
    """{tid: {name, start, cpu_s}} for one process; start is module+RVA of the thread routine."""
    modules = _modules(pid)
    snapshot = K.CreateToolhelp32Snapshot(0x4, 0)
    entry, found = _Thread(size=C.sizeof(_Thread)), {}
    more = K.Thread32First(snapshot, C.byref(entry))
    while more:
        if entry.pid == pid:
            found[entry.tid] = {}
        more = K.Thread32Next(snapshot, C.byref(entry))
    K.CloseHandle(snapshot)
    for tid, info in found.items():
        handle = K.OpenThread(0x0840, False, tid)  # QUERY_INFORMATION | QUERY_LIMITED_INFORMATION
        if not handle:
            continue
        times = [W.FILETIME() for _ in range(4)]
        if K.GetThreadTimes(handle, *[C.byref(t) for t in times]):
            info["cpu_s"] = sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7
        text = C.c_void_p()
        if _describe and _describe(handle, C.byref(text)) >= 0 and text.value:
            info["name"] = C.wstring_at(text.value)
            K.LocalFree(text)
        start = C.c_void_p()
        if _NT.NtQueryInformationThread(handle, 9, C.byref(start), C.sizeof(start), None) >= 0 and start.value:
            info["start"] = hex(start.value)  # ThreadQuerySetWin32StartAddress
            for base, size, name in modules:
                if base <= start.value < base + size:
                    info["start"] = f"{name}+0x{start.value - base:X}"
                    break
        K.CloseHandle(handle)
    return found


def thread_cores(before, after, wall):
    """Game threads sorted by CPU cores used between two threads() calls."""
    rows = []
    for tid, info in after.items():
        if "cpu_s" in info and "cpu_s" in before.get(tid, {}):
            rows.append({"tid": tid, "name": info.get("name", ""), "start": info.get("start", ""),
                         "cores": (info["cpu_s"] - before[tid]["cpu_s"]) / wall})
    return sorted(rows, key=lambda r: -r["cores"])


def foreground_pid():
    pid = W.DWORD()
    U.GetWindowThreadProcessId(U.GetForegroundWindow(), C.byref(pid))
    return pid.value


def presentmon():
    found = sorted((ROOT / "research" / "tools").glob("PresentMon-*.exe"))
    if not found:
        raise RuntimeError("PresentMon not found in research/tools")
    return found[-1]


def _try(client, command, **arguments):
    try:
        return client.request(command, **arguments)
    except Exception as error:  # A stopped relay or Tier 0 leaves some counters unavailable.
        return {"unavailable": str(error)}


def counters(client):
    hooks = _try(client, "hooks")
    rig = _try(client, "camera_rig")
    stream = _try(client, "stream", action="status")
    snapshot = _try(client, "snapshot")
    passes = _try(client, "parallel_passes")
    result = {"qpc": time.perf_counter(), "frame_id": snapshot.get("frame_id"), "paused": snapshot.get("paused"),
              "tier": _try(client, "tier").get("tier"), "sdk_timing": _try(client, "sdk_timing"),
              "qpc_frequency": hooks.get("qpc_frequency"),
              "hooks": {h.get("name", str(i)): h.get("timing", {}) for i, h in enumerate(hooks.get("hooks", []))},
              "rig": {str(v["slot"]): {"applied": v.get("applied", 0), "native_applied": v.get("native_applied", 0)}
                      for v in rig.get("views", []) if isinstance(v, dict) and "slot" in v},
              "native_mirrors": rig.get("native_mirrors"), "native_suppressed": rig.get("native_suppressed", 0),
              "stream": {k: stream.get(k) for k in ("armed", "completed", "published", "queue_dropped", "gpu_ring_busy",
                                                      "same_frame_skipped", "failed", "canceled") if k in stream},
              # docs/28 M3a: counters difference over the sample; keys are cumulative.
              "parallel_passes": {k: passes.get(k) for k in ("requested_mode", "frames", "passes", "join", "defer_frame_mean",
                                                             "keys", "key_count", "off_reason", "error", "game_folder_dxgi")
                                  if k in passes}}
    placement = (snapshot.get("sdk") or {}).get("truck.world.placement")
    if isinstance(placement, dict) and {"x", "z"} <= placement.keys():
        result["truck_xy"] = [placement["x"], -placement["z"]]
    return result


def traffic(client, truck_xy, radius=250.0):
    state = _try(client, "traffic")
    vehicles = state.get("vehicles") or []
    result = {"available": state.get("available", False), "spawned": len(vehicles)}
    if truck_xy and vehicles:
        result[f"within_{int(radius)}m"] = sum(math.hypot(v["x"] - truck_xy[0], v["y"] - truck_xy[1]) <= radius
                                               for v in vehicles)
    if state.get("error") or state.get("unavailable"):
        result["error"] = state.get("error") or state.get("unavailable")
    return result


def _delta(after, before):
    if isinstance(after, dict):
        return {k: _delta(v, before.get(k) if isinstance(before, dict) else None) for k, v in after.items()}
    if isinstance(after, (int, float)) and not isinstance(after, bool) and isinstance(before, (int, float)):
        return after - before
    return after


def frame_stats(path):
    with open(path, encoding="utf-8-sig", newline="") as source:
        rows = [r for r in csv.DictReader(source) if r["Application"].lower() == GAME]
    if len(rows) < 2:
        raise RuntimeError(f"PresentMon recorded {len(rows)} frames; is the game rendering?")

    def column(name):
        return sorted(float(r[name]) for r in rows if r.get(name) not in (None, "", "NA"))
    frame, busy, gpu = column("MsBetweenPresents"), column("MsCPUBusy"), column("MsGPUBusy")
    span = (float(rows[-1]["TimeInMs"]) - float(rows[0]["TimeInMs"])) / 1000
    return {"frames": len(rows), "seconds": span, "fps": len(rows) / span,
            "frame_ms_p50": statistics.median(frame), "frame_ms_p99": frame[int(.99 * (len(frame) - 1))],
            "cpu_busy_ms_p50": statistics.median(busy) if busy else None,
            "gpu_busy_ms_p50": statistics.median(gpu) if gpu else None}


def fetch_stacks(client):
    """All pages of the last main_profile's distinct stacks."""
    page = client.request("main_profile", action="stacks", offset=0)
    result = {"samples": page["samples"], "images": page["images"], "stacks": page["stacks"]}
    while page.get("next") is not None:
        page = client.request("main_profile", action="stacks", offset=page["next"])
        result["stacks"].extend(page["stacks"])
    return result


def sample(client, label, seconds=30.0, out=None, profile_hz=0.0, profile_depth=48):
    out = Path(out) if out else ROOT / "research" / "live" / f"{date.today():%Y-%m-%d}-perf"
    out.mkdir(parents=True, exist_ok=True)
    csv_path, json_path = out / f"{label}.csv", out / f"{label}.json"
    if csv_path.exists() or json_path.exists():
        raise FileExistsError(f"{label} already exists in {out}")
    game = processes().get(GAME, [])
    if not game:
        raise RuntimeError("ETS2 is not running")
    before = counters(client)
    traffic_before = traffic(client, before.get("truck_xy"))
    cpu_before = helper_cpu((GAME,) + HELPERS)
    # Stamp each thread snapshot at its midpoint; counters() stamps differ by hundreds of ms.
    stamp = time.perf_counter()
    threads_before = threads(game[0])
    threads_before_s = (stamp + time.perf_counter()) / 2
    if profile_hz:
        client.request("main_profile", action="start", hz=profile_hz, seconds=seconds, depth=profile_depth)
    front, polls, done = [0], [0], threading.Event()

    def watch():
        while not done.wait(0.5):
            polls[0] += 1
            front[0] += foreground_pid() in game
    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    subprocess.run([str(presentmon()), "--process_name", GAME, "--output_file", str(csv_path), "--timed", str(int(seconds)),
                    "--terminate_after_timed", "--stop_existing_session", "--session_name", "ot_perf", "--no_console_stats"],
                   capture_output=True, check=False)
    done.set()
    watcher.join()
    after = counters(client)
    cpu_after = helper_cpu((GAME,) + HELPERS)
    stamp = time.perf_counter()
    threads_after = threads(game[0])
    threads_wall = (stamp + time.perf_counter()) / 2 - threads_before_s
    traffic_after = traffic(client, after.get("truck_xy"))
    frames = frame_stats(csv_path)
    wall = after["qpc"] - before["qpc"]
    change = _delta(after, before)
    frequency = after.get("qpc_frequency") or 1
    renders = sum(v["applied"] for v in change["rig"].values()) if change.get("rig") else 0
    native = sum(v["native_applied"] for v in change["rig"].values()) if change.get("rig") else 0
    hook_ms = {name: t.get("total_ticks", 0) / frequency * 1000 for name, t in change["hooks"].items()
               if isinstance(t, dict) and t.get("total_ticks")}
    sdk = change.get("sdk_timing") or {}
    sdk_frequency = (after.get("sdk_timing") or {}).get("qpc_frequency") or 1
    sdk_ms = {k: v["total_ticks"] / sdk_frequency * 1000 for k, v in sdk.items()
              if isinstance(v, dict) and isinstance(v.get("total_ticks"), (int, float))}

    def cpu(name):
        total = 0.0
        for pid, seconds_after in cpu_after.get(name, {}).items():
            seconds_before = cpu_before.get(name, {}).get(pid)
            if seconds_after is not None and seconds_before is not None:
                total += seconds_after - seconds_before
        return total / wall
    summary = {
        "label": label, "seconds": seconds, "date": time.strftime("%Y-%m-%d %H:%M:%S"), **frames,
        "foreground_share": front[0] / polls[0] if polls[0] else None,
        "paused": bool(before.get("paused") or after.get("paused")), "tier": [before.get("tier"), after.get("tier")],
        "sensor_renders_per_s": renders / wall, "native_mirror_renders_per_s": native / wall,
        "native_mirrors": after.get("native_mirrors"), "stream": change.get("stream"),
        "hook_ms_per_frame": {k: v / frames["frames"] for k, v in hook_ms.items()},
        "sdk_callback_ms_per_s": {k: v / wall for k, v in sdk_ms.items()},
        "sdk_frames_per_s": (change.get("frame_id") or 0) / wall,
        "parallel_passes": {**(change.get("parallel_passes") or {}),
                            "requested_mode": [before.get("parallel_passes", {}).get("requested_mode"),
                                               after.get("parallel_passes", {}).get("requested_mode")]},
        "cores": {name: cpu(name) for name in (GAME,) + HELPERS},
        "traffic": [traffic_before, traffic_after],
        "thread_count": len(threads_after),
        "threads": thread_cores(threads_before, threads_after, threads_wall)[:24],
    }
    if profile_hz:
        deadline = time.monotonic() + 10
        result = client.request("main_profile")
        while result.get("running") and time.monotonic() < deadline:
            time.sleep(0.2)
            result = client.request("main_profile")
        summary["main_profile"] = result
        summary["main_thread_id"] = result.get("thread_id")
        if profile_depth and not result.get("running") and result.get("samples"):
            stacks_path = out / f"{label}.stacks.json"
            stacks = {"label": label, "hz": profile_hz, "elapsed_s": result.get("elapsed_s"), "fps": frames["fps"],
                      **fetch_stacks(client)}
            stacks_path.write_text(json.dumps(stacks, separators=(",", ":")), encoding="utf-8")
            summary["stacks_file"] = stacks_path.name
    json_path.write_text(json.dumps(summary, indent=1, ensure_ascii=False), encoding="utf-8")
    return summary


def line(s):
    cores = s.get("cores", {})
    traffic_counts = "/".join(str(t.get("within_250m", t.get("spawned", "?"))) for t in s.get("traffic", []))
    hooks = sum(s.get("hook_ms_per_frame", {}).values())
    flags = ("PAUSED " if s.get("paused") else "") + ("BACKGROUND " if (s.get("foreground_share") or 0) < .95 else "")
    modes = (s.get("parallel_passes") or {}).get("requested_mode") or []
    if any(m not in (None, "off") for m in modes):
        flags += "passes=" + "/".join(str(m) for m in modes) + " "
    return (f"{s['label']:24} {s['fps']:5.1f} fps  p50 {s['frame_ms_p50']:5.1f} p99 {s['frame_ms_p99']:5.1f}  "
            f"CPU {s['cpu_busy_ms_p50'] or float('nan'):5.1f} GPU {s['gpu_busy_ms_p50'] or float('nan'):5.1f}  "
            f"sensor {s['sensor_renders_per_s']:5.1f}/s native {s['native_mirror_renders_per_s']:5.1f}/s  "
            f"hooks {hooks:4.2f} ms/f  traffic {traffic_counts:7}  "
            f"viewer {cores.get('Foxglove.exe', 0):4.2f} relay {cores.get('ets2_relay.exe', 0):4.2f} cores  {flags}")


def wait_speed(client, min_kmh, hold=10.0, timeout=1200.0):
    """Block until the truck has held at least min_kmh, unpaused, for `hold` seconds."""
    deadline, since = time.monotonic() + timeout, None
    while time.monotonic() < deadline:
        state = _try(client, "snapshot")
        speed = ((state.get("sdk") or {}).get("truck.speed") or {}).get("value")
        kmh = 3.6 * speed if isinstance(speed, (int, float)) else -1.0
        if kmh >= min_kmh and not state.get("paused", True):
            since = since or time.monotonic()
            if time.monotonic() - since >= hold:
                return kmh
        else:
            since = None
        time.sleep(1.0)
    raise TimeoutError(f"no sustained {min_kmh} km/h within {timeout:.0f} s")


def speed_kmh(client):
    state = _try(client, "snapshot")
    speed = ((state.get("sdk") or {}).get("truck.speed") or {}).get("value")
    return round(3.6 * speed, 1) if isinstance(speed, (int, float)) else None


def passes_abab(client, prefix, pairs=2, seconds=30.0, settle=10.0, profile_hz=0.0, min_kmh=None, out=None, report=print):
    """docs/28 §6: alternate parallel_passes off (A) and defer (B) samples, A first.
    Labels <prefix>-off-<n> and <prefix>-on-<n>; returns to off at the end."""
    if min_kmh:
        report(f"waiting for {min_kmh} km/h")
        wait_speed(client, min_kmh)
    summaries = []
    try:
        for n in range(1, pairs + 1):
            for mode, tag in (("off", "off"), ("defer", "on")):
                client.request("parallel_passes", mode=mode)
                time.sleep(settle)
                before = speed_kmh(client)
                summary = sample(client, f"{prefix}-{tag}-{n}", seconds, out, profile_hz)
                summary["speed_kmh"] = [before, speed_kmh(client)]
                summaries.append(summary)
                report(line(summary) + f" {summary['speed_kmh'][0]}->{summary['speed_kmh'][1]} km/h")
    finally:
        client.request("parallel_passes", mode="off")
    return summaries


def table(paths):
    return "\n".join(line(json.loads(Path(p).read_text(encoding="utf-8"))) for p in paths)
