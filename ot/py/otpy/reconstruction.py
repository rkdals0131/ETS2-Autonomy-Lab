"""Offline reconstruction of DLL captures using the captured CPU pass camera.

NumPy is optional for the pipe/shared-state client and imported only by users of
this module. Geometry depth and shader-adjusted attributes Z remain distinct.
"""
import json
from pathlib import Path

import numpy as np
from .color import read_color


def load_view(directory, camera=None):
    """Load one camera's metadata and binary reader from a folder or bundle."""
    if camera is not None:
        from .bundles import load_bundle
        bundle = load_bundle(directory)
        view = next((v for v in bundle["manifest"]["views"] if v["camera"] == camera), None)
        if view is None:
            raise ValueError("Camera is absent from the bundle: " + camera)
        files = {item["file"]: item["data"] for item in bundle["files"] if item["camera"] == camera}

        def read_data(filename, dtype):
            return np.frombuffer(files[filename], dtype=dtype)
        return view["metadata"], read_data
    directory = Path(directory).resolve()
    meta = json.loads((directory / "images.json").read_text(encoding="utf-8"))

    def read_data(filename, dtype):
        path = (directory / filename).resolve()
        if not path.is_relative_to(directory):
            raise ValueError("Image file is outside the capture directory")
        return np.fromfile(path, dtype=dtype)
    return meta, read_data


def reconstruct_capture(directory, depth_source="geometry"):
    """Return image-aligned point arrays and their coordinate/source metadata.

    World coordinates use the pass's base camera, not a per-draw override trace.
    Invalid points are NaN. Lengths retain game units; no metric calibration is
    inferred from agreement between two rendering paths.
    """
    meta, read_data = load_view(directory)
    return reconstruct_view(meta, read_data, depth_source)


def reconstruct_view(meta, read_data, depth_source="geometry", *, stride=1, color=False):
    """Decode one pass from files or immutable bundle bytes with the same math.

    read_data(filename, dtype) returns the raw one-dimensional array. Stride
    samples original pixel centers; it never changes intrinsics or downsizes Z.
    """
    if depth_source not in ("geometry", "attributes"):
        raise ValueError("depth_source must be geometry or attributes")
    if not isinstance(stride, int) or stride < 1:
        raise ValueError("Pixel stride must be a positive integer")
    camera = meta["geometry_pass"]["camera_at_compile"]
    name = meta["camera"]
    descriptors = {item["file"]: item for item in meta["images"]}
    attributes_info = descriptors.get(name + "_attributes0.bin")
    packed = attributes_info is None
    dimensions = descriptors[name + "_depth_f32.bin"] if packed else attributes_info
    height, width = dimensions["height"], dimensions["width"]
    if height <= 0 or width <= 0:
        raise ValueError("Image dimensions must be positive")

    # File decoding is the boundary: establish format, row layout and alignment
    # here, then use the decoded arrays directly in the numerical path below.
    def read_image(info, fmt, dtype, channels):
        if (info["format"] != fmt or (info["height"], info["width"]) != (height, width)
                or info["row_bytes"] != width * np.dtype(dtype).itemsize * channels):
            raise ValueError("Unsupported or unaligned image: " + info["file"])
        return read_data(info["file"], dtype).reshape(height, width, channels)[::stride, ::stride]

    if packed:
        if depth_source != "geometry":
            raise ValueError("Compact RGB-D does not contain shader-adjusted attributes depth")
        info = meta["geometry_gpu"]["packed_depth_texture"]
        if info["encoding"] != "viewport_depth_nan_invalid":
            raise ValueError("Unsupported packed depth encoding")
        depth = read_image(info, "R32_FLOAT", "<f4", 1)[..., 0].astype(np.float64)
        valid = np.isfinite(depth)
    else:
        attributes = read_image(attributes_info, "R16G16B16A16_FLOAT", "<f2", 4)
        flags = read_image(descriptors[name + "_attributes3.bin"], "R16G16B16A16_UINT", "<u2", 4)
        bits = (((flags[..., 3] >> 13) & 7) | ((flags[..., 2] & 3) << 3)).astype(np.uint8)
        z = attributes[..., 3].astype(np.float64)
        valid = np.isfinite(z) & (z != 0) & ((bits & 16) == 0)

    rotation = np.asarray(camera["camera_rotation_row_major"], dtype=np.float64).reshape(4, 4)[:3, :3]
    origin = np.asarray(camera["camera_world_xyz"], dtype=np.float64).reshape(3)
    projection = np.asarray(camera["projection_row_major"], dtype=np.float64).reshape(4, 4)
    ray = np.asarray(camera["ray"], dtype=np.float64).reshape(4)
    if not all(np.isfinite(a).all() for a in (rotation, origin, projection, ray)):
        raise ValueError("Nonfinite captured camera parameters")
    if camera["projection_modifier_flag"]:
        raise ValueError("This capture uses a projection modifier that is not decoded")
    viewports = meta["geometry_gpu"]["viewports"]
    if len(viewports) != 1:
        raise ValueError("Reconstruction requires one geometry viewport")
    vp = viewports[0]
    viewport = np.array([vp[k] for k in ("x", "y", "width", "height", "min_depth", "max_depth")])
    if (not np.isfinite(viewport).all() or vp["width"] <= 0 or vp["height"] <= 0
            or vp["max_depth"] <= vp["min_depth"]):
        raise ValueError("Invalid captured geometry viewport")
    y, x = np.mgrid[0:height:stride, 0:width:stride]
    u = (x + 0.5 - vp["x"]) / vp["width"]
    v = (y + 0.5 - vp["y"]) / vp["height"]
    valid &= (u >= 0) & (u < 1) & (v >= 0) & (v < 1)

    if depth_source == "geometry" and not packed:
        info = meta["geometry_gpu"]["depth_texture"]
        fmt = info["format"]
        if fmt == "D32_FLOAT_S8X24_UINT":
            raw = read_image(info, fmt, "<u4", 2)
            depth = raw[..., 0].view("<f4").astype(np.float64)
        elif fmt == "D32_FLOAT":
            depth = read_image(info, fmt, "<f4", 1)[..., 0].astype(np.float64)
        elif fmt == "D24_UNORM_S8_UINT":
            raw = read_image(info, fmt, "<u4", 1)[..., 0]
            depth = (raw & 0xFFFFFF).astype(np.float64) / 0xFFFFFF
        else:
            raise ValueError("Unsupported depth format: " + fmt)
    if depth_source == "geometry":
        ndc_z = (depth - vp["min_depth"]) / (vp["max_depth"] - vp["min_depth"])
        valid &= np.isfinite(ndc_z) & (ndc_z >= 0) & (ndc_z <= 1)
        # Prism's captured projection is GL-style. The observed DX11 correction
        # reverses Z and maps [-1, 1] to [1, 0], before the viewport depth range.
        correction = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, -.5, .5], [0, 0, 0, 1]])
        clip = np.stack((2 * u - 1, 1 - 2 * v, ndc_z, np.ones_like(u)), axis=-1)
        homogeneous = clip @ np.linalg.inv(correction @ projection).T
        valid &= np.isfinite(homogeneous).all(axis=-1) & (homogeneous[..., 3] != 0)
        xyz = np.full((*u.shape, 3), np.nan)
        np.divide(homogeneous[..., :3], homogeneous[..., 3:4], out=xyz, where=valid[..., None])
    else:
        # This is the game's deferred shading ray, including material-specific
        # Z adjustments already present in attributes0.w (e.g. leaf billboards).
        xyz = np.stack((ray[0] + u * ray[2], ray[1] + v * ray[3], np.ones_like(u)), axis=-1) * z[..., None]
    valid &= np.isfinite(xyz).all(axis=-1) & (xyz[..., 2] < 0)
    xyz[~valid] = np.nan
    world = xyz @ np.linalg.inv(rotation).T + origin
    arrays = {"xyz_camera": xyz.astype(np.float32), "xyz_world": world, "valid": valid}
    if not packed:
        arrays["material_bits"] = bits
    color_encoding = None
    if color:
        arrays["rgb_linear"], color_encoding = read_color(meta, read_data, stride, expected_size=(height, width))
    description = {
        "camera": name, "render_frame_id": meta["render_frame_id"],
        "observation_session_qpc": meta["observation_session_qpc"],
        "depth_source": depth_source, "camera_source": "geometry_pass.camera_at_compile",
        "camera_scope": camera["scope"], "world_units": camera["world_units"],
        "camera_convention": "negative Z forward; pixel rows preserved",
        "invalid_points": "NaN; zero/nonfinite attributes Z, bit 16, invalid depth or outside viewport",
        "valid_points": int(valid.sum()), "width": width, "height": height, "pixel_stride": stride,
        "camera_world_xyz": origin.tolist(),
        "color_encoding": color_encoding,
    }
    return arrays, description


def reconstruct_bundle(bundle, *, stride=1):
    """Merge observed RGB-D samples in game world coordinates, preserving sources."""
    views = bundle["manifest"]["views"]
    frame_keys = {(v["metadata"]["observation_session_qpc"], v["metadata"]["render_frame_id"]) for v in views}
    if len(frame_keys) != 1:
        raise ValueError("Point fusion requires views from one observation session and Present interval")
    files = {(item["camera"], item["file"]): item["data"] for item in bundle["files"]}
    xyz, rgb, sources, origins = [], [], [], []
    descriptions = []
    for index, view in enumerate(views):
        name = view["camera"]
        arrays, description = reconstruct_view(view["metadata"],
            lambda filename, dtype: np.frombuffer(files[name, filename], dtype=dtype), stride=stride, color=True)
        valid = arrays["valid"]
        xyz.append(arrays["xyz_world"][valid])
        rgb.append(arrays["rgb_linear"][valid])
        sources.append(np.full(np.count_nonzero(valid), index, dtype=np.uint8))
        origins.append(description["camera_world_xyz"])
        descriptions.append(description)
    session, frame = next(iter(frame_keys))
    return {"xyz_world": np.concatenate(xyz), "rgb_linear": np.concatenate(rgb),
            "camera_index": np.concatenate(sources), "camera_origins_world": np.asarray(origins),
            "metadata": {"render_frame_id": frame, "observation_session_qpc": session,
                         "depth_source": "geometry", "world_units": descriptions[0]["world_units"],
                         "color_encoding": descriptions[0]["color_encoding"],
                         "cameras": [v["camera"] for v in views], "views": descriptions,
                         "missing_views": bundle["manifest"].get("missing_views", [])}}


def save_reconstruction(directory, output, depth_source="geometry"):
    arrays, description = reconstruct_capture(directory, depth_source)
    # Exclusive creation protects an existing recording; the CLI never overwrites it.
    with open(output, "xb") as stream:
        np.savez(stream, **arrays, metadata_json=json.dumps(description, ensure_ascii=False))
    return description
