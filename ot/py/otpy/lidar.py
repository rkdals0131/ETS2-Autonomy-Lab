"""Instantaneous ideal LiDAR beams sampled from co-located rendered depth views.

No ray casting, surface interpolation, intensity, noise or rolling scan model.
Missing depth is unknown, never evidence of an empty beam.
"""
import io
import json
from pathlib import Path

import numpy as np

from .bundles import load_bundle
from .reconstruction import reconstruct_view


def load_lidar_profile(path):
    """Decode the external beam pattern once, before processing any frames."""
    config = json.loads(Path(path).read_text(encoding="utf-8"))
    sensors = []
    for item in config["sensors"]:
        start, end, step = np.asarray(item["azimuth_deg"], dtype=float)
        bands = np.asarray(item["elevation_bands_deg"], dtype=float).reshape(-1, 3)
        maximum = float(item["max_range"])
        if (not np.isfinite([start, end, step, maximum]).all()
                or not -180 <= start < end <= 180 or step <= 0 or maximum <= 0
                or not len(bands) or not np.isfinite(bands).all()
                or np.any(bands[:, 0] <= -90) or np.any(bands[:, 1] >= 90)
                or np.any(bands[:, 0] >= bands[:, 1]) or np.any(bands[:, 2] < 2)
                or np.any(bands[:, 2] != np.floor(bands[:, 2]))):
            raise ValueError("Invalid beam angles, channel count or maximum range")
        sources = list(item["sources"])
        if not sources or len(set(sources)) != len(sources) or item["axis_camera"] not in sources:
            raise ValueError("Each sensor needs distinct sources including its axis camera")
        azimuths = np.arange(start, end + step*1e-7, step)
        elevations = np.unique(np.concatenate([np.linspace(lo, hi, int(n)) for lo, hi, n in bands]))
        azimuth, elevation = np.meshgrid(azimuths, elevations)
        az, el = np.deg2rad(azimuth.ravel()), np.deg2rad(elevation.ravel())
        direction = np.column_stack((np.cos(el)*np.cos(az), np.cos(el)*np.sin(az), np.sin(el)))
        sensors.append({"settings": item, "azimuths": azimuths, "elevations": elevations,
                        "azimuth": azimuth.ravel(), "elevation": elevation.ravel(),
                        "direction": direction, "max_range": maximum})
    if not sensors or len({s["settings"]["name"] for s in sensors}) != len(sensors):
        raise ValueError("LiDAR sensor names must be present and distinct")
    return sensors


def sample_lidars(bundle, profile, *, include_points=True):
    """Return all beams, including misses, from a decoded same-frame bundle.

    profile is the canonical result of load_lidar_profile(). Source priority is
    geometric coverage, not depth validity: a missing narrow-view pixel is not
    replaced with a potentially different broad-view surface.
    include_points=False omits derivable XYZ/angle arrays for compact recording.
    """
    views = {v["camera"]: v["metadata"] for v in bundle["manifest"]["views"]}
    names = list(dict.fromkeys(name for s in profile for name in s["settings"]["sources"]))
    missing = set(names) - views.keys()
    if missing:
        raise ValueError("Missing LiDAR source cameras: " + ", ".join(sorted(missing)))
    keys = {(views[n]["observation_session_qpc"], views[n]["render_frame_id"]) for n in names}
    units = {views[n]["geometry_pass"]["camera_at_compile"]["world_units"] for n in names}
    manifest = bundle["manifest"]
    if (keys != {(manifest["observation_session_qpc"], manifest["render_frame_id"])}
            or len(units) != 1):
        raise ValueError("LiDAR sources must share one observation session, Present interval and unit system")
    files = {(f["camera"], f["file"]): f["data"] for f in bundle["files"]}
    pieces, descriptions = [], []
    first_beam = 0
    # Convert LiDAR forward/left/up to the game's pass camera right/up/back.
    camera_from_sensor = np.array([[0., -1., 0.], [0., 0., 1.], [-1., 0., 0.]])
    for sensor_index, sensor in enumerate(profile):
        settings, direction = sensor["settings"], sensor["direction"]
        axis = views[settings["axis_camera"]]["geometry_pass"]["camera_at_compile"]
        origin = np.asarray(axis["camera_world_xyz"], dtype=float)
        axis_rotation = np.asarray(axis["camera_rotation_row_major"]).reshape(4, 4)[:3, :3]
        world_from_sensor = np.linalg.inv(axis_rotation) @ camera_from_sensor
        world_direction = direction @ world_from_sensor.T
        world_direction /= np.linalg.norm(world_direction, axis=1, keepdims=True)
        count = len(direction)
        ranges = np.full(count, np.nan, dtype=np.float32)
        status = np.ones(count, dtype=np.uint8)  # 1 = outside all source views
        source_index = np.full(count, -1, dtype=np.int16)
        pixels = np.full((count, 2), -1, dtype=np.int32)
        angular_error = np.full(count, np.nan, dtype=np.float32)
        source_info = []
        for name in settings["sources"]:
            meta = views[name]
            camera = meta["geometry_pass"]["camera_at_compile"]
            source_origin = np.asarray(camera["camera_world_xyz"], dtype=float)
            # Co-location is essential: translated views reveal different hidden
            # surfaces. 1e-4 covers captured FP32 cell-local rounding, not mounts.
            delta = source_origin-origin
            if not np.isfinite(delta).all() or np.linalg.norm(delta) > 1e-4:
                raise ValueError("LiDAR source cameras have different optical origins: " + name)
            rotation = np.asarray(camera["camera_rotation_row_major"]).reshape(4, 4)[:3, :3]
            projection = np.asarray(camera["projection_row_major"]).reshape(4, 4)
            if np.any(projection[3, [0, 1, 3]] != 0) or projection[3, 2] == 0:
                raise ValueError("LiDAR requires a perspective source camera")
            vp = meta["geometry_gpu"]["viewports"][0]
            width, height = meta["images"][0]["width"], meta["images"][0]["height"]
            eye_direction = world_direction @ rotation.T
            clip = np.column_stack((eye_direction, np.zeros(count))) @ projection.T
            ndc = np.full((count, 2), np.nan)
            np.divide(clip[:, :2], clip[:, 3:4], out=ndc, where=clip[:, 3:4] > 0)
            uv = np.column_stack((vp["x"]+(ndc[:, 0]+1)*vp["width"]/2,
                                  vp["y"]+(1-ndc[:, 1])*vp["height"]/2))
            covered = ((source_index < 0) & (clip[:, 3] > 0) & (eye_direction[:, 2] < 0)
                       & np.isfinite(uv).all(axis=1) & (np.abs(ndc) <= 1).all(axis=1)
                       & (uv >= 0).all(axis=1) & (uv < [width, height]).all(axis=1))
            indices = np.flatnonzero(covered)
            # Pixel coordinates above refer to boundaries; floor chooses the
            # nearest center, with no interpolation across object boundaries.
            selected_pixels = np.floor(uv[indices]).astype(np.int32)
            arrays, _ = reconstruct_view(meta,
                lambda filename, dtype: np.frombuffer(files[name, filename], dtype=dtype),
                pixel_xy=selected_pixels)
            sample = arrays["xyz_camera"].astype(np.float64)
            # Use the chosen pixel's Z on the requested beam (constant-Z pixel
            # footprint approximation), rather than snapping the beam's angle.
            eye_offset = (origin-source_origin) @ rotation.T
            distance = (sample[:, 2]-eye_offset[2]) / eye_direction[indices, 2]
            source_index[indices] = names.index(name)
            pixels[indices] = selected_pixels
            status[indices] = 2  # covered, but missing/invalid depth
            has_depth = arrays["valid"] & np.isfinite(distance) & (distance > 0)
            inside = has_depth & (distance <= sensor["max_range"])
            status[indices[has_depth & ~inside]] = 3  # beyond maximum range
            status[indices[inside]] = 0
            ranges[indices[inside]] = distance[inside]
            actual_direction = sample[has_depth] @ np.linalg.inv(rotation).T
            actual_direction /= np.linalg.norm(actual_direction, axis=1, keepdims=True)
            dot = (actual_direction * world_direction[indices[has_depth]]).sum(axis=1)
            angular_error[indices[has_depth]] = np.rad2deg(np.arccos(np.clip(dot, -1, 1)))
            source_info.append({"camera": name, "selected_beams": len(indices),
                                "origin_offset_world_xyz": delta.tolist(),
                                "camera_qpc": camera["qpc"], "copy_qpc": meta["copy_submission_qpc"],
                                "width": width, "height": height})
        pieces.append({"range": ranges, "status": status,
                       "source_camera_index": source_index,
                       "source_pixel_xy": pixels, "source_ray_error_deg": angular_error})
        if include_points:
            pieces[-1].update({"sensor_index": np.full(count, sensor_index, dtype=np.int16),
                               "azimuth_deg": sensor["azimuth"].astype(np.float32),
                               "elevation_deg": sensor["elevation"].astype(np.float32),
                               "xyz_sensor": (direction*ranges[:, None]).astype(np.float32),
                               "xyz_world": origin + world_direction*ranges[:, None]})
        descriptions.append({"name": settings["name"], "settings": settings,
                             "first_beam": first_beam, "beam_count": count,
                             "shape_channels_columns": [len(sensor["elevations"]), len(sensor["azimuths"])],
                             "azimuth_deg": sensor["azimuths"].tolist(),
                             "elevation_deg": sensor["elevations"].tolist(),
                             "origin_world_xyz": origin.tolist(),
                             "world_from_sensor_rotation": world_from_sensor.tolist(),
                             "source_views": source_info,
                             "returns": int(np.count_nonzero(status == 0)),
                             "outside_views": int(np.count_nonzero(status == 1)),
                             "invalid_depth": int(np.count_nonzero(status == 2)),
                             "beyond_range": int(np.count_nonzero(status == 3))})
        first_beam += count
    session, frame = next(iter(keys))
    arrays = {name: np.concatenate([p[name] for p in pieces]) for name in pieces[0]}
    metadata = {"render_frame_id": frame, "observation_session_qpc": session,
                "world_units": next(iter(units)), "source_cameras": names, "sensors": descriptions,
                "sensor_axes": "x forward, y left, z up; optical axis and origin of axis_camera",
                "status_codes": {"0": "return", "1": "outside_source_views", "2": "invalid_depth", "3": "beyond_max_range"},
                "depth_source": "geometry DSV; nearest pixel, constant-Z footprint on requested beam",
                "scope": "Ideal instantaneous rendered-depth samples; no noise, intensity, rolling scan or ray casting. Missing beams are unknown. Engine visibility omissions remain."}
    return arrays, metadata


def attach_lidar(bundle, profile):
    """Add a same-frame LiDAR NPZ without changing the input bundle.

    Store the inner NPZ without compression: the existing outer Zstandard
    writer compresses it together with RGB-D, avoiding two codec passes.
    """
    arrays, metadata = sample_lidars(bundle, profile, include_points=False)
    with io.BytesIO() as stream:
        np.savez(stream, **arrays, metadata_json=json.dumps(metadata, ensure_ascii=False))
        data = stream.getvalue()
    return {**bundle, "manifest": {**bundle["manifest"], "lidar_file": "lidar.npz"}, "lidar": data}


def read_lidar(bundle):
    """Decode the optional recorded LiDAR, bound to its containing frame."""
    with np.load(io.BytesIO(bundle["lidar"]), allow_pickle=False) as recorded:
        metadata = json.loads(str(recorded["metadata_json"]))
        arrays = {key: recorded[key] for key in recorded.files if key != "metadata_json"}
    manifest = bundle["manifest"]
    if ((metadata["observation_session_qpc"], metadata["render_frame_id"])
            != (manifest["observation_session_qpc"], manifest["render_frame_id"])):
        raise ValueError("Recorded LiDAR belongs to a different camera bundle")
    count = len(arrays["range"])
    for key, shape in (("range", (count,)), ("status", (count,)),
                       ("source_camera_index", (count,)), ("source_pixel_xy", (count, 2)),
                       ("source_ray_error_deg", (count,))):
        if arrays[key].shape != shape:
            raise ValueError("Recorded LiDAR arrays do not align: " + key)
    if "xyz_world" not in arrays:
        # Range plus full-precision beam angles and captured pose define XYZ.
        # Expand only on consumption; do not persist/compress duplicate points.
        parts = []
        offset = 0
        for index, sensor in enumerate(metadata["sensors"]):
            az, el = np.meshgrid(sensor["azimuth_deg"], sensor["elevation_deg"])
            radians_az, radians_el = np.deg2rad(az.ravel()), np.deg2rad(el.ravel())
            direction = np.column_stack((np.cos(radians_el)*np.cos(radians_az),
                                         np.cos(radians_el)*np.sin(radians_az), np.sin(radians_el)))
            start, length = sensor["first_beam"], sensor["beam_count"]
            if start != offset or length != len(direction) or start+length > count:
                raise ValueError("Recorded LiDAR beam pattern does not align with ranges")
            offset += length
            ranges = arrays["range"][start:start+length, None]
            world_direction = direction @ np.asarray(sensor["world_from_sensor_rotation"]).T
            world_direction /= np.linalg.norm(world_direction, axis=1, keepdims=True)
            parts.append({"sensor_index": np.full(length, index, dtype=np.int16),
                          "azimuth_deg": az.ravel().astype(np.float32),
                          "elevation_deg": el.ravel().astype(np.float32),
                          "xyz_sensor": (direction*ranges).astype(np.float32),
                          "xyz_world": np.asarray(sensor["origin_world_xyz"])+world_direction*ranges})
        if not parts or offset != count:
            raise ValueError("Recorded LiDAR beam pattern does not cover its ranges")
        arrays.update({key: np.concatenate([p[key] for p in parts]) for key in parts[0]})
    else:
        for key, shape in (("sensor_index", (count,)), ("azimuth_deg", (count,)), ("elevation_deg", (count,)),
                           ("xyz_sensor", (count, 3)), ("xyz_world", (count, 3))):
            if arrays[key].shape != shape:
                raise ValueError("Recorded LiDAR arrays do not align: " + key)
    return arrays, metadata


def save_lidar(directory, config, output):
    arrays, metadata = sample_lidars(load_bundle(directory), load_lidar_profile(config))
    with open(output, "xb") as destination:
        np.savez_compressed(destination, **arrays, metadata_json=json.dumps(metadata, ensure_ascii=False))
    return {"output": str(output), "render_frame_id": metadata["render_frame_id"],
            "world_units": metadata["world_units"], "beams": len(arrays["range"]),
            "sensors": [{k:s[k] for k in ("name", "beam_count", "returns", "outside_views", "invalid_depth", "beyond_range")}
                        for s in metadata["sensors"]]}
