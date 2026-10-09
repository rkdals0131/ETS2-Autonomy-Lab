"""Create local pairing/settings; never writes credentials to tracked configuration."""
import json
from pathlib import Path
import secrets

path = Path(__file__).parent / "config" / "bridge.local.json"
if path.exists():
    raise SystemExit(f"Existing settings preserved: {path}")
path.write_text(json.dumps({
    "token": secrets.token_hex(32), "state_port": 17401, "bulk_port": 17400,
    "rig": "../../ot/presets/phase1-highway-private.json", "slots": [3, 4, 6, 7],
    "lidar": "../../ot/presets/phase1-lidar-private.json",
    "duration_s": 60, "color_gain": 1.0, "auto_exposure": True, "shared_gpu": True,
    "camera_hz": 30, "lidar_hz": 10,
    "imu_mount_base_m": [0, 0, 1], "gnss_mount_base_m": [0, 0, 1],
    "gnss_reference_lla": [0, 0, 0],
}, indent=2) + "\n", encoding="utf-8")
print(path)
