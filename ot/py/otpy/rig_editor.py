"""Camera placement controls; the saved contract remains quaternion WXYZ."""
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import tempfile
import tkinter as tk
from tkinter import filedialog, ttk


def quaternion_from_angles(yaw, pitch, roll):
    """Camera-to-basis rotation Ry(yaw) Rx(pitch) Rz(roll), degrees."""
    y, p, r = (math.radians(v)/2 for v in (yaw, pitch, roll))
    cy, sy, cp, sp, cr, sr = math.cos(y), math.sin(y), math.cos(p), math.sin(p), math.cos(r), math.sin(r)
    return [cy*cp*cr+sy*sp*sr, cy*sp*cr+sy*cp*sr,
            sy*cp*cr-cy*sp*sr, cy*cp*sr-sy*sp*cr]


def angles_from_quaternion(values):
    w, x, y, z = values
    norm = math.hypot(w, x, y, z)
    if not math.isfinite(norm) or norm == 0:
        raise ValueError("Camera quaternion must be finite and nonzero")
    w, x, y, z = (v/norm for v in (w, x, y, z))
    sine = max(-1.0, min(1.0, 2*(w*x-y*z)))
    pitch = math.asin(sine)
    if abs(sine) < 1-1e-12:
        yaw = math.atan2(2*(x*z+w*y), 1-2*(x*x+y*y))
        roll = math.atan2(2*(x*y+w*z), 1-2*(x*x+z*z))
    else:
        # At vertical pitch, yaw/roll are coupled. Choose equivalent roll=0.
        yaw = math.atan2(2*(w*y-x*z), 1-2*(y*y+z*z))
        roll = 0.0
    return [math.degrees(v) for v in (yaw, pitch, roll)]


class RigEditor(tk.Frame):
    """Edits snapshots on the Tk thread; capture worker applies them between bundles."""
    FIELDS = (("x", "X", .1), ("y", "Y", .1), ("z", "Z", .1),
              ("yaw", "Yaw left °", 1), ("pitch", "Pitch up °", 1), ("roll", "Roll °", 1),
              ("hfov", "Horizontal FOV °", 1), ("vfov", "Vertical FOV °", 1))

    def __init__(self, parent, config, submit, applied):
        super().__init__(parent, bg="#18232e", padx=12, pady=8)
        self.layout = deepcopy(config)
        self.original = deepcopy(config)
        self.submit, self.applied = submit, applied
        self.submitted = config
        self.index, self.pending = 0, None
        self.loading, self.dirty = False, set()
        self.result_seen = None
        self.status = tk.StringVar(value="Starting camera rig…")
        self.basis = tk.StringVar()
        self.auto = tk.BooleanVar(value=True)
        self.variables = {name: tk.StringVar() for name, _, _ in self.FIELDS}
        toolbar = tk.Frame(self, bg=self["bg"])
        toolbar.pack(fill="x")
        self.selector = ttk.Combobox(toolbar, state="readonly", width=22,
            values=[f"{v['slot']}: {v.get('name', 'Camera')}" for v in config["views"]])
        self.selector.current(0)
        self.selector.pack(side="left", padx=(0, 12))
        self.selector.bind("<<ComboboxSelected>>", self.select)
        tk.Checkbutton(toolbar, text="Auto apply", variable=self.auto, command=self.schedule,
                       bg=self["bg"], fg="white", selectcolor="#18232e").pack(side="left")
        tk.Button(toolbar, text="Apply", command=self.apply).pack(side="left", padx=4)
        tk.Button(toolbar, text="Reset camera", command=self.reset).pack(side="left", padx=4)
        tk.Label(toolbar, textvariable=self.basis, bg=self["bg"], fg="#d8e4ef").pack(side="left", padx=12)
        tk.Button(toolbar, text="Save layout…", command=self.save).pack(side="right")
        fields = tk.Frame(self, bg=self["bg"])
        fields.pack(fill="x", pady=(5, 0))
        for column, (name, label, step) in enumerate(self.FIELDS):
            fields.columnconfigure(column, weight=1)
            tk.Label(fields, text=label, bg=self["bg"], fg="#d8e4ef").grid(row=0, column=column, sticky="w")
            entry = tk.Spinbox(fields, textvariable=self.variables[name], increment=step,
                from_=-16000000, to=16000000, width=12, bg="#223344", fg="white", insertbackground="white")
            entry.grid(row=1, column=column, sticky="ew", padx=(0, 8))
            entry.bind("<Return>", lambda event: self.apply())
            self.variables[name].trace_add("write", lambda *args, key=name: self.changed(key))
        tk.Label(self, textvariable=self.status, bg=self["bg"], fg="#d8e4ef", anchor="w").pack(fill="x", pady=(5, 0))
        self.load_fields()

    def load_fields(self):
        view = self.layout["views"][self.index]
        self.basis.set("World XYZ" if view.get("basis", "chassis") == "world" else "Chassis: X right · Y up · Z back")
        values = list(view["position"]) + angles_from_quaternion(view["quaternion_wxyz"]) + [view["hfov_deg"], view["vfov_deg"]]
        self.loading = True
        try:
            for (name, _, _), value in zip(self.FIELDS, values):
                self.variables[name].set(f"{value:.12g}")
        finally:
            self.loading = False
        self.dirty.clear()

    def changed(self, name):
        if not self.loading:
            self.dirty.add(name)
            self.schedule()

    def schedule(self):
        if self.pending is not None:
            self.after_cancel(self.pending)
            self.pending = None
        if self.auto.get() and self.dirty:
            self.pending = self.after(250, self.apply)

    def apply(self):
        if self.pending is not None:
            self.after_cancel(self.pending)
            self.pending = None
        if not self.dirty:
            return True
        try:
            values = {name: float(var.get()) for name, var in self.variables.items()}
            if not all(math.isfinite(value) for value in values.values()):
                raise ValueError("Enter finite camera coordinates and angles")
            view = self.layout["views"][self.index]
            for index, name in enumerate(("x", "y", "z")):
                if name in self.dirty:
                    view["position"][index] = values[name]
            if self.dirty.intersection(("yaw", "pitch", "roll")):
                view["quaternion_wxyz"] = quaternion_from_angles(*(values[k] for k in ("yaw", "pitch", "roll")))
            for name in ("hfov", "vfov"):
                if name in self.dirty:
                    view[name+"_deg"] = values[name]
        except ValueError as error:
            self.status.set(str(error))
            return False
        self.dirty.clear()
        self.send()
        return True

    def send(self):
        self.submitted = deepcopy(self.layout)
        self.submit(self.submitted)
        self.status.set("Applying after the current camera bundle…")

    def select(self, event=None):
        index = self.selector.current()
        if not self.apply():
            self.selector.current(self.index)
            return
        self.index = index
        self.load_fields()

    def reset(self):
        if self.pending is not None:
            self.after_cancel(self.pending)
            self.pending = None
        self.layout["views"][self.index] = deepcopy(self.original["views"][self.index])
        self.load_fields()
        self.send()

    def sync(self, result):
        if result is not None and result is not self.result_seen:
            self.result_seen = result
            settings, error = result
            if settings is self.submitted:
                self.status.set("Layout rejected: "+error if error else
                                "Applied · basis: "+self.layout["views"][self.index].get("basis", "chassis"))

    def save(self):
        if not self.apply():
            return
        settings = self.applied()
        if settings is not self.submitted:
            self.status.set("Wait for this layout to be applied before saving")
            return
        filename = filedialog.asksaveasfilename(parent=self, defaultextension=".json",
            initialfile="camera-layout.json", filetypes=[("Camera layout", "*.json")])
        if not filename:
            return
        try:
            self.save_applied(filename)
        except (OSError, RuntimeError) as error:
            self.status.set("Save failed: "+str(error))

    def save_applied(self, filename):
        """Publish the accepted layout atomically, preserving the old file on error."""
        settings = self.applied()
        if settings is None or settings is not self.submitted:
            raise RuntimeError("The current layout has not been applied")
        temporary = None
        try:
            destination = Path(filename)
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=destination.parent,
                                             prefix=destination.name+".", suffix=".tmp", delete=False) as stream:
                temporary = stream.name
                json.dump(settings, stream, indent=2, ensure_ascii=False)
                stream.write("\n")
            os.replace(temporary, destination)
            self.status.set("Saved "+str(destination))
        finally:
            if temporary is not None and os.path.exists(temporary):
                os.unlink(temporary)
