"""Function-level main-thread profile from saved stacks (<label>.stacks.json).

Each frame maps to the function containing it through the image's .pdata on
disk, following chained unwind info so a split-off chunk counts for the
function that owns it. Leaf functions have no .pdata; a Ghidra function table
(research/ghidra/out/<image stem>.functions.csv) resolves those and names
functions. Self time is the sampled frame; inclusive time counts a function
once per stack. Times are wall-clock milliseconds of the main thread per
rendered frame, so waits (Present, sleeps) appear as functions too.
"""
import bisect
import csv
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GHIDRA_OUT = ROOT / "research" / "ghidra" / "out"


class Image:
    """Function boundaries of one PE file from .pdata and, if present, Ghidra."""

    def __init__(self, name, path):
        self.name, self.path = name, path
        self.begins, self.ends, self.roots = [], [], []
        self.names, self.ghidra = {}, []
        try:
            self._read_pdata(Path(path).read_bytes())
        except (OSError, ValueError, struct.error):
            pass
        table = GHIDRA_OUT / f"{Path(name).stem}.functions.csv"
        if table.exists():
            with open(table, encoding="utf-8", newline="") as source:
                for row in csv.DictReader(source):
                    entry, end = int(row["entry"], 16), int(row["end"], 16)
                    if not row["name"].startswith("FUN_"):  # Ghidra's default names add nothing to the RVA.
                        self.names[entry] = row["name"] if not row.get("namespace") else f"{row['namespace']}::{row['name']}"
                    self.ghidra.append((entry, end))
            self.ghidra.sort()
            self.ghidra_entries = [g[0] for g in self.ghidra]

    def _read_pdata(self, data):
        header = struct.unpack_from("<I", data, 0x3C)[0]
        if data[header:header + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        count, optional_size = struct.unpack_from("<H", data, header + 6)[0], struct.unpack_from("<H", data, header + 20)[0]
        optional = header + 24
        if struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise ValueError("not PE32+")
        directory_rva, directory_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
        sections = []
        for i in range(count):
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8)
            sections.append((rva, max(virtual_size, raw_size), raw_offset, raw_size))

        def offset(rva):
            for start, size, raw_offset, raw_size in sections:
                if start <= rva < start + size and rva - start < raw_size:
                    return raw_offset + rva - start
            raise ValueError(f"RVA 0x{rva:X} outside the file")

        def root(begin, unwind):
            for _ in range(32):
                if unwind & 1:  # Indirect: another RUNTIME_FUNCTION.
                    begin, _end, unwind = struct.unpack_from("<III", data, offset(unwind & ~1))
                    continue
                info = offset(unwind)
                if not (data[info] >> 3) & 0x4:  # UNW_FLAG_CHAININFO
                    return begin
                codes = data[info + 2]
                begin, _end, unwind = struct.unpack_from("<III", data, info + 4 + 2 * ((codes + 1) & ~1))
            return begin

        table = offset(directory_rva)
        for i in range(directory_size // 12):
            begin, end, unwind = struct.unpack_from("<III", data, table + 12 * i)
            if not begin:
                continue
            self.begins.append(begin)
            self.ends.append(end)
            self.roots.append(root(begin, unwind))

    def function(self, rva):
        """(entry RVA, exact) of the function containing rva."""
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and rva < self.ends[i]:
            return self.roots[i], True
        if self.ghidra:
            j = bisect.bisect_right(self.ghidra_entries, rva) - 1
            if j >= 0 and rva < self.ghidra[j][1]:
                return self.ghidra[j][0], True
        return rva & ~0xF, False

    def label(self, entry, exact):
        name = self.names.get(entry)
        return f"{self.name}!{name}" if name else f"{self.name}+0x{entry:X}{'' if exact else '?'}"


def load(paths):
    """Merge stacks files into ([(samples, wall ms, end, [(Image, rva) or (None, address)])], frames, wall ms).

    Each sample stands for its own file's wall time per sample, so files taken
    at different rates merge without one dominating. Image files are read only
    when a stack refers to them.
    """
    stacks, images, frames, wall_ms = [], {}, 0.0, 0.0
    for path in paths:
        path = Path(path)
        if not path.name.endswith(".stacks.json"):
            path = path.with_name(path.name.removesuffix(".json") + ".stacks.json")
        data = json.loads(path.read_text(encoding="utf-8"))
        listed = data["images"]

        def image(index):
            key = listed[index]["path"] or listed[index]["name"]
            if key not in images:
                images[key] = Image(listed[index]["name"], listed[index]["path"])
            return images[key]
        seconds = data.get("elapsed_s") or data["samples"] / data["hz"]
        per_sample = seconds * 1000 / max(1, data["samples"])
        frames += data["fps"] * seconds
        wall_ms += seconds * 1000
        for entry in data["stacks"]:
            stacks.append((entry[0], entry[0] * per_sample, entry[1],
                           [(image(entry[i]) if entry[i] >= 0 else None, entry[i + 1]) for i in range(2, len(entry), 2)]))
    return stacks, frames, wall_ms


def functions(stack):
    """Function keys from the sampled frame outward; return addresses step back into their call."""
    keys = []
    for depth, (image, rva) in enumerate(stack):
        if image is None:
            keys.append((None, rva, False))
        else:
            entry, exact = image.function(rva - (1 if depth else 0))
            keys.append((image, entry, exact))
    return keys


def label(key):
    image, entry, exact = key
    return f"?0x{entry:X}" if image is None else image.label(entry, exact)


def tree(paths, minimum=0.02, depth=24):
    """Root-first call tree; children below `minimum` of all wall time are folded away."""
    stacks, frames, wall_ms = load(paths)
    root = {"weight": 0.0, "children": {}}
    for _, weight, _, stack in stacks:
        node = root
        node["weight"] += weight
        for key in reversed(functions(stack)):
            node = node["children"].setdefault(key, {"weight": 0.0, "children": {}})
            node["weight"] += weight
    out = [f"{frames:.0f} frames, {wall_ms / frames if frames else 0:.2f} ms/frame wall; ms/frame, share of wall time"]

    def walk(node, level):
        if level >= depth:
            return
        children = sorted(node["children"].items(), key=lambda item: -item[1]["weight"])
        for key, child in children:
            if child["weight"] < minimum * wall_ms:
                break
            # Collapse single-child chains so the tree stays readable.
            name, inner = label(key), child
            while len(inner["children"]) == 1:
                (next_key, next_child), = inner["children"].items()
                if next_child["weight"] < inner["weight"] * 0.98:
                    break
                name += " > " + label(next_key)
                inner = next_child
            out.append(f"{'  ' * level}{child['weight'] / frames if frames else 0:7.3f} {child['weight'] / wall_ms:6.1%}  {name}")
            walk(inner, level + 1)
    walk(root, 0)
    return "\n".join(out)


def report(paths, top=40, focus=None, folded=None):
    stacks, frames, wall_ms = load(paths)
    samples = sum(count for count, _, _, _ in stacks)
    self_time, inclusive, callers, callees, ends = Counter(), Counter(), Counter(), Counter(), Counter()
    lines = Counter()
    for count, weight, end, stack in stacks:
        keys = functions(stack)
        ends[end] += weight
        if keys:
            self_time[keys[0]] += weight
        for key in set(keys):
            inclusive[key] += weight
        if focus is not None:
            for i, key in enumerate(keys):
                if key[0] is not None and key[0].name.lower() == focus[0] and key[1] == focus[1]:
                    callees[keys[i - 1] if i else ("(self)",)] += weight
                    callers[keys[i + 1] if i + 1 < len(keys) else ("(root)",)] += weight
                    break
        if folded:
            lines[";".join(label(k) for k in reversed(keys))] += count
    out = [f"{samples} samples, {frames:.0f} frames, {wall_ms / frames if frames else 0:.2f} ms/frame wall",
           "stack ends: " + ", ".join(f"{end} {weight / wall_ms:.1%}" for end, weight in ends.most_common())]

    def table(title, counter, limit):
        out.append(f"\n{title}")
        out.append(f"{'ms/frame':>9} {'share':>7}  function")
        for key, weight in counter.most_common(limit):
            name = key[0] if len(key) == 1 else label(key)
            out.append(f"{weight / frames if frames else 0:9.3f} {weight / wall_ms:7.2%}  {name}")
    table("Inclusive (function or its callees on the stack)", inclusive, top)
    table("Self (sampled instruction in the function)", self_time, top)
    if focus is not None:
        table(f"Callers of {focus[0]}+0x{focus[1]:X}", callers, top)
        table(f"Callees of {focus[0]}+0x{focus[1]:X} (share of all samples)", callees, top)
    if folded:
        Path(folded).write_text("".join(f"{k} {v}\n" for k, v in lines.most_common()), encoding="utf-8")
        out.append(f"\nfolded stacks: {folded} (speedscope.app or flamegraph.pl)")
    return "\n".join(out)
