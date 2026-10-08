"""Convert RenderDoc raw texture exports to NPY and a comparison figure.

Run with the workspace Python 3.13 after export_frame3160.py completes.
Raw values are retained. RGB previews use a common exposure, Reinhard mapping, then sRGB encoding;
Z previews show absolute attributes0.w with a shared logarithmic display scale.
"""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", nargs="?", type=Path, default=Path(__file__).resolve().parent / "pixels",
                    help="Directory containing images.json and raw texture files (RenderDoc or ot_core)")
ROOT = parser.parse_args().directory.resolve()
metadata = json.loads((ROOT / "images.json").read_text(encoding="utf-8"))
views = {}
formats = {"R16G16B16A16_FLOAT": np.dtype("<f2"), "R16G16B16A16_UINT": np.dtype("<u2")}
for image in metadata["images"]:
    name = Path(image["file"]).name
    dtype = formats[image["format"].removeprefix("DXGI_FORMAT_")]
    data = np.fromfile(ROOT / name, dtype=dtype).reshape(image["height"], image["width"], 4)
    np.save(ROOT / (Path(name).stem + ".npy"), data, allow_pickle=False)
    view, channel = Path(name).stem.rsplit("_", 1)
    views.setdefault(view, {})[channel] = data

stats = {}
all_depth = []
for name, channels in views.items():
    color, attributes, flags = channels["color"], channels["attributes0"], channels["attributes3"]
    if color.shape != attributes.shape or flags.shape != attributes.shape:
        raise ValueError(f"Unaligned color/G-buffer dimensions in {name}")
    z = attributes[..., 3].astype(np.float32)
    mask = (((flags[..., 3] >> 13) & 7) | ((flags[..., 2] & 3) << 3)).astype(np.uint8)
    np.save(ROOT / (name + "_camera_z.npy"), z, allow_pickle=False)
    np.save(ROOT / (name + "_material_bits.npy"), mask, allow_pickle=False)
    finite = np.isfinite(z)
    depth = np.abs(z[finite & (z != 0)])
    all_depth.append(depth)
    values, counts = np.unique(mask, return_counts=True)
    stats[name] = {
        "shape": list(z.shape),
        "finite_z_fraction": float(finite.mean()),
        "zero_z_fraction": float((z == 0).mean()),
        "z_range": [float(z[finite].min()), float(z[finite].max())] if finite.any() else None,
        "nonzero_abs_z_percentiles_1_50_99": np.percentile(depth, [1, 50, 99]).tolist() if depth.size else None,
        "material_bits_counts": {str(int(value)): int(count) for value, count in zip(values, counts)},
    }
    channels["z"], channels["bits"] = z, mask

depth_values = np.concatenate(all_depth)
if not depth_values.size:
    raise ValueError("No finite nonzero Z values were exported; inspect the selected G-buffer outputs")
low, high = np.percentile(depth_values, [1, 99])
if high <= low:
    low, high = max(float(low) / 2, np.finfo(np.float32).tiny), float(high) * 2
norm = LogNorm(vmin=low, vmax=high, clip=True)
rgb_values = np.concatenate([channels["color"][..., :3].astype(np.float32).ravel() for channels in views.values()])
rgb_white = np.percentile(rgb_values[np.isfinite(rgb_values) & (rgb_values > 0)], 99)
exposure = 1.0 / float(rgb_white) if rgb_white > 0 else 1.0
figure, axes = plt.subplots(3, len(views) + 1, figsize=(max(8, 3.4 * len(views) + 1), 9),
                           gridspec_kw={"width_ratios": [1] * len(views) + [0.05]},
                           squeeze=False, layout="constrained")
for column, (name, channels) in enumerate(views.items()):
    linear = np.nan_to_num(channels["color"][..., :3].astype(np.float32), nan=0, posinf=0, neginf=0)
    linear = np.maximum(linear, 0) * exposure
    mapped = linear / (1 + linear)
    srgb = np.where(mapped <= 0.0031308, 12.92 * mapped, 1.055 * mapped ** (1 / 2.4) - 0.055)
    plt.imsave(ROOT / (name + "_color_preview.png"), np.clip(srgb, 0, 1))
    axes[0, column].imshow(srgb)
    axes[0, column].set_title(name)
    z = np.ma.masked_where(~np.isfinite(channels["z"]) | (channels["z"] == 0), np.abs(channels["z"]))
    depth_image = axes[1, column].imshow(z, norm=norm, cmap="magma")
    axes[2, column].imshow(channels["bits"], vmin=0, vmax=31, cmap="tab20")
    for row in range(3):
        axes[row, column].set_xticks([])
        axes[row, column].set_yticks([])
axes[0, 0].set_ylabel("HDR color preview")
axes[1, 0].set_ylabel("|attributes0.w|")
axes[2, 0].set_ylabel("Packed material bits (0–31)")
axes[0, -1].set_axis_off()
axes[2, -1].set_axis_off()
figure.colorbar(depth_image, cax=axes[1, -1], label="Absolute camera-space Z; shared log display scale")
capture_label = (f"RenderDoc frame {metadata['frame_number']}" if "frame_number" in metadata
                 else f"ot_core {metadata.get('camera', 'camera')} sample {metadata['capture_sequence']}")
figure.suptitle(f"ETS2 {capture_label}\n"
               f"RGB preview: exposure x{exposure:.1f}, Reinhard + sRGB\nRaw arrays unchanged", fontsize=12)
figure.savefig(ROOT / "comparison.png", dpi=150)
plt.close(figure)
(ROOT / "pixel-statistics.json").write_text(json.dumps(stats, indent=2), encoding="utf-8")
print(json.dumps(stats, indent=2))
print("Saved", ROOT / "comparison.png")
