"""Project actor boxes using simulation or captured model poses.

This measures geometric overlap and occlusion, not object identity, detection
accuracy, or synchronization between simulation and rendering.
"""
import itertools
import json
from pathlib import Path

import numpy as np

from .reconstruction import load_view, reconstruct_view


def compare_boxes(directory, objects, *, pose_source="actor", camera=None):
    meta, read_data = load_view(directory, camera)
    return compare_view_boxes(meta, read_data, objects, pose_source=pose_source)


def compare_view_boxes(meta, read_data, objects, *, pose_source="actor"):
    """Compare {address, placement, aabb_raw} records in game world units.

    placement uses world_xyz and quaternion_wxyz, as in the external memory
    reader. The local AABB follows min_xyz, max_xyz. With pose_source="model",
    records also contain the native model_world_xyz, model_rotation_row_major,
    and model_reference_offset_raw fields. Output edges preserve the original
    image rows; pixel coordinates refer to image boundaries.
    """
    if pose_source not in ("actor", "model"):
        raise ValueError("pose_source must be actor or model")
    arrays, description = reconstruct_view(meta, read_data)
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
        bounds = np.asarray(item["aabb_raw"], dtype=np.float64).reshape(2, 3)
        if not np.isfinite(bounds).all() or np.any(bounds[0] > bounds[1]):
            raise ValueError("Invalid local AABB: " + str(item.get("address")))
        if pose_source == "model":
            model_position = np.asarray(item["model_world_xyz"], dtype=np.float64).reshape(3)
            model = np.asarray(item["model_rotation_row_major"], dtype=np.float64).reshape(4, 4)[:3, :3]
            offset = np.asarray(item["model_reference_offset_raw"], dtype=np.float64).reshape(3)
            if not all(np.isfinite(a).all() for a in (model_position, model, offset)):
                raise ValueError("Invalid model transform: " + str(item.get("address")))
            # Model slot +0x90 adds this offset to mesh bounds before the actor
            # stores them. Undo that basis change using the captured transform:
            # world = P_model + R_model @ (point_actor - offset).
            position = model_position - model @ offset
        else:
            position = np.asarray(item["placement"]["world_xyz"], dtype=np.float64).reshape(3)
            quaternion = np.asarray(item["placement"]["quaternion_wxyz"], dtype=np.float64).reshape(4)
            norm = np.linalg.norm(quaternion)
            if (not all(np.isfinite(a).all() for a in (position, quaternion))
                    or not np.isfinite(norm) or norm == 0):
                raise ValueError("Invalid actor placement: " + str(item.get("address")))
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

        inverse_model = np.linalg.inv(model)
        local_eye = (origin-position) @ inverse_model.T
        local_rays = rays_world @ inverse_model.T
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
            "kind": item.get("kind"), "lod_index": item.get("lod_index"),
            "box_world_center_xyz": (position + model @ bounds.mean(axis=0)).tolist(),
            "box_size_xyz": (bounds[1]-bounds[0]).tolist(),
            "box_rotation_row_major": model.ravel().tolist(),
            "actor_origin_range_game_units": float(np.linalg.norm(position-origin)),
            "pixels_in_projected_box": int(hit.sum()),
            "pixels_without_valid_depth": int((hit & ~arrays["valid"]).sum()),
            "depth_inside_box": int((supported & (depth >= enter) & (depth <= leave)).sum()),
            "occluded_by_nearer_depth": int((supported & (depth < enter)).sum()),
            "depth_beyond_box": int((supported & (depth > leave)).sum()),
        })
    scope = ("Actor-local bounds transformed by the model component observed at pass compilation; "
             "final per-draw GPU transforms not verified" if pose_source == "model" else
             "Geometric comparison with supplied actor poses; timestamps are not synchronized")
    return {"capture": description, "objects": rows, "pose_source": pose_source,
            "scope": scope,
            "depth_comparison": "DSV distance along each pixel ray versus its clipped OBB entry/exit interval",
            "interpretation": "Depth inside an actor box is overlap evidence, not an object-ID label; nearer depth is occlusion"}


def captured_objects(meta, pose_source):
    snapshot = meta["geometry_pass"].get("vehicles_at_compile")
    if snapshot is None:
        raise ValueError("Capture has no vehicle metadata; supply --objects or capture with render_probe on --vehicles")
    if "error" in snapshot:
        raise ValueError("Vehicle metadata read failed: " + snapshot["error"])
    objects = [{"address": hex(v["actor_address"]), "kind": v["kind"], "lod_index": v["lod_index"],
                **v["actor_observation"]} for v in snapshot["vehicles"]]
    if pose_source == "model":
        for item, vehicle in zip(objects, snapshot["vehicles"]):
            item.update({key: vehicle[key] for key in
                         ("model_world_xyz", "model_rotation_row_major", "model_reference_offset_raw")})
    source = {"pose_source": "model_component_at_compile" if pose_source == "model" else
                            "simulation_actor_observations_at_compile",
              "geometry_scope": snapshot["geometry_scope"], "read_errors": snapshot["errors"],
              "truncated_for_read_budget": snapshot["truncated_for_read_budget"]}
    return objects, source


def draw_box_overlay(meta, read_data, comparison):
    from PIL import Image, ImageDraw, ImageFont
    from .color import read_color
    rgb, encoding = read_color(meta, read_data)
    rgb = np.maximum(np.nan_to_num(rgb.astype(np.float32)), 0)
    if encoding == "linear_hdr":
        rgb /= max(float(np.percentile(rgb[::16, ::16], 99)), 1e-7)
        rgb /= 1+rgb
    srgb = np.where(rgb <= .0031308, 12.92*rgb, 1.055*rgb**(1/2.4)-.055)
    image = Image.fromarray(np.uint8(np.clip(srgb, 0, 1)*255))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype("segoeui.ttf", 14)
    for row in comparison["objects"]:
        if not row["segments_px"]:
            continue
        color = "#56edbc" if row["depth_inside_box"] else "#ffc65b"
        for segment in row["segments_px"]:
            draw.line([tuple(p) for p in segment], fill=color, width=2)
        points = np.asarray(row["segments_px"]).reshape(-1, 2)
        x, y = points.min(axis=0)
        label = f"{str(row['address'])[-6:]} {row['kind'] or ''} | {row['actor_origin_range_game_units']:.1f} units"
        y = max(24, y-17)
        draw.text((max(0, x), y), label, font=font, fill=color, stroke_width=1, stroke_fill="black")
    draw.rectangle((0, 0, image.width, 23), fill="#101820")
    draw.text((6, 2), f"{meta['camera']} frame {meta['render_frame_id']} | {comparison['pose_source']} pose | green: DSV inside box; amber: no inside depth",
              font=font, fill="white")
    return image


def save_box_comparison(directory, objects_file, output, *, pose_source="actor", camera=None, overlay=None, actor=None):
    meta, read_data = load_view(directory, camera)
    source = {"pose_source": "external_actor_records"}
    if objects_file:
        if pose_source != "actor":
            raise ValueError("--pose model uses captured vehicle metadata and cannot be combined with --objects")
        objects = json.loads(Path(objects_file).read_text(encoding="utf-8"))
    else:
        objects, source = captured_objects(meta, pose_source)
    if actor is not None:
        objects = [item for item in objects if int(str(item["address"]), 0) == actor]
        if not objects:
            raise ValueError("Actor is absent from the captured object records: " + hex(actor))
    result = compare_view_boxes(meta, read_data, objects, pose_source=pose_source)
    result["object_source"] = source
    with open(output, "x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
    if overlay:
        image = draw_box_overlay(meta, read_data, result)
        with open(overlay, "xb") as stream:
            image.save(stream, format="PNG")
    return {"output": str(output), "objects_intersecting_view": len(result["objects"]),
            "scope": result["scope"], "overlay": str(overlay) if overlay else None}
