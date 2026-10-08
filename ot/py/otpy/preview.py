"""Live camera mosaic using OT_Bundles; closing it returns the game to Tier 0."""
from dataclasses import dataclass
import json
import queue
import threading
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageTk, ImageOps
import tkinter as tk
from tkinter import filedialog

from .client import Client
from .bundles import BundleReader
from .rig_editor import RigEditor


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
            desc = next(d for d in meta["images"] if d["file"] == name + "_color.bin")
            if desc["format"] != "R16G16B16A16_FLOAT":
                raise ValueError("Preview requires RGBA16F RGB readback")
            width, height = desc["width"], desc["height"]
            rgb = np.frombuffer(files[name, desc["file"]], dtype="<f2").reshape(height, width, 4)[..., :3]
            projection = meta["geometry_pass"]["camera_at_compile"]["projection_row_major"]
            aspect = abs(projection[5] / projection[0])
            decoded.append((name, rgb, width, height, aspect))
        # One common exposure across cameras; sparse samples keep preview work
        # independent of the full RGB/depth recording cost.
        samples = np.concatenate([rgb[::16, ::16].ravel() for _, rgb, *_ in decoded])
        target = max(float(np.nanpercentile(samples, 99)), 1e-7)
        self.white = target if self.white is None else .9*self.white + .1*target
        canvas = Image.new("RGB", (1456, 650), "#111820")
        drawing = ImageDraw.Draw(canvas)
        for index, (name, rgb, width, height, aspect) in enumerate(decoded):
            x, y = 8 + (index % 3)*484, 8 + (index // 3)*321
            title = f"{self.names.get(name, name)}  |  {name}  |  {width} x {height}"
            drawing.text((x+4, y+4), title, font=self.font, fill="#e3eef8")
            # Resize in float32 before tone mapping; leave raw sensor arrays
            # untouched. FOV aspect also handles non-square native pixels.
            out_width = max(1, min(476, round(280*aspect)))
            out_height = max(1, min(280, round(out_width/aspect)))
            small = np.stack([np.asarray(Image.fromarray(rgb[..., channel].astype(np.float32)).resize(
                (out_width, out_height), Image.Resampling.BILINEAR)) for channel in range(3)], axis=-1)
            small = np.maximum(np.nan_to_num(small), 0) * (2.0**exposure_ev / self.white)
            small /= 1.0 + small
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


def _capture(config, hz, stop, output, state, updates):
    client, reader = Client(timeout=3), None
    try:
        client.tier(1)
        client.request("render_probe", enabled=True)
        client.request("camera_rig", **config)
        state.applied_config = config
        state.config_result = (config, "")
        mosaic = Mosaic(config)
        due = time.monotonic()
        while not stop.is_set():
            if stop.wait(max(0, due-time.monotonic())):
                break
            try:
                settings = updates.get_nowait()
            except queue.Empty:
                pass
            else:
                try:
                    client.request("camera_rig", **settings)
                except RuntimeError as error:
                    # A rejected edit leaves the previous native configuration
                    # intact. Keep displaying it so the user can fix the input.
                    state.config_result = (settings, str(error))
                else:
                    state.applied_config = settings
                    state.config_result = (settings, "")
                    state.config_updates += 1
            client.request("capture_mirrors", action="arm", metadata=False)
            deadline = time.monotonic()+3
            while not stop.is_set():
                capture = client.request("capture_mirrors", metadata=False)
                if all(v["phase"] in ("ready", "error") for v in capture["views"]):
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Camera capture did not finish; check the game is rendering")
                stop.wait(.005)
            if stop.is_set():
                break
            if capture["phase"] != "ready":
                state.misses += 1
                missing = ", ".join(v["camera"] for v in capture["views"] if v["phase"] != "ready")
                _latest(output, ("status", "Waiting for cameras: " + missing))
                due = time.monotonic()+.5
                continue
            publication = client.request("capture_mirrors", action="publish")
            if reader is None:
                reader = BundleReader()
            bundle = None
            while True:
                candidate = reader.read_next()
                if candidate is None:
                    break
                bundle = candidate
            if not publication["published"] or bundle is None:
                raise RuntimeError("Camera bundle queue did not deliver the requested frame")
            picture = mosaic.draw(bundle, state.exposure_ev)
            state.captures += 1
            # Only decoded pictures cross to Tk; raw bundles are released here.
            _latest(output, ("frame", picture, bundle["manifest"]["render_frame_id"], time.monotonic()))
            due = max(due+1/hz, time.monotonic())
    except Exception as error:
        state.error = str(error)
        _latest(output, ("status", state.error))
    finally:
        if reader is not None:
            reader.close()
        try:
            client.panic()
        except Exception as error:
            state.cleanup_error = str(error)
        _latest(output, ("finished",))


def run_preview(config_file, hz=5.0, duration=None, snapshot=None):
    with open(config_file, encoding="utf-8") as stream:
        config = json.load(stream)
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
                                              initialfile=f"ets2-six-{last_frame}.png",
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
    worker = threading.Thread(target=_capture, args=(config, hz, stop, messages, state, updates), name="ot-preview-capture")

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
