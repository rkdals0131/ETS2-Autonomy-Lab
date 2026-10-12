"""Windows control window for one owned relay, Foxglove server, GT drive and recorder.

Run inside the pixi environment (`pixi run launcher`): every child inherits the
activated ROS 2 environment and the shared DDS profile.
"""
from collections import deque
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid

APP_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = APP_ROOT.parents[1]
BRIDGE_ROOT = PROJECT_ROOT / "bridge"
RELAY = PROJECT_ROOT / "ros2" / "install" / "lib" / "ets2_relay" / "ets2_relay.exe"
FOXGLOVE_PORT = 8765
sys.path.insert(0, str(PROJECT_ROOT / "ot" / "py"))
from otpy.client import Client, LoaderClient
from bag_recording import BagRecording
import cpu_layout

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
    def __init__(self, affinity=0):
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
        if affinity:  # Children stay off the game's cores (cpu_layout).
            limits.basic.flags |= 0x10
            limits.basic.affinity = affinity
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


class StopEvent:
    """Named manual-reset event a child waits on (relay, --stop-event of ROS nodes)."""
    def __init__(self, role):
        self.name = f"Local\\ETS2AutonomyLab.Stop.{role}.{uuid.uuid4().hex}"
        self.handle = K.CreateEventW(None, True, False, self.name)
        if not self.handle:
            raise C.WinError(C.get_last_error())

    def set(self):
        if self.handle:
            K.SetEvent(self.handle)

    def close(self):
        if self.handle:
            K.CloseHandle(self.handle)
            self.handle = None


def foxglove_listening():
    with socket.socket() as probe:
        probe.settimeout(0.2)
        return probe.connect_ex(("127.0.0.1", FOXGLOVE_PORT)) == 0


class Controller:
    """All state transitions run on one worker; snapshots report observed state."""
    def __init__(self, config=BRIDGE_ROOT / "config" / "bridge.local.json"):
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
        self.state = {"phase": "stopped", "message": "시작을 누르면 게임 릴레이와 Foxglove 서버를 띄웁니다.",
                      "game": "확인 중", "config": str(self.config), "relay": {}, "foxglove": False,
                      "recording": {"phase": "idle"}, "recording_error": ""}
        self.relay = self.foxglove = self.drive = self.provider = self.job = None
        self.relay_stop = self.drive_stop_event = None
        self.recording = None
        self.mode_change = None
        self.desired = False
        self.input_only = False
        self.drive_request = None
        self.last_relay = 0.0
        self.layout = None
        self.last_layout = self.last_listen = 0.0
        self.listening = False
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

    def spawn(self, command, source, extra_flags=0):
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                   bufsize=1, creationflags=subprocess.CREATE_NO_WINDOW | extra_flags)
        self.job.add(process)
        def read():
            with process.stdout:
                for line in process.stdout:
                    self.lines.put((source, process.pid, line.rstrip()))
        threading.Thread(target=read, daemon=True).start()
        return process

    def _start(self, duration, camera_hz=None, lidar_hz=None, preview_hz=None, input_only=False, cpu_placement=None,
               mirrors_off=None):
        if self.desired or self.relay:
            return
        if not shutil.which("ros2"):
            raise RuntimeError("ROS 2 환경이 없습니다. apps/launch.cmd(pixi run launcher)로 실행해 주세요.")
        if not RELAY.is_file():
            raise RuntimeError("릴레이가 빌드되지 않았습니다. 저장소에서 pixi run build를 먼저 실행해 주세요.")
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
        if cpu_placement is not None and bool(cpu_placement) != settings.get("cpu_layout", True):
            settings["cpu_layout"] = bool(cpu_placement)
            changed = True
        if mirrors_off is not None and (not mirrors_off) != settings.get("native_mirrors", True):
            settings["native_mirrors"] = not mirrors_off
            changed = True
        if changed:
            temporary = self.config.with_name(self.config.name + ".tmp")
            try:
                temporary.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
                os.replace(temporary, self.config)
            finally:
                temporary.unlink(missing_ok=True)
        duration = float(settings.get("duration_s", 60))
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError("duration_s must be positive")
        if Client(timeout=0.5).tier()["tier"] != 0:
            raise RuntimeError("다른 수집 또는 리그가 실행 중입니다. 기존 작업을 먼저 중지해 주세요.")
        if foxglove_listening():
            raise RuntimeError(f"127.0.0.1:{FOXGLOVE_PORT}을 이미 다른 프로세스가 쓰고 있습니다. 기존 Foxglove 서버를 먼저 종료해 주세요.")
        self.desired = True
        self.input_only = bool(input_only)
        self.started = time.monotonic()
        placement = settings.get("cpu_layout", True)
        plan = None
        if placement:
            try:
                plan = cpu_layout.Plan()
            except OSError as error:
                self.log(f"CPU 배치 건너뜀: {error}")
        self.job = Job(plan.helpers if plan else 0)
        self.recording = BagRecording(self.config, self.job)
        self.update(phase="starting", message="Foxglove 서버와 게임 릴레이를 시작합니다.", relay={}, foxglove=False,
                    duration_s=duration)
        self.foxglove = self.spawn(["ros2", "run", "foxglove_bridge", "foxglove_bridge", "--ros-args",
                                    "--params-file", str(BRIDGE_ROOT / "config" / "foxglove.yaml"),
                                    "-p", "address:=127.0.0.1", "-p", f"port:={FOXGLOVE_PORT}"], "foxglove")
        self.relay_stop = StopEvent("relay")
        command = [str(RELAY), str(self.config), self.relay_stop.name]
        if self.input_only:
            command.append("--input-only")
        self.relay = self.spawn(command, "relay")
        self.last_relay = time.monotonic()
        self.update(foxglove_pid=self.foxglove.pid, relay_pid=self.relay.pid,
                    phase="connecting", message="게임 릴레이의 첫 상태 보고를 기다립니다.")
        if plan:
            try:
                self.layout = cpu_layout.Layout(Client(timeout=1).request("ping")["pid"],
                                                LoaderClient(timeout=1).status()["sdk_thread_id"])
                self.layout.refresh()
                self.log("CPU 배치: " + self.layout.plan.describe())
            except Exception as error:
                self.layout = None
                self.log(f"CPU 배치 건너뜀: {error}")

    def _release_drive(self, timeout=3):
        """Disarm through the GT node's stop event; kill only if it does not exit."""
        if self.drive and self.drive.poll() is None:
            self.drive_stop_event.set()
            try:
                self.drive.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.drive.kill()  # The core then releases on the 200 ms command window.
                self.drive.wait(timeout=3)
        if self.drive_stop_event:
            self.drive_stop_event.close()
        self.drive = self.drive_stop_event = None

    def _drive(self, action, speed_kmh=30, acc=True, lcc=True):
        if action == "drive_stop":
            if self.provider and self.provider.poll() is None:
                self.provider.terminate()
                self.provider.wait(timeout=3)
            self.provider = self.drive_request = None
            self._release_drive()
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
        if self.drive and self.drive.poll() is None:
            self._set_drive_mode(speed, bool(acc), bool(lcc))
            return
        dotnet = PROJECT_ROOT / "research" / "tools" / "dotnet10" / "dotnet.exe"
        provider = BRIDGE_ROOT / "build" / "map_lane_provider" / "Release" / "net10.0" / "map_lane_provider.dll"
        if not dotnet.is_file() or not provider.is_file():
            raise RuntimeError("지도 provider 빌드가 필요합니다: bridge/map_lane_provider/README.md")
        output = BRIDGE_ROOT / "recordings" / "gt-path" / "current-lane.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        game = Path(LoaderClient(timeout=2).status()["module_path"]).parents[4]
        self.drive_request = {"path": str(output), "speed_mps": speed, "acc": bool(acc), "lcc": bool(lcc)}
        self.provider = self.spawn([str(dotnet), str(provider), "--game", str(game), "--out", str(output), "--distance-m", "1000"], "lane")
        self.update(message="현재 도로의 실제 차로 기준선을 준비합니다.")

    def _start_drive(self, request):
        self.drive_stop_event = StopEvent("drive")
        self.drive = self.spawn(["ros2", "run", "ets2_autonomy", "drive_speed", "--stop-event", self.drive_stop_event.name,
                                 "--ros-args", "-p", "arm:=true", "-p", "path_file:=" + request["path"],
                                 "-p", "acc_enabled:=" + str(request["acc"]).lower(),
                                 "-p", "lcc_enabled:=" + str(request["lcc"]).lower(),
                                 "-p", "target_speed_mps:=" + str(float(request["speed_mps"]))], "drive")
        self.update(message="GT 주행을 시작합니다. 게임 창으로 돌아가세요.")

    def _set_drive_mode(self, speed, acc, lcc):
        """One atomic parameter change keeps the owner and epoch (no re-arm)."""
        if self.mode_change and self.mode_change.poll() is None:
            raise RuntimeError("이전 모드 변경 응답을 기다리고 있습니다.")
        def value(kind, field, data):
            return "{type: %d, %s: %s}" % (kind, field, data)
        request = "{parameters: [%s]}" % ", ".join([
            "{name: target_speed_mps, value: %s}" % value(3, "double_value", repr(float(speed))),
            "{name: acc_enabled, value: %s}" % value(1, "bool_value", str(acc).lower()),
            "{name: lcc_enabled, value: %s}" % value(1, "bool_value", str(lcc).lower())])
        self.mode_change = self.spawn(["ros2", "service", "call", "/ets2_drive_speed/set_parameters_atomically",
                                       "rcl_interfaces/srv/SetParametersAtomically", request], "mode")

    def _record(self, action, profile="state"):
        if action == "record_start":
            if self.snapshot()["phase"] != "running" or not self.recording:
                self.update(recording_error="브리지 연결 후 기록을 시작해 주세요.")
                return
            self.update(recording_error="")
            try:
                self.recording.start(profile)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                self.update(recording_error=str(error))
        elif self.recording:
            self.recording.stop()

    def _stop(self, phase="stopped", message="중지됨"):
        self.desired = False
        self.drive_request = None
        self.update(phase="stopping", message="수집 해제와 프로세스 종료를 확인합니다.")
        errors = []
        try:
            if self.recording:
                self.recording.close()
                self.update(recording=self.recording.poll())
            self._release_drive()
            if self.layout:
                self.layout.restore()
                self.layout = None
            if self.relay_stop:
                self.relay_stop.set()
            if self.relay:
                try:
                    self.relay.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    self.relay.kill()
                    self.relay.wait(timeout=3)
                    # The game's lease expires five seconds after its last heartbeat.
                    time.sleep(5.1)
        except Exception as error:
            errors.append(str(error))
        finally:
            if self.job:
                self.job.close()  # Foxglove, provider and anything left in the job end here.
            for process in (self.relay, self.foxglove, self.provider, self.mode_change):
                if process:
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        errors.append(f"PID {process.pid} 종료 확인 실패")
            if self.relay_stop:
                self.relay_stop.close()
            self.relay = self.foxglove = self.provider = self.mode_change = self.job = self.relay_stop = None
        self.update(phase="error" if errors else phase, foxglove=False,
                    message="; ".join(errors) if errors else message, relay_pid=None, foxglove_pid=None)

    def _observe(self):
        now = time.monotonic()
        while not self.lines.empty():
            source, pid, line = self.lines.get()
            process = {"relay": self.relay, "foxglove": self.foxglove, "lane": self.provider,
                       "drive": self.drive, "mode": self.mode_change}.get(source)
            if not process or process.pid != pid:
                continue
            if source == "relay":
                try:
                    data = json.loads(line)
                except ValueError:
                    data = None
                if isinstance(data, dict) and "sent_bundles" in data:
                    self.last_relay = now
                    self.update(relay=data)
                    continue
            if source == "mode" and "successful=False" in line:
                self.update(message="GT 모드 변경 거절: " + line)
            self.log(source + ": " + line)
        if self.provider and self.provider.poll() is not None:
            code = self.provider.returncode
            self.provider = None
            request, self.drive_request = self.drive_request, None
            if code:
                raise RuntimeError("현재 도로의 GT 차로 생성 실패: 로그를 확인해 주세요.")
            if request:
                self._start_drive(request)
        if self.drive and self.drive.poll() is not None:
            self.log(f"drive: GT 주행 종료 (code={self.drive.returncode})")
            self._release_drive()
        if self.recording:
            self.update(recording=self.recording.poll())
        if not self.desired:
            return
        if self.foxglove and self.foxglove.poll() is not None:
            raise RuntimeError(f"Foxglove 서버가 종료됐습니다 (code={self.foxglove.returncode}). 로그를 확인해 주세요.")
        if self.relay and self.relay.poll() is not None:
            code = self.relay.returncode
            self._stop("stopped" if code == 0 else "error",
                       "실행이 종료됐습니다. 시작을 누르면 새 세션을 엽니다." if code == 0 else
                       ("현재 차량과 센서 보정이 다릅니다. 차량에 맞는 rig를 선택하거나 ‘입력·상태만’을 선택하고 시작하세요." if code == 2 else
                        "릴레이 종료: F11·게임 연결·아래 로그를 확인해 주세요. 자동 재시작은 멈췄습니다."))
            return
        if self.layout and now - self.last_layout > 5:
            self.last_layout = now
            try:
                self.layout.refresh()  # New game threads and viewer windows.
            except OSError as error:
                self.log(f"CPU 배치 갱신 실패: {error}")
        if now - self.last_listen > 1:
            self.listening, self.last_listen = foxglove_listening(), now
        foxglove = self.listening
        relay = self.snapshot()["relay"]
        if now - self.last_relay < 3 and relay:
            label = "게임 일시정지" if relay.get("paused") else (
                "센서 구독 대기" if relay.get("demand_topics", 0) <= 2 and relay.get("sent_bundles", 0) == 0 else "연결됨")
            self.update(phase="running", foxglove=foxglove, message=label + ("" if foxglove else " · Foxglove 서버 시작 중"))
        else:
            if now - self.started > 35:
                raise TimeoutError("게임 릴레이의 상태 보고가 없습니다. 게임과 로그를 확인해 주세요.")
            self.update(phase="connecting", foxglove=foxglove, message="게임 릴레이의 상태 보고를 기다립니다.")

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
                        self._start(values.get("duration"), values.get("camera_hz"), values.get("lidar_hz"), values.get("preview_hz"), values.get("input_only", False), values.get("cpu_layout"),
                                    values.get("mirrors_off"))
                    elif action == "start":
                        self._start(values.get("duration"), values.get("camera_hz"), values.get("lidar_hz"), values.get("preview_hz"), values.get("input_only", False), values.get("cpu_layout"),
                                    values.get("mirrors_off"))
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


def observed_drive(relay, age):
    """A stale observation never presents an armed axis as enabled."""
    drive = relay.get("drive") if relay else None
    if not drive or age >= 2:
        return 0, "게임 상태 응답 대기"
    return (drive.get("axes", 0) if drive.get("armed") else 0), drive.get("reason", "")


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
    cpu_placement = tk.BooleanVar(value=True)
    mirrors_off = tk.BooleanVar(value=False)
    try:
        settings = json.loads(controller.config.read_text(encoding="utf-8"))
        duration.set(str(settings["duration_s"]))
        camera_hz.set(str(settings.get("camera_hz", 30)))
        lidar_hz.set(str(settings.get("lidar_hz", 10)))
        preview_hz.set(str(settings.get("preview_hz", 10)))
        cpu_placement.set(bool(settings.get("cpu_layout", True)))
        mirrors_off.set(not settings.get("native_mirrors", True))
    except (OSError, ValueError, KeyError):
        pass
    def request(action):
        controller.request(action, duration=duration.get(), camera_hz=camera_hz.get(), lidar_hz=lidar_hz.get(), preview_hz=preview_hz.get(), input_only=input_only.get(), cpu_layout=cpu_placement.get(),
                           mirrors_off=mirrors_off.get())
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
    ttk.Checkbutton(frame, text="CPU 배치: 게임 메인 스레드에 코어 하나, 릴레이·Foxglove 뷰어는 다른 L3 묶음 (다음 시작에 적용)",
                    variable=cpu_placement).pack(anchor="w")
    ttk.Checkbutton(frame, text="게임 미러 렌더 끄기: 센서 실행 중 화면의 미러가 마지막 장면에서 멈춤, 메인 스레드 절약 (다음 시작에 적용)",
                    variable=mirrors_off).pack(anchor="w")
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
    for index, key in enumerate(("게임 SDK", "Windows 릴레이", "Foxglove 서버", "발행", "수집 상태", "Foxglove 주소")):
        ttk.Label(grid, text=key, width=22).grid(row=index, column=0, sticky="w", pady=4)
        rows[key] = ttk.Label(grid, text="—")
        rows[key].grid(row=index, column=1, sticky="w")
    endpoint = f"ws://127.0.0.1:{FOXGLOVE_PORT}"
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
    shown = {}
    def put(widget, **options):
        """Reconfigure only on change: config calls re-lay out wrapped labels (35% of a core at 4 Hz)."""
        if shown.get(str(widget)) != options:
            widget.config(**options)
            shown[str(widget)] = options
    def close():
        nonlocal closing
        closing = True
        controller.request("close")
    window.protocol("WM_DELETE_WINDOW", close)
    def refresh():
        nonlocal last_logs, recording_path
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
        put(start, state="disabled" if active or closing else "normal")
        put(stop, state="normal" if active and phase != "stopping" and not closing else "disabled")
        put(restart, state="normal" if phase in ("running", "connecting") and not closing else "disabled")
        put(duration_entry, state="disabled" if active else "normal")
        for entry in rate_entries:
            put(entry, state="disabled" if active else "normal")
        put(summary, text=state["message"])
        relay = state["relay"]
        put(drive_start, state="normal" if phase == "running" and not closing else "disabled")
        put(drive_stop, state="normal" if phase == "running" and not closing else "disabled")
        mask, reason = observed_drive(relay, time.monotonic() - controller.last_relay)
        put(drive_status, text=f"실제 상태: ACC {'켜짐' if mask & 2 else '꺼짐'} · LCC {'켜짐' if mask & 1 else '꺼짐'} · {reason}")
        recording = state["recording"]
        record_phase = recording.get("phase", "idle")
        writing = record_phase in ("starting", "recording", "stopping")
        put(record_start, state="normal" if phase == "running" and not writing and not closing else "disabled")
        put(record_stop, state="normal" if writing and record_phase != "stopping" and not closing else "disabled")
        put(profile_entry, state="disabled" if writing else "readonly")
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
        put(recording_text, text=record_label)
        put(rows["게임 SDK"], text=state["game"])
        put(rows["Windows 릴레이"], text=f"PID {state['relay_pid']}" if state.get("relay_pid") else "중지됨")
        put(rows["Foxglove 서버"], text=(f"PID {state['foxglove_pid']} · " + ("대기 중" if state.get("foxglove") else "시작 중"))
                                     if state.get("foxglove_pid") else "중지됨")
        commands = relay.get("drive_commands") or {}
        put(rows["발행"], text=f"{relay.get('sent_bundles', 0)} 묶음 · 큐 누락 {relay.get('queue_dropped', 0)}"
                               f" · 50 ms 넘은 발행 {(relay.get('slow_publishes') or {}).get('count', 0)}"
                               f" · 명령 {commands.get('forwarded_commands', 0)} (지연 폐기 {commands.get('stale_commands', 0)})")
        put(rows["수집 상태"], text=("활성" if relay.get("capture_active") else "대기") if phase == "running" else
                                ("연결 확인 중" if active else "중지됨"))
        put(rows["Foxglove 주소"], text=endpoint if state.get("foxglove") else "—")
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
