"""Second, independent check of rebase_rvas.py: re-find pinned RVAs by a string-reference rule.

  py -3 research/xref_crosscheck.py generate [--old-version 1.61.1.1] [--from-rebase ot/schema/1.61.1.9/rebase.json]
  py -3 research/xref_crosscheck.py apply --new-version 1.61.1.9

Rule per address (research/xref_rules.json, readable by hand):
  unique string (exact up to 80 bytes, else its 80-byte prefix)
    -> the instruction that references it (xref_index among the string's references)
    -> the function holding that instruction (.pdata, chained unwind chunks merged into the root)
    -> one selector inside that function:
         function_start | call n (n-th E8 call after/before the reference, or from the start)
         | call_return n (address after that call) | rip_target (n-th instruction whose RIP-relative
         target is a vtable or a .data object) | signature (instruction bytes with relative
         displacements masked, n-th match in the function)

generate: reads the old build's image dump (research/ghidra/out/image.bin, from ghidra.py image), builds a
rule for each known old RVA when it finds a string that is unique in that build, and keeps the rule only if
applying it to the same dump returns the original RVA. Addresses without such a rule stay "rule: null" with
the reason; no rule is forced.
apply: runs the rules on the installed exe (hash checked against ot/schema/<new>/game.json) and compares
with ot/schema/<new>/rebase.json (agree / disagree / no rule / rule not applicable). A disagreement keeps
both values and the evidence for a person; nothing is chosen automatically. Output:
research/ghidra/out/<new>/xref_crosscheck.json and a table on stdout. Reads files only.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import types
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research" / "snowymoon"))
sys.path.insert(0, str(ROOT / "research"))
import xref_engine as xe                      # noqa: E402  (string/xref engine, Game class)
from rebase_rvas import Image, out_dir, GAME  # noqa: E402  (only the PE image reader and paths)

RULES = ROOT / "research" / "xref_rules.json"
RIP = xe.RIP
PRINTABLE = set(range(0x20, 0x7F)) | {9, 10}
PREFIX = 80          # strings longer than this are matched by their first PREFIX bytes
FUNC_LIMIT = 0x20000  # largest function body decoded for a rule
DATA_FUNCS = 60       # referencing functions examined per data/vtable address


class RuleError(Exception):
    pass


class ImageGame(xe.Game):
    """xe.Game over a mapped image dump (the old build's image.bin) instead of an exe on disk."""

    def __init__(self, image_path):
        self.path = Path(image_path)
        self.image = Image(self.path.read_bytes())
        self.base, self.mem = self.image.base, self.image.data
        sec = {n: (rva, size) for n, rva, size in self.image.sections}
        self.text = sec[".text"]
        self.strings = (sec[".rdata"][0], sec[".data"][0] + sec[".data"][1] - sec[".rdata"][0])
        self.cs = xe.capstone.Cs(xe.capstone.CS_ARCH_X86, xe.capstone.CS_MODE_64)
        self.cs.skipdata = True
        self.detail = xe.capstone.Cs(xe.capstone.CS_ARCH_X86, xe.capstone.CS_MODE_64)
        self.detail.detail = True

    def _cache_file(self):
        st = self.path.stat()
        return xe.CACHE / f"xref_scan_image_{st.st_size}_{int(st.st_mtime)}.npz"


class Build:
    """Lookups the rules need on top of the engine: sections, .pdata functions, xref index, strings."""

    def __init__(self, g, sections):
        self.g, self.base, self.mem = g, g.base, g.mem
        self.sections = sections                      # [(name, rva, size)]
        self.text_lo, self.text_hi = g.text[0], g.text[0] + g.text[1]
        self.str_lo, self.str_hi = g.strings[0], g.strings[0] + g.strings[1]
        self._pdata()
        tgt = np.frombuffer(g.rip_target, np.uint64)
        order = np.argsort(tgt, kind="stable")
        self.xt = tgt[order]
        self.xa = np.frombuffer(g.sym_addr, np.uint64)[np.frombuffer(g.rip_idx, np.uint32)[order]]
        self._decoded = {}

    # --- .pdata ---
    def _pdata(self):
        mem = self.mem
        pe = struct.unpack_from("<I", mem, 0x3C)[0]
        rva, size = struct.unpack_from("<II", mem, pe + 24 + 112 + 24)
        raw = np.frombuffer(mem, np.uint32, size // 12 * 3, rva).reshape(-1, 3)
        raw = raw[np.argsort(raw[:, 0], kind="stable")]
        self.begins, self.ends = raw[:, 0].astype(np.int64), raw[:, 1].astype(np.int64)
        index = {int(b): i for i, b in enumerate(self.begins)}

        def root(i):
            for _ in range(8):
                u = int(raw[i, 2]) & ~1
                if (mem[u] >> 3) & 4:                 # UNW_FLAG_CHAININFO: parent entry follows the codes
                    p = u + 4 + ((mem[u + 2] + 1) & ~1) * 2
                    j = index.get(struct.unpack_from("<I", mem, p)[0])
                    if j is None or j == i:
                        return i
                    i = j
                else:
                    return i
            return i
        self.root = [root(i) for i in range(len(raw))]
        # primary range of a function: its root chunk plus the chunks that chain to it back to back
        # (MSVC splits one body into several .pdata entries); chunks elsewhere are cold paths.
        self.prim_end = {}
        for i, r in enumerate(self.root):
            if r == i:
                self.prim_end[i] = int(self.ends[i])
        for i, r in enumerate(self.root):
            if r != i and int(self.begins[i]) == self.prim_end.get(r):
                self.prim_end[r] = int(self.ends[i])

    def func(self, rva):
        """(start, end, in_primary) of the function holding rva (root chunk range), or None."""
        i = int(np.searchsorted(self.begins, rva, side="right")) - 1
        if i < 0 or not (self.begins[i] <= rva < self.ends[i]):
            return None
        r = self.root[i]
        s, e = int(self.begins[r]), self.prim_end[r]
        return s, e, s <= rva < e

    # --- xrefs, instructions, strings ---
    def xrefs(self, rva_lo, rva_hi=None):
        """RVAs of instructions whose first RIP-relative target is in [rva_lo, rva_hi); (insn, target) pairs."""
        lo = self.base + rva_lo
        hi = lo + 1 if rva_hi is None else self.base + rva_hi
        a, b = np.searchsorted(self.xt, lo), np.searchsorted(self.xt, hi)
        out = sorted((int(x) - self.base, int(t) - self.base) for x, t in zip(self.xa[a:b], self.xt[a:b]))
        return out

    def insns(self, start, end):
        key = (start, end)
        if key not in self._decoded:
            rows = []
            for ins in self.g.cs.disasm(self.mem[start:end], self.base + start):
                a = ins.address - self.base
                m = RIP.search(ins.op_str)
                t = None
                if m:
                    d = int(m.group(2), 0) if m.group(2) else 0
                    t = a + ins.size + (-d if m.group(1) == "-" else d)
                rows.append((a, ins.size, ins.mnemonic, ins.op_str, self.mem[a], t))
            self._decoded[key] = rows
        return self._decoded[key]

    def secname(self, rva):
        for n, s, size in self.sections:
            if s <= rva < s + size:
                return n
        return None

    def u64(self, rva):
        return struct.unpack_from("<Q", self.mem, rva)[0]

    def string_at(self, rva):
        if not (self.str_lo <= rva < self.str_hi) or (rva and self.mem[rva - 1] in PRINTABLE):
            return None
        e = self.mem.find(b"\0", rva, rva + 400)
        if e < 0 or e - rva < 5:
            return None
        s = bytes(self.mem[rva:e])
        return s if all(c in PRINTABLE for c in s) else None

    def occurrences(self, needle, cap=3):
        out, pos = [], self.str_lo
        while len(out) < cap:
            pos = self.mem.find(needle, pos, self.str_hi)
            if pos < 0:
                break
            out.append(pos)
            pos += 1
        return out

    def cls(self, target):
        """vtable-like (first qword points into .text) or the section name."""
        if self.secname(target) in (".data", ".rdata", "_RDATA"):
            q = self.u64(target) - self.base
            if self.text_lo <= q < self.text_hi:
                return "vtable"
        return self.secname(target)


def string_key(text):
    """(needle bytes, match kind) for a string found in a function."""
    if len(text) > PREFIX:
        return text[:PREFIX], "prefix"
    return text, "exact"


def find_string(b, text, match):
    hits = b.occurrences(text + (b"\0" if match == "exact" else b""))
    if not hits:
        raise RuleError("string not found")
    if len(hits) > 1:
        raise RuleError("string no longer unique")
    return hits[0]


# --- signature (instruction bytes, relative displacements masked) ---

def signature_at(b, rva, min_bytes=16, min_open=8):
    """Hex pattern of whole instructions from rva, '..' where a RIP/branch displacement is, or None."""
    data = bytes(b.mem[rva:rva + 0x60])
    out, n, openb = [], 0, 0
    for ins in b.g.detail.disasm(data, b.base + rva):
        raw = bytes(ins.bytes)
        mask = set()
        if ins.disp_size and any(op.type == xe.x86.X86_OP_MEM and op.mem.base == xe.x86.X86_REG_RIP for op in ins.operands):
            mask |= set(range(ins.disp_offset, ins.disp_offset + ins.disp_size))
        if ins.imm_size and (ins.mnemonic[0] in "jc" or ins.mnemonic.startswith("loop")) and ins.operands \
                and ins.operands[0].type == xe.x86.X86_OP_IMM:
            mask |= set(range(ins.imm_offset, ins.imm_offset + ins.imm_size))
        for k, c in enumerate(raw):
            out.append("..") if k in mask else out.append(f"{c:02x}")
        openb += len(raw) - len(mask)
        n += len(raw)
        if n >= min_bytes:
            break
    return " ".join(out) if n >= min_bytes and openb >= min_open else None


def signature_matches(b, pattern, start, end):
    toks = pattern.split()
    rx = re.compile(b"".join(b"." if t == ".." else re.escape(bytes([int(t, 16)])) for t in toks), re.DOTALL)
    hits, pos = [], start
    body = bytes(b.mem[start:end])
    while True:
        m = rx.search(body, pos - start)
        if not m:
            return hits
        hits.append(start + m.start())
        pos = start + m.start() + 1


# --- applying a rule ---

def resolve(b, rule):
    """RVA the rule gives in build b; raises RuleError with the reason."""
    s = rule["string"]
    hit = find_string(b, s["text"].encode(), s["match"])
    xs = [x for x, _ in b.xrefs(hit)]
    if len(xs) != s["xrefs"]:
        raise RuleError(f"string references {s['xrefs']} -> {len(xs)}")
    x = xs[s["xref_index"]]
    f = b.func(x)
    if not f:
        raise RuleError("reference not inside a .pdata function")
    start, end, primary = f
    sel = rule["select"]
    t = sel["type"]
    if t == "function_start":
        return start
    if not primary:
        raise RuleError("reference is in a cold chunk")
    ins = b.insns(start, end)
    xi = next((i for i, r in enumerate(ins) if r[0] == x), None)
    if xi is None:
        raise RuleError("reference not an instruction boundary")
    if t in ("call", "call_return"):
        calls = [i for i, r in enumerate(ins) if r[1] == 5 and r[4] == 0xE8 and r[2] == "call"]
        before = sum(1 for i in calls if i < xi)
        n = sel["n"]
        c = n if sel["anchor"] == "start" else (before + n - 1 if n > 0 else before + n)
        if not 0 <= c < len(calls):
            raise RuleError(f"call {n} out of range ({len(calls)} calls)")
        a = ins[calls[c]][0]
        return a + 5 if t == "call_return" else a
    if t == "rip_target":
        sub = [i for i, r in enumerate(ins) if r[5] is not None and b.cls(r[5]) == sel["filter"]]
        before = sum(1 for i in sub if i < xi)
        n = sel["n"]
        c = n if sel["anchor"] == "start" else (before + n - 1 if n > 0 else before + n)
        if not 0 <= c < len(sub):
            raise RuleError(f"rip target {n} out of range ({len(sub)})")
        return ins[sub[c]][5] + sel.get("adjust", 0)
    if t == "signature":
        hits = signature_matches(b, sel["pattern"], start, end)
        k = sel["occurrence"]
        if k >= len(hits):
            raise RuleError(f"signature matches {len(hits)}x, wanted #{k}")
        return hits[k]
    raise RuleError(f"unknown selector {t}")


def describe(rule):
    s, sel = rule["string"], rule["select"]
    text = s["text"] if len(s["text"]) <= 48 else s["text"][:48] + "..."
    st = f'"{text}" ({"정확" if s["match"] == "exact" else "접두"}, 참조 {s["xrefs"]}개 중 {s["xref_index"] + 1}번째)'
    t = sel["type"]
    if t == "function_start":
        tail = "함수 시작"
    elif t in ("call", "call_return"):
        where = "함수 시작부터" if sel["anchor"] == "start" else ("참조 뒤" if sel["n"] > 0 else "참조 앞")
        n = sel["n"] + 1 if sel["anchor"] == "start" else abs(sel["n"])
        tail = f"{where} {n}번째 call" + (" 복귀 주소" if t == "call_return" else "")
    elif t == "rip_target":
        where = "함수 시작부터" if sel["anchor"] == "start" else ("참조 뒤" if sel["n"] > 0 else "참조 앞")
        n = sel["n"] + 1 if sel["anchor"] == "start" else abs(sel["n"])
        tail = f"{where} {n}번째 {sel['filter']} RIP 목표" + (f" {sel['adjust']:+#x}" if sel.get("adjust") else "")
    else:
        tail = f"바이트 서명 {sel['occurrence'] + 1}번째 일치"
    return f"{st} → 참조 명령 → .pdata 함수 → {tail}"


# --- generating rules ---

def strings_in(b, rows):
    """Distinct (text, rva) of strings referenced by instruction rows, in order."""
    seen, out = set(), []
    for r in rows:
        t = r[5]
        if t is not None and t not in seen:
            seen.add(t)
            s = b.string_at(t)
            if s is not None:
                out.append((s, t))
    return out


def candidates(b, A, f, role_target=None):
    """Rules for pinned address A (code) or role_target within function f; yields (score, rule)."""
    start, end, primary = f
    if end - start > FUNC_LIMIT:
        return
    rows = b.insns(start, end)
    by_addr = {r[0]: i for i, r in enumerate(rows)}
    for text, trva in strings_in(b, rows):
        needle, match = string_key(text)
        hits = b.occurrences(needle + (b"\0" if match == "exact" else b""))
        if hits != [trva]:
            continue
        xs = [x for x, _ in b.xrefs(trva)]
        if not 1 <= len(xs) <= 4:
            continue
        inside = [k for k, x in enumerate(xs) if b.func(x) and b.func(x)[0] == start]
        if not inside:
            continue
        k = inside[0]
        base_rule = {"string": {"text": needle.decode(), "match": match, "xrefs": len(xs), "xref_index": k},
                     "function": "pdata"}
        sel_list = []
        if role_target is None and A == start:
            sel_list.append((0, 0, {"type": "function_start"}))
        xi = by_addr.get(xs[k]) if primary and start <= xs[k] < end else None
        if xi is not None:
            if role_target is None:
                calls = [i for i, r in enumerate(rows) if r[1] == 5 and r[4] == 0xE8 and r[2] == "call"]
                before = sum(1 for i in calls if i < xi)
                for c, i in enumerate(calls):
                    for typ, addr in (("call", rows[i][0]), ("call_return", rows[i][0] + 5)):
                        if addr == A:
                            nx = c - before + 1 if c >= before else c - before
                            for anchor, n in (("xref", nx), ("start", c)):
                                sel_list.append((1, abs(nx) if anchor == "xref" else c + 1000,
                                                 {"type": typ, "n": n, "anchor": anchor}))
                if A != start and start <= A < end:
                    pat = signature_at(b, A)
                    if pat:
                        hits_ = signature_matches(b, pat, start, end)
                        if A in hits_:
                            sel_list.append((2, 0, {"type": "signature", "pattern": pat, "occurrence": hits_.index(A)}))
            else:
                target, adjust = role_target
                want = b.cls(target)
                sub = [i for i, r in enumerate(rows) if r[5] is not None and b.cls(r[5]) == want]
                before = sum(1 for i in sub if i < xi)
                for c, i in enumerate(sub):
                    if rows[i][5] == target:
                        nx = c - before + 1 if c >= before else c - before
                        for anchor, n in (("xref", nx), ("start", c)):
                            sel = {"type": "rip_target", "filter": want, "n": n, "anchor": anchor}
                            if adjust:
                                sel["adjust"] = adjust
                            sel_list.append((1, abs(nx) if anchor == "xref" else c + 1000, sel))
                        break
        for rank, dist, sel in sel_list:
            rule = dict(base_rule, select=sel)
            yield (rank, dist, len(xs), end - start), rule


def build_rule(b, A, label):
    sec = b.secname(A)
    best, why = None, None
    if sec == ".text":
        f = b.func(A)
        if not f:
            return None, "no .pdata function (leaf stub): no string can be tied to it"
        funcs = [(f, None)]
    else:
        near = b.xrefs(A, A + 0x40)
        if not near:
            return None, f"no RIP-relative reference to the {sec} address"
        funcs, seen = [], set()
        exact = sorted(near, key=lambda p: abs(p[1] - A))
        for x, t in exact:
            f = b.func(x)
            if f and f[0] not in seen:
                seen.add(f[0])
                funcs.append((f, (t, A - t)))
        funcs.sort(key=lambda p: p[0][1] - p[0][0])
        funcs = [p for p in funcs if p[0][1] - p[0][0] <= FUNC_LIMIT][:DATA_FUNCS]
        why = f"{len(near)} references, none of the first {len(funcs)} functions has a unique string it can be chosen by"
    tried = 0
    for f, role in funcs:
        for score, rule in candidates(b, A, f, role):
            tried += 1
            if best is None or score < best[0]:
                best = (score, rule)
    if best is None:
        return None, why or "the function holds no unique string (1-4 references) that reaches the address with a rule"
    return best[1], None


def cmd_generate(a):
    img = out_dir(a.old_version) / "image.bin"
    g = ImageGame(img)
    print(f"scan {img} ...", file=sys.stderr)
    g.load()
    b = Build(g, g.image.sections)
    rebase = json.loads(Path(a.from_rebase).read_text(encoding="utf-8"))
    old_game = json.loads((ROOT / "ot" / "schema" / a.old_version / "game.json").read_text(encoding="utf-8"))
    entries, ok, none, failed = [], 0, 0, 0
    for r in rebase["results"]:
        A = int(r["old"], 16)
        e = {"old": r["old"], "kind": r.get("kind"), "used_by": r.get("used_by", [])}
        rule, why = build_rule(b, A, e["used_by"])
        if rule:
            try:
                got = resolve(b, rule)
            except RuleError as ex:
                got, why = None, f"self-check error: {ex}"
            if got == A:
                e.update(rule=describe(rule), string=rule["string"], function="pdata", select=rule["select"],
                         old_check="ok")
                ok += 1
            else:
                if why is None:
                    why = f"self-check on {a.old_version} gave {got if got is None else hex(got)}"
                e.update(rule=None, reason=why)
                failed += 1
        else:
            e.update(rule=None, reason=why)
            none += 1
        entries.append(e)
    doc = {"old_version": a.old_version, "old_exe_sha256": old_game["exe_sha256"],
           "built_from": "research/ghidra/out/image.bin" if a.old_version == "1.61.1.1" else str(img.relative_to(ROOT)),
           "address_list": a.from_rebase,
           "note": "rule: string -> referencing instruction -> .pdata function -> selector; null = no rule (reason). "
                   "Every rule recovers its old RVA from the old image dump.",
           "rules": entries}
    RULES.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"rules {ok}, none {none}, dropped by self-check {failed}, total {len(entries)} -> {RULES.relative_to(ROOT)}")


# --- apply ---

def evidence(b, rva, old_pattern):
    f = b.func(rva) if b.secname(rva) == ".text" else None
    ev = {"rva": hex(rva), "section": b.secname(rva)}
    if f:
        ev["function"] = f"{f[0]:#x}-{f[1]:#x}"
        d = b.g.detail.disasm(bytes(b.mem[rva:rva + 0x20]), b.base + rva)
        ev["insns"] = [f"{i.mnemonic} {i.op_str}" for _, i in zip(range(3), d)]
        if old_pattern:
            ev["old_signature_matches_here"] = signature_matches(b, old_pattern, rva, rva + len(old_pattern.split())) == [rva]
    else:
        ev["referencing_instructions"] = len(b.xrefs(rva, rva + 1))
        ev["class"] = b.cls(rva)
    return ev


def cmd_apply(a):
    new_dir = ROOT / "ot" / "schema" / a.new_version
    want = json.loads((new_dir / "game.json").read_text(encoding="utf-8"))["exe_sha256"]
    if hashlib.sha256(GAME.read_bytes()).hexdigest() != want:
        raise SystemExit(f"{GAME} is not {a.new_version}")
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    rebase = {r["old"]: r for r in json.loads((new_dir / "rebase.json").read_text(encoding="utf-8"))["results"]}
    g = xe.Game()
    g.load()
    pe_sections = [(s.Name.rstrip(b"\0").decode(), s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData))
                   for s in g.pe.sections]
    b = Build(g, pe_sections)
    old = None
    if any(e.get("rule") for e in rules["rules"]):
        og = ImageGame(out_dir(rules["old_version"]) / "image.bin")   # signatures only; no scan needed
        old = types.SimpleNamespace(g=og, base=og.base, mem=og.mem, text=og.text)
    rows, tally = [], {"agree": 0, "disagree": 0, "no rule": 0, "not applicable": 0}
    for e in rules["rules"]:
        rb = rebase.get(e["old"])
        row = {"old": e["old"], "used_by": e.get("used_by"), "rebase_new": rb["new"] if rb else None,
               "rebase_status": rb["status"] if rb else None, "rebase_votes": rb.get("votes") if rb else None}
        if not e.get("rule"):
            row.update(verdict="no rule", reason=e.get("reason"))
        else:
            rule = {"string": e["string"], "select": e["select"]}
            row["rule"] = e["rule"]
            try:
                got = resolve(b, rule)
                row["xref_new"] = hex(got)
                if row["rebase_new"] and got == int(row["rebase_new"], 16):
                    row["verdict"] = "agree"
                else:
                    row["verdict"] = "disagree"
                    oa = int(e["old"], 16)
                    pat = signature_at(old, oa) if old and old.text[0] <= oa < old.text[0] + old.text[1] else None
                    row["evidence"] = {"xref_rule": evidence(b, got, pat),
                                       "rebase": evidence(b, int(row["rebase_new"], 16), pat) if row["rebase_new"] else None,
                                       "old_signature": pat}
            except RuleError as ex:
                row.update(verdict="not applicable", reason=str(ex))
        tally[row["verdict"]] += 1
        rows.append(row)
    with_rule = tally["agree"] + tally["disagree"] + tally["not applicable"]
    out = {"new_version": a.new_version, "exe_sha256": want, "rules_from": rules["old_version"],
           "summary": dict(tally, rules=with_rule, agree_rate_of_applied=(
               round(tally["agree"] / (tally["agree"] + tally["disagree"]), 4) if tally["agree"] + tally["disagree"] else None)),
           "results": rows}
    path = out_dir(a.new_version) / "xref_crosscheck.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"{'옛 RVA':>10}  {'규칙 결과':>10}  {'rebase.json':>11}  판정")
    label = {"agree": "일치", "disagree": "불일치", "no rule": "규칙 없음", "not applicable": "적용 불가"}
    for r in rows:
        extra = ""
        if r["verdict"] == "disagree":
            extra = f"  (rebase {r['rebase_status']} {r['rebase_votes']})"
        elif r["verdict"] in ("not applicable",):
            extra = f"  ({r['reason']})"
        print(f"{r['old']:>10}  {r.get('xref_new', '-'):>10}  {str(r['rebase_new']):>11}  {label[r['verdict']]}{extra}")
    s = out["summary"]
    print(f"\n규칙 {s['rules']}개: 일치 {s['agree']}, 불일치 {s['disagree']}, 적용 불가 {s['not applicable']} / 규칙 없음 {s['no rule']}"
          f" (적용된 것 중 일치율 {s['agree_rate_of_applied']})\n-> {path.relative_to(ROOT)}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--old-version", default="1.61.1.1")
    g.add_argument("--from-rebase", default=str(ROOT / "ot" / "schema" / "1.61.1.9" / "rebase.json"),
                   help="rebase.json whose 'old' addresses (and labels) get rules")
    p = sub.add_parser("apply")
    p.add_argument("--new-version", required=True)
    a = ap.parse_args()
    {"generate": cmd_generate, "apply": cmd_apply}[a.cmd](a)


if __name__ == "__main__":
    main()
