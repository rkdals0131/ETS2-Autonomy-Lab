"""Launcher-owned ROS session. systemd owns the complete Linux process tree."""
import json
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
from ets2_msgs.msg import VehicleState
from rclpy.qos import QoSProfile, ReliabilityPolicy
from bag_recording import BagRecording


def main():
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
                elif action == "drive_start":
                    if drive and drive.poll() is None:
                        continue  # A repeated start keeps the current owner.
                    release_drive()
                    drive_log = (runtime_directory / "drive.log").open("w")
                    drive = subprocess.Popen(["ros2", "run", "ets2_bridge", "drive_speed", "--ros-args",
                        "-p", "arm:=true", "-p", "path_file:=" + request["path"],
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
            if now - heartbeat > 8:
                raise RuntimeError("Windows launcher heartbeat expired")
            if child.poll() is not None:
                raise RuntimeError("ROS launch exited: " + log_path.read_text(errors="replace")[-4000:])
            if now - last_report >= 1:
                interfaces = json.loads(subprocess.check_output(["ip", "-j", "-4", "addr", "show", "eth0"]))
                ip = next(a["local"] for i in interfaces for a in i["addr_info"] if a["family"] == "inet")
                print(json.dumps({"type": "wsl_status", "pid": os.getpid(), "ros_pid": child.pid,
                                  "ip": ip, "listeners_ready": ports <= listeners(), "paused": paused,
                                  "state_age_s": now - last_state if last_state else None,
                                  "diagnostics_age_s": now - last_diagnostics if last_diagnostics else None,
                                  "recording": recording.poll(),
                                  "drive_pid": drive.pid if drive and drive.poll() is None else None,
                                  "drive_exit": drive.poll() if drive else drive_exit,
                                  "drive_log": (runtime_directory / "drive.log").read_text(errors="replace")[-1500:] if (runtime_directory / "drive.log").exists() else ""}), flush=True)
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


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(json.dumps({"type": "wsl_error", "error": str(error)}), flush=True)
        sys.exit(1)
