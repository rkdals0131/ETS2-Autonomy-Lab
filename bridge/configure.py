"""Create the local relay settings file; tracked configuration stays untouched."""
import json
from pathlib import Path

path = Path(__file__).parent / "config" / "bridge.local.json"
if path.exists():
    raise SystemExit(f"Existing settings preserved: {path}")
path.write_text(json.dumps({
    "gt_localization_tf": True,
    "rig": "../../ot/presets/phase1-highway-fh4-private.json", "slots": [3, 4, 6, 7],
    "lidar": "../../ot/presets/phase1-lidar-private.json",
    "duration_s": 60, "color_gain": 1.0, "auto_exposure": True, "shared_gpu": True,
    "camera_hz": 30, "lidar_hz": 10,
    "preview_hz": 10, "lidar_preview_stride": 4, "capture_warmup": True,
    "imu_mount_base_m": [0, 0, 1], "gnss_mount_base_m": [0, 0, 1],
    "gnss_reference_lla": [0, 0, 0],
}, indent=2) + "\n", encoding="utf-8")
print(path)
