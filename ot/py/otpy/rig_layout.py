"""Resolve rear-axle-relative mounts into the DLL's SDK/chassis vehicle frame."""
from copy import deepcopy
import math


def resolve_layout(config, client):
    if not any("position_base_link" in view for view in config["views"]):
        return config
    truck = client.request("truck_config")
    attributes = {(a["name"], a["index"]): a["value"] for a in truck["attributes"]}
    indices = config.get("base_link_wheels")
    if indices is None:
        indices = [index for (name, index), value in attributes.items()
                   if name == "wheel.powered" and value
                   and attributes.get(("wheel.simulated", index), True)]
    if not indices:
        raise ValueError("No powered wheels for base_link; specify base_link_wheels")
    wheels = []
    for index in indices:
        position = attributes["wheel.position", index]
        radius = attributes["wheel.radius", index]
        if len(position) != 3 or not all(math.isfinite(v) for v in (*position, radius)) or radius <= 0:
            raise ValueError("Invalid SDK wheel geometry for base_link")
        wheels.append((position, radius))
    # One axle must define the mount origin. Multiple driven axles require an
    # explicit wheel selection instead of silently choosing a different origin.
    if max(p[2] for p, _ in wheels)-min(p[2] for p, _ in wheels) > .001:
        raise ValueError("Selected wheels span multiple axles; specify base_link_wheels for the reference axle")
    origin = [sum(p[0] for p, _ in wheels)/len(wheels),
              sum(p[1]-r for p, r in wheels)/len(wheels),
              sum(p[2] for p, _ in wheels)/len(wheels)]
    result = deepcopy(config)
    for view in result["views"]:
        if "position_base_link" not in view:
            continue
        x, y, z = view.pop("position_base_link")
        # REP-103 (+forward,+left,+up) -> SDK (+right,+up,+back).
        view["position"] = [origin[0]-y, origin[1]+z, origin[2]-x]
        view["basis"] = "chassis"
    result["mount_calibration"] = {
        "sdk_truck_id": attributes.get(("id", None)), "reference_wheel_indices": indices,
        "base_link_origin_in_chassis": origin,
        "method": "SDK nominal wheel centers minus radii; ground/suspension not measured",
        "base_link_axes": "x forward, y left, z up",
    }
    return result
