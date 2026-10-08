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
    for name in ("ping", "version", "hooks", "frames", "schema", "snapshot", "state", "panic", "dump", "reload_permissions"):
        sub.add_parser(name)
    sub.add_parser("read").add_argument("field")
    sub.add_parser("tier").add_argument("value", type=int, choices=(0, 1), nargs="?")
    probe = sub.add_parser("render_probe", help="Inspect or switch the Tier 1 render-call observer")
    probe.add_argument("mode", choices=("on", "off", "status"), default="status", nargs="?")
    probe.add_argument("--vehicles", action="store_true", help="With on: read vehicle model metadata at mirror pass compilation")
    sub.add_parser("capture_mirror5", help="One requested mirror5 GPU readback").add_argument(
        "action", choices=("arm", "status", "save", "cancel"), default="status", nargs="?")
    sub.add_parser("capture_mirrors", help="Request mirror 0/1/2/5 in one Present interval").add_argument(
        "action", choices=("arm", "status", "save", "cancel"), default="status", nargs="?")
    record = sub.add_parser("record_mirror5", help="Record bounded mirror5 samples; restores Tier 0 on exit")
    record.add_argument("--hz", type=float, default=10.0)
    record.add_argument("--duration", type=float, required=True)
    record.add_argument("--output", required=True, help="New JSONL index; raw images are saved by the DLL")
    reconstruct = sub.add_parser("reconstruct", help="Offline pass-camera point cloud from a saved DLL capture (NumPy)")
    reconstruct.add_argument("directory", help="Capture directory containing images.json")
    reconstruct.add_argument("--output", required=True, help="New NPZ file; existing files are not overwritten")
    reconstruct.add_argument("--depth-source", choices=("geometry", "attributes"), default="geometry")
    boxes = sub.add_parser("project_boxes", help="Offline actor box projection and DSV occlusion comparison (NumPy)")
    boxes.add_argument("directory", help="Capture directory containing images.json")
    boxes.add_argument("--objects", help="JSON actor records; default: actor observations in the capture's vehicle metadata")
    boxes.add_argument("--pose", choices=("actor", "model"), default="actor",
                       help="actor: simulation pose; model: captured model transform with actor-box origin correction")
    boxes.add_argument("--output", required=True, help="New JSON file containing projected edges and depth counts")
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
        elif args.command == "project_boxes":
            from .projection import save_box_comparison
            result = save_box_comparison(args.directory, args.objects, args.output, pose_source=args.pose)
        elif args.command == "loader":
            client = LoaderClient(timeout=args.timeout)
            result = client.status() if args.action == "status" else client.control(args.action)
        elif args.command == "record_mirror5":
            if not (math.isfinite(args.hz) and args.hz > 0 and math.isfinite(args.duration) and args.duration > 0):
                parser.error("--hz and --duration must be finite and positive")
            record_mirror5(client, args.hz, args.duration, args.output)
            return 0
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
            if args.vehicles:
                if args.mode != "on":
                    parser.error("--vehicles requires render_probe on")
                options["vehicle_metadata"] = True
            result = client.request("render_probe", **options)
        elif args.command in ("capture_mirror5", "capture_mirrors"):
            result = client.request(args.command, action=args.action)
        else:
            result = client.request(args.command)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, ImportError) as error:
        print(f"otpy: {error}", file=sys.stderr)
        return 1


def record_mirror5(client, hz, duration, filename):
    with open(filename, "x", encoding="utf-8") as output:
        def write(value):
            print(json.dumps(value, ensure_ascii=False), file=output, flush=True)

        write({"type": "start", "version": client.request("version"), "requested_hz": hz, "duration_s": duration})
        count = after_id = missed_schedule = 0
        try:
            client.tier(1)
            client.request("render_probe", enabled=True)
            start = due = time.monotonic()
            deadline = start + duration
            while time.monotonic() < deadline:
                now = time.monotonic()
                if due > now:
                    time.sleep(min(due, deadline) - now)
                if time.monotonic() >= deadline:
                    break
                value = client.request("capture_mirror5", action="arm")
                while time.monotonic() < deadline:
                    value = client.request("capture_mirror5")
                    if value["phase"] in ("ready", "error"):
                        break
                    time.sleep(0.005)
                if value["phase"] == "error":
                    raise RuntimeError(value["error"])
                if value["phase"] != "ready":
                    break
                saved = client.request("capture_mirror5", action="save")
                write({"type": "capture", "saved_directory": saved["saved_directory"], "metadata": saved["metadata"]})
                count += 1
                frames = client.request("frames", after_id=after_id)
                if frames["records"]:
                    after_id = frames["records"][-1]["present_id"]
                    write({"type": "frames", **frames})
                due += 1 / hz
                now = time.monotonic()
                if due < now:
                    missed = math.floor((now - due) * hz) + 1
                    missed_schedule += missed
                    due += missed / hz
            elapsed = time.monotonic() - start
            summary = {"type": "summary", "captures": count, "elapsed_s": elapsed,
                       "captures_per_s": count / elapsed, "missed_schedule_slots": missed_schedule}
            write(summary)
            print(json.dumps(summary, ensure_ascii=False, indent=2))
        finally:
            write({"type": "stop", "panic": client.panic()})


if __name__ == "__main__":
    sys.exit(main())
