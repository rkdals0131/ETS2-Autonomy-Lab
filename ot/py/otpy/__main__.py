import argparse
from contextlib import ExitStack
import json
import math
import sys
import time
from . import Client, LoaderClient, StateReader


def main():
    parser = argparse.ArgumentParser(description="ot_core local client (Windows x64)")
    parser.add_argument("--timeout", type=float, default=5.0)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("loader", help="Resident loader control; module changes run at SDK frame end").add_argument(
        "action", choices=("status", "load", "unload", "reload"), default="status", nargs="?")
    for name in ("ping", "version", "hooks", "frames", "schema", "snapshot", "state", "panic", "dump", "reload_permissions", "truck_config"):
        sub.add_parser(name)
    sub.add_parser("read").add_argument("field")
    sub.add_parser("tier").add_argument("value", type=int, choices=(0, 1), nargs="?")
    probe = sub.add_parser("render_probe", help="Inspect or switch the Tier 1 render-call observer")
    probe.add_argument("mode", choices=("on", "off", "status"), default="status", nargs="?")
    probe.add_argument("--vehicles", action="store_true", help="With on: read vehicle model metadata at mirror pass compilation")
    probe.add_argument("--mode", dest="hook_mode", choices=("observe", "rig"), default="observe",
                       help="observe: capture hooks; rig: only the four camera placement hooks")
    probe.add_argument("--frames", action="store_true", help="With rig mode: also observe Present intervals")
    rig = sub.add_parser("camera_rig", help="Place cameras in slots 0-8; +X right, +Y up, -Z forward")
    rig.add_argument("mode", choices=("status", "apply", "off"), default="status", nargs="?")
    rig.add_argument("--config", help="JSON containing views with slot, basis, position, quaternion_wxyz and FOVs")
    rig.add_argument("--rig-only", action="store_true", help="Apply without pass observation or GPU readback hooks")
    rig.add_argument("--frames", action="store_true", help="With --rig-only: also observe Present intervals")
    preview = sub.add_parser("preview", help="Live camera mosaic; closes with Tier 0 restored (NumPy, Pillow, Tk)")
    preview.add_argument("--config", required=True)
    preview.add_argument("--hz", type=float, default=5.0)
    preview.add_argument("--duration", type=float, required=True, help="Automatically close after this many seconds")
    preview.add_argument("--snapshot", help="Save the last displayed mosaic to a new PNG on exit")
    preview.add_argument("--format", choices=("raw", "rgbd8"), default="raw")
    preview.add_argument("--color-gain", type=float, help="Fixed GPU gain; omitted: calibrate once from a raw bundle")
    stream = sub.add_parser("stream", help="Bounded native GPU ring; consumes via OT_Bundles")
    stream.add_argument("action", choices=("start", "status", "stop"), default="status", nargs="?")
    stream.add_argument("--hz", type=float, default=10.0)
    stream.add_argument("--duration", type=float, help="Required for start; native stop deadline in seconds")
    stream.add_argument("--format", choices=("raw", "rgbd8", "raw+rgbd8"), default="rgbd8")
    stream.add_argument("--color-gain", type=float, default=1.0)
    reconstruct = sub.add_parser("reconstruct", help="Offline pass-camera point cloud from a saved DLL capture (NumPy)")
    reconstruct.add_argument("directory", help="Capture directory containing images.json")
    reconstruct.add_argument("--output", required=True, help="New NPZ file; existing files are not overwritten")
    reconstruct.add_argument("--depth-source", choices=("geometry", "attributes"), default="geometry")
    lidar = sub.add_parser("lidar", help="Offline ideal LiDAR beams from co-located bundle depth views")
    lidar.add_argument("directory", help="Bundle directory, .zip or .tar.zst capture")
    lidar.add_argument("--config", required=True, help="JSON beam pattern and camera sources")
    lidar.add_argument("--output", required=True, help="New NPZ with beams, source pixels, misses and world points")
    birdseye = sub.add_parser("birdseye", help="Fuse a saved same-frame RGB-D bundle into a world-axis top-down view")
    birdseye.add_argument("directory", help="Bundle directory, .zip or .tar.zst capture")
    birdseye.add_argument("--output", required=True, help="New PNG")
    birdseye.add_argument("--points", help="Optional new NPZ with world points, colors, sources and observation grid")
    birdseye.add_argument("--radius", type=float, default=40.0, help="Crop radius in game world units")
    birdseye.add_argument("--stride", type=int, default=1, help="Sample every Nth source pixel without resizing depth")
    boxes = sub.add_parser("project_boxes", help="Offline actor box projection and DSV occlusion comparison (NumPy)")
    boxes.add_argument("directory", help="Camera directory, or bundle directory/.zip/.tar.zst with --camera")
    boxes.add_argument("--camera", help="Select one camera from a bundle, e.g. mirror2")
    boxes.add_argument("--actor", type=lambda value: int(value, 0), help="Only this actor address, e.g. 0x1234")
    boxes.add_argument("--objects", help="JSON actor records; default: actor observations in the capture's vehicle metadata")
    boxes.add_argument("--pose", choices=("actor", "model"), default="actor",
                       help="actor: simulation pose; model: captured model transform with actor-box origin correction")
    boxes.add_argument("--output", required=True, help="New JSON file containing projected edges and depth counts")
    boxes.add_argument("--overlay", help="New PNG showing projected boxes on captured RGB")
    watch = sub.add_parser("watch", help="Print newest shared state as JSON lines; Ctrl+C closes the reader")
    watch.add_argument("--hz", type=float, default=10.0)
    watch.add_argument("--duration", type=float, help="Stop after this many seconds")
    watch.add_argument("--output", help="New JSONL file; existing files are not overwritten")
    args = parser.parse_args()
    try:
        client = Client(timeout=args.timeout)
        if args.command == "reconstruct":
            from .reconstruction import save_reconstruction
            result = save_reconstruction(args.directory, args.output, args.depth_source)
        elif args.command == "lidar":
            from .lidar import save_lidar
            result = save_lidar(args.directory, args.config, args.output)
        elif args.command == "birdseye":
            from .birdseye import save_birdseye
            result = save_birdseye(args.directory, args.output, radius=args.radius, stride=args.stride, points=args.points)
        elif args.command == "preview":
            if not math.isfinite(args.hz) or args.hz <= 0:
                parser.error("--hz must be finite and positive")
            if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
                parser.error("--duration must be finite and positive")
            from .preview import run_preview
            if args.color_gain is not None and (not math.isfinite(args.color_gain) or args.color_gain <= 0):
                parser.error("--color-gain must be finite and positive")
            result = run_preview(args.config, args.hz, args.duration, args.snapshot, args.format, args.color_gain)
        elif args.command == "project_boxes":
            from .projection import save_box_comparison
            result = save_box_comparison(args.directory, args.objects, args.output, pose_source=args.pose,
                                         camera=args.camera, overlay=args.overlay, actor=args.actor)
        elif args.command == "loader":
            client = LoaderClient(timeout=args.timeout)
            result = client.status() if args.action == "status" else client.control(args.action)
        elif args.command == "stream":
            if args.action == "start" and args.duration is None:
                parser.error("stream start requires --duration")
            options = dict(hz=args.hz, duration=args.duration, format=args.format, color_gain=args.color_gain) if args.action == "start" else {}
            result = client.request("stream", action=args.action, **options)
        elif args.command == "watch":
            if not 0 < args.hz <= 240:
                parser.error("--hz must be in (0, 240]")
            if args.duration is not None and not args.duration > 0:
                parser.error("--duration must be positive")
            with ExitStack() as resources:
                reader = resources.enter_context(StateReader())
                output = resources.enter_context(open(args.output, "x", encoding="utf-8")) if args.output else sys.stdout
                deadline = time.monotonic() + args.duration if args.duration is not None else None
                while deadline is None or time.monotonic() < deadline:
                    value = reader.read_latest()
                    if value is not None:
                        print(json.dumps(value, ensure_ascii=False), file=output, flush=True)
                    time.sleep(1 / args.hz)
            return 0
        elif args.command == "read":
            result = client.read(args.field)
        elif args.command == "tier":
            result = client.tier(args.value)
        elif args.command == "render_probe":
            options = {} if args.mode == "status" else {"enabled": args.mode == "on"}
            if args.mode == "on":
                options.update(mode=args.hook_mode, frame_timing=args.frames)
            elif args.hook_mode != "observe" or args.frames:
                parser.error("--mode and --frames require render_probe on")
            if args.vehicles:
                if args.mode != "on":
                    parser.error("--vehicles requires render_probe on")
                options["vehicle_metadata"] = True
            result = client.request("render_probe", **options)
        elif args.command == "camera_rig":
            if (args.rig_only or args.frames) and args.mode != "apply":
                parser.error("--rig-only and --frames require camera_rig apply")
            if args.frames and not args.rig_only:
                parser.error("--frames requires --rig-only; capture mode already observes frames")
            if args.mode == "apply":
                if not args.config:
                    parser.error("camera_rig apply requires --config")
                with open(args.config, encoding="utf-8") as source:
                    settings = json.load(source)
                from .rig_layout import resolve_layout
                settings = resolve_layout(settings, client)
                client.tier(1)
                client.request("render_probe", enabled=True, mode="rig" if args.rig_only else "observe", frame_timing=args.frames)
                try:
                    result = client.request("camera_rig", **settings)
                except Exception:
                    client.panic()
                    raise
            else:
                result = client.request("camera_rig", **({"enabled": False} if args.mode == "off" else {}))
        else:
            result = client.request(args.command)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, ImportError) as error:
        print(f"otpy: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
