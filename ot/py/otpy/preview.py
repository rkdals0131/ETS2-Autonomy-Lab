"""Live camera mosaic using OT_Bundles; closing it returns the game to Tier 0."""
from dataclasses import dataclass
import json
import math
import queue
import threading
import time
import uuid

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageTk, ImageOps
import tkinter as tk
from tkinter import filedialog

from .client import Client
from .bundles import BundleReader
from .rig_editor import RigEditor
from .color import read_color
from .rig_layout import resolve_layout


def _latest(output, message):
    # A stalled/minimized UI must not accumulate decoded frames indefinitely.
    while True:
        try:
            output.put_nowait(message)
            return
        except queue.Full:
            try:
                output.get_nowait()
            except queue.Empty:
                pass


class Mosaic:
    """Decode the capture's RGB and display it with its actual pinhole aspect."""
    def __init__(self, config):
        self.names = {f"mirror{v['slot']}": v.get("name", f"Camera {v['slot']}")
                      for v in config["views"]}
        self.white = None
        self.font = ImageFont.truetype("segoeui.ttf", 17)

    def draw(self, bundle, exposure_ev=0.0):
        files = {(f["camera"], f["file"]): f["data"] for f in bundle["files"]}
        decoded = []
        for view in bundle["manifest"]["views"]:
            name, meta = view["camera"], view["metadata"]
            rgb, encoding = read_color(meta, lambda filename, dtype: np.frombuffer(files[name, filename], dtype=dtype))
            height, width = rgb.shape[:2]
            projection = meta["geometry_pass"]["camera_at_compile"]["projection_row_major"]
            aspect = abs(projection[5] / projection[0])
            decoded.append((name, rgb, width, height, aspect, encoding))
        # One common exposure across cameras; sparse samples keep preview work
        # independent of the full RGB/depth recording cost.
        samples = [rgb[::16, ::16].ravel().astype(np.float32) for _, rgb, *_, encoding in decoded if encoding == "linear_hdr"]
        if samples:
            target = max(float(np.nanpercentile(np.concatenate(samples), 99)), 1e-7)
            self.white = target if self.white is None else .9*self.white + .1*target
        columns = 2 if len(decoded) <= 4 else 3
        rows = (len(decoded)+columns-1)//columns
        canvas = Image.new("RGB", (columns*484+4, rows*321+8), "#111820")
        drawing = ImageDraw.Draw(canvas)
        for index, (name, rgb, width, height, aspect, encoding) in enumerate(decoded):
            x, y = 8 + (index % columns)*484, 8 + (index // columns)*321
            title = f"{self.names.get(name, name)}  |  {name}  |  {width} x {height}"
            drawing.text((x+4, y+4), title, font=self.font, fill="#e3eef8")
            # Resize in float32 before tone mapping; leave raw sensor arrays
            # untouched. FOV aspect also handles non-square native pixels.
            out_width = max(1, min(476, round(280*aspect)))
            out_height = max(1, min(280, round(out_width/aspect)))
            small = np.stack([np.asarray(Image.fromarray(rgb[..., channel].astype(np.float32)).resize(
                (out_width, out_height), Image.Resampling.BILINEAR)) for channel in range(3)], axis=-1)
            small = np.maximum(np.nan_to_num(small), 0)
            if encoding == "linear_hdr":
                small *= 2.0**exposure_ev / self.white
                small /= 1.0 + small
            else:
                # Exposure adjustment within the existing Reinhard display
                # curve; highlights quantized to white remain white.
                gain = 2.0**exposure_ev
                small = gain*small/(1+(gain-1)*small)
            small = np.where(small <= .0031308, 12.92*small, 1.055*small**(1/2.4)-.055)
            display = Image.fromarray(np.uint8(np.clip(small, 0, 1)*255))
            canvas.paste(display, (x+(476-out_width)//2, y+34+(280-out_height)//2))
        return canvas


@dataclass
class PreviewState:
    exposure_ev: float = 0.0
    captures: int = 0
    misses: int = 0
    error: str = ""
    cleanup_error: str = ""
    applied_config: dict | None = None
    config_result: tuple | None = None
    config_updates: int = 0


def _capture(config, hz, stop, output, state, updates, capture_format, color_gain, duration):
    client, reader = Client(timeout=3), None
    owner = uuid.uuid4().hex
    claimed = False
    def request(command, **arguments):
        return client.request(command, owner=owner, **arguments)
    try:
        request("lease", action="claim")
        claimed = True
        request("tier", value=1)
        request("render_probe", enabled=True)
        request("camera_rig", **config)
        state.applied_config = config
        state.config_result = (config, "")
        mosaic = Mosaic(config)
        deadline = time.monotonic()+duration
        calibrating = capture_format == "rgbd8" and color_gain is None
        def start_stream():
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                return None
            return request("stream", action="start", hz=hz, duration=remaining,
                           format="raw" if calibrating else capture_format, color_gain=color_gain or 1.0)["stream_id"]
        stream_id = start_stream()
        heartbeat = last_status = time.monotonic()
        while not stop.is_set() and time.monotonic() < deadline:
            now = time.monotonic()
            if now-heartbeat >= 1:
                request("lease", action="heartbeat")
                heartbeat = now
            try:
                settings = updates.get_nowait()
            except queue.Empty:
                pass
            else:
                request("stream", action="stop")
                try:
                    request("camera_rig", **settings)
                except RuntimeError as error:
                    state.config_result = (settings, str(error))
                else:
                    state.applied_config = settings
                    state.config_result = (settings, "")
                    state.config_updates += 1
                stream_id = start_stream()
            if reader is None:
                if not request("bundles")["enabled"]:
                    stop.wait(.01)
                    continue
                reader = BundleReader()
            bundle = None
            while True:
                candidate = reader.read_next()
                if candidate is None:
                    break
                if candidate["manifest"]["stream_id"] == stream_id:
                    bundle = candidate
            if bundle is not None:
                picture = mosaic.draw(bundle, state.exposure_ev)
                state.captures += 1
                _latest(output, ("frame", picture, bundle["manifest"]["render_frame_id"], now))
                if calibrating:
                    color_gain = 1.0/mosaic.white
                    calibrating = False
                    request("stream", action="stop")
                    stream_id = start_stream()
            elif now-last_status >= 1:
                status = request("stream")
                if not status["running"]:
                    raise RuntimeError("Capture stream ended: "+status.get("last_error", status.get("reason", "stopped")))
                last_status = now
            stop.wait(.01)
    except Exception as error:
        state.error = str(error)
        _latest(output, ("status", state.error))
    finally:
        if reader is not None:
            reader.close()
        if claimed:
            try:
                request("lease", action="release")
            except Exception as error:
                state.cleanup_error = str(error)
        _latest(output, ("finished",))


def run_preview(config_file, hz=5.0, duration=None, snapshot=None, capture_format="raw", color_gain=None):
    if duration is None or not math.isfinite(duration) or duration <= 0:
        raise ValueError("Rig editing requires a finite positive duration")
    with open(config_file, encoding="utf-8") as stream:
        config = json.load(stream)
    config = resolve_layout(config, Client())
    root = tk.Tk()
    root.title("ETS2 camera rig — live RGB")
    root.configure(bg="#111820")
    root.geometry(f"{min(1500, root.winfo_screenwidth()-80)}x{min(940, root.winfo_screenheight()-80)}")
    state, stop, messages = PreviewState(), threading.Event(), queue.Queue(maxsize=2)
    label = tk.Label(root, bg="#111820", text="Connecting to ETS2…", fg="white", width=120, height=28)
    label.pack(fill="both", expand=True)
    updates = queue.Queue(maxsize=1)
    editor = RigEditor(root, config, lambda settings: _latest(updates, settings), lambda: state.applied_config)
    editor.pack(fill="x")
    footer = tk.Frame(root, bg="#111820")
    footer.pack(fill="x", padx=12, pady=8)
    status = tk.StringVar(value="Starting the camera rig")
    tk.Label(footer, textvariable=status, bg="#111820", fg="#d8e4ef").pack(anchor="w")
    last_picture, last_frame, last_time = None, None, None
    image_reference = None
    closing, finished, capture_status = False, False, ""
    start = time.monotonic()

    def close():
        nonlocal closing
        closing = True
        status.set("Stopping cameras and restoring normal mirrors…")
        stop.set()

    def save():
        if last_picture is None:
            return
        filename = filedialog.asksaveasfilename(parent=root, defaultextension=".png",
                                              initialfile=f"ets2-rig-{last_frame}.png",
                                              filetypes=[("PNG image", "*.png")])
        if filename:
            last_picture.save(filename)

    tk.Button(footer, text="Close / restore mirrors", command=close).pack(side="right", padx=5)
    tk.Button(footer, text="Save PNG", command=save).pack(side="right", padx=5)
    exposure = tk.Scale(footer, from_=-4, to=4, resolution=.25, orient="horizontal", label="Exposure EV",
                        command=lambda value: setattr(state, "exposure_ev", float(value)),
                        bg="#111820", fg="white", highlightthickness=0, length=180)
    exposure.pack(side="right", padx=15)
    root.protocol("WM_DELETE_WINDOW", close)
    root.bind("<F11>", lambda event: close())
    worker = threading.Thread(target=_capture, args=(config, hz, stop, messages, state, updates, capture_format, color_gain, duration), name="ot-preview-capture")

    def tick():
        nonlocal finished, last_picture, last_frame, last_time, image_reference, capture_status
        while True:
            try:
                message = messages.get_nowait()
            except queue.Empty:
                break
            if message[0] == "frame":
                capture_status = ""
                last_picture, last_frame, last_time = message[1:]
                display = ImageOps.contain(last_picture, (max(1, label.winfo_width()), max(1, label.winfo_height())))
                image_reference = ImageTk.PhotoImage(display)
                label.configure(image=image_reference, text="", width=0, height=0)
            elif message[0] == "status":
                capture_status = message[1]
            elif message[0] == "finished":
                finished = True
        editor.sync(state.config_result)
        if not closing and (state.cleanup_error or capture_status):
            status.set(state.cleanup_error or capture_status)
        elif last_time is not None and not closing and not state.error:
            elapsed = max(time.monotonic()-start, .001)
            status.set(f"Frame {last_frame}  |  {state.captures/elapsed:.1f} captures/s  |  "
                       f"updated {time.monotonic()-last_time:.1f}s ago  |  missing bundles {state.misses}")
        if duration is not None and time.monotonic()-start >= duration:
            close()
        if closing and finished:
            root.destroy()
            return
        root.after(30, tick)

    try:
        worker.start()
        root.after(30, tick)
        root.mainloop()
    finally:
        stop.set()
        if worker.ident is not None:
            worker.join()
        try:
            root.destroy()
        except tk.TclError:
            pass
    if snapshot and last_picture is not None:
        with open(snapshot, "xb") as destination:
            last_picture.save(destination, format="PNG")
    result = {"captures": state.captures, "missing_bundles": state.misses,
              "config_updates": state.config_updates,
              "elapsed_s": time.monotonic()-start, "last_frame": last_frame,
              "error": state.error, "cleanup_error": state.cleanup_error}
    if state.error or state.cleanup_error:
        raise RuntimeError(json.dumps(result))
    return result
