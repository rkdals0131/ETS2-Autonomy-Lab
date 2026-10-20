"""Carry pinned image RVAs from an analyzed game build to a newer one.

  py -3 research/rebase_rvas.py --old-version 1.61.1.1 --new-version 1.61.1.9 [--analyzed] [rva ...]
  py -3 research/rebase_rvas.py --new-version 1.61.1.9 --write [--accept 646DC9=reason ...]

The old build is read from its Ghidra project dump (research/ghidra/ghidra.py image,
which keeps the bytes after Steam has replaced the executable) and its function list
(ghidra.py export); the new build is the installed executable, checked against
ot/schema/<new>/game.json. Without RVAs on the command line, every *_rva / *_rvas value
and device slot in ot/schema/<old>/fields.json is carried, plus the RVA literals in
ot/native. Results go to research/ghidra/out/<new>/rebase.json and a table on stdout.

How each RVA is carried:
- code: the containing Ghidra function (or 48 bytes from the RVA outside any) is
  located by voting over byte windows of its body; only windows found exactly once
  near the expected place vote. Both bodies are then disassembled (capstone) and
  compared instruction by instruction, with branch and RIP-relative targets replaced
  by where the old target moved to. "same" means only moves: every struct offset and
  constant inside is unchanged. Otherwise the differing instructions are listed in
  rebase.json for review; the RVA itself moves with its instruction.
- data (.rdata/.data): references to it in the old code (RIP-relative, or an
  image-relative displacement as in [base + index*8 + rva]) are located in the new
  code, and the new displacement gives the new target (vote over references). Without
  a direct reference, the references within 1 KiB of it vote for the delta.
- vtables (a run of pointers into .text; the game has no RTTI): at the voted place,
  80 % of the slots, or the first 4 in a row, must point to the moved slot functions. Without a reference, the
  code pointers in and around the table propose places that are then checked so.
With --write, the byte signatures that find_target() checks in ot/native are re-read
at the carried sites too (they may hold relative displacements).
A result is "ok" only when the methods agree and the code is the same; anything else
needs a look. This never opens a process; it reads files only.
"""
import argparse
import csv
import difflib
import hashlib
import json
import os
import re
import struct
from bisect import bisect_right
from collections import Counter
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).resolve().parents[1]
GHIDRA_OUT = ROOT / "research" / "ghidra" / "out"
GAME = Path(os.environ.get("ETS2_DIR", r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")) / "bin" / "win_x64" / "eurotrucks2.exe"
LEGACY_VERSION = "1.61.1.1"  # keeps the original research/ghidra/out layout
WINDOW = 24        # bytes per voting window
RADIUS = 0x80000   # search radius around the expected new place
BRANCHES = ("call", "j", "loop")


def out_dir(version):
    return GHIDRA_OUT if version == LEGACY_VERSION else GHIDRA_OUT / version


class Image:
    """A mapped PE image: bytes indexed by RVA plus the section table."""

    def __init__(self, data):
        self.data = bytes(data)
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        count, optional_size = struct.unpack_from("<H", self.data, pe + 6)[0], struct.unpack_from("<H", self.data, pe + 20)[0]
        optional = pe + 24
        self.base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        self.sections = []
        for i in range(count):
            name, size, rva = struct.unpack_from("<8sII", self.data, optional + optional_size + i * 40)
            self.sections.append((name.rstrip(b"\0").decode(), rva, size))
        self.text = next(s for s in self.sections if s[0] == ".text")

    @classmethod
    def from_file(cls, path):
        """Map a PE file from disk (headers, then each section at its RVA)."""
        raw = Path(path).read_bytes()
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        optional = pe + 24
        size_of_image, headers = struct.unpack_from("<II", raw, optional + 56)
        count, optional_size = struct.unpack_from("<H", raw, pe + 6)[0], struct.unpack_from("<H", raw, pe + 20)[0]
        image = bytearray(size_of_image)
        image[:headers] = raw[:headers]
        for i in range(count):
            _, vsize, rva, raw_size, raw_offset = struct.unpack_from("<8sIIII", raw, optional + optional_size + i * 40)
            n = min(vsize, raw_size)
            image[rva:rva + n] = raw[raw_offset:raw_offset + n]
        return cls(image)

    def section(self, rva):
        for name, start, size in self.sections:
            if start <= rva < start + max(size, 1):
                return name, start, size
        return None

    def in_text(self, rva):
        return self.text[1] <= rva < self.text[1] + self.text[2]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.data, rva)[0]

    def vtable_slots(self, rva, limit=128):
        """Slot targets (RVAs) when rva starts a run of at least two .text pointers."""
        slots = []
        while len(slots) < limit and rva + 8 * len(slots) + 8 <= len(self.data):
            target = self.u64(rva + 8 * len(slots)) - self.base
            if not self.in_text(target):
                break
            slots.append(target)
        return slots if len(slots) >= 2 else []


def informative(window, distinct=10):
    return len(set(window)) >= distinct


def unique_near(data, window, expected, radius=RADIUS):
    lo, hi = max(0, expected - radius), min(len(data), expected + radius + len(window))
    first = data.find(window, lo, hi)
    if first < 0 or data.find(window, first + 1, hi) >= 0:
        return None
    return first


class Functions:
    def __init__(self, path):
        rows = list(csv.DictReader(open(path, encoding="utf-8")))
        self.entries = sorted((int(r["entry"], 16), int(r["end"], 16) - int(r["entry"], 16) + 1) for r in rows)
        self.keys = [e for e, _ in self.entries]

    def containing(self, rva):
        i = bisect_right(self.keys, rva) - 1
        if i >= 0 and self.entries[i][0] <= rva < sum(self.entries[i]):
            return self.entries[i]
        return None


class Mapper:
    """Old RVA -> new RVA: coarse anchors every 4 KiB, then exact per-function votes."""

    def __init__(self, old, new, functions):
        self.old, self.new, self.functions = old, new, functions
        self.anchors, self.function_votes = {}, {}
        for name, start, size in old.sections:
            match = [s for s in new.sections if s[0] == name]
            if not match:
                continue
            guess, points = match[0][1] - start, []
            for p in range(start, start + size - WINDOW, 4096):
                w = old.data[p:p + WINDOW]
                if informative(w):
                    hit = unique_near(new.data, w, p + guess)
                    if hit is not None:
                        guess = hit - p
                        points.append((p, guess))
            self.anchors[name] = points

    def coarse(self, rva):
        sec = self.old.section(rva)
        points = self.anchors.get(sec[0]) if sec else None
        if not points:
            return 0
        i = bisect_right([p for p, _ in points], rva)
        return points[i - 1][1] if i else points[0][1]

    def function(self, entry, size):
        """(delta, votes, windows, all deltas) of a code range, voting over byte windows."""
        key = (entry, size)
        if key not in self.function_votes:
            guess = self.coarse(entry)
            votes, windows = Counter(), 0
            step = 8 if size > 64 else 2
            for off in range(0, max(1, size - 12 + 1), step):
                w = self.old.data[entry + off:entry + off + min(WINDOW, size)]
                if len(w) < 12 or not informative(w, min(10, len(w) // 2)):
                    continue
                windows += 1
                hit = unique_near(self.new.data, w, entry + off + guess)
                if hit is not None:
                    votes[hit - entry - off] += 1
            best = votes.most_common(1)[0] if votes else (None, 0)
            self.function_votes[key] = (best[0], best[1], windows, set(votes))
        return self.function_votes[key]

    def code(self, rva):
        """Delta of a code address via its function's vote (or its own bytes), or None."""
        fn = self.functions.containing(rva)
        delta, votes, windows, _ = self.function(*(fn or (rva, 32)))
        return delta if votes and votes * 2 >= windows else None

    def plausible(self, old_target, new_target):
        return abs(new_target - old_target - self.coarse(old_target)) <= 0x4000

    def corresponds(self, old_target, new_target, local_delta=None):
        """Whether a branch or RIP-relative target moved consistently with the image.

        Code: the new target moved like the referring function (local_delta) or the
        surrounding anchors, or where some window of the old target's function moved
        to, or it starts with the same 16 bytes. Data: the same non-zero 4 bytes within
        256 KiB (pooled constants and strings are reordered between builds), or near
        the section delta and either exactly the delta of the surrounding anchors, or
        with 16-byte contents equal apart from qwords that both hold addresses
        (pointers and import thunks move with the image)."""
        if (not self.old.in_text(old_target) and abs(new_target - old_target) <= 0x40000
                and self.old.data[old_target:old_target + 4] == self.new.data[new_target:new_target + 4]
                and any(self.old.data[old_target:old_target + 4])):
            return True
        if not self.plausible(old_target, new_target):
            return False
        if self.old.in_text(old_target):
            # cold blocks past the Ghidra function end move with the referring function
            if new_target - old_target in (local_delta, self.coarse(old_target)):
                return True
            fn = self.functions.containing(old_target)
            if new_target - old_target in self.function(*(fn or (old_target, 32)))[3]:
                return True
            return self.old.data[old_target:old_target + 16] == self.new.data[new_target:new_target + 16]
        if new_target - old_target == self.coarse(old_target):
            return True  # exactly the delta of the surrounding anchors
        size = len(self.old.data)
        for k in range(0, 16, 8):
            x = struct.unpack_from("<Q", self.old.data, old_target + k)[0]
            y = struct.unpack_from("<Q", self.new.data, new_target + k)[0]
            if x == y:
                continue
            for value in (x, y):
                if not (0x1000 <= value < size or self.old.base <= value < self.old.base + size):
                    return False
            if abs((y - x) - self.coarse(old_target)) > 0x40000 and abs(y - x) > 0x40000:
                return False
        return True

    def summary(self):
        lines = []
        for name, points in self.anchors.items():
            if points:
                deltas = Counter(d for _, d in points)
                common = ", ".join(f"{d:+#x} x{n}" for d, n in deltas.most_common(4))
                lines.append(f"  {name:8} {len(points):5} anchors, {len(deltas):3} distinct deltas: {common}")
        return "\n".join(lines)


DISASM = Cs(CS_ARCH_X86, CS_MODE_64)
DISASM.detail = True


def instructions(image, start, size):
    """(rva, text, target, normalized text) per instruction.

    The target is the RVA an operand names: a branch, a RIP-relative operand, an
    image-relative displacement (switch tables: [base + index*4 + rva]) or an
    absolute address in the image; the normalized text has it replaced by T."""
    out = []
    end = len(image.data)
    for ins in DISASM.disasm(image.data[start:start + size], start):
        text = f"{ins.mnemonic} {ins.op_str}"
        target, literal, relative = None, None, False
        for op in ins.operands:
            if op.type == X86_OP_IMM:
                if ins.mnemonic.startswith(BRANCHES):
                    target, literal, relative = op.imm, op.imm, True
                elif image.base + 0x1000 <= op.imm < image.base + end:
                    target, literal = op.imm - image.base, op.imm
            elif op.type == X86_OP_MEM:
                if op.mem.base == X86_REG_RIP:
                    target, literal, relative = ins.address + ins.size + op.mem.disp, op.mem.disp, True
                elif 0x100000 <= op.mem.disp < end:
                    target, literal = op.mem.disp, op.mem.disp
        norm = text
        if literal is not None:
            norm = re.sub(rf"(?<![0-9a-fx])0x{abs(literal):x}(?![0-9a-f])", "T", text, count=1)
        # relative: the target is an address by construction; otherwise the literal may
        # also be a plain large constant, which is fine when it is unchanged
        out.append((ins.address, text, target, norm, relative))
    return out


def compare_code(old, new, mapper, entry, size, delta):
    """Instruction diff of a function between the builds: (same, differing lines).

    Listings are aligned on the instruction text with targets removed; aligned pairs
    then must have corresponding targets."""
    a = instructions(old, entry, size)
    b = instructions(new, entry + delta, size + max(64, size // 4))
    na, nb = [x[3] for x in a], [x[3] for x in b]
    ops = difflib.SequenceMatcher(None, na, nb, autojunk=False).get_opcodes()
    # the new listing runs past the function end on purpose; drop that tail
    while ops and ops[-1][0] == "insert":
        ops.pop()
    if ops and ops[-1][0] == "replace" and ops[-1][2] == len(na):
        tag, i1, i2, j1, j2 = ops[-1]
        ops[-1] = (tag, i1, i2, j1, min(j2, j1 + (i2 - i1)))
    lines = []
    for tag, i1, i2, j1, j2 in ops:
        if tag == "equal":
            for x, y in zip(range(i1, i2), range(j1, j2)):
                ta, tb = a[x][2], b[y][2]
                if ta is None or ta == tb and not a[x][4]:
                    continue  # an unchanged large constant (struct offset), not an address
                if tb is None or not mapper.corresponds(ta, tb, delta):
                    lines.append(f"~ {a[x][0]:#x}/{b[y][0]:#x}  {na[x]}  (target {ta:#x} -> {tb:#x})")
            continue
        lines.extend(f"- {a[x][0]:#x}  {a[x][1]}" for x in range(i1, i2))
        lines.extend(f"+ {b[y][0]:#x}  {b[y][1]}" for y in range(j1, j2))
    return not lines, lines


def carry_code(old, new, mapper, fn, rva):
    entry, size = fn
    delta, votes, windows, _ = mapper.function(entry, size)
    same, lines = (False, []) if delta is None else compare_code(old, new, mapper, entry, size, delta)
    if not same:
        # Repetitive stubs have no unique window: try the deltas of the neighbouring
        # functions and keep one under which the code is the same.
        i = bisect_right(mapper.functions.keys, entry)
        neighbours = mapper.functions.entries[max(0, i - 2):i + 2]
        for d in dict.fromkeys([mapper.coarse(entry)] + [mapper.function(*n)[0] for n in neighbours]):
            if d is not None and d != delta and compare_code(old, new, mapper, entry, size, d)[0]:
                delta, same, lines, votes = d, True, [], "neighbour"
                break
    if delta is None:
        return {"status": "lost", "function": f"{entry:X}", "windows": windows}
    # An instruction-for-instruction match at the delta is itself strong evidence, so a
    # weak window vote only matters when the code differs.
    result = {"status": "ok" if same else "changed" if votes * 2 >= windows else "weak",
              "new": rva + delta, "delta": delta, "function": f"{entry:X}", "function_size": size,
              "votes": f"{votes}/{windows}", "body": "same" if same else f"{len(lines)} differing instruction lines"}
    if not same:
        result["diff"] = lines[:80]
        if rva != entry:
            # where the RVA itself sits relative to the change
            result["rva_instruction"] = next((l for l in lines if l[2:].startswith(f"{rva:#x}")), "unchanged")
    return result


def references(old, lo_target, hi_target):
    """Old .text references to [lo_target, hi_target): (position, imm, old target).

    imm >= 0: a RIP-relative disp32 at position with imm bytes of immediate after it.
    imm == -1: an image-relative disp32 (base register holds the image base), only
    for targets of 0x100000 and up so that small struct offsets do not match."""
    _, start, size = old.text
    hits, chunk = [], 1 << 22
    for lo in range(start, start + size - 4, chunk):
        hi = min(start + size - 4, lo + chunk)
        raw = np.frombuffer(old.data, np.uint8, hi - lo + 3, lo).astype(np.int64)
        value = raw[:-3] | (raw[1:-2] << 8) | (raw[2:-1] << 16) | (raw[3:] << 24)
        disp = np.where(value >= 1 << 31, value - (1 << 32), value)
        end = np.arange(lo, hi, dtype=np.int64) + 4 + disp
        for imm in (0, 1, 2, 4):
            target = end + imm
            for p in np.flatnonzero((target >= lo_target) & (target < hi_target)):
                hits.append((lo + int(p), imm, int(target[p])))
        if lo_target >= 0x100000:
            for p in np.flatnonzero((value >= lo_target) & (value < hi_target)):
                hits.append((lo + int(p), -1, int(value[p])))
    return hits


def instruction_covering(image, start, size, address):
    """The decoded instruction of [start, start+size) that covers address, or None."""
    found = None
    for ins in instructions(image, start, size):
        if ins[0] > address:
            break
        found = ins
    return found


def vote_references(old, new, mapper, refs, limit=60):
    """Counter of target deltas: each old reference located in the new code.

    A reference counts only when it decodes as an instruction naming that target and
    the instruction at the new place has the same text apart from the target. The new
    place comes from a unique byte window before it, else from its function's delta."""
    votes = Counter()
    for p, imm, target in refs[:limit]:
        fn = mapper.functions.containing(p)
        start, size = (fn[0], fn[1]) if fn else (max(old.text[1], p - 15), 32)
        ins = instruction_covering(old, start, size, p)
        if ins is None or ins[2] != target:
            continue  # bytes that only look like a reference
        offset = ins[0] - start
        places = []
        hit = unique_near(new.data, old.data[p - 20:p], p - 20 + mapper.coarse(p))
        if hit is not None:
            places.append(hit + 20 - (p - ins[0]))
        d = mapper.function(start, size)[0] if fn else None
        if d is not None:
            places.append(ins[0] + d)
        for place in dict.fromkeys(places):
            moved = instructions(new, place, 16)
            if moved and moved[0][0] == place and moved[0][3] == ins[3] and moved[0][2] is not None:
                votes[moved[0][2] - target] += 1
                break
    return votes


def carry_data(old, new, mapper, rva):
    refs = references(old, rva, rva + 1)
    votes = vote_references(old, new, mapper, refs)
    result = {"refs": len(refs)}
    if votes:
        delta, count = votes.most_common(1)[0]
        total = sum(votes.values())
        result.update(new=rva + delta, delta=delta, votes=f"{count}/{total}", status="ok" if count == total else "split")
    else:
        # No usable reference to the RVA itself (an array reached by index, say): the
        # references to its surroundings tell how the neighbourhood moved.
        near = [r for r in references(old, rva - 0x400, rva + 0x400) if r[2] != rva]
        votes = vote_references(old, new, mapper, near, 120)
        if votes:
            delta, count = votes.most_common(1)[0]
            total = sum(votes.values())
            result.update(new=rva + delta, delta=delta, votes=f"{count}/{total} near",
                          status="ok" if count == total and count >= 2 else "near-split")
        else:
            result.update(new=rva + mapper.coarse(rva), delta=mapper.coarse(rva), status="section-only")
    slots = old.vtable_slots(rva)
    if slots:
        result.update(vtable_check(old, new, mapper, rva, len(slots), result))
    return result


def vtable_check(old, new, mapper, rva, count, result):
    """Confirm (or, without references, find) a vtable by its slots.

    At a candidate place every slot pointer must correspond to the old slot function
    (Mapper.corresponds); 80 % is required since tiny shared slot functions may not be
    carried. Without a reference vote, candidates come from the code pointers in and
    around the table sitting at the same offsets in the new image."""
    def corresponding(place):
        """(slots that correspond, length of the corresponding run from slot 0)."""
        ok, prefix = 0, None
        for i in range(count):
            old_slot = old.u64(rva + 8 * i) - old.base
            new_slot = new.u64(place + 8 * i) - new.base
            good = new.in_text(new_slot) and mapper.corresponds(old_slot, new_slot)
            ok += good
            if not good and prefix is None:
                prefix = i
        return ok, count if prefix is None else prefix

    def accepted(ok, prefix):
        # The pointer run has no end marker and may run on into the next tables, so a
        # leading run of 4 corresponding slots also counts.
        return ok * 10 >= count * 8 or prefix >= min(count, 4)

    if result["status"] in ("ok", "split"):
        ok, prefix = corresponding(result["new"])
        out = {"vtable": f"{ok}/{count} slots correspond, first {prefix} in a row"}
        if accepted(ok, prefix):
            if result["status"] == "split":
                out["status"] = "ok"  # the slots confirm the majority of the reference vote
        else:
            out["status"] = "vtable-disagrees"
        return out
    score = Counter()
    guess = rva + mapper.coarse(rva)
    lo = max(0, guess - 0x200000)
    region = new.data[lo:guess + 0x200000]
    for at in range(rva - 256, rva + 8 * count + 256, 8):
        target = old.u64(at) - old.base
        if old.in_text(target):
            d = mapper.function(*(mapper.functions.containing(target) or (target, 32)))[0]
            if d is not None:
                for m in re.finditer(re.escape(struct.pack("<Q", new.base + target + d)), region):
                    score[lo + m.start() - (at - rva)] += 1
    for place, _ in score.most_common(5):
        ok, prefix = corresponding(place)
        if accepted(ok, prefix):
            return {"new": place, "delta": place - rva, "status": "ok",
                    "vtable": f"{ok}/{count} slots correspond, first {prefix} in a row (searched)"}
    return {"status": "vtable-lost", "vtable_best": [(hex(h), n) for h, n in score.most_common(3)]}


def schema_rvas(path):
    found = {}

    def walk(node, where):
        if isinstance(node, dict):
            for key, value in node.items():
                here = f"{where}.{key}" if where else key
                if key.endswith("_rva") and isinstance(value, int):
                    found.setdefault(value, []).append(here)
                elif key.endswith("_rvas") and isinstance(value, list):
                    for i, v in enumerate(value):
                        found.setdefault(v, []).append(f"{here}[{i}]")
                elif key == "device_slots_checked":
                    for slot, v in value.items():
                        found.setdefault(int(v, 16) if isinstance(v, str) else v, []).append(f"{here}.{slot}")
                else:
                    walk(value, here)
    walk(json.loads(path.read_text(encoding="utf-8")), "")
    return found


def native_rvas(image):
    """RVA-looking literals (0x100000 and up, inside the image) in ot/native, by file:line."""
    found = {}
    for path in sorted((ROOT / "ot" / "native").glob("*.[ch]pp")):
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for literal in re.findall(r"\b0x[0-9A-Fa-f]{6,7}\b", line):
                value = int(literal, 16)
                if 0x100000 <= value < len(image.data) and image.section(value):
                    found.setdefault(value, []).append(f"{path.name}:{n}")
    return found


def carry_signatures(old, new, by_old, usable):
    """Byte signatures checked by find_target(signature, adjustment, rva) in ot/native.

    A signature may hold relative displacements, so after a move its bytes are
    re-read at the carried site. The old bytes must match the old image first, and
    the new ones must occur exactly once in the new executable sections. Only the
    changed byte tokens are rewritten, so the source keeps its layout."""
    by_new = {r["new"]: r for r in by_old.values() if "new" in r and usable(r)}
    text_sections = [s for s in new.sections if s[0] == ".text"]
    changed_files = 0
    for path in sorted((ROOT / "ot" / "native").glob("*.cpp")):
        newline = "\r\n" if b"\r\n" in path.read_bytes() else "\n"
        text = path.read_text(encoding="utf-8")
        constants = {m[1]: int(m[2], 16) for m in re.finditer(r"\b(\w+_rva)=(0x[0-9A-Fa-f]+)", text)}
        arrays = {m[1]: m for m in re.finditer(r"std::array<uint8_t,\d+> (\w+)=\{([^}]*)\}", text)}
        edits = []
        for call in re.finditer(r"find_target\((\w+),(\d+),(\w+)\)", text):
            name, adjustment, rva_name = call[1], int(call[2]), call[3]
            if name not in arrays or rva_name not in constants:
                raise SystemExit(f"{path.name}: cannot resolve find_target({name},{adjustment},{rva_name})")
            value = constants[rva_name]
            r = by_new.get(value) or by_old.get(value)
            if r is None or "new" not in r:
                raise SystemExit(f"{path.name}: {rva_name}={value:#x} was not carried")
            tokens = arrays[name][2].split(",")
            pattern = bytes(int(t.strip(), 0) for t in tokens)
            old_start = r["old"] - adjustment
            if old.data[old_start:old_start + len(pattern)] != pattern:
                raise SystemExit(f"{path.name}: {name} does not match the old image at {old_start:#x}")
            new_start = r["new"] - adjustment
            fresh = new.data[new_start:new_start + len(pattern)]
            hits = sum(new.data.count(fresh, start, start + size) for _, start, size in text_sections)
            if hits != 1:
                raise SystemExit(f"{path.name}: carried {name} occurs {hits} times in the new image")
            if fresh != pattern:
                # replace only the changed tokens, keeping their whitespace and line breaks
                edits.append((arrays[name], [t.replace(t.strip(), f"0x{y:02X}") if x != y else t
                                             for t, x, y in zip(tokens, pattern, fresh)]))
                print(f"  {path.name}: {name} rewritten ({sum(a != b for a, b in zip(pattern, fresh))} bytes)")
        for match, tokens in sorted(edits, key=lambda e: -e[0].start(2)):
            text = text[:match.start(2)] + ",".join(tokens) + text[match.end(2):]
        if edits:
            path.write_text(text, encoding="utf-8", newline=newline)
            changed_files += 1
    print(f"{changed_files} files with carried signatures")


def write_outputs(args, old, new, results):
    """ot/schema/<new>/fields.json and rebase.json, and the literals in ot/native."""
    by_old = {r["old"]: r for r in results}
    accepted = {int(x, 16): reason for x, reason in (a.split("=", 1) if "=" in a else (a, "checked by hand")
                                                      for a in args.accept)}
    for rva, reason in accepted.items():
        if rva not in by_old or "new" not in by_old[rva]:
            raise SystemExit(f"--accept {rva:#x}: no carried result to accept")
        by_old[rva]["accepted"] = reason
    usable = lambda r: r["status"] == "ok" or "accepted" in r
    blocking = [r for r in results if not usable(r) and not all(u.startswith("decomp/") for u in r["used_by"])]
    if blocking:
        for r in blocking:
            print(f"  {r['old']:#x} {r['status']} used by {', '.join(r['used_by'])}")
        raise SystemExit("not written: resolve these (decompile) or pass --accept <rva>=<reason>")

    schema_dir = ROOT / "ot" / "schema"
    schema = json.loads((schema_dir / args.old_version / "fields.json").read_text(encoding="utf-8"))
    carry = lambda v: by_old[v]["new"]

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key.endswith("_rva") and isinstance(value, int):
                    node[key] = carry(value)
                elif key.endswith("_rvas") and isinstance(value, list):
                    node[key] = [carry(v) for v in value]
                elif key == "device_slots_checked":
                    node[key] = {slot: f"{carry(int(v, 16)):#x}" if isinstance(v, str) else carry(v)
                                 for slot, v in value.items()}
                elif key.endswith("_signature") and isinstance(value, str):
                    site = node[key.replace("_signature", "_rva")]
                    pattern = bytes.fromhex(value.replace(" ", ""))
                    if new.data[site:site + len(pattern)] != pattern:
                        raise SystemExit(f"{key}: bytes at the carried {site:#x} differ")
                else:
                    walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)
    schema["game_version"] = args.new_version
    walk(schema)
    # Rewrite the numbers in the old text so the new file keeps its layout and diffs
    # against the old one show only the carried values; then check it parses to the
    # carried structure.
    text = (schema_dir / args.old_version / "fields.json").read_text(encoding="utf-8")
    number = lambda m: str(carry(int(m[0])))
    text = re.sub(r'("\w+_rva"\s*:\s*)(\d+)', lambda m: m[1] + number(re.match(r"\d+", m[2])), text)
    text = re.sub(r'("\w+_rvas"\s*:\s*\[)([^\]]*)', lambda m: m[1] + re.sub(r"\d+", number, m[2]), text)
    text = re.sub(r'("device_slots_checked"\s*:\s*\{)([^}]*)',
                  lambda m: m[1] + re.sub(r'(:\s*)(\d+|"0x[0-9a-fA-F]+")', lambda v: v[1] + (
                      f'"{carry(int(v[2][1:-1], 16)):#x}"' if v[2].startswith('"') else str(carry(int(v[2])))), m[2]),
                  text)
    text = re.sub(r'("game_version"\s*:\s*)"[^"]*"', lambda m: f'{m[1]}"{args.new_version}"', text, count=1)
    if json.loads(text) != schema:
        raise SystemExit("rewritten fields.json does not match the carried schema")
    target = schema_dir / args.new_version
    (target / "fields.json").write_text(text, encoding="utf-8", newline="")
    keep = ("old", "new", "kind", "status", "votes", "function", "body", "vtable", "accepted", "used_by")
    table = [{k: (f"{r[k]:#x}" if k in ("old", "new") else r[k]) for k in keep if k in r}
             for r in results if not all(u.startswith("decomp/") for u in r["used_by"])]
    (target / "rebase.json").write_text(json.dumps({
        "from": args.old_version, "to": args.new_version, "tool": "research/rebase_rvas.py",
        "note": "Addresses in docs/ refer to the build they were found in; this table carries them.",
        "results": table}, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {target / 'fields.json'} and rebase.json")

    # Guard against carrying twice: literals already holding new values are left alone.
    moved = {r["old"]: r["new"] for r in by_old.values() if "new" in r and r["new"] != r["old"] and usable(r)}
    sources = "".join(p.read_text(encoding="utf-8") for p in (ROOT / "ot" / "native").glob("*.[ch]pp"))
    literals = {int(x, 16) for x in re.findall(r"\b0x[0-9A-Fa-f]{6,7}\b", sources)}
    olds, news = literals & set(moved), literals & (set(moved.values()) - set(moved))
    if news and olds:
        raise SystemExit(f"ot/native mixes old and carried literals ({len(olds)} old, {len(news)} new); fix by hand")
    carry_literals = not news
    if not carry_literals:
        print("ot/native literals are already carried; only signatures are checked")
    changed = 0
    for path in sorted((ROOT / "ot" / "native").glob("*.[ch]pp")) if carry_literals else []:
        newline = "\r\n" if b"\r\n" in path.read_bytes() else "\n"
        text = path.read_text(encoding="utf-8")

        def replace(match):
            value = int(match[0], 16)
            r = by_old.get(value)
            if not r or "new" not in r or r["new"] == value or not usable(r):
                return match[0]
            digits = f"{r['new']:X}" if match[0][2:].isupper() or match[0][2:].isdigit() else f"{r['new']:x}"
            return "0x" + digits
        updated = re.sub(r"\b0x[0-9A-Fa-f]{6,7}\b", replace, text)
        if updated != text:
            path.write_text(updated, encoding="utf-8", newline=newline)
            changed += 1
            print(f"updated {path.relative_to(ROOT)}")
    print(f"{changed} native files updated")
    carry_signatures(old, new, by_old, usable)


def layout_changes(old, new, functions, mapper):
    """Struct displacements that changed between the builds, over the whole binary.

    Every function whose bytes differ is aligned instruction by instruction (by
    mnemonic); aligned memory operands on the same base register (not rip/rsp/rbp)
    give (old displacement, new displacement) pairs. Returns per old displacement
    the counts of unchanged uses and of each shift. A few minutes for 70k functions."""
    shifted, unchanged = {}, Counter()

    def operands(image, start, size):
        out = []
        for ins in DISASM.disasm(image.data[start:start + size], start):
            out.append((ins.mnemonic, tuple((ins.reg_name(op.mem.base), op.mem.disp) for op in ins.operands
                                            if op.type == X86_OP_MEM and op.mem.base
                                            and ins.reg_name(op.mem.base) not in ("rip", "rsp", "rbp"))))
        return out

    for entry, size in functions.entries:
        if not 16 <= size <= 40000 or not informative(old.data[entry:entry + 24], 8):
            continue
        hit = unique_near(new.data, old.data[entry:entry + 24], entry + mapper.coarse(entry), 0x8000)
        if hit is None or old.data[entry:entry + size] == new.data[hit:hit + size]:
            continue
        a, b = operands(old, entry, size), operands(new, hit, size + 64)
        matcher = difflib.SequenceMatcher(None, [x[0] for x in a], [x[0] for x in b], autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag != "equal":
                continue
            for x, y in zip(range(i1, i2), range(j1, j2)):
                for (ra, da), (rb, db) in zip(a[x][1], b[y][1]):
                    if ra != rb:
                        continue
                    if da == db:
                        unchanged[da] += 1
                    else:
                        shifted.setdefault(da, Counter())[db - da] += 1
    return shifted, unchanged


def check_native_offsets(shifted, unchanged):
    """ot/native struct offsets (+/-0x100..0xFFFF) that moved somewhere in the binary.

    A displacement value is shared by many classes, so a hit is a lead to confirm in
    the code that reads the same object (as camera_rig's vehicle+0x1098 was)."""
    leads = []
    for path in sorted((ROOT / "ot" / "native").glob("*.[ch]pp")):
        if path.name.endswith("_test.cpp"):
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for literal in re.findall(r"[+-]\s*(0x[0-9A-Fa-f]{3,4})\b", line):
                value = int(literal, 16)
                if shifted.get(value):
                    moves = ", ".join(f"{d:+#x} x{c}" for d, c in shifted[value].most_common())
                    leads.append(f"{path.name}:{n} {literal}: unchanged x{unchanged[value]}, moved {moves} | {line.strip()[:100]}")
    return leads


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--old-version", default=LEGACY_VERSION)
    parser.add_argument("--new-version", required=True)
    parser.add_argument("--analyzed", action="store_true",
                        help="also compare every function decompiled for the old build (out/decomp/<rva>.c),"
                             " to find struct layout changes")
    parser.add_argument("--layout", action="store_true",
                        help="scan the whole binary for changed struct displacements and list the ot/native"
                             " offsets among them (a few minutes); out/<new>/layout.json")
    parser.add_argument("--write", action="store_true",
                        help="write ot/schema/<new>/fields.json and rebase.json and update the RVA literals in"
                             " ot/native; refused while an RVA in use is not ok")
    parser.add_argument("--accept", action="append", default=[], metavar="RVA=REASON",
                        help="treat this old RVA's carried result as checked (repeatable)")
    parser.add_argument("rvas", nargs="*", help="hex RVAs; default: schema and ot/native")
    args = parser.parse_args()
    old_dir = out_dir(args.old_version)
    old = Image((old_dir / "image.bin").read_bytes())
    expected = json.loads((ROOT / "ot" / "schema" / args.new_version / "game.json").read_text(encoding="utf-8"))["exe_sha256"]
    if hashlib.sha256(GAME.read_bytes()).hexdigest() != expected:
        raise SystemExit(f"{GAME} is not {args.new_version}")
    new = Image.from_file(GAME)
    functions = Functions(old_dir / "eurotrucks2.functions.csv")
    mapper = Mapper(old, new, functions)
    print("section map:\n" + mapper.summary())

    if args.rvas:
        sources = {int(r, 16): ["command line"] for r in args.rvas}
    else:
        sources = schema_rvas(ROOT / "ot" / "schema" / args.old_version / "fields.json")
        # After a --write the native literals already hold new values; the earlier
        # table maps them back so a rerun analyzes (and carries) the same old RVAs.
        previous = ROOT / "ot" / "schema" / args.new_version / "rebase.json"
        back = {}
        if previous.exists():
            back = {int(r["new"], 16): int(r["old"], 16)
                    for r in json.loads(previous.read_text(encoding="utf-8"))["results"] if "new" in r}
        for value, where in native_rvas(old).items():
            sources.setdefault(back.get(value, value), []).extend(where)
    if args.analyzed:
        for path in sorted((old_dir / "decomp").glob("*.c")):
            sources.setdefault(int(path.stem, 16), []).append(f"decomp/{path.name}")

    results = []
    for rva in sorted(sources):
        sec = old.section(rva)
        if sec and sec[0] == ".text":
            fn = functions.containing(rva)
            r = carry_code(old, new, mapper, fn or (rva, 48), rva)
            r["kind"] = "code"
            if not fn:
                r["function"] = "none (48 bytes from the RVA)"
        else:
            r = carry_data(old, new, mapper, rva)
            r["kind"] = "data"
        r.update(old=rva, section=sec[0] if sec else None, used_by=sources[rva])
        results.append(r)
        new_text = f"{r['new']:#x}" if "new" in r else "-"
        extra = r.get("body") or r.get("vtable") or ""
        print(f"{rva:#9x} -> {new_text:>9} {r['kind']:4} {r['status']:17} {r.get('votes', ''):>8}  {extra[:34]:34}  {sources[rva][0]}")
    out = out_dir(args.new_version)
    out.mkdir(parents=True, exist_ok=True)
    (out / "rebase.json").write_text(json.dumps({"old": args.old_version, "new": args.new_version,
                                                 "results": results}, indent=1), encoding="utf-8")
    bad = [r for r in results if r["status"] != "ok"]
    print(f"{len(results)} RVAs, {len(results) - len(bad)} ok, {len(bad)} need a look -> {out / 'rebase.json'}")
    if args.layout:
        shifted, unchanged = layout_changes(old, new, functions, mapper)
        deltas = Counter()
        for counts in shifted.values():
            deltas.update(counts)
        print("struct displacement shifts (uses):", ", ".join(f"{d:+#x} x{c}" for d, c in deltas.most_common(8)))
        (out / "layout.json").write_text(json.dumps({f"{v:#x}": {"unchanged": unchanged[v], "moved": {
            f"{d:+#x}": c for d, c in counts.items()}} for v, counts in sorted(shifted.items())}, indent=1),
            encoding="utf-8", newline="\n")
        leads = check_native_offsets(shifted, unchanged)
        print(f"{len(leads)} ot/native offsets moved somewhere (confirm each in the code that reads it):")
        for lead in leads:
            print("  " + lead)
    if args.write:
        if args.rvas:
            raise SystemExit("--write carries the whole schema; leave out explicit RVAs")
        write_outputs(args, old, new, results)


if __name__ == "__main__":
    main()
