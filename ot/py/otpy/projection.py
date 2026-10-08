"""Project externally observed actor boxes onto saved mirror captures.

This measures geometric overlap and occlusion, not object identity, detection
accuracy, or synchronization between simulation and rendering.
"""
import itertools
import json
from pathlib import Path

import numpy as np

from .reconstruction import reconstruct_capture


def compare_boxes(directory, objects):
    """Compare {address, placement, aabb_raw} records in game world units.

    placement uses world_xyz and quaternion_wxyz, as in the external memory
    reader. The local AABB follows min_xyz, max_xyz. Output edges preserve the
    original image rows; pixel coordinates refer to image boundaries.
    """
    arrays, description = reconstruct_capture(directory)
    meta = json.loads((Path(directory) / "images.json").read_text(encoding="utf-8"))
    camera = meta["geometry_pass"]["camera_at_compile"]
    projection = np.asarray(camera["projection_row_major"]).reshape(4, 4)
    rotation = np.asarray(camera["camera_rotation_row_major"]).reshape(4, 4)[:3, :3]
    origin = np.asarray(camera["camera_world_xyz"])
    vp = meta["geometry_gpu"]["viewports"][0]
    height, width = arrays["valid"].shape
    # A single eye origin is the mathematical precondition of these rays.
    if np.any(projection[3, [0, 1, 3]] != 0) or projection[3, 2] == 0:
        raise ValueError("Box comparison requires a perspective pass camera")
    y, x = np.mgrid[:height, :width]
    u = (x + .5 - vp["x"]) / vp["width"]
    v = (y + .5 - vp["y"]) / vp["height"]
    clip = np.stack((2*u-1, 1-2*v, np.zeros_like(u), np.ones_like(u)), axis=-1)
    inverse = np.linalg.inv(projection)
    homogeneous = clip @ inverse.T
    rays = homogeneous[..., :3] / -homogeneous[..., 2:3]
    rays_world = rays @ np.linalg.inv(rotation).T
    # Clip the ray/box interval to the actual camera frustum as well.
    limits = []
    for plane in (-1, 1):
        clip[..., 2] = plane
        point = clip @ inverse.T
        limits.append(-point[..., 2] / point[..., 3])
    frustum_near, frustum_far = np.minimum(*limits), np.maximum(*limits)
    in_viewport = (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
    depth = -arrays["xyz_camera"][..., 2].astype(np.float64)
    selectors = np.array(list(itertools.product((0, 1), repeat=3)))
    edges = [(i, i ^ bit) for i in range(8) for bit in (1, 2, 4) if i < (i ^ bit)]
    rows = []
    for item in objects:
        # Decode each external record once, before its numerical operations.
        position = np.asarray(item["placement"]["world_xyz"], dtype=np.float64).reshape(3)
        quaternion = np.asarray(item["placement"]["quaternion_wxyz"], dtype=np.float64).reshape(4)
        bounds = np.asarray(item["aabb_raw"], dtype=np.float64).reshape(2, 3)
        norm = np.linalg.norm(quaternion)
        if (not all(np.isfinite(a).all() for a in (position, quaternion, bounds))
                or not np.isfinite(norm) or norm == 0 or np.any(bounds[0] > bounds[1])):
            raise ValueError("Invalid actor placement or local AABB: " + str(item.get("address")))
        w, qx, qy, qz = quaternion / norm
        model = np.array([
            [1-2*(qy*qy+qz*qz), 2*(qx*qy-w*qz), 2*(qx*qz+w*qy)],
            [2*(qx*qy+w*qz), 1-2*(qx*qx+qz*qz), 2*(qy*qz-w*qx)],
            [2*(qx*qz-w*qy), 2*(qy*qz+w*qx), 1-2*(qx*qx+qy*qy)]])
        corners = (bounds[0] + selectors * (bounds[1]-bounds[0])) @ model.T + position
        eye_corners = (corners-origin) @ rotation.T
        clip_corners = np.column_stack((eye_corners, np.ones(8))) @ projection.T
        segments = []
        for first, last in edges:
            a, b = clip_corners[first], clip_corners[last]
            lo, hi = 0., 1.
            # Homogeneous clipping avoids projecting behind-camera endpoints.
            for axis in range(3):
                for sign in (-1, 1):
                    fa, fb = a[3]+sign*a[axis], b[3]+sign*b[axis]
                    if fa < 0 and fb < 0:
                        lo, hi = 1., 0.
                        break
                    if (fa < 0) != (fb < 0):
                        t = fa/(fa-fb)
                        if fa < 0:
                            lo = max(lo, t)
                        else:
                            hi = min(hi, t)
                if lo > hi:
                    break
            if lo <= hi:
                points = np.stack((a+lo*(b-a), a+hi*(b-a)))
                ndc = points[:, :2] / points[:, 3:4]
                pixels = np.column_stack((vp["x"]+(ndc[:, 0]+1)*vp["width"]/2,
                                          vp["y"]+(1-ndc[:, 1])*vp["height"]/2))
                segments.append(pixels.tolist())

        local_eye = (origin-position) @ model
        local_rays = rays_world @ model
        enter, leave = np.maximum(frustum_near, 0), frustum_far.copy()
        for axis in range(3):
            direction = local_rays[..., axis]
            nonparallel = direction != 0
            a = np.full((height, width), -np.inf)
            b = np.full((height, width), np.inf)
            np.divide(bounds[0, axis]-local_eye[axis], direction, out=a, where=nonparallel)
            np.divide(bounds[1, axis]-local_eye[axis], direction, out=b, where=nonparallel)
            near, far = np.minimum(a, b), np.maximum(a, b)
            if not bounds[0, axis] <= local_eye[axis] <= bounds[1, axis]:
                near[~nonparallel], far[~nonparallel] = np.inf, -np.inf
            enter = np.maximum(enter, near)
            leave = np.minimum(leave, far)
        hit = in_viewport & (leave >= enter) & (leave > 0)
        if not segments and not hit.any():
            continue
        supported = hit & arrays["valid"]
        rows.append({
            "address": item["address"], "segments_px": segments,
            "actor_origin_range_game_units": float(np.linalg.norm(position-origin)),
            "pixels_in_projected_box": int(hit.sum()),
            "pixels_without_valid_depth": int((hit & ~arrays["valid"]).sum()),
            "depth_inside_box": int((supported & (depth >= enter) & (depth <= leave)).sum()),
            "occluded_by_nearer_depth": int((supported & (depth < enter)).sum()),
            "depth_beyond_box": int((supported & (depth > leave)).sum()),
        })
    return {"capture": description, "objects": rows,
            "scope": "Geometric comparison with supplied actor poses; timestamps are not synchronized",
            "depth_comparison": "DSV distance along each pixel ray versus its clipped OBB entry/exit interval",
            "interpretation": "Depth inside an actor box is overlap evidence, not an object-ID label; nearer depth is occlusion"}


def save_box_comparison(directory, objects_file, output):
    source = {"pose_source": "external_actor_records"}
    if objects_file:
        objects = json.loads(Path(objects_file).read_text(encoding="utf-8"))
    else:
        meta = json.loads((Path(directory) / "images.json").read_text(encoding="utf-8"))
        snapshot = meta["geometry_pass"].get("vehicles_at_compile")
        if snapshot is None:
            raise ValueError("Capture has no vehicle metadata; supply --objects or capture with render_probe on --vehicles")
        if "error" in snapshot:
            raise ValueError("Vehicle metadata read failed: " + snapshot["error"])
        # Actor boxes have a known local frame. Do not attach them to the model
        # origin until the actor-to-model transform has been established.
        objects = [{"address": hex(v["actor_address"]), **v["actor_observation"]} for v in snapshot["vehicles"]]
        source = {"pose_source": "simulation_actor_observations_at_compile; model transforms are not substituted",
                  "geometry_scope": snapshot["geometry_scope"], "read_errors": snapshot["errors"],
                  "truncated_for_read_budget": snapshot["truncated_for_read_budget"]}
    result = compare_boxes(directory, objects)
    result["object_source"] = source
    with open(output, "x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
    return {"output": str(output), "objects_intersecting_view": len(result["objects"]),
            "scope": result["scope"]}
