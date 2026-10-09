"""Native Linux owner for the shared ROS session; no Windows host imports."""
import argparse
import fcntl
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

UNIT = "ets2-linux-launcher"
PROJECT_ROOT = Path(__file__).resolve().parents[2]


def unit_state():
    result = subprocess.run(["systemctl", "--user", "show", UNIT, "-p", "ActiveState", "-p", "MainPID"],
                            capture_output=True, text=True, timeout=5)
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


def describe(status):
    lag = max(0.0, time.monotonic() - status["observed_at"])
    age = status.get("state_age_s")
    connected = age is not None and age + lag < 3
    drive = status.get("drive_state") or {}
    drive_age = status.get("drive_state_age_s")
    axes = drive.get("axes", 0) if drive.get("armed") and drive_age is not None and drive_age + lag < 2 else 0
    recording = status.get("recording") or {}
    return (f"ROS PID {status['ros_pid']} · " +
            ("게임 상태 수신 중" if connected else "Windows 릴레이 연결 대기") +
            f" · ACC {'켜짐' if axes & 2 else '꺼짐'} · LCC {'켜짐' if axes & 1 else '꺼짐'}" +
            (f" · {drive.get('reason', '')}" if drive_age is not None and drive_age + lag < 2 else "") +
            f" · 기록 {recording.get('phase', 'idle')}" +
            (f" · {recording['path']}" if recording.get("path") else ""))


def start(config, runtime):
    settings = json.loads(config.read_text(encoding="utf-8"))
    duration = float(settings.get("duration_s", 60))
    if not math.isfinite(duration) or duration <= 0:
        raise ValueError("duration_s must be positive")
    # Keep this lock for the controller lifetime, including startup and cleanup.
    with (runtime / (UNIT + ".lock")).open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("Linux 런처가 이미 실행 중입니다. status로 확인하거나 stop으로 중지하세요.")
            return
        if unit_state().get("ActiveState") in ("active", "activating", "deactivating"):
            print("Linux ROS 세션이 이미 실행 중입니다.")
            return
        command = ["systemd-run", "--user", "--quiet", "--pipe", "--wait", "--collect", "--unit=" + UNIT,
                   "--property=KillMode=control-group", "--property=RuntimeDirectory=" + UNIT,
                   "--property=TimeoutStopSec=15", "--property=RuntimeMaxSec=" + str(duration + 20), "--",
                   "/bin/bash", "-c",
                   f"source {shlex.quote(str(PROJECT_ROOT / 'bridge' / 'ros-env.sh'))} && "
                   f"exec /usr/bin/python3 -u {shlex.quote(str(Path(__file__).with_name('wsl_session.py')))} {shlex.quote(str(config))}"]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)
        lines = queue.Queue()
        def read():
            for line in process.stdout:
                lines.put(("event", line.rstrip()))
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        def input_commands():
            for line in sys.stdin:
                lines.put(("command", line.strip()))
            lines.put(("command", "stop"))
        threading.Thread(target=input_commands, daemon=True).start()
        previous = None
        heartbeat = 0.0
        started = time.monotonic()
        try:
            print("Linux ROS를 시작합니다. Windows에서는 apps/launch.cmd --host-only로 릴레이를 연결하세요.", flush=True)
            print("Ctrl+C 또는 apps/launch.sh stop으로 이 ROS 세션을 중지합니다.", flush=True)
            print("명령: status | stop | record state/sensors | record-stop | assist <차로JSON> <km/h> both/acc/lcc | mode <km/h> both/acc/lcc | assist-stop", flush=True)
            while process.poll() is None:
                now = time.monotonic()
                if now - started >= duration:
                    break
                if now - heartbeat >= 1:
                    process.stdin.write('{"action":"heartbeat"}\n')
                    process.stdin.flush()
                    heartbeat = now
                try:
                    source, line = lines.get(timeout=0.1)
                except queue.Empty:
                    continue
                if source == "command":
                    try:
                        words = shlex.split(line)
                        if not words:
                            continue
                        if words == ["status"]:
                            status = runtime / UNIT / "status.json"
                            print(describe(json.loads(status.read_text())) if status.is_file() else "ROS 시작 중", flush=True)
                            continue
                        if words == ["stop"]:
                            break
                        if words == ["assist-stop"]:
                            request = {"action": "drive_stop"}
                        elif words == ["record-stop"]:
                            request = {"action": "record_stop"}
                        elif len(words) == 2 and words[0] == "record" and words[1] in ("state", "sensors"):
                            request = {"action": "record_start", "profile": words[1]}
                        elif (len(words) == 4 and words[0] == "assist") or (len(words) == 3 and words[0] == "mode"):
                            speed = float(words[-2]) / 3.6
                            mode = words[-1]
                            if not math.isfinite(speed) or speed < 0 or mode not in ("both", "acc", "lcc"):
                                raise ValueError("속도는 0 이상, 모드는 both/acc/lcc입니다.")
                            request = {"action": "drive_start" if words[0] == "assist" else "drive_update",
                                       "speed_mps": speed, "acc": mode != "lcc", "lcc": mode != "acc"}
                            if words[0] == "assist":
                                path = Path(words[1]).expanduser().resolve()
                                if not path.is_file():
                                    raise FileNotFoundError("현재 위치용 차로 JSON을 먼저 생성해 주세요: " + str(path))
                                request["path"] = str(path)
                        else:
                            raise ValueError("지원하는 명령을 입력해 주세요.")
                        process.stdin.write(json.dumps(request) + "\n")
                        process.stdin.flush()
                    except (OSError, ValueError) as error:
                        print(error, flush=True)
                    continue
                try:
                    event = json.loads(line)
                except ValueError:
                    print(line, flush=True)
                    continue
                if event.get("type") == "wsl_status":
                    text = describe(event)
                    if text != previous:
                        print(text, flush=True)
                        previous = text
                elif event.get("type") in ("wsl_error", "drive_error", "recording_error"):
                    print(event["error"], flush=True)
            reader.join(timeout=1)
            while not lines.empty():
                source, line = lines.get()
                if source == "event" and '"wsl_error"' in line:
                    print(json.loads(line)["error"], flush=True)
            if process.returncode:
                raise RuntimeError("Linux ROS 세션이 종료됐습니다. 위 오류를 확인해 주세요.")
        except KeyboardInterrupt:
            pass
        finally:
            # A duplicate CLI cannot reach this branch while the owner holds its lock.
            subprocess.run(["systemctl", "--user", "stop", UNIT], capture_output=True, timeout=20)
            try:
                process.wait(timeout=5)
            finally:
                reader.join(timeout=1)
                process.stdin.close()
                process.stdout.close()
        print("Linux ROS 세션을 중지했습니다.", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Linux ROS 실행·상태·중지. Windows 게임/릴레이는 별도로 실행합니다.")
    parser.add_argument("action", nargs="?", choices=("start", "status", "stop"), default="start")
    parser.add_argument("--config", type=Path, default=PROJECT_ROOT / "bridge" / "config" / "bridge.local.json")
    args = parser.parse_args()
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    if args.action == "start":
        start(args.config.resolve(), runtime)
    elif args.action == "stop":
        subprocess.run(["systemctl", "--user", "stop", UNIT], check=True, timeout=20)
        print("Linux ROS 세션 중지 완료. Windows 런처의 세션은 변경하지 않습니다.")
    else:
        state = unit_state()
        print(f"Linux ROS: {state.get('ActiveState', 'inactive')} · PID {state.get('MainPID', '0')}")
        status = runtime / UNIT / "status.json"
        if state.get("ActiveState") == "active" and status.is_file():
            print(describe(json.loads(status.read_text(encoding="utf-8"))))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, RuntimeError) as error:
        raise SystemExit(str(error))
