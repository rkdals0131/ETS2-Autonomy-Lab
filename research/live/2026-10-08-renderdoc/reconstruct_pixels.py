"""Reconstruct frame3160 camera-space points from its captured fog constants.

The captured shader_2_vs and shader_4_ps define the screen UV and ray equations.
Z=0 and packed bit 16 are excluded as in the game's SSAO zfetch path. The latter
needs interleaved-layer handling; none of the four exported views contains it.
Coordinates retain the game's camera convention and unverified length units.
"""
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent
PIXELS = ROOT / "pixels"
CONSTANTS = ROOT / "constants"
OUTPUT = ROOT / "reconstruction"
OUTPUT.mkdir(exist_ok=True)
draw_state = json.loads((CONSTANTS / "draw-state.json").read_text(encoding="utf-8"))
images = json.loads((PIXELS / "images.json").read_text(encoding="utf-8"))
image_by_file = {item["file"]: item for item in images["images"]}
draws = {(item["view_candidate"], item["selection"]): item for item in draw_state["events"]}
views = [name for name, phase in draws if phase == "gbuffer_end"]
summary = {"frame_number": draw_state["frame_number"],
           "coordinate_space": "per-view game camera; negative Z forward; length unit unverified",
           "invalid_xyz": "NaN for zero/nonfinite Z or packed material bit 16",
           "views": {}}
figure = plt.figure(figsize=(4.5 * len(views), 5), layout="constrained")

for column, name in enumerate(views):
    geometry = draws[name, "gbuffer_end"]
    fog = draws[name, "fullscreen_second"]
    attributes_info = image_by_file[name + "_attributes0.bin"]
    flags_info = image_by_file[name + "_attributes3.bin"]
    inputs = fog["stages"]["ps"]["read_only_resources"]
    if attributes_info["resource"] not in inputs or flags_info["resource"] not in inputs:
        raise ValueError("The selected fog draw does not consume the exported G-buffer: " + name)
    z = np.load(PIXELS / (name + "_camera_z.npy"))
    bits = np.load(PIXELS / (name + "_material_bits.npy"))
    rgb = np.asarray(Image.open(PIXELS / (name + "_color_preview.png")).convert("RGB"))
    height, width = z.shape
    if bits.shape != z.shape or rgb.shape != (height, width, 3):
        raise ValueError("Unaligned input images: " + name)
    vs = np.fromfile(CONSTANTS / fog["stages"]["vs"]["constant_blocks"][0]["raw_file"],
                     dtype="<f4").reshape(-1, 4).astype(np.float64)
    ps = np.fromfile(CONSTANTS / fog["stages"]["ps"]["constant_blocks"][0]["raw_file"],
                     dtype="<f4").reshape(-1, 4).astype(np.float64)
    transform = np.fromfile(CONSTANTS / geometry["stages"]["vs"]["constant_blocks"][0]["raw_file"],
                            dtype="<f4").reshape(-1, 4).astype(np.float64)
    y, x = np.mgrid[:height, :width]
    vp = fog["viewport"]
    # Invert shader_2_vs clip position; D3D11 raster coordinates start at top left.
    input_x = (x + 0.5 - vp["x"]) / vp["width"]
    ndc_y = 1 - 2 * (y + 0.5 - vp["y"]) / vp["height"]
    input_y = (1 - ndc_y / vs[1, 0]) / 2
    uv = (np.stack((input_x, input_y), axis=-1) * vs[0, 2:] + vs[0, :2]) * vs[2, :2]
    texels = np.trunc(uv * ps[1, :2] / ps[1, 2:]).astype(np.int64)
    if ((texels[..., 0] < 0) | (texels[..., 0] >= width) |
            (texels[..., 1] < 0) | (texels[..., 1] >= height)).any():
        raise ValueError("Captured fog coordinates exceed the exported image: " + name)
    sampled_z = z[texels[..., 1], texels[..., 0]]
    sampled_bits = bits[texels[..., 1], texels[..., 0]]
    valid = np.isfinite(sampled_z) & (sampled_z != 0) & ((sampled_bits & 16) == 0)
    rays = np.ones((height, width, 3), dtype=np.float64)
    rays[..., :2] = uv * ps[0, 2:] * ps[3, 2:] + ps[3, :2]
    xyz = rays * sampled_z[..., None]
    xyz[~valid] = np.nan
    np.save(OUTPUT / (name + "_xyz_camera.npy"), xyz.astype(np.float32), allow_pickle=False)
    np.save(OUTPUT / (name + "_valid.npy"), valid, allow_pickle=False)

    # Compare independently captured geometry projection with the fog ray path.
    # This tests coordinate consistency, not metric accuracy or a world pose.
    model_view = np.eye(4)
    model_view[:3] = transform[:3]
    projection = transform[4:8] @ np.linalg.inv(model_view)
    points = xyz[valid]
    clip = points @ projection[:, :3].T + projection[:, 3]
    ndc = clip[:, :2] / clip[:, 3:4]
    gvp = geometry["viewport"]
    screen = np.column_stack((gvp["x"] + (ndc[:, 0] + 1) * gvp["width"] / 2,
                              gvp["y"] + (1 - ndc[:, 1]) * gvp["height"] / 2))
    target_screen = np.column_stack((x[valid] + 0.5, y[valid] + 0.5))
    error = np.linalg.norm(screen - target_screen, axis=1)
    records = np.empty(len(points), dtype=[("x", "<f4"), ("y", "<f4"), ("z", "<f4"),
                                           ("red", "u1"), ("green", "u1"), ("blue", "u1")])
    for index, field in enumerate(("x", "y", "z")):
        records[field] = points[:, index]
    for index, field in enumerate(("red", "green", "blue")):
        records[field] = rgb[valid, index]
    header = ("ply\nformat binary_little_endian 1.0\n"
              "comment game camera coordinates; units unverified; RGB is exposure-adjusted preview\n"
              "element vertex {}\nproperty float x\nproperty float y\nproperty float z\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n").format(len(points))
    with (OUTPUT / (name + ".ply")).open("wb") as output:
        output.write(header.encode("ascii"))
        records.tofile(output)
    summary["views"][name] = {
        "gbuffer_event": geometry["event_id"], "fog_event": fog["event_id"],
        "ray_constants": ps[3].tolist(), "projection_from_geometry": projection.tolist(),
        "gbuffer_viewport": gvp, "valid_points": len(points),
        "excluded_bit16_pixels": int(((sampled_bits & 16) != 0).sum()),
        "ray_projection_error_pixels_max": float(error.max()),
        "ray_projection_error_pixels_median": float(np.median(error)),
    }
    # Limit only this overview to near geometry and a bounded number of points.
    # Full resolution and all accepted distances remain in the NPY and PLY files.
    overview = np.flatnonzero(np.linalg.norm(points, axis=1) <= 15)
    overview = overview[::max(1, int(np.ceil(len(overview) / 16000)))]
    shown = points[overview]
    ax = figure.add_subplot(1, len(views), column + 1, projection="3d")
    ax.scatter(shown[:, 0], -shown[:, 2], shown[:, 1], c=rgb[valid][overview] / 255,
               s=0.6, depthshade=False, rasterized=True)
    ax.set(xlabel="camera X", ylabel="forward (-Z)", zlabel="camera Y", title=name)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=15, azim=-70)
    print(name, "points", len(points), "max projection discrepancy (pixels)", error.max())

figure.suptitle("Frame 3160: separate camera-space point clouds\n"
               "Overview: range <= 15 game units, sampled points; full arrays and PLY retain all valid points")
figure.savefig(OUTPUT / "pointclouds.png", dpi=150)
plt.close(figure)
(OUTPUT / "reconstruction.json").write_text(json.dumps(summary, indent=2, allow_nan=False), encoding="utf-8")
print("Saved", OUTPUT)
