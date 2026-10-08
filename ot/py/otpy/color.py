"""Decode captured RGB with an explicit distinction between HDR and tone-mapped color."""
import numpy as np


def read_color(meta, read_data, stride=1, *, expected_size=None):
    name = meta["camera"]
    images = {item["file"]: item for item in meta["images"]}
    raw = name + "_color.bin" in images
    info = images[name + ("_color.bin" if raw else "_color_ldr.bin")]
    dtype = "<f2" if raw else "u1"
    fmt = "R16G16B16A16_FLOAT" if raw else "R8G8B8A8_UNORM"
    width, height = info["width"], info["height"]
    if (info["format"] != fmt or width <= 0 or height <= 0
            or info["row_bytes"] != width*4*np.dtype(dtype).itemsize):
        raise ValueError("Unsupported color image layout")
    if expected_size is not None and (height, width) != expected_size:
        raise ValueError("Color and depth dimensions do not align")
    rgb = read_data(info["file"], dtype).reshape(height, width, 4)[::stride, ::stride, :3]
    if raw:
        return rgb, "linear_hdr"
    if info["encoding"] != "srgb_reinhard":
        raise ValueError("Unsupported packed color encoding")
    srgb = rgb.astype(np.float32)/255
    # This is linear display color. The original HDR radiance is not recovered.
    return np.where(srgb <= .04045, srgb/12.92, ((srgb+.055)/1.055)**2.4), "linear_reinhard"
