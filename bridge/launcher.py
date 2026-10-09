"""Windows control window for one owned relay + WSL ROS/Foxglove session."""
from collections import deque
import ctypes as C
from ctypes import wintypes as W
import json
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
from otpy.client import Client

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
                      "game": "확인 중", "config": str(self.config), "wsl": {}, "relay": {}}
        self.relay = self.wsl = self.job = self.stop_event = None
        self.unit = None
        self.desired = False
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

    def _start(self, duration):
        if self.desired or self.wsl or self.relay:
            return
        if self.unit:
            raise RuntimeError("이전 WSL 실행의 종료 확인이 필요합니다. 중지를 다시 눌러 주세요.")
        if not self.config.is_file():
            raise RuntimeError("bridge/configure.py를 먼저 실행해 로컬 설정을 만들어 주세요.")
        settings = json.loads(self.config.read_text(encoding="utf-8"))
        if duration is not None:
            duration = float(duration)
            if not 1 <= duration <= 86400:
                raise ValueError("실행 시간은 1~86400초로 입력해 주세요.")
            settings["duration_s"] = duration
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
                              "--property=TimeoutStopSec=6", "--", "/bin/bash", "-c", shell], "wsl")
        self.started = time.monotonic()
        self.update(wsl_pid=self.wsl.pid, unit=self.unit)

    def _signal_wsl(self, action):
        if self.wsl and self.wsl.poll() is None:
            self.wsl.stdin.write(json.dumps({"action": action}) + "\n")
            self.wsl.stdin.flush()

    def _start_relay(self):
        name = "Local\\ETS2AutonomyLab.Stop." + uuid.uuid4().hex
        self.stop_event = K.CreateEventW(None, True, False, name)
        if not self.stop_event:
            raise C.WinError(C.get_last_error())
        self.relay = self.spawn([str(ROOT / "dist" / "ets2_relay.exe"), str(self.config), name], "relay")
        self.last_relay = time.monotonic()
        self.update(phase="connecting", message="게임과 ROS 사이의 실제 응답을 기다립니다.", relay_pid=self.relay.pid)

    def _stop(self, phase="stopped", message="중지됨"):
        self.desired = False
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
                    self.wsl.wait(timeout=7)
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
        except Exception as error:
            errors.append(str(error))
        finally:
            if self.job:
                self.job.close()
            for process in (self.relay, self.wsl):
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
            process = self.wsl if source == "wsl" else self.relay
            if not process or process.pid != pid:
                continue
            try:
                data = json.loads(line)
            except ValueError:
                self.log(source + ": " + line)
                continue
            if source == "wsl" and data.get("type") == "wsl_error":
                raise RuntimeError(data["error"])
            if source == "wsl" and data.get("type") == "wsl_status":
                self.last_wsl = now
                self.update(wsl=data)
                if self.relay and not data["listeners_ready"]:
                    raise RuntimeError("ROS 또는 Foxglove 서버 연결이 내려갔습니다.")
                if data["listeners_ready"] and not self.relay:
                    self._start_relay()
            elif source == "relay" and "sent_bundles" in data:
                self.last_relay = now
                self.update(relay=data)
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
                        self._start(values.get("duration"))
                    elif action == "start":
                        self._start(values.get("duration"))
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
    window.geometry("820x610")
    window.minsize(720, 550)
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
    try:
        duration.set(str(json.loads(controller.config.read_text(encoding="utf-8"))["duration_s"]))
    except (OSError, ValueError, KeyError):
        pass
    def request(action):
        controller.request(action, duration=duration.get())
    start = ttk.Button(actions, text="시작", command=lambda: request("start"))
    start.pack(side="left")
    stop = ttk.Button(actions, text="중지", command=lambda: request("stop"))
    stop.pack(side="left", padx=6)
    restart = ttk.Button(actions, text="재시작", command=lambda: request("restart"))
    restart.pack(side="left")
    ttk.Label(actions, text="실행 시간(초)").pack(side="left", padx=(22, 5))
    duration_entry = ttk.Entry(actions, textvariable=duration, width=9)
    duration_entry.pack(side="left")
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
        nonlocal endpoint, last_logs
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
        summary.config(text=state["message"])
        relay, wsl = state["relay"], state["wsl"]
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
