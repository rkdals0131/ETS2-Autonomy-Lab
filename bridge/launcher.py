"""Windows control window for one owned relay + WSL ROS/Foxglove session."""
from collections import deque
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import queue
import shlex
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "ot" / "py"))
from otpy.client import Client, LoaderClient

K = C.WinDLL("kernel32", use_last_error=True)
for name, result, args in [
    ("CreateMutexW", W.HANDLE, [C.c_void_p, W.BOOL, W.LPCWSTR]),
    ("CreateEventW", W.HANDLE, [C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR]),
    ("CreateJobObjectW", W.HANDLE, [C.c_void_p, W.LPCWSTR]),
    ("SetInformationJobObject", W.BOOL, [W.HANDLE, C.c_int, C.c_void_p, W.DWORD]),
    ("AssignProcessToJobObject", W.BOOL, [W.HANDLE, W.HANDLE]),
    ("WaitForSingleObject", W.DWORD, [W.HANDLE, W.DWORD]),
    ("SetEvent", W.BOOL, [W.HANDLE]),
    ("CloseHandle", W.BOOL, [W.HANDLE]),
]:
    fn = getattr(K, name)
    fn.restype, fn.argtypes = result, args


class Job:
    def __init__(self):
        class Basic(C.Structure):
            _fields_ = [("process_time", C.c_int64), ("job_time", C.c_int64),
                        ("flags", W.DWORD), ("min_working", C.c_size_t),
                        ("max_working", C.c_size_t), ("active", W.DWORD),
                        ("affinity", C.c_size_t), ("priority", W.DWORD), ("scheduling", W.DWORD)]
        class Limits(C.Structure):
            _fields_ = [("basic", Basic), ("io", C.c_uint64 * 6),
                        ("process_memory", C.c_size_t), ("job_memory", C.c_size_t),
                        ("peak_process", C.c_size_t), ("peak_job", C.c_size_t)]
        self.handle = K.CreateJobObjectW(None, None)
        limits = Limits()
        limits.basic.flags = 0x2000  # KILL_ON_JOB_CLOSE
        if not self.handle or not K.SetInformationJobObject(self.handle, 9, C.byref(limits), C.sizeof(limits)):
            self.close()
            raise C.WinError(C.get_last_error())

    def add(self, process):
        if not K.AssignProcessToJobObject(self.handle, W.HANDLE(int(process._handle))):
            process.kill()
            process.wait()
            raise C.WinError(C.get_last_error())

    def close(self):
        if self.handle:
            K.CloseHandle(self.handle)
            self.handle = None


class Controller:
    """All state transitions run on one worker; snapshots report observed state."""
    def __init__(self, config=ROOT / "config" / "bridge.local.json"):
        self.config = Path(config).resolve()
        self.mutex = K.CreateMutexW(None, False, "Local\\ETS2AutonomyLab.Launcher")
        self.show = K.CreateEventW(None, False, False, "Local\\ETS2AutonomyLab.Launcher.Show")
        if not self.mutex or not self.show:
            raise C.WinError(C.get_last_error())
        if K.WaitForSingleObject(self.mutex, 0) not in (0, 0x80):
            K.SetEvent(self.show)
            K.CloseHandle(self.show)
            K.CloseHandle(self.mutex)
            raise FileExistsError("Launcher is already open")
        self.commands, self.lines = queue.Queue(), queue.Queue()
        self.lock = threading.Lock()
        self.logs = deque(maxlen=80)
        self.state = {"phase": "stopped", "message": "시작을 누르면 WSL과 Windows 브리지를 연결합니다.",
                      "game": "확인 중", "config": str(self.config), "wsl": {}, "relay": {},
                      "recording": {"phase": "idle"}, "recording_error": ""}
        self.relay = self.wsl = self.job = self.stop_event = None
        self.unit = None
        self.desired = False
        self.input_only = False
        self.provider = None
        self.drive_request = None
        self.closed = threading.Event()
        self.worker = threading.Thread(target=self._loop, daemon=False)
        self.worker.start()

    def request(self, action, **values):
        self.commands.put((action, values))

    def snapshot(self):
        with self.lock:
            return {**self.state, "logs": list(self.logs)}

    def update(self, **values):
        with self.lock:
            self.state.update(values)

    def log(self, line):
        with self.lock:
            self.logs.append(line)

    @staticmethod
    def linux_paths(*paths):
        result = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", "/bin/sh", "-c",
                                 'for p do wslpath -a "$p" || exit; done', "ets2-launcher", *map(str, paths)],
                                capture_output=True, text=True, encoding="utf-8", timeout=30,
                                creationflags=subprocess.CREATE_NO_WINDOW, check=True)
        return result.stdout.strip().splitlines()

    def spawn(self, command, source):
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                   bufsize=1, creationflags=subprocess.CREATE_NO_WINDOW)
        self.job.add(process)
        def read():
            with process.stdout:
                for line in process.stdout:
                    self.lines.put((source, process.pid, line.rstrip()))
        threading.Thread(target=read, daemon=True).start()
        return process

    def _start(self, duration, camera_hz=None, lidar_hz=None, preview_hz=None, input_only=False):
        if self.desired or self.wsl or self.relay:
            return
        if self.unit:
            raise RuntimeError("이전 WSL 실행의 종료 확인이 필요합니다. 중지를 다시 눌러 주세요.")
        if not self.config.is_file():
            raise RuntimeError("bridge/configure.py를 먼저 실행해 로컬 설정을 만들어 주세요.")
        settings = json.loads(self.config.read_text(encoding="utf-8"))
        changed = any(value is not None for value in (duration, camera_hz, lidar_hz, preview_hz))
        if duration is not None:
            duration = float(duration)
            if not 1 <= duration <= 86400:
                raise ValueError("실행 시간은 1~86400초로 입력해 주세요.")
            settings["duration_s"] = duration
        camera_hz = float(camera_hz if camera_hz is not None else settings.get("camera_hz", 30))
        lidar_hz = float(lidar_hz if lidar_hz is not None else settings.get("lidar_hz", 10))
        preview_hz = float(preview_hz if preview_hz is not None else settings.get("preview_hz", 10))
        if not math.isfinite(camera_hz) or not 0 < lidar_hz <= camera_hz <= 60:
            raise ValueError("주기는 0 < 라이다 ≤ 카메라 ≤ 60 Hz로 입력해 주세요.")
        if not math.isfinite(preview_hz) or not 0 < preview_hz <= camera_hz:
            raise ValueError("미리보기 주기는 0보다 크고 카메라 주기 이하여야 합니다.")
        settings.update(camera_hz=camera_hz, lidar_hz=lidar_hz, preview_hz=preview_hz)
        if changed:
            temporary = self.config.with_name(self.config.name + ".tmp")
            try:
                temporary.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
                os.replace(temporary, self.config)
            finally:
                temporary.unlink(missing_ok=True)
        duration = float(settings.get("duration_s", 60))
        if duration <= 0:
            raise ValueError("duration_s must be positive")
        client = Client(timeout=0.5)
        if client.tier()["tier"] != 0:
            raise RuntimeError("다른 수집 또는 리그가 실행 중입니다. 기존 작업을 먼저 중지해 주세요.")
        if not (ROOT / "dist" / "ets2_relay.exe").is_file():
            raise RuntimeError("bridge/build.cmd를 먼저 실행해 주세요.")
        self.desired = True
        self.input_only = bool(input_only)
        self.started = time.monotonic()
        self.last_wsl = self.last_relay = self.last_heartbeat = 0.0
        self.job = Job()
        self.update(phase="starting", message="Ubuntu의 ROS·Foxglove 서버를 시작합니다.",
                    wsl={}, relay={}, duration_s=duration)
        repo, config = self.linux_paths(ROOT, self.config)
        self.unit = "ets2-launcher-" + uuid.uuid4().hex
        shell = f"source {shlex.quote(repo + '/ros-env.sh')} && exec python3 -u {shlex.quote(repo + '/wsl_session.py')} {shlex.quote(config)}"
        self.wsl = self.spawn(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec",
                              "systemd-run", "--user", "--quiet", "--pipe", "--wait", "--collect",
                              "--unit=" + self.unit, "--property=KillMode=control-group",
                              "--property=RuntimeDirectory=" + self.unit,
                              "--property=TimeoutStopSec=15", "--", "/bin/bash", "-c", shell], "wsl")
        self.started = time.monotonic()
        self.update(wsl_pid=self.wsl.pid, unit=self.unit)

    def _signal_wsl(self, action, **values):
        if self.wsl and self.wsl.poll() is None:
            self.wsl.stdin.write(json.dumps({"action": action, **values}) + "\n")
            self.wsl.stdin.flush()

    def _drive(self, action, speed_kmh=30, acc=True, lcc=True):
        if action == "drive_stop":
            if self.provider and self.provider.poll() is None:
                self.provider.terminate()
                self.provider.wait(timeout=3)
            self.provider = self.drive_request = None
            self._signal_wsl("drive_stop")
            return
        if self.snapshot()["phase"] != "running" or not self.input_only:
            raise RuntimeError("입력·상태만으로 브리지를 시작한 뒤 GT 주행을 눌러 주세요.")
        speed = float(speed_kmh) / 3.6
        if not math.isfinite(speed) or speed < 0:
            raise ValueError("목표 속도는 0 이상의 km/h 값입니다.")
        if not acc and not lcc:
            self._drive("drive_stop")
            return
        if self.provider:
            self.drive_request.update(speed_mps=speed, acc=bool(acc), lcc=bool(lcc))
            return
        if self.snapshot()["wsl"].get("drive_pid"):
            self._signal_wsl("drive_update", speed_mps=speed, acc=bool(acc), lcc=bool(lcc))
            return
        dotnet = ROOT.parent / "research" / "tools" / "dotnet10" / "dotnet.exe"
        provider = ROOT / "build" / "map_lane_provider" / "Release" / "net10.0" / "map_lane_provider.dll"
        if not dotnet.is_file() or not provider.is_file():
            raise RuntimeError("지도 provider 빌드가 필요합니다: bridge/map_lane_provider/README.md")
        output = ROOT / "recordings" / "gt-path" / "current-lane.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        game = Path(LoaderClient(timeout=2).status()["module_path"]).parents[4]
        path, = self.linux_paths(output)
        self.drive_request = {"path": path, "speed_mps": speed, "acc": bool(acc), "lcc": bool(lcc)}
        self.provider = self.spawn([str(dotnet), str(provider), "--game", str(game), "--out", str(output), "--distance-m", "1000"], "lane")
        self.update(message="현재 도로의 실제 차로 기준선을 준비합니다.")

    def _record(self, action, profile="state"):
        if action == "record_start" and self.snapshot()["phase"] != "running":
            self.update(recording_error="브리지 연결 후 기록을 시작해 주세요.")
            return
        self.update(recording_error="")
        self._signal_wsl(action, profile=profile)

    def _start_relay(self):
        name = "Local\\ETS2AutonomyLab.Stop." + uuid.uuid4().hex
        self.stop_event = K.CreateEventW(None, True, False, name)
        if not self.stop_event:
            raise C.WinError(C.get_last_error())
        command = [str(ROOT / "dist" / "ets2_relay.exe"), str(self.config), name]
        if self.input_only:
            command.append("--input-only")
        self.relay = self.spawn(command, "relay")
        self.last_relay = time.monotonic()
        self.update(phase="connecting", message="게임과 ROS 사이의 실제 응답을 기다립니다.", relay_pid=self.relay.pid)

    def _stop(self, phase="stopped", message="중지됨"):
        self.desired = False
        self.drive_request = None
        self.update(phase="stopping", message="수집 해제와 프로세스 종료를 확인합니다.")
        errors = []
        try:
            if self.stop_event:
                K.SetEvent(self.stop_event)
            if self.relay:
                try:
                    self.relay.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    self.relay.kill()
                    self.relay.wait(timeout=3)
                    # The game's existing lease expires five seconds after its last heartbeat.
                    time.sleep(5.1)
            try:
                self._signal_wsl("stop")
            except (OSError, ValueError):
                pass
            if self.wsl:
                try:
                    self.wsl.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    pass
            if self.unit:
                subprocess.run(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", "systemctl", "--user",
                                         "stop", self.unit], capture_output=True, timeout=15,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                check = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", "systemctl", "--user",
                                        "show", self.unit, "-p", "ActiveState", "-p", "MainPID"],
                                       capture_output=True, text=True, timeout=10,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                properties = dict(line.split("=", 1) for line in check.stdout.splitlines() if "=" in line)
                if check.returncode or properties.get("MainPID") != "0" or properties.get("ActiveState") not in ("inactive", "failed"):
                    errors.append("WSL 종료 확인 실패: " + self.unit)
                else:
                    self.unit = None
            # Preserve the recorder's final counts/path before releasing the WSL process.
            while not self.lines.empty():
                source, pid, line = self.lines.get()
                if source == "wsl" and self.wsl and pid == self.wsl.pid:
                    try:
                        value = json.loads(line)
                        if value.get("type") == "recording_status":
                            self.update(recording=value["recording"])
                    except ValueError:
                        pass
        except Exception as error:
            errors.append(str(error))
        finally:
            if self.job:
                self.job.close()
            for process in (self.relay, self.wsl, self.provider):
                if process:
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        errors.append(f"PID {process.pid} 종료 확인 실패")
                    process.stdin.close()
            if self.stop_event:
                K.CloseHandle(self.stop_event)
            self.relay = self.wsl = self.job = self.stop_event = None
        self.update(phase="error" if errors else phase,
                    message="; ".join(errors) if errors else message, relay_pid=None, wsl_pid=None, unit=self.unit)

    def _observe(self):
        now = time.monotonic()
        while not self.lines.empty():
            source, pid, line = self.lines.get()
            process = self.wsl if source == "wsl" else (self.provider if source == "lane" else self.relay)
            if not process or process.pid != pid:
                continue
            try:
                data = json.loads(line)
            except ValueError:
                self.log(source + ": " + line)
                continue
            if source == "wsl" and data.get("type") == "wsl_error":
                raise RuntimeError(data["error"])
            if source == "wsl" and data.get("type") == "recording_error":
                self.update(recording_error=data["error"])
            if source == "wsl" and data.get("type") == "drive_error":
                self.update(message=data["error"])
            if source == "wsl" and data.get("type") == "recording_status":
                self.update(recording=data["recording"])
            if source == "wsl" and data.get("type") == "wsl_status":
                self.last_wsl = now
                self.update(wsl=data, recording=data.get("recording", {"phase": "idle"}))
                if self.relay and not data["listeners_ready"]:
                    raise RuntimeError("ROS 또는 Foxglove 서버 연결이 내려갔습니다.")
                if data["listeners_ready"] and not self.relay:
                    self._start_relay()
            elif source == "relay" and "sent_bundles" in data:
                self.last_relay = now
                self.update(relay=data)
        if self.provider and self.provider.poll() is not None:
            code = self.provider.returncode
            self.provider = None
            request, self.drive_request = self.drive_request, None
            if code:
                raise RuntimeError("현재 도로의 GT 차로 생성 실패: 로그를 확인해 주세요.")
            if request:
                self._signal_wsl("drive_start", **request)
                self.update(message="GT 주행을 시작합니다. 게임 창으로 돌아가세요.")
        if not self.desired:
            return
        if now - self.last_heartbeat > 1:
            self._signal_wsl("heartbeat")
            self.last_heartbeat = now
        if self.wsl and self.wsl.poll() is not None:
            raise RuntimeError("WSL 세션이 종료됐습니다. 로그를 확인한 뒤 다시 시작해 주세요.")
        if self.relay and self.relay.poll() is not None:
            code = self.relay.returncode
            self._stop("stopped" if code == 0 else "error",
                       "실행이 종료됐습니다. 시작을 누르면 새 세션을 엽니다." if code == 0 else
                       "릴레이 종료: F11·게임 연결·아래 로그를 확인해 주세요. 자동 재시작은 멈췄습니다.")
            return
        if not self.relay and now - self.started > 35:
            raise TimeoutError("WSL 서버 준비 시간이 초과됐습니다.")
        if self.last_wsl and now - self.last_wsl > 6:
            raise TimeoutError("WSL 상태 응답이 끊겼습니다.")
        if self.relay:
            snapshot = self.snapshot()
            relay, wsl = snapshot["relay"], snapshot["wsl"]
            ack, age = relay.get("ros_ack_age_ms"), wsl.get("state_age_s")
            connected = now - self.last_relay < 3 and ack is not None and ack < 3000 and age is not None and age < 3
            if connected:
                label = "게임 일시정지" if wsl.get("paused") else (
                    "센서 구독 대기" if relay.get("demand_topics", 0) <= 2 and relay.get("sent_bundles", 0) == 0 else "연결됨")
                self.update(phase="running", message=label + " · ROS 수신 확인")
            else:
                self.update(phase="connecting", message="연결 응답 대기 · 현재 수신이 확인되지 않습니다.")

    def _loop(self):
        last_game = 0.0
        try:
            while not self.closed.is_set():
                try:
                    action, values = self.commands.get(timeout=0.1)
                    if action == "close":
                        break
                    if action == "stop":
                        self._stop()
                    elif action == "restart":
                        self._stop()
                        self._start(values.get("duration"), values.get("camera_hz"), values.get("lidar_hz"), values.get("preview_hz"), values.get("input_only", False))
                    elif action == "start":
                        self._start(values.get("duration"), values.get("camera_hz"), values.get("lidar_hz"), values.get("preview_hz"), values.get("input_only", False))
                    elif action in ("drive_start", "drive_stop"):
                        self._drive(action, values.get("speed_kmh", 30), values.get("acc", True), values.get("lcc", True))
                    elif action in ("record_start", "record_stop"):
                        self._record(action, values.get("profile", "state"))
                except queue.Empty:
                    pass
                except Exception as error:
                    self.log(str(error))
                    self._stop("error", str(error))
                try:
                    self._observe()
                except Exception as error:
                    self.log(str(error))
                    self._stop("error", str(error))
                if time.monotonic() - last_game > 2:
                    try:
                        tier = Client(timeout=0.3).tier()["tier"]
                        self.update(game=f"연결됨 · Tier {tier}")
                    except Exception:
                        self.update(game="SDK 연결 없음 · 게임을 DX11로 실행해 주세요")
                    last_game = time.monotonic()
        finally:
            self._stop()
            self.closed.set()

    def close(self):
        self.request("close")
        self.worker.join()
        K.CloseHandle(self.show)
        K.CloseHandle(self.mutex)


def show_window(controller):
    import tkinter as tk
    from tkinter import ttk
    window = tk.Tk()
    window.title("유로파일럿 · 센서 브리지")
    window.geometry("820x740")
    window.minsize(720, 680)
    style = ttk.Style(window)
    style.configure("Title.TLabel", font=("Malgun Gothic", 18, "bold"))
    frame = ttk.Frame(window, padding=22)
    frame.pack(fill="both", expand=True)
    ttk.Label(frame, text="유로파일럿 센서 브리지", style="Title.TLabel").pack(anchor="w")
    summary = ttk.Label(frame, text="상태 확인 중", font=("Malgun Gothic", 11))
    summary.pack(anchor="w", pady=(8, 18))
    actions = ttk.Frame(frame)
    actions.pack(fill="x")
    duration = tk.StringVar(value="110")
    camera_hz, lidar_hz = tk.StringVar(value="30"), tk.StringVar(value="10")
    preview_hz = tk.StringVar(value="10")
    input_only = tk.BooleanVar(value=False)
    try:
        settings = json.loads(controller.config.read_text(encoding="utf-8"))
        duration.set(str(settings["duration_s"]))
        camera_hz.set(str(settings.get("camera_hz", 30)))
        lidar_hz.set(str(settings.get("lidar_hz", 10)))
        preview_hz.set(str(settings.get("preview_hz", 10)))
    except (OSError, ValueError, KeyError):
        pass
    def request(action):
        controller.request(action, duration=duration.get(), camera_hz=camera_hz.get(), lidar_hz=lidar_hz.get(), preview_hz=preview_hz.get(), input_only=input_only.get())
    start = ttk.Button(actions, text="시작", command=lambda: request("start"))
    start.pack(side="left")
    stop = ttk.Button(actions, text="중지", command=lambda: request("stop"))
    stop.pack(side="left", padx=6)
    restart = ttk.Button(actions, text="재시작", command=lambda: request("restart"))
    restart.pack(side="left")
    ttk.Label(actions, text="실행 시간(초)").pack(side="left", padx=(22, 5))
    duration_entry = ttk.Entry(actions, textvariable=duration, width=9)
    duration_entry.pack(side="left")
    rates = ttk.Frame(frame)
    rates.pack(fill="x", pady=(10, 0))
    rate_entries = []
    for label, variable in (("카메라 Hz", camera_hz), ("라이다 Hz", lidar_hz), ("미리보기 Hz", preview_hz)):
        ttk.Label(rates, text=label).pack(side="left", padx=(0, 5))
        entry = ttk.Entry(rates, textvariable=variable, width=7)
        entry.pack(side="left", padx=(0, 18))
        rate_entries.append(entry)
    ttk.Checkbutton(frame, text="입력·상태만 (다음 시작에 적용, 센서 설정 유지)", variable=input_only).pack(anchor="w", pady=(8, 0))
    driving_box = ttk.LabelFrame(frame, text="GT 주행 보조", padding=8)
    driving_box.pack(fill="x", pady=(8, 0))
    driving_controls = ttk.Frame(driving_box)
    driving_controls.pack(fill="x")
    ttk.Label(driving_controls, text="목표 km/h").pack(side="left")
    drive_speed = tk.StringVar(value="30")
    ttk.Entry(driving_controls, textvariable=drive_speed, width=7).pack(side="left", padx=8)
    acc_enabled, lcc_enabled = tk.BooleanVar(value=True), tk.BooleanVar(value=True)
    ttk.Checkbutton(driving_controls, text="ACC 속도", variable=acc_enabled).pack(side="left", padx=5)
    ttk.Checkbutton(driving_controls, text="LCC 차로", variable=lcc_enabled).pack(side="left", padx=5)
    drive_start = ttk.Button(driving_controls, text="선택 적용 / 켜기", command=lambda: controller.request("drive_start", speed_kmh=drive_speed.get(), acc=acc_enabled.get(), lcc=lcc_enabled.get()))
    drive_start.pack(side="left")
    drive_stop = ttk.Button(driving_controls, text="입력 해제", command=lambda: controller.request("drive_stop"))
    drive_stop.pack(side="left", padx=8)
    drive_status = ttk.Label(driving_box, text="입력·상태만 시작 → GT 주행 → 게임 전경", wraplength=730)
    drive_status.pack(anchor="w", pady=(5, 0))
    ttk.Label(driving_box, text="ACC만: 직접 조향 · LCC만: 직접 가감속. 자동 축을 직접 조작하면 모든 보조가 해제됩니다.", wraplength=730).pack(anchor="w")
    recording_box = ttk.LabelFrame(frame, text="주행 기록", padding=8)
    recording_box.pack(fill="x", pady=(12, 0))
    recording_actions = ttk.Frame(recording_box)
    recording_actions.pack(fill="x")
    profiles = {"차량 상태 (소용량)": "state", "영상·라이다 포함": "sensors"}
    record_profile = tk.StringVar(value="차량 상태 (소용량)")
    profile_entry = ttk.Combobox(recording_actions, textvariable=record_profile, values=list(profiles),
                                 state="readonly", width=23)
    profile_entry.pack(side="left", padx=(0, 8))
    record_start = ttk.Button(recording_actions, text="기록 시작",
        command=lambda: controller.request("record_start", profile=profiles[record_profile.get()]))
    record_start.pack(side="left")
    record_stop = ttk.Button(recording_actions, text="기록 종료", command=lambda: controller.request("record_stop"))
    record_stop.pack(side="left", padx=6)
    recording_text = ttk.Label(recording_box, text="기록 안 함", wraplength=730)
    recording_text.pack(anchor="w", pady=(6, 0))
    recording_path = ""
    def copy_recording_path():
        window.clipboard_clear()
        window.clipboard_append(recording_path)
    ttk.Button(recording_actions, text="경로 복사", command=copy_recording_path).pack(side="left")
    ttk.Label(frame, text="F11로 중지하면 자동으로 다시 켜지지 않습니다. 창을 닫으면 이 창에서 시작한 프로세스가 종료됩니다.",
              wraplength=750).pack(anchor="w", pady=(12, 14))
    rows = {}
    grid = ttk.Frame(frame)
    grid.pack(fill="x")
    for index, key in enumerate(("게임 SDK", "Windows 릴레이", "WSL ROS / Foxglove", "전송 / ROS 수신", "수집 상태", "Foxglove 주소")):
        ttk.Label(grid, text=key, width=22).grid(row=index, column=0, sticky="w", pady=4)
        rows[key] = ttk.Label(grid, text="—")
        rows[key].grid(row=index, column=1, sticky="w")
    endpoint = ""
    def copy_address():
        window.clipboard_clear()
        window.clipboard_append(endpoint)
    ttk.Button(frame, text="Foxglove 주소 복사", command=copy_address).pack(anchor="w", pady=8)
    ttk.Label(frame, text="설정: " + str(controller.config), wraplength=750).pack(anchor="w")
    ttk.Button(frame, text="설정 파일 열기", command=lambda: os.startfile(controller.config)).pack(anchor="w", pady=6)
    log = tk.Text(frame, height=8, wrap="word", font=("Consolas", 9), state="disabled")
    log.pack(fill="both", expand=True, pady=(8, 0))
    closing = False
    last_logs = None
    def close():
        nonlocal closing
        closing = True
        controller.request("close")
    window.protocol("WM_DELETE_WINDOW", close)
    def refresh():
        nonlocal endpoint, last_logs, recording_path
        if controller.closed.is_set():
            window.destroy()
            return
        if K.WaitForSingleObject(controller.show, 0) == 0:
            window.deiconify()
            window.lift()
            window.focus_force()
        state = controller.snapshot()
        phase = state["phase"]
        active = phase in ("starting", "connecting", "running", "stopping")
        start.config(state="disabled" if active or closing or state.get("unit") else "normal")
        stop.config(state="normal" if (active or state.get("unit")) and phase != "stopping" and not closing else "disabled")
        restart.config(state="normal" if phase in ("running", "connecting") and not closing else "disabled")
        duration_entry.config(state="disabled" if active else "normal")
        for entry in rate_entries:
            entry.config(state="disabled" if active else "normal")
        summary.config(text=state["message"])
        relay, wsl = state["relay"], state["wsl"]
        driving = bool(wsl.get("drive_pid"))
        drive_start.config(state="normal" if phase == "running" and not closing else "disabled")
        drive_stop.config(state="normal" if phase == "running" and not closing else "disabled")
        actual = wsl.get("drive_state") or {}
        fresh = wsl.get("drive_state_age_s") is not None and wsl["drive_state_age_s"] < 2
        mask = actual.get("axes", 0) if fresh and actual.get("armed") else 0
        drive_status.config(text=(f"실제 상태: ACC {'켜짐' if mask & 2 else '꺼짐'} · LCC {'켜짐' if mask & 1 else '꺼짐'} · " +
                                  (actual.get("reason", "") if fresh else "게임 상태 응답 대기")))
        recording = state["recording"]
        record_phase = recording.get("phase", "idle")
        writing = record_phase in ("starting", "recording", "stopping")
        record_start.config(state="normal" if phase == "running" and not writing and not closing else "disabled")
        record_stop.config(state="normal" if writing and record_phase != "stopping" and not closing else "disabled")
        profile_entry.config(state="disabled" if writing else "readonly")
        labels = {"idle": "기록 안 함", "starting": "기록 준비", "recording": "기록 중", "stopping": "파일 마무리 중",
                  "saved": "저장 완료", "error": "기록 오류"}
        recording_path = recording.get("path", "")
        record_label = labels.get(record_phase, record_phase)
        if recording_path:
            record_label += f" · {recording.get('bytes', 0) / 1024**2:.1f} MiB"
            if record_phase in ("saved", "error"):
                record_label += f" · {recording.get('message_count', 0):,}개 메시지"
            record_label += "\n" + recording_path
        error = state.get("recording_error") or recording.get("error")
        if error:
            record_label += "\n" + error
        recording_text.config(text=record_label)
        rows["게임 SDK"].config(text=state["game"])
        rows["Windows 릴레이"].config(text=f"PID {state['relay_pid']}" if state.get("relay_pid") else "중지됨")
        rows["WSL ROS / Foxglove"].config(text=f"Ubuntu · ROS PID {wsl.get('ros_pid', '—')}" if state.get("wsl_pid") else "중지됨")
        rows["전송 / ROS 수신"].config(text=f"{relay.get('sent_bundles', 0)} / {relay.get('ros_received_bundles', 0)} 묶음 · 큐 누락 {relay.get('queue_dropped', 0)}")
        rows["수집 상태"].config(text=("활성" if relay.get("capture_active") else "대기") if phase == "running" else
                              ("연결 확인 중" if active else "중지됨"))
        endpoint = "ws://" + wsl["ip"] + ":8765" if state.get("wsl_pid") and wsl.get("ip") else ""
        rows["Foxglove 주소"].config(text=endpoint or "—")
        text = "\n".join(state["logs"])
        if text != last_logs:
            log.config(state="normal")
            log.delete("1.0", "end")
            log.insert("end", text)
            log.see("end")
            log.config(state="disabled")
            last_logs = text
        window.after(250, refresh)
    refresh()
    window.mainloop()


def main():
    try:
        controller = Controller()
    except FileExistsError:
        return
    try:
        show_window(controller)
    finally:
        controller.close()


if __name__ == "__main__":
    main()
