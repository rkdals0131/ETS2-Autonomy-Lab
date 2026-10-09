"""Shared ROS session protocol used by both launcher frontends."""
import json
from pathlib import Path
import shlex
import time


def command(unit, config, ros_env, app_root, duration):
    shell = (f"source {shlex.quote(str(ros_env))} && exec /usr/bin/python3 -u "
             f"{shlex.quote(str(Path(app_root) / 'ros_session.py'))} {shlex.quote(str(config))}")
    return ["systemd-run", "--user", "--quiet", "--pipe", "--wait", "--collect",
            "--unit=" + unit, "--property=KillMode=control-group",
            "--property=RuntimeDirectory=" + unit, "--property=TimeoutStopSec=15",
            "--property=RuntimeMaxSec=" + str(duration + 20), "--", "/bin/bash", "-c", shell]


def send(process, action, **values):
    if process and process.poll() is None:
        process.stdin.write(json.dumps({"action": action, **values}) + "\n")
        process.stdin.flush()


def observed_drive(status, received_at=None):
    """A stale observation never presents an armed axis as enabled."""
    # Windows and Linux monotonic clocks have different origins.
    observed_at = received_at if received_at is not None else status.get("observed_at", time.monotonic())
    lag = max(0.0, time.monotonic() - observed_at)
    age = status.get("drive_state_age_s")
    if age is None or age + lag >= 2:
        return 0, "게임 상태 응답 대기"
    drive = status.get("drive_state") or {}
    return (drive.get("axes", 0) if drive.get("armed") else 0), drive.get("reason", "")
