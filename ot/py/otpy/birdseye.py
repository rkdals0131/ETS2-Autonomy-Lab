"""Top-down display of observed RGB-D points; empty cells remain unobserved."""
import json
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from .bundles import load_bundle
from .reconstruction import reconstruct_bundle


def draw_birdseye(cloud, radius=40.0, pixels=800, exposure_ev=0.0):
    if not math.isfinite(radius) or radius <= 0 or pixels < 1:
        raise ValueError("Bird's-eye radius and pixel count must be positive")
    points = cloud["xyz_world"]
    center = np.mean(cloud["camera_origins_world"], axis=0)
    xz = points[:, [0, 2]] - center[[0, 2]]
    inside = (np.abs(xz) < radius).all(axis=1)
    selected = np.flatnonzero(inside)
    cells = np.floor((xz[inside]+radius) * (pixels/(2*radius))).astype(np.int64)
    flat = cells[:, 1]*pixels+cells[:, 0]
    # From above, the highest observed surface wins. Sort by cell, height and
    # source order so overlap selection is deterministic, with no hole filling.
    order = np.lexsort((selected, points[selected, 1], flat))
    sorted_cells = flat[order]
    last = np.r_[sorted_cells[1:] != sorted_cells[:-1], True] if len(order) else np.empty(0, dtype=bool)
    winners = selected[order[last]]
    pixel_indices = sorted_cells[last]
    observed = np.zeros(pixels*pixels, dtype=bool)
    observed[pixel_indices] = True
    heights = np.full(pixels*pixels, np.nan, dtype=np.float32)
    heights[pixel_indices] = points[winners, 1]-center[1]
    rgb = np.maximum(np.nan_to_num(cloud["rgb_linear"][winners].astype(np.float32)), 0)
    if cloud["metadata"].get("color_encoding", "linear_hdr") == "linear_hdr":
        white = max(float(np.percentile(rgb, 99)), 1e-7) if rgb.size else 1.0
        rgb *= 2**exposure_ev/white
        rgb /= 1+rgb
    else:
        gain = 2**exposure_ev
        rgb = gain*rgb/(1+(gain-1)*rgb)
    rgb = np.where(rgb <= .0031308, 12.92*rgb, 1.055*rgb**(1/2.4)-.055)
    raster = np.full((pixels*pixels, 3), (18, 26, 35), dtype=np.uint8)
    raster[pixel_indices] = np.uint8(np.clip(rgb, 0, 1)*255)
    image = Image.new("RGB", (pixels, pixels+80), "#121a23")
    image.paste(Image.fromarray(raster.reshape(pixels, pixels, 3)), (0, 80))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("segoeui.ttf", 16)
    draw.text((12, 8), f"Observed RGB-D · {len(cloud['metadata']['cameras'])} cameras · frame {cloud['metadata']['render_frame_id']}", font=font, fill="white")
    draw.text((12, 32), f"World +X right / +Z down · radius {radius:g} game units", font=font, fill="#c4d6e4")
    draw.text((12, 55), "Dark background = unobserved · camera locations marked in cyan", font=font, fill="#c4d6e4")
    for name, origin in zip(cloud["metadata"]["cameras"], cloud["camera_origins_world"]):
        pos = (origin[[0, 2]]-center[[0, 2]]+radius)*(pixels/(2*radius))
        x, y = float(pos[0]), float(pos[1])+80
        draw.ellipse((x-3, y-3, x+3, y+3), fill="#5be7ff")
        draw.text((x+5, y), name, font=font, fill="#5be7ff")
    return image, {"observed": observed.reshape(pixels, pixels),
                   "height_above_camera_mean": heights.reshape(pixels, pixels),
                   "center_world_xyz": center, "game_units_per_pixel": 2*radius/pixels}


def save_birdseye(directory, output, *, radius=40.0, stride=1, points=None):
    cloud = reconstruct_bundle(load_bundle(directory), stride=stride)
    image, grid = draw_birdseye(cloud, radius=radius)
    with open(output, "xb") as destination:
        image.save(destination, format="PNG")
    if points:
        with open(points, "xb") as destination:
            np.savez(destination, **{k:v for k,v in cloud.items() if k!="metadata"}, **grid,
                     metadata_json=json.dumps(cloud["metadata"]))
    return {**cloud["metadata"], "points": len(cloud["xyz_world"]),
            "observed_cells": int(grid["observed"].sum()), "total_cells": grid["observed"].size,
            "radius_game_units": radius, "output": str(output)}
