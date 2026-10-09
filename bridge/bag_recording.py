"""One launcher-owned rosbag2 recorder, with observed process and file status."""
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
import uuid

import yaml


STATE_TOPICS = [
    "/clock", "/tf_static", "/ets2/vehicle/state", "/ets2/vehicle/actuation",
    "/ets2/ground_truth/ego/pose", "/ets2/imu/data_raw", "/ets2/wheels/state",
    "/ets2/wheels/odometry", "/ets2/gnss/fix", "/ets2/sensors/config", "/diagnostics",
]


class BagRecording:
    def __init__(self, config):
        self.config = Path(config)
        self.process = None
        self.directory = None
        self.stop_at = None
        self.status = {"phase": "idle"}

    def start(self, profile):
        if self.process is not None:
            return  # A repeated start never creates a second recorder.
        if profile not in ("state", "sensors"):
            raise ValueError("Unknown recording profile")
        settings = json.loads(self.config.read_text())
        selected = settings.get("sensor_recording_root" if profile == "sensors" else "recording_root")
        if profile == "sensors" and not selected:
            raise ValueError("영상·라이다 기록은 설정의 sensor_recording_root에 저장 폴더를 지정해 주세요.")
        if selected:
            if len(selected) > 2 and selected[1] == ":":
                selected = subprocess.check_output(["wslpath", "-u", selected], text=True, timeout=5).strip()
            root = Path(selected).expanduser().resolve()
            if not root.is_dir():
                raise FileNotFoundError("기록 저장 폴더를 찾을 수 없습니다: " + str(root))
        else:
            root = self.config.parent.parent / "recordings"
            root.mkdir(exist_ok=True)
        free = shutil.disk_usage(root).free
        # Leave room for the recorder's buffers and final MCAP indexes on stop.
        if free < 1024**3:
            raise OSError("기록 저장 공간이 1 GiB 미만입니다: " + str(root))
        topics = list(STATE_TOPICS)
        if profile == "sensors":
            topics += ["/tf", "/ets2/frame_info", "/ets2/frame_info/exposure"]
            for camera in ("C_FN", "C_FW", "C_RL", "C_RR"):
                topics += [f"/ets2/camera/{camera}/preview/image/compressed",
                           f"/ets2/camera/{camera}/preview/camera_info",
                           f"/ets2/ground_truth/{camera}/objects"]
            topics += [f"/ets2/lidar/{name}/points" for name in ("L_F", "L_PL", "L_PR")]
        self.directory = root / (datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
        self.directory.mkdir()
        self.stop_at = None
        self.status = {"phase": "starting", "profile": profile, "path": str(self.directory / "bag"),
                       "bytes": 0, "free_bytes": free, "message_count": 0}
        command = ["ros2", "bag", "record", "-s", "mcap", "-o", self.status["path"],
                   "--disable-keyboard-controls", "--node-name", "ets2_drive_recorder",
                   "--max-cache-size", str(8 * 1024**2 if profile == "state" else 32 * 1024**2),
                   "--topics", *topics]
        try:
            with (self.directory / "recorder.log").open("w") as log:
                self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                                start_new_session=True)
        except OSError:
            self.status["phase"] = "error"
            raise
        self.status.update(phase="recording", pid=self.process.pid)

    def stop(self):
        if self.process is None or self.stop_at is not None:
            return
        self.stop_at = time.monotonic()
        self.status["phase"] = "stopping"
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGINT)
            except ProcessLookupError:
                pass

    def poll(self):
        try:
            if self.directory:
                self.status["bytes"] = sum(p.stat().st_size for p in self.directory.glob("bag/*.mcap"))
                self.status["free_bytes"] = shutil.disk_usage(self.directory).free
        except OSError as error:
            self.status["error"] = "기록 저장 장치 오류: " + str(error)
            self.stop()
        if self.process is None:
            return dict(self.status)
        if self.status["free_bytes"] < 1024**3:
            self.status["error"] = "남은 공간이 1 GiB 미만이어서 기록을 종료했습니다."
            self.stop()
        code = self.process.poll()
        if code is None:
            if self.stop_at is not None and time.monotonic() - self.stop_at > 8:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait(timeout=2)
                self.status["error"] = "기록기 정상 종료 시간 초과. bag 복구가 필요할 수 있습니다."
                return self.poll()
            return dict(self.status)
        expected_stop = self.stop_at is not None
        self.process.wait()
        self.process = None
        metadata = self.directory / "bag" / "metadata.yaml"
        try:
            if metadata.is_file():
                info = yaml.safe_load(metadata.read_text())["rosbag2_bagfile_information"]
                self.status["message_count"] = info["message_count"]
                self.status["topics"] = {t["topic_metadata"]["name"]: t["message_count"]
                                         for t in info["topics_with_message_count"]}
            else:
                self.status["error"] = "기록 종료 정보가 없습니다. recorder.log를 확인해 주세요."
        except (OSError, ValueError, KeyError, yaml.YAMLError) as error:
            self.status["error"] = "기록 결과를 읽지 못했습니다: " + str(error)
        if code != 0 or not expected_stop:
            try:
                log = (self.directory / "recorder.log").read_text(errors="replace")[-2000:]
            except OSError:
                log = "기록 로그를 읽을 수 없습니다."
            self.status["error"] = f"기록기가 종료됐습니다 (code={code}): {log}"
        self.status["phase"] = "error" if self.status.get("error") else "saved"
        self.status.pop("pid", None)
        return dict(self.status)

    def close(self):
        self.stop()
        while self.process is not None:
            self.poll()
            if self.process is not None:
                time.sleep(.1)
