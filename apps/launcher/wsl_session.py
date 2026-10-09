"""Launcher-owned ROS session. systemd owns the complete Linux process tree."""
import json
import fcntl
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import time

import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from ets2_msgs.msg import VehicleState, DriveState
from rcl_interfaces.srv import SetParametersAtomically
from rclpy.parameter import Parameter
from rclpy.qos import QoSProfile, ReliabilityPolicy
from bag_recording import BagRecording


def run_session():
    config = Path(sys.argv[1]).resolve()
    settings = json.loads(config.read_text())
    runtime_directory = Path(os.environ["RUNTIME_DIRECTORY"])
    os.environ["ROS_LOG_DIR"] = str(runtime_directory / "ros-logs")
    ports = {int(settings.get("state_port", 17401)), int(settings.get("bulk_port", 17400)), 8765}
    # A listener owned by another run is a conflict, not a process to adopt or kill.
    def listeners():
        found = set()
        for table in ("/proc/net/tcp", "/proc/net/tcp6"):
            for line in Path(table).read_text().splitlines()[1:]:
                fields = line.split()
                if fields[3] == "0A":
                    found.add(int(fields[1].split(":")[1], 16))
        return found
    conflicts = ports & listeners()
    if conflicts:
        raise RuntimeError(f"Existing listeners on {sorted(conflicts)}; stop the existing bridge first")
    commands = queue.Queue()
    def read_commands():
        try:
            for line in sys.stdin:
                commands.put(json.loads(line))
        finally:
            commands.put({"action": "stop"})
    threading.Thread(target=read_commands, daemon=True).start()
    running = True
    def stop(*_):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    rclpy.init()
    node = rclpy.create_node("ets2_launcher_monitor")
    last_state = last_diagnostics = 0.0
    paused = None
    def state(message):
        nonlocal last_state, paused
        last_state, paused = time.monotonic(), message.paused
    def diagnostics(_):
        nonlocal last_diagnostics
        last_diagnostics = time.monotonic()
    qos = QoSProfile(depth=1, reliability=ReliabilityPolicy.RELIABLE)
    node.create_subscription(VehicleState, "/ets2/vehicle/state", state, qos)
    node.create_subscription(DiagnosticArray, "/diagnostics", diagnostics, qos)
    drive_state = None
    drive_state_time = 0.0
    def input_state(message):
        nonlocal drive_state, drive_state_time
        drive_state_time = time.monotonic()
        drive_state = {"armed": message.armed, "axes": message.axes, "reason": message.reason,
                       "independent_axes": message.independent_axes}
    node.create_subscription(DriveState, "/ets2/drive/state", input_state, qos)
    mode_client = node.create_client(SetParametersAtomically, "/ets2_drive_speed/set_parameters_atomically")
    mode_future = None
    mode_started = 0.0
    child = None
    drive = None
    drive_log = None
    drive_exit = None
    def release_drive():
        nonlocal drive, drive_log, drive_exit
        if drive and drive.poll() is None:
            os.killpg(drive.pid, signal.SIGINT)
            try:
                drive.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(drive.pid, signal.SIGKILL)
                drive.wait(timeout=2)
        if drive:
            drive_exit = drive.returncode
        if drive_log:
            drive_log.close()
            drive_log = None
        drive = None
    recording = BagRecording(config)
    try:
        # systemd removes this run's log even when the supervisor is killed.
        log_path = runtime_directory / "ros.log"
        with log_path.open("w") as log:
            child = subprocess.Popen(["ros2", "launch", "ets2_bridge", "bridge.launch.py",
                                      "config:=" + str(config)], stdout=log, stderr=log,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        heartbeat = last_report = time.monotonic()
        while running:
            rclpy.spin_once(node, timeout_sec=0.1)
            now = time.monotonic()
            while not commands.empty():
                request = commands.get()
                action = request["action"]
                if action == "heartbeat":
                    heartbeat = now
                elif action == "stop":
                    running = False
                elif action in ("drive_start", "drive_update"):
                    if action == "drive_update":
                        if not drive or drive.poll() is not None:
                            print(json.dumps({"type": "drive_error", "error": "보조가 해제됐습니다. 상태 확인 후 명시적으로 다시 켜 주세요."}), flush=True)
                            continue
                        if mode_future is not None:
                            print(json.dumps({"type": "drive_error", "error": "모드 적용 응답을 기다리고 있습니다."}), flush=True)
                            continue
                        if not mode_client.service_is_ready():
                            print(json.dumps({"type": "drive_error", "error": "주행 제어기가 응답하지 않습니다."}), flush=True)
                            continue
                        update = SetParametersAtomically.Request()
                        update.parameters = [Parameter("acc_enabled", value=bool(request.get("acc", True))).to_parameter_msg(),
                                             Parameter("lcc_enabled", value=bool(request.get("lcc", True))).to_parameter_msg(),
                                             Parameter("target_speed_mps", value=float(request["speed_mps"])).to_parameter_msg()]
                        mode_future = mode_client.call_async(update)
                        mode_started = now
                        continue  # Mode change keeps the current owner and epoch.
                    if drive and drive.poll() is None:
                        continue  # A duplicate start cannot create a second owner.
                    release_drive()
                    drive_log = (runtime_directory / "drive.log").open("w")
                    drive = subprocess.Popen(["ros2", "run", "ets2_autonomy", "drive_speed", "--ros-args",
                        "-p", "arm:=true", "-p", "path_file:=" + request["path"],
                        "-p", "acc_enabled:=" + str(bool(request.get("acc", True))).lower(),
                        "-p", "lcc_enabled:=" + str(bool(request.get("lcc", True))).lower(),
                        "-p", "target_speed_mps:=" + str(float(request["speed_mps"]))],
                        stdout=drive_log, stderr=drive_log, stdin=subprocess.DEVNULL, start_new_session=True)
                    drive_exit = None
                elif action == "drive_stop":
                    release_drive()
                elif action == "record_start":
                    try:
                        recording.start(request.get("profile", "state"))
                    except (OSError, ValueError, subprocess.SubprocessError) as error:
                        print(json.dumps({"type": "recording_error", "error": str(error)}), flush=True)
                elif action == "record_stop":
                    recording.stop()
            if mode_future is not None:
                if mode_future.done():
                    response = mode_future.result()
                    if not response.result.successful:
                        print(json.dumps({"type": "drive_error", "error": response.result.reason}), flush=True)
                    mode_future = None
                elif now - mode_started > 1:
                    mode_client.remove_pending_request(mode_future)
                    mode_future = None
                    print(json.dumps({"type": "drive_error", "error": "모드 적용 응답 시간 초과"}), flush=True)
            if now - heartbeat > 8:
                raise RuntimeError("Launcher heartbeat expired")
            if child.poll() is not None:
                raise RuntimeError("ROS launch exited: " + log_path.read_text(errors="replace")[-4000:])
            if now - last_report >= 1:
                ip = settings.get("bind_address")
                if not ip:
                    interfaces = json.loads(subprocess.check_output(["ip", "-j", "-4", "addr", "show", "eth0"]))
                    ip = next(a["local"] for i in interfaces for a in i["addr_info"] if a["family"] == "inet")
                status = {"type": "wsl_status", "pid": os.getpid(), "ros_pid": child.pid,
                                  "observed_at": now,
                                  "ip": ip, "listeners_ready": ports <= listeners(), "paused": paused,
                                  "state_age_s": now - last_state if last_state else None,
                                  "diagnostics_age_s": now - last_diagnostics if last_diagnostics else None,
                                  "recording": recording.poll(),
                                  "drive_pid": drive.pid if drive and drive.poll() is None else None,
                                  "drive_exit": drive.poll() if drive else drive_exit,
                                  "drive_state": drive_state, "drive_state_age_s": now - drive_state_time if drive_state_time else None,
                                  "drive_log": (runtime_directory / "drive.log").read_text(errors="replace")[-1500:] if (runtime_directory / "drive.log").exists() else ""}
                temporary = runtime_directory / "status.json.tmp"
                temporary.write_text(json.dumps(status), encoding="utf-8")
                os.replace(temporary, runtime_directory / "status.json")
                print(json.dumps(status), flush=True)
                last_report = now
        print(json.dumps({"type": "wsl_stopping"}), flush=True)
    finally:
        try:
            release_drive()
            recording.close()
            print(json.dumps({"type": "recording_status", "recording": recording.poll()}), flush=True)
        finally:
            if child and child.poll() is None:
                os.killpg(child.pid, signal.SIGINT)
                try:
                    child.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=2)
            node.destroy_node()
            rclpy.shutdown()


def main():
    # The same lock excludes Windows and Linux launchers before either spawns ROS.
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    with (runtime / "ets2-bridge-session.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("다른 런처가 ROS 브리지를 실행 중입니다. 해당 런처에서 먼저 중지해 주세요.") from None
        run_session()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(json.dumps({"type": "wsl_error", "error": str(error)}), flush=True)
        sys.exit(1)
