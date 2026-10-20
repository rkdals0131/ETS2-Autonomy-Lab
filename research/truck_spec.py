"""Static specification of the player's truck from the save and the game's def files.

The ECU-configuration half of a CAN picture (J1939 EC1-like): engine torque and its
curve, gear and final drive ratios, retarder steps and what each step engages, axle
kerb weights, powered and lift axles, tyre rolling resistance and wet grip, and the
global physics constants (brake, viscosities, air resistance). Live values come from
the SDK channels and research/can_scan.py.

The truck is taken from the newest save of the profile (decrypted with
research/savedec, TruckLib), choosing the owned truck that matches the SDK truck
configuration (id, wheel count, gears, retarder steps) with the highest odometer. Defs
resolve through layers, first match wins: extracted active mods that change vehicles
(research/extracted/mod_*), then the extracted base game and DLCs.

Usage: py -3 research/truck_spec.py [--save <dir with game.sii>] [--out spec.json]
"""
import argparse, json, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRACTED = ROOT / "research" / "extracted"
PROFILE_SAVES = Path(r"C:\Program Files (x86)\Steam\userdata\110779611\227300\remote\profiles\4554535F5379734944\save")
DOTNET = ROOT / "research" / "tools" / "dotnet10" / "dotnet.exe"
SAVEDEC = ROOT / "research" / "savedec" / "bin" / "Release" / "net10.0" / "savedec.dll"
sys.path.insert(0, str(ROOT / "ot" / "py"))


def layers():
    mods = sorted(p for p in EXTRACTED.glob("mod_*") if p.is_dir())
    base = [EXTRACTED / "def"] + sorted(p for p in EXTRACTED.glob("dlc_*") if p.is_dir())
    return mods + base


def find(def_path):
    rel = def_path.lstrip("/")
    for layer in layers():
        f = layer / rel
        if f.exists():
            return f
    raise FileNotFoundError(def_path)


def parse(def_path):
    """First unit of a def file as {key: value or [values]}, @include resolved."""
    path = find(def_path)
    lines = expand(path, def_path)
    unit, depth = {}, 0
    for line in lines:
        line = line.split("#", 1)[0].split("//", 1)[0].strip()
        if not line:
            continue
        if line.endswith("{"):
            depth += 1
            continue
        if line == "}":
            depth -= 1
            if depth <= 1 and unit:
                break
            continue
        m = re.match(r"([\w.]+)(\[(\d*)\])?\s*:\s*(.*)$", line)
        if not m or depth < 2:
            continue
        key, is_array, value = m.group(1), m.group(2), value_of(m.group(4))
        if is_array:
            if not isinstance(unit.get(key, []), list):  # a count line ("key: n") before the items
                unit[key] = []
            unit.setdefault(key, []).append(value)
        else:
            unit[key] = value
    return {"source": str(path.relative_to(ROOT)), **unit}


def expand(path, def_path):
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r'\s*@include\s+"([^"]+)"', line)
        if m:
            inc = m.group(1)
            target = inc if inc.startswith("/") else str(Path(def_path).parent / inc).replace("\\", "/")
            out += expand(find(target), target)
        else:
            out.append(line)
    return out


def value_of(text):
    text = text.strip()
    if text.startswith('"'):
        return text.strip('"')
    if text.startswith("("):
        return [value_of(v) for v in text.strip("()").split(",")]
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def decrypt(save_dir):
    out = Path(tempfile.gettempdir()) / "truck_spec_game.sii"
    subprocess.run([str(DOTNET), str(SAVEDEC), str(Path(save_dir) / "game.sii"), str(out)], check=True)
    return out.read_text(encoding="utf-8", errors="replace")


def owned_trucks(text):
    units = {m.group(2): m.group(3) for m in re.finditer(r"^(\w+) : ([\w.]+) \{\n(.*?)^\}", text, re.S | re.M)}
    player = next(body for name, body in units.items() if re.search(r"^ my_vehicles_mode:", body, re.M))
    trucks = []
    for vid in re.findall(r"^ trucks\[\d+\]: ([\w.]+)", player, re.M):
        body = units[vid]
        paths = []
        for acc in re.findall(r"accessories\[\d+\]: ([\w.]+)", body):
            m = re.search(r'data_path: "([^"]+)"', units.get(acc, ""))
            if m:
                paths.append(m.group(1))
        odometer = re.search(r"^ odometer: (\d+)", body, re.M)
        trucks.append({"unit": vid, "odometer_km": int(odometer.group(1)) if odometer else 0, "accessories": paths})
    return trucks


def sdk_config():
    try:
        from otpy.client import Client
        attrs = Client(timeout=1.0).request("truck_config")["attributes"]
    except Exception:
        return None
    one = lambda n: next((a["value"] for a in attrs if a["name"] == n), None)
    return {"id": one("id"), "wheels": one("wheels.count"), "gears": one("gears.forward"), "retarder": one("retarder.steps")}


def spec_of(truck):
    pick = lambda part: next((p for p in truck["accessories"] if f"/{part}/" in p), None)
    engine, transmission, chassis = (parse(pick(k)) for k in ("engine", "transmission", "chassis"))
    tires = {"front": [parse(p) for p in truck["accessories"] if "/f_tire/" in p],
             "rear": [parse(p) for p in truck["accessories"] if "/r_tire/" in p]}
    physics = parse("/def/vehicle/physics.sii")
    kerb = [float(w) for w in chassis.get("kerb_weight", [])]
    aux = [{"step": i + 1, "retarder_fraction": a[0], "engine_brake": bool(a[1]), "third": a[2]}
           for i, a in enumerate(transmission.get("auxiliary_brakes", []))]
    return {
        "truck_unit": truck["unit"], "odometer_km": truck["odometer_km"], "accessories": truck["accessories"],
        "engine": {"source": engine["source"], "name": engine.get("name"), "torque_nm": engine.get("torque"),
                   "torque_curve_rpm_fraction": engine.get("torque_curve"), "volume_l": engine.get("volume")},
        "transmission": {"source": transmission["source"], "differential_ratio": transmission.get("differential_ratio"),
                         "ratios_forward": transmission.get("ratios_forward"), "ratios_reverse": transmission.get("ratios_reverse"),
                         "shift_time_s": transmission.get("shift_time"), "retarder_steps": transmission.get("retarder", 0),
                         "auxiliary_brakes": aux},
        "chassis": {"source": chassis["source"], "kerb_weight_kg_per_axle": kerb, "kerb_weight_kg": sum(kerb),
                    "powered_axle": chassis.get("powered_axle"), "liftable_axle": chassis.get("liftable_axle"),
                    "residual_travel_m": chassis.get("residual_travel")},
        "tires": {side: [{"source": t["source"], "roll_resistance": t.get("roll_resistance"), "wet_grip": t.get("wet_grip")}
                         for t in ts] for side, ts in tires.items()},
        "physics": physics,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--save", help="save directory with game.sii (default: newest of the profile)")
    p.add_argument("--out")
    a = p.parse_args()
    save = Path(a.save) if a.save else max((d for d in PROFILE_SAVES.iterdir() if (d / "game.sii").exists()),
                                           key=lambda d: (d / "game.sii").stat().st_mtime)
    trucks = owned_trucks(decrypt(save))
    sdk = sdk_config()
    candidates = trucks
    if sdk and sdk["id"]:
        brand = sdk["id"].split(".", 1)[1]
        candidates = [t for t in trucks if any(f"/truck/{brand}/" in x for x in t["accessories"])] or trucks
    truck = max(candidates, key=lambda t: t["odometer_km"])
    spec = {"save": str(save), "sdk": sdk, **spec_of(truck)}
    if sdk:
        spec["sdk_check"] = {"gears": sdk["gears"] == len(spec["transmission"]["ratios_forward"] or []),
                             "retarder": sdk["retarder"] == spec["transmission"]["retarder_steps"]}
    text = json.dumps(spec, indent=1, ensure_ascii=False)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    sys.stdout.reconfigure(errors="replace")
    print(text)


if __name__ == "__main__":
    main()
