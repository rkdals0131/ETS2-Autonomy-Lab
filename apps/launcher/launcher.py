"""Windows control window for one owned relay, Foxglove server, cruise-button assistance and recorder.

Run inside the pixi environment (`pixi run launcher`): every child inherits the
activated ROS 2 environment and the shared DDS profile.
"""
from collections import deque
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime
import json
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
LOG_ROOT = BRIDGE_ROOT / "recordings" / "logs"
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
    """A LISTEN socket on the Foxglove port, from the TCP table: a connect probe made the
    server log "handshake failed" every second (2026-10-21)."""
    iphlpapi = C.WinDLL("iphlpapi")
    size = W.DWORD(0)
    iphlpapi.GetExtendedTcpTable(None, C.byref(size), False, 2, 3, 0)  # AF_INET, TCP_TABLE_OWNER_PID_LISTENER
    buffer = C.create_string_buffer(size.value)
    if iphlpapi.GetExtendedTcpTable(buffer, C.byref(size), False, 2, 3, 0) != 0:
        return False
    count = C.cast(buffer, C.POINTER(W.DWORD))[0]
    rows = C.cast(C.addressof(buffer) + 4, C.POINTER(W.DWORD * 6))  # state, local addr, local port, remote addr, remote port, pid
    return any(socket.ntohs(rows[i][2] & 0xFFFF) == FOXGLOVE_PORT for i in range(count))




class SessionLog:
    """Every child line, one file per source in one directory per bridge start. The relay's
    1 Hz status JSON is kept once per 5 s; all other lines are kept."""
    def __init__(self, root=LOG_ROOT):
        self.directory = root / datetime.now().strftime("%Y%m%d-%H%M%S")
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.files, self.last_status = {}, 0.0

    def write(self, source, line):
        now = time.time()
        with self.lock:
            if self.files is None:
                return
            if source == "relay" and line.startswith("{") and '"sent_bundles"' in line:
                if now - self.last_status < 5:
                    return
                self.last_status = now
            try:  # A full disk must not stop the reader that drains the child's pipe.
                file = self.files.get(source)
                if file is None:
                    file = self.files[source] = (self.directory / f"{source}.log").open("a", encoding="utf-8", buffering=1)
                file.write(time.strftime("%H:%M:%S", time.localtime(now)) + f".{int(now % 1 * 1000):03d} {line}\n")
            except OSError:
                pass

    def close(self):
        with self.lock:
            for file in (self.files or {}).values():
                file.close()
            self.files = None


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
        self.state = {"phase": "stopped", "message": "중지됨", "game": "확인 중", "relay": {}, "foxglove": False,
                      "mode": None, "assist": "off", "assist_note": "", "log_dir": "",
                      "recording": {"phase": "idle"}, "recording_error": ""}
        self.relay = self.foxglove = self.drive = self.follow = self.lcc_change = self.job = self.hud = None
        self.relay_stop = self.drive_stop_event = None
        self.relay_restart = False  # the game session changed: start a new relay once the game is ready
        self.recording = self.session_log = None
        self.readers = []
        self.desired = self.assist = self.assist_started = False
        self.lcc = self.lcc_applied = True
        self.lcc_confirmed = False
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

    def log(self, line, persist=True):
        with self.lock:
            self.logs.append(line)
        if persist and self.session_log:
            self.session_log.write("launcher", line)

    def spawn(self, command, source, extra_flags=0):
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                   bufsize=1, creationflags=subprocess.CREATE_NO_WINDOW | extra_flags)
        self.job.add(process)
        session = self.session_log
        def read():
            with process.stdout:
                for line in process.stdout:
                    line = line.rstrip()
                    self.lines.put((source, process.pid, line))
                    if session:
                        session.write(source, line)
            self.lines.put((source, process.pid, None))  # End of output, after its last line.
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        self.readers.append(reader)
        return process

    def _start(self, mode="assist", lcc=True):
        """assist: input and state only, then the cruise-button standby; sensors: the configured rig."""
        if self.desired or self.relay:
            return
        if not shutil.which("ros2"):
            raise RuntimeError("ROS 2 환경이 없습니다. apps/launch.cmd(pixi run launcher)로 실행해 주세요.")
        if not RELAY.is_file():
            raise RuntimeError("릴레이가 빌드되지 않았습니다. 저장소에서 pixi run build를 먼저 실행해 주세요.")
        if not self.config.is_file():
            raise RuntimeError("bridge/configure.py를 먼저 실행해 로컬 설정을 만들어 주세요.")
        settings = json.loads(self.config.read_text(encoding="utf-8"))
        if Client(timeout=0.5).tier()["tier"] != 0:
            raise RuntimeError("다른 수집 또는 리그가 실행 중입니다. 기존 작업을 먼저 중지해 주세요.")
        if foxglove_listening():
            raise RuntimeError(f"127.0.0.1:{FOXGLOVE_PORT}을 이미 다른 프로세스가 쓰고 있습니다. 기존 Foxglove 서버를 먼저 종료해 주세요.")
        self.desired = True
        self.assist, self.assist_started = mode == "assist", False
        self.lcc = self.lcc_applied = bool(lcc)
        self.started = time.monotonic()
        self.session_log = SessionLog()
        self.update(mode="assist" if self.assist else "sensors", lcc=self.lcc, assist="off", assist_note="",
                    log_dir=str(self.session_log.directory))
        placement = settings.get("cpu_layout", True)
        plan = None
        if placement:
            try:
                plan = cpu_layout.Plan()
            except OSError as error:
                self.log(f"CPU 배치 건너뜀: {error}")
        self.job = Job(plan.helpers if plan else 0)
        self.recording = BagRecording(self.config, self.job)
        self.update(phase="starting", message="Foxglove 서버와 게임 릴레이를 시작합니다.", relay={}, foxglove=False)
        self.foxglove = self.spawn(["ros2", "run", "foxglove_bridge", "foxglove_bridge", "--ros-args",
                                    "--params-file", str(BRIDGE_ROOT / "config" / "foxglove.yaml"),
                                    "-p", "address:=127.0.0.1", "-p", f"port:={FOXGLOVE_PORT}"], "foxglove")
        self._spawn_relay()
        self.update(phase="connecting", message="게임 릴레이의 첫 상태 보고를 기다립니다.")
        if plan:
            try:
                self.layout = cpu_layout.Layout(Client(timeout=1).request("ping")["pid"],
                                                LoaderClient(timeout=1).status()["sdk_thread_id"])
                self.layout.refresh()
                self.log("CPU 배치: " + self.layout.plan.describe())
            except Exception as error:
                self.layout = None
                self.log(f"CPU 배치 건너뜀: {error}")

    def _spawn_relay(self):
        if self.relay_stop:
            self.relay_stop.close()
        self.relay_stop = StopEvent("relay")
        command = [str(RELAY), str(self.config), self.relay_stop.name]
        if self.assist:
            command.append("--input-only")
        self.relay = self.spawn(command, "relay")
        self.last_relay = time.monotonic()
        self.relay_restart = False

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

    def _stop_follow(self):
        if self.follow and self.follow.poll() is None:
            self.follow.terminate()
            self.follow.wait(timeout=3)
        self.follow = None

    def _drive_cruise(self):
        """Resident assistance: the map provider follows the truck and drive_speed waits for the
        game's cruise control button (LB+RT), which engages at the cruise speed and disengages."""
        dotnet = PROJECT_ROOT / "research" / "tools" / "dotnet10" / "dotnet.exe"
        provider = BRIDGE_ROOT / "build" / "map_lane_provider" / "Release" / "net10.0" / "map_lane_provider.dll"
        if not dotnet.is_file() or not provider.is_file():
            raise RuntimeError("지도 provider 빌드가 필요합니다: bridge/map_lane_provider/README.md")
        output = BRIDGE_ROOT / "recordings" / "gt-path" / "follow-lane.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        game = Path(LoaderClient(timeout=2).status()["module_path"]).parents[4]
        self.follow = self.spawn([str(dotnet), str(provider), "--game", str(game), "--follow", "--out", str(output), "--distance-m", "1000"], "follow")
        self.drive_stop_event = StopEvent("drive")
        self.lcc_applied = self.lcc
        self.drive = self.spawn(["ros2", "run", "ets2_autonomy", "drive_speed", "--stop-event", self.drive_stop_event.name,
                                 "--ros-args", "-p", "arm:=true", "-p", "engage:=cruise", "-p", "path_file:=" + str(output),
                                 "-p", "acc_enabled:=true", "-p", "lcc_enabled:=" + str(self.lcc).lower()], "drive")
        # HUD marks from the assistance state, apart from the control loop (ends with the job).
        self.hud = self.spawn(["ros2", "run", "ets2_autonomy", "hud"], "hud")
        self.update(assist="standby")

    def _apply_lcc(self):
        """LCC is a runtime parameter of the standby (ACC stays on: the cruise button needs it).
        One change at a time; the latest choice follows when the previous call ends."""
        if not self.drive or self.drive.poll() is not None or self.lcc == self.lcc_applied:
            return
        if self.lcc_change:
            return
        self.lcc_applied, self.lcc_confirmed = self.lcc, False
        self.lcc_change = self.spawn(["ros2", "param", "set", "/ets2_drive_speed", "lcc_enabled", str(self.lcc).lower()], "lcc")

    def _lcc_result(self):
        """Without "Set parameter successful" the node keeps its previous choice; the window follows."""
        if self.lcc_change.wait() or not self.lcc_confirmed:
            self.lcc = self.lcc_applied = not self.lcc_applied
            self.update(lcc=self.lcc, assist_note="LCC 변경 실패, 이전 선택을 유지합니다. lcc.log를 확인해 주세요.")
        self.lcc_change = None

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
        self.relay_restart = False
        self.update(phase="stopping", message="기록·주행 보조·릴레이를 정상 종료합니다.")
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
            for process in (self.relay, self.foxglove, self.follow, self.lcc_change, self.hud):
                if process:
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        errors.append(f"PID {process.pid} 종료 확인 실패")
            if self.relay_stop:
                self.relay_stop.close()
            self.relay = self.foxglove = self.follow = self.lcc_change = self.job = self.relay_stop = self.hud = None
        for reader in self.readers:
            reader.join(timeout=2)  # The last lines of each child reach its log file.
        self.readers = []
        if errors:
            self.log("; ".join(errors))
        if self.session_log:
            self.session_log.close()
            self.session_log = None
        self.update(phase="error" if errors else phase, foxglove=False, assist="off",
                    message="; ".join(errors) if errors else message)

    def _observe(self):
        now = time.monotonic()
        while not self.lines.empty():
            source, pid, line = self.lines.get()
            process = {"relay": self.relay, "foxglove": self.foxglove, "follow": self.follow,
                       "drive": self.drive, "lcc": self.lcc_change, "hud": self.hud}.get(source)
            if not process or process.pid != pid:
                continue
            if line is None:
                if source == "lcc":
                    self._lcc_result()
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
            # The window shows events; drive's 1 Hz status and the provider's progress stay in the files.
            if source == "drive" and line.startswith("speed=") or source == "follow" and '"error":"' not in line:
                continue
            if source == "drive":
                self.update(assist_note="drive: " + line)
            if source == "lcc" and line.startswith("Set parameter successful"):
                self.lcc_confirmed = True
            self.log(source + ": " + line, persist=False)
        if self.hud and self.hud.poll() is not None:
            self.log(f"hud: HUD 노드 종료 (code={self.hud.returncode}), 게임 화면 표시가 멈춥니다. hud.log를 확인해 주세요.")
            self.hud = None
        if self.follow and self.follow.poll() is not None:
            self.log(f"follow: 지도 provider 종료 (code={self.follow.returncode}), LCC 차로가 더 늘지 않습니다. follow.log를 확인해 주세요.")
            self.follow = None
        if self.drive and self.drive.poll() is not None:
            code = self.drive.returncode
            self.log(f"drive: 크루즈 버튼 대기 종료 (code={code}). 다시 쓰려면 중지 후 시작해 주세요.")
            self._release_drive()
            self._stop_follow()
            self.update(assist=f"exited:{code}")
        self._apply_lcc()
        if self.recording:
            self.update(recording=self.recording.poll())
        if not self.desired:
            return
        if self.foxglove and self.foxglove.poll() is not None:
            raise RuntimeError(f"Foxglove 서버가 종료됐습니다 (code={self.foxglove.returncode}). 로그를 확인해 주세요.")
        if self.relay_restart:
            # The game is ready again once the core answers with the current truck configuration.
            try:
                Client(timeout=1).request("truck_config")
            except Exception:
                return
            self.log("relay: 게임이 준비돼 릴레이를 다시 시작합니다.")
            self._spawn_relay()
            return
        if self.relay and self.relay.poll() is not None:
            code = self.relay.returncode
            if code == 3:
                # A save loaded or another truck: a new sensor session, started automatically. The
                # cruise standby keeps waiting and engages again on the cruise control button.
                self.log("relay: 게임 세션이 바뀌었습니다(세이브 불러오기·다른 트럭). 게임이 준비되면 릴레이를 자동으로 다시 시작합니다.")
                self.relay, self.relay_restart = None, True
                return
            message = ("실행이 종료됐습니다. 시작을 누르면 새 세션을 엽니다." if code == 0 else
                       "현재 차량과 센서 보정이 다릅니다. 설정의 rig를 차량에 맞추거나 주행 보조 모드로 시작해 주세요." if code == 2 else
                       "릴레이 종료: F11·게임 연결·relay.log를 확인해 주세요. 자동 재시작은 하지 않습니다.")
            self.log(f"relay: 종료 (code={code}) {message}")
            self._stop("stopped" if code == 0 else "error", message)
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
                "센서 구독 대기" if not self.assist and relay.get("demand_topics", 0) <= 2 and relay.get("sent_bundles", 0) == 0 else "연결됨")
            self.update(phase="running", foxglove=foxglove, message=label)
            if self.assist and not self.assist_started:
                self.assist_started = True  # Once per start: an exited standby is not respawned.
                self._drive_cruise()
        else:
            if now - self.started > 35:
                raise TimeoutError("게임 릴레이의 상태 보고가 없습니다. 게임과 relay.log를 확인해 주세요.")
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
                    elif action == "start":
                        self._start(values.get("mode", "assist"), values.get("lcc", True))
                    elif action == "lcc":
                        self.lcc = bool(values.get("enabled", True))
                        self.update(lcc=self.lcc)
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


def settings_summary(config):
    """The rarely changed settings, as the next start will read them."""
    try:
        settings = json.loads(Path(config).read_text(encoding="utf-8"))
        duration = float(settings.get("duration_s", 60))
        limit = f"{duration / 3600:g}시간" if duration >= 3600 else f"{duration / 60:g}분" if duration >= 60 else f"{duration:g}초"
        return (f"센서 수집 rig {Path(str(settings.get('rig', ''))).stem or '없음'}\n"
                f"카메라 {float(settings.get('camera_hz', 30)):g} Hz · 라이다 {float(settings.get('lidar_hz', 10)):g} Hz"
                f" · 미리보기 {float(settings.get('preview_hz', 10)):g} Hz"
                f" · 게임 미러 렌더 {'켬' if settings.get('native_mirrors', True) else '끔'}\n"
                f"CPU 배치 {'켬' if settings.get('cpu_layout', True) else '끔'} · 실행 제한 {limit}")
    except (OSError, ValueError, TypeError, AttributeError) as error:
        return f"설정 파일을 읽지 못했습니다: {error}"


def show_window(controller):
    import tkinter as tk
    from tkinter import font, ttk
    window = tk.Tk()
    window.title("유로파일럿 런처")
    window.geometry("780x700")
    window.minsize(660, 560)
    for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont"):
        font.nametofont(name).configure(family="Malgun Gothic", size=10)
    style = ttk.Style(window)
    style.configure("Key.TLabel", foreground="#555555")
    style.configure("Note.TLabel", foreground="#666666")
    style.configure("Error.TLabel", foreground="#b3261e")
    style.configure("Strong.TLabel", font=("Malgun Gothic", 10, "bold"))
    style.configure("Primary.TButton", font=("Malgun Gothic", 10, "bold"), padding=(18, 3))
    frame = ttk.Frame(window, padding=(16, 12, 16, 10))
    frame.pack(fill="both", expand=True)
    frame.columnconfigure(1, weight=1)
    row = 0
    def key(text):
        ttk.Label(frame, text=text, style="Key.TLabel").grid(row=row, column=0, sticky="nw", padx=(0, 14), pady=2)
    def value(**options):
        label = ttk.Label(frame, **options)
        label.grid(row=row, column=1, sticky="w", pady=2)
        return label
    def rule():
        nonlocal row
        ttk.Separator(frame).grid(row=row, column=0, columnspan=3, sticky="ew", pady=8)
        row += 1

    # Mode and the one start/stop button.
    mode = tk.StringVar(value="assist")
    lcc = tk.BooleanVar(value=True)
    key("모드")
    modes = ttk.Frame(frame)
    modes.grid(row=row, column=1, sticky="w")
    radios = [ttk.Radiobutton(modes, text=text, value=name, variable=mode)
              for text, name in (("주행 보조", "assist"), ("센서 수집", "sensors"))]
    for radio in radios:
        radio.pack(side="left", padx=(0, 16))
    def toggle():
        if controller.snapshot()["phase"] in ("stopped", "error"):
            controller.request("start", mode=mode.get(), lcc=lcc.get())
        else:
            controller.request("stop")
    start = ttk.Button(frame, text="시작", style="Primary.TButton", command=toggle)
    start.grid(row=row, column=2, rowspan=2, sticky="ne")
    row += 1
    lcc_check = ttk.Checkbutton(frame, text="LCC(차로 유지)도 함께 · 실행 중 바꾸면 바로 적용", variable=lcc,
                                command=lambda: controller.request("lcc", enabled=lcc.get()))
    lcc_check.grid(row=row, column=1, sticky="w", pady=(2, 0))
    row += 1
    mode_note = value(style="Note.TLabel", wraplength=520, justify="left")
    row += 1
    notes = {"assist": "시작하면 크루즈 버튼 대기까지 켭니다. 게임에서 LB+RT로 켜고 다시 누르면 끕니다. "
                       "브레이크나 3초 넘는 조향은 둘 다 끄고, 짧은 조향에는 LCC가 비켜 줍니다. 방향지시등으로 차로를 바꿉니다.",
             "sensors": "설정 파일의 rig·주기로 카메라·라이다를 수집합니다. 주행 보조는 켜지지 않습니다."}
    rule()

    # Live status.
    rows = {}
    for name in ("게임", "브리지", "주행 보조", "", "Foxglove"):
        key(name)
        rows[name] = value(wraplength=540, justify="left")
        row += 1
    rows[""].configure(style="Note.TLabel")
    note_labels = frame.grid_slaves(row=rows[""].grid_info()["row"])
    rule()

    # Recording and the paths sent after a drive.
    key("기록")
    record_actions = ttk.Frame(frame)
    record_actions.grid(row=row, column=1, columnspan=2, sticky="w")
    record_state = ttk.Button(record_actions, text="차량 상태 기록",
                              command=lambda: controller.request("record_start", profile="state"))
    record_sensors = ttk.Button(record_actions, text="영상·라이다 포함 기록",
                                command=lambda: controller.request("record_start", profile="sensors"))
    record_stop = ttk.Button(record_actions, text="기록 종료", command=lambda: controller.request("record_stop"))
    for button in (record_state, record_sensors, record_stop):
        button.pack(side="left", padx=(0, 6))
    row += 1
    recording_text = value(wraplength=540, justify="left")
    row += 1
    paths = {}
    def copy(variable, button):
        if variable.get():
            window.clipboard_clear()
            window.clipboard_append(variable.get())
            button.config(text="복사됨")
            window.after(1200, lambda: button.config(text="복사"))
    for name in ("bag 경로", "실행 로그"):
        key(name)
        variable = tk.StringVar()
        entry = ttk.Entry(frame, textvariable=variable, state="readonly", font=("Consolas", 9))
        entry.grid(row=row, column=1, sticky="ew", pady=2)
        # The run folder name is at the end of the path.
        variable.trace_add("write", lambda *_, e=entry: window.after(50, lambda: e.xview_moveto(1)))
        entry.bind("<Configure>", lambda event: event.widget.xview_moveto(1))
        button = ttk.Button(frame, text="복사", width=6)
        button.config(command=lambda v=variable, b=button: copy(v, b))
        button.grid(row=row, column=2, sticky="e", padx=(8, 0), pady=2)
        paths[name] = variable
        row += 1
    rule()

    # Rare settings live in the file; the window only shows what the next start reads.
    key("설정")
    settings_text = value(justify="left", wraplength=520)
    ttk.Button(frame, text="설정 파일 열기", command=lambda: os.startfile(controller.config)).grid(
        row=row, column=2, sticky="ne", padx=(8, 0))
    row += 1
    rule()

    log_frame = ttk.Frame(frame)
    log_frame.grid(row=row, column=0, columnspan=3, sticky="nsew")
    frame.rowconfigure(row, weight=1)
    log = tk.Text(log_frame, height=8, wrap="word", font=("Consolas", 9), state="disabled",
                  relief="solid", borderwidth=1, highlightthickness=0, padx=6, pady=4)
    scroll = ttk.Scrollbar(log_frame, command=log.yview)
    log.config(yscrollcommand=scroll.set)
    scroll.pack(side="right", fill="y")
    log.pack(side="left", fill="both", expand=True)
    row += 1
    ttk.Label(frame, text="창을 닫으면 이 창에서 시작한 프로세스가 모두 종료됩니다.", style="Note.TLabel").grid(
        row=row, column=0, columnspan=3, sticky="w", pady=(6, 0))

    closing = False
    last_logs = None
    shown = {}
    settings_seen = last_lcc = None
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
        nonlocal last_logs, settings_seen, last_lcc
        if controller.closed.is_set():
            window.destroy()
            return
        if K.WaitForSingleObject(controller.show, 0) == 0:
            window.deiconify()
            window.lift()
            window.focus_force()
        state = controller.snapshot()
        phase = state["phase"]
        idle = phase in ("stopped", "error")
        if not idle and state.get("mode") and mode.get() != state["mode"]:
            mode.set(state["mode"])
        session = mode.get()
        if state.get("lcc") != last_lcc:  # A refused LCC change puts the box back.
            last_lcc = state.get("lcc")
            if last_lcc is not None and lcc.get() != last_lcc:
                lcc.set(last_lcc)
        put(start, text="시작" if idle else "중지", state="disabled" if closing or phase == "stopping" else "normal")
        for radio in radios:
            put(radio, state="normal" if idle and not closing else "disabled")
        put(lcc_check, state="normal" if session == "assist" and not closing else "disabled")
        put(mode_note, text=notes[session or "assist"])
        relay = state["relay"]
        put(rows["게임"], text=state["game"])
        bridge = state["message"]
        if phase == "running" and session == "assist":
            commands = relay.get("drive_commands") or {}
            bridge += f" · 입력·상태만 · 명령 {commands.get('forwarded_commands', 0):,}개 전달, 지연 폐기 {commands.get('stale_commands', 0)}"
        elif phase == "running":
            bridge += (f" · 수집 {'활성' if relay.get('capture_active') else '대기'} · {relay.get('sent_bundles', 0):,} 묶음"
                       f" · 큐 누락 {relay.get('queue_dropped', 0)} · 50 ms 넘은 발행 {(relay.get('slow_publishes') or {}).get('count', 0)}")
        put(rows["브리지"], text=bridge, style="Error.TLabel" if phase == "error" else "TLabel")
        assist = state["assist"]
        mask, reason = observed_drive(relay, time.monotonic() - controller.last_relay)
        if session != "assist":
            text = "센서 수집 모드에서는 켜지 않습니다."
        elif assist == "standby":
            text = (f"ACC {'켜짐' if mask & 2 else '꺼짐'} · LCC {'켜짐' if mask & 1 else '꺼짐'}" if mask else
                    "크루즈 버튼 대기 중 · 게임에서 LB+RT" + (f" · 마지막 해제 {reason}" if reason else ""))
        elif assist.startswith("exited"):
            text = f"대기 종료됨 (code={assist.split(':')[1]}) · 중지 후 다시 시작해 주세요."
        else:
            text = "브리지가 연결되면 크루즈 버튼 대기를 켭니다." if not idle else "꺼짐"
        put(rows["주행 보조"], text=text, style="Error.TLabel" if assist.startswith("exited") else
            "Strong.TLabel" if session == "assist" and assist == "standby" else "TLabel")
        note = state["assist_note"] if session == "assist" else ""
        put(rows[""], text=note)
        for label in note_labels:  # No empty line under the assist state.
            if bool(note) != bool(label.winfo_manager()):
                label.grid() if note else label.grid_remove()
        put(rows["Foxglove"], text=f"ws://127.0.0.1:{FOXGLOVE_PORT} · " + ("접속 대기" if state.get("foxglove") else
                                    "시작 중" if not idle else "꺼짐"))
        recording = state["recording"]
        record_phase = recording.get("phase", "idle")
        writing = record_phase in ("starting", "recording", "stopping")
        can_start = phase == "running" and not writing and not closing
        put(record_state, state="normal" if can_start else "disabled")
        put(record_sensors, state="normal" if can_start and session == "sensors" else "disabled")
        put(record_stop, state="normal" if writing and record_phase != "stopping" and not closing else "disabled")
        labels = {"idle": "기록 안 함", "starting": "기록 준비", "recording": "기록 중", "stopping": "파일 마무리 중",
                  "saved": "저장 완료", "error": "기록 오류"}
        record_label = labels.get(record_phase, record_phase)
        if recording.get("path"):
            record_label += {"state": " · 차량 상태", "sensors": " · 영상·라이다 포함"}.get(recording.get("profile"), "")
            record_label += f" · {recording.get('bytes', 0) / 1024**2:.1f} MiB"
            if record_phase in ("saved", "error"):
                record_label += f" · {recording.get('message_count', 0):,}개 메시지"
        error = state.get("recording_error") or recording.get("error")
        if error:
            record_label += "\n" + (error if len(error) < 300 else error[:300] + " … recorder.log 참고")
        put(recording_text, text=record_label, style="Error.TLabel" if error else "TLabel")
        if paths["bag 경로"].get() != recording.get("path", ""):
            paths["bag 경로"].set(recording.get("path", ""))
        if paths["실행 로그"].get() != state["log_dir"]:
            paths["실행 로그"].set(state["log_dir"])
        try:
            stamp = controller.config.stat().st_mtime
        except OSError:
            stamp = None
        if stamp != settings_seen:
            settings_seen = stamp
            put(settings_text, text=settings_summary(controller.config))
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
