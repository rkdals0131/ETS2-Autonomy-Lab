"""Bounded native RGB-D stream, recorded through OT_Bundles without a UI."""
import json
from pathlib import Path
import time

from .bundles import BundleReader, save_bundle_archive
from .rig_layout import resolve_layout


def calibrate_gain(client):
    """One raw sample sets a shared, fixed exposure for the whole recording."""
    import numpy as np
    from .color import read_color

    client.request("capture_mirrors", action="arm", format="raw", metadata=False)
    deadline = time.monotonic()+3
    while True:
        sample = client.request("capture_mirrors", metadata=False)
        if sample["phase"] == "ready":
            break
        if sample["phase"] == "error" or time.monotonic() >= deadline:
            raise RuntimeError("Exposure sample failed; check the game is rendering: " + str(sample))
        time.sleep(.005)
    if not client.request("capture_mirrors", action="publish")["published"]:
        raise RuntimeError("Exposure queue is full; drain its existing reader before recording")
    with BundleReader() as reader:
        bundle = None
        while (candidate := reader.read_next()) is not None:
            bundle = candidate
        if bundle is None:
            raise RuntimeError("Exposure sample was not delivered")
        files = {(f["camera"], f["file"]): f["data"] for f in bundle["files"]}
        samples = []
        for view in bundle["manifest"]["views"]:
            rgb, _ = read_color(view["metadata"],
                                lambda name, dtype: np.frombuffer(files[view["camera"], name], dtype=dtype), stride=16)
            samples.append(rgb.ravel().astype(np.float32))
        return 1.0/max(float(np.nanpercentile(np.concatenate(samples), 99)), 1e-7)


def record_bundles(client, config_file, hz, duration, output, capture_format="rgbd8", color_gain=None, vehicles=False):
    config = resolve_layout(json.loads(Path(config_file).read_text(encoding="utf-8")), client)
    directory = Path(output).resolve()
    directory.mkdir()  # Never append to or overwrite an earlier recording.
    report = {"config": config, "requested_hz": hz, "duration_s": duration,
              "format": capture_format, "vehicle_metadata": vehicles, "saved": 0, "binary_bytes": 0}
    reader = None
    started = False
    failure = None
    begin = time.monotonic()
    try:
        client.tier(1)
        client.request("render_probe", enabled=True, vehicle_metadata=vehicles)
        client.request("camera_rig", **config)
        if color_gain is None:
            color_gain = calibrate_gain(client) if capture_format != "raw" else 1.0
        report["color_gain"] = color_gain
        report["version"] = client.request("version")["plugin_version"]
        status = client.request("stream", action="start", hz=hz, duration=duration,
                                format=capture_format, color_gain=color_gain)
        started = True
        report["stream_id"] = status["stream_id"]
        # A status request is only for lifetime/errors. Frame scheduling and
        # publication proceed in the DLL even while Python is writing files.
        next_status = 0
        with open(directory / "index.jsonl", "x", encoding="utf-8") as index:
            while True:
                now = time.monotonic()
                if now >= next_status:
                    status = client.request("stream")
                    next_status = now+.5
                if reader is None and client.request("bundles")["enabled"]:
                    reader = BundleReader()
                bundle = reader.read_next() if reader is not None else None
                if bundle is not None:
                    manifest = bundle["manifest"]
                    if manifest.get("stream_id") != report["stream_id"]:
                        continue  # A queued sample from a preceding capture.
                    name = f"frame-{manifest['render_frame_id']:08d}.zip"
                    save_bundle_archive(bundle, directory / name)
                    size = sum(len(item["data"]) for item in bundle["files"])
                    report["saved"] += 1
                    report["binary_bytes"] += size
                    index.write(json.dumps({"bundle": name, "sequence": bundle["sequence"],
                                            "render_frame_id": manifest["render_frame_id"], "binary_bytes": size})+"\n")
                    index.flush()
                elif not status["running"]:
                    break
                else:
                    time.sleep(.002)
        report["stream"] = status
        if status.get("reason") == "error":
            raise RuntimeError(status["last_error"])
        if not report["saved"]:
            raise RuntimeError("Stream ended without a complete bundle; check the game is rendering")
    except BaseException as error:
        failure = error
        report["error"] = str(error) or type(error).__name__
        raise
    finally:
        cleanup_errors = []
        try:
            if started:
                report["stream"] = client.request("stream", action="stop")
        except Exception as error:
            cleanup_errors.append(str(error))
        if reader is not None:
            reader.close()
        try:
            report["cleanup"] = client.panic()
        except Exception as error:
            cleanup_errors.append(str(error))
        report["elapsed_s"] = time.monotonic()-begin
        report["cleanup_errors"] = cleanup_errors
        (directory / "run.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        if cleanup_errors and failure is None:
            raise RuntimeError("Recording cleanup failed: " + "; ".join(cleanup_errors))
    return {"directory": str(directory), "saved": report["saved"], "binary_bytes": report["binary_bytes"],
            "color_gain": color_gain, "stream": report["stream"], "cleanup": report["cleanup"]}
