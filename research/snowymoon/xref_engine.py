"""Reusable string-xref engine for eurotrucks2.exe, modelled on the snowymoon proxy (dxgi+0x6B060..0x6BD80).

One linear Capstone pass over .text (SKIPDATA) builds the instruction symbols, first RIP-relative
targets, j*/c* immediates and function ranges that close at int3, ret or a far jmp; the lookups
(find, xrefs, branch_sites, range_of, decode, ...) are the DLL's selections. The scan takes about
45 s, so it is cached under research/snowymoon/work/ (git-ignored), keyed by exe size and mtime.

  py -3 research/snowymoon/xref_engine.py str "<text>" [--exact]   # string RVA, xrefs, range of each
  py -3 research/snowymoon/xref_engine.py branches <rva>           # j*/c* sites targeting an RVA
  py -3 research/snowymoon/xref_engine.py range <rva>              # range holding an RVA

As a library: g = Game(); g.load()  (scan or cache), then g.find / g.xrefs / g.range_of ...
Game path from ETS2_DIR or the Steam default. The exe is only read; no relocations are applied.
"""
import argparse
import os
import re
import sys
import time
from array import array
from bisect import bisect_right
from pathlib import Path

import capstone
import capstone.x86 as x86
import pefile

EXE = Path(os.environ.get("ETS2_DIR", r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")) / "bin" / "win_x64" / "eurotrucks2.exe"
CACHE = Path(__file__).resolve().parent / "work"
HEX = re.compile(r"^0x[0-9a-f]+$")
RIP = re.compile(r"\[rip(?: ([+-]) (0x[0-9a-f]+|\d+))?\]")
ARRAYS = {"sym_addr": "Q", "sym_id": "I", "sym_size": "B", "sym_b0": "B",
          "rip_idx": "I", "rip_target": "Q", "br_idx": "I", "br_target": "Q"}


class Game:
    def __init__(self, path=EXE):
        self.path = path
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        sections = {s.Name.rstrip(b"\0").decode(): s for s in self.pe.sections}
        text, rdata, data = sections[".text"], sections[".rdata"], sections[".data"]
        # dxgi+0x6ABE0: .text by raw size; the string search runs from .rdata to the end of .data raw.
        self.text = (text.VirtualAddress, text.SizeOfRawData)
        self.strings = (rdata.VirtualAddress, data.VirtualAddress + data.SizeOfRawData - rdata.VirtualAddress)
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.skipdata = True
        self.detail = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.detail.detail = True

    # --- cache ---

    def _cache_file(self):
        st = Path(self.path).stat()
        return CACHE / f"xref_scan_{st.st_size}_{int(st.st_mtime)}.npz"

    def load(self, use_cache=True):
        """Scan, or restore the vectors saved by an earlier scan of the same exe."""
        import numpy as np
        f = self._cache_file()
        if use_cache and f.exists():
            z = np.load(f)
            for name, code in ARRAYS.items():
                a = array(code)
                a.frombytes(z[name].tobytes())
                setattr(self, name, a)
            self.ranges = [tuple(int(v) for v in r) for r in z["ranges"]]
            self.range_starts = [r[0] for r in self.ranges]
            print(f"  cache {f.name}: {len(self.sym_addr):,} instructions, {len(self.ranges):,} ranges", file=sys.stderr)
            return
        self.scan()
        if use_cache:
            CACHE.mkdir(exist_ok=True)
            dtype = {"Q": "u8", "I": "u4", "B": "u1"}
            np.savez(f, ranges=np.array(self.ranges, dtype=np.uint64),
                     **{n: np.frombuffer(getattr(self, n), dtype=dtype[c]) for n, c in ARRAYS.items()})

    def scan(self):
        """dxgi+0x6B060: one linear pass, building symbols, RIP targets, branch targets and ranges."""
        va, size = self.text
        code = self.mem[va:va + size]
        self.sym_addr, self.sym_id, self.sym_size, self.sym_b0 = array("Q"), array("I"), array("B"), array("B")
        self.rip_idx, self.rip_target = array("I"), array("Q")
        self.br_idx, self.br_target = array("I"), array("Q")
        self.ranges = []                          # (start, end, first_idx, last_idx)
        start = first = max_target = 0
        t0 = time.time()
        for idx, ins in enumerate(self.cs.disasm(code, self.base + va)):
            addr, length, mnem, ops = ins.address, ins.size, ins.mnemonic, ins.op_str
            self.sym_addr.append(addr)
            self.sym_id.append(ins.id)
            self.sym_size.append(length)
            b0 = code[addr - self.base - va]
            self.sym_b0.append(b0)
            target = 0
            if mnem[0] in "jc" and HEX.match(ops):
                self.br_idx.append(idx)
                self.br_target.append(int(ops, 16))
                if mnem[0] == "j":
                    target = int(ops, 16)
            m = RIP.search(ops)
            if m:
                disp = int(m.group(2), 0) if m.group(2) else 0
                self.rip_idx.append(idx)
                self.rip_target.append(addr + length + (-disp if m.group(1) == "-" else disp))
            close = max_target < addr and (b0 == 0xCC or mnem.startswith("ret") or
                                           target and mnem.startswith("jmp") and abs(target - addr) >= 0x400)
            if close:
                if start:
                    if b0 == 0xCC:
                        self.ranges.append((start, start + ((addr - start) & 0xFFFFFFFF), first, idx - 1))
                    else:
                        self.ranges.append((start, start + ((length + addr - start) & 0xFFFFFFFF), first, idx))
                    start = 0
            else:
                if not start:
                    start, first = addr, idx
                if target and start < target and max_target < target and target < addr + 0x400:
                    max_target = target
            if idx % 2_000_000 == 0 and idx:
                print(f"  {idx:,} instructions, {addr - self.base:#x}, {time.time() - t0:.0f} s", file=sys.stderr)
        self.range_starts = [r[0] for r in self.ranges]
        print(f"  scan done: {len(self.sym_addr):,} instructions, {len(self.ranges):,} ranges, "
              f"{time.time() - t0:.0f} s", file=sys.stderr)

    # --- selections of dxgi+0x6BD80 ---

    def find(self, text, exact):
        """dxgi+0x6AFB0: first occurrence only; exact includes the terminating NUL."""
        va, size = self.strings
        needle = text.encode() + (b"\0" if exact else b"")
        hit = self.mem.find(needle, va, va + size)
        return self.base + hit if hit >= 0 else 0

    def xrefs(self, target):
        """dxgi+0x6B750: symbol addresses whose first RIP-relative operand resolves to target."""
        return [self.sym_addr[i] for i, t in zip(self.rip_idx, self.rip_target) if t == target]

    def branch_sites(self, target):
        """dxgi+0x6B810: symbol addresses of j*/c* whose immediate is target, in scan order."""
        return [self.sym_addr[i] for i, t in zip(self.br_idx, self.br_target) if t == target]

    def range_of(self, addr):
        """dxgi+0x6B6E0: first range with start <= addr < end."""
        for r in self.ranges[:bisect_right(self.range_starts, addr)]:
            if r[0] <= addr < r[1]:
                return r
        return None

    def decode(self, addr):
        off = addr - self.base
        return next(self.detail.disasm(self.mem[off:off + 0x50], addr, 1), None)

    def call_target(self, addr):
        """dxgi+0x6BB60: immediate of operand 0."""
        ins = self.decode(addr)
        return ins.operands[0].imm if ins and ins.operands and ins.operands[0].type == x86.X86_OP_IMM else 0

    def rip_of(self, addr):
        """dxgi+0x6BAB0: first RIP-relative memory operand, as address + size + disp."""
        ins = self.decode(addr)
        if ins and any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in ins.operands):
            return ins.address + ins.size + ins.disp
        return 0

    def imms(self, addr):
        """dxgi+0x6B8D0: low dwords of immediates from operand 1 on whose size is at most 4."""
        ins = self.decode(addr)
        if not ins:
            return []
        return [op.imm & 0xFFFFFFFF for op in ins.operands[1:] if op.type == x86.X86_OP_IMM and op.size <= 4]

    def is_e8_call(self, i):
        return self.sym_size[i] == 5 and self.sym_id[i] == x86.X86_INS_CALL and self.sym_b0[i] == 0xE8


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--no-cache", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("str"); s.add_argument("text"); s.add_argument("--exact", action="store_true")
    for name in ("branches", "range"):
        sub.add_parser(name).add_argument("rva")
    a = ap.parse_args()
    g = Game()
    g.load(not a.no_cache)
    rva = lambda v: f"{v - g.base:#x}" if v else "-"
    if a.cmd == "str":
        hit = g.find(a.text, a.exact)
        print(f"string {rva(hit)}")
        for x in g.xrefs(hit):
            r = g.range_of(x)
            print(f"  xref {rva(x)}  range {rva(r[0])}-{rva(r[1])}  (+{x - r[0]:#x})" if r else f"  xref {rva(x)}  no range")
    elif a.cmd == "branches":
        print(" ".join(rva(x) for x in g.branch_sites(int(a.rva, 16) + g.base)))
    else:
        r = g.range_of(int(a.rva, 16) + g.base)
        print(f"{rva(r[0])}-{rva(r[1])}" if r else "no range")


if __name__ == "__main__":
    main()
