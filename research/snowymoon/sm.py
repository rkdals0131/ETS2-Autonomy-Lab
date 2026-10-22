"""Static helpers for the snowymoon dxgi.dll copy: map RVAs, list .pdata functions,
find RIP-relative references and disassemble. Never loads or runs the DLL.

  py -3 research/snowymoon/sm.py dis <rva> [count]       # disassemble from an RVA
  py -3 research/snowymoon/sm.py func <rva>              # disassemble the .pdata function holding an RVA
  py -3 research/snowymoon/sm.py xref <rva> [...]        # rel32 / RIP-relative references in .text
  py -3 research/snowymoon/sm.py str <regex>             # ASCII strings and their code references
"""
import bisect
import re
import struct
import sys
from functools import cached_property
from pathlib import Path

import capstone
import pefile

COPY = Path(__file__).resolve().parent / "work" / "snowymoon_dxgi.bin"


class Image:
    def __init__(self, path=COPY):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b"\0").decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in self.pe.sections}
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.detail = True

    def section(self, rva):
        for name, (va, size) in self.sections.items():
            if va <= rva < va + size:
                return name
        return None

    @cached_property
    def functions(self):
        """(begin, end) of every .pdata entry, sorted; chained unwind entries are kept as their own rows."""
        va, size = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].VirtualAddress, self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size
        rows = [struct.unpack_from("<III", self.mem, va + i)[:2] for i in range(0, size, 12)]
        return sorted(rows)

    def function(self, rva):
        starts = [b for b, _ in self.functions]
        i = bisect.bisect_right(starts, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return None

    @cached_property
    def imports(self):
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
        return {imp.address - self.base: f"{entry.dll.decode()}!{(imp.name or b'#%d' % imp.ordinal).decode()}"
                for entry in self.pe.DIRECTORY_ENTRY_IMPORT for imp in entry.imports}

    def disasm(self, rva, size):
        return list(self.cs.disasm(self.mem[rva:rva + size], self.base + rva))

    def annotate(self, ins):
        """Instruction text with RIP-relative and branch targets turned into RVAs, imports and strings."""
        text = f"{ins.address - self.base:#08x}  {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}"
        targets = []
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                targets.append(ins.address + ins.size + op.mem.disp - self.base)
            elif op.type == capstone.x86.X86_OP_IMM and ins.group(capstone.CS_GRP_JUMP) | ins.group(capstone.CS_GRP_CALL):
                targets.append(op.imm - self.base)
        notes = []
        for t in targets:
            if t in self.imports:
                notes.append(self.imports[t])
            else:
                note = f"rva {t:#x} {self.section(t) or '?'}"
                s = self.cstring(t)
                if s:
                    note += f" {s!r}"
                notes.append(note)
        return text + (("    ; " + "; ".join(notes)) if notes else "")

    def cstring(self, rva):
        if not (0 <= rva < len(self.mem)) or self.section(rva) not in (".rdata", ".data"):
            return None
        m = re.match(rb"[\x20-\x7e]{4,}", self.mem[rva:rva + 200])
        if m and self.mem[rva + len(m.group())] == 0:
            return m.group().decode()
        m = re.match(rb"(?:[\x20-\x7e]\x00){4,}", self.mem[rva:rva + 400])
        return m.group().decode("utf-16le") if m else None

    def xrefs(self, targets, section=".text"):
        """Sites whose rel32 resolves to one of the targets, assuming the displacement ends the
        instruction or is followed by a 1- or 4-byte immediate. Candidates are confirmed by decoding."""
        va, size = self.sections[section]
        data = self.mem[va:va + size]
        targets = set(targets)
        found = []
        for i in range(size - 4):
            disp = struct.unpack_from("<i", data, i)[0]
            for imm in (0, 1, 4):
                t = va + i + 4 + imm + disp
                if t in targets:
                    found.append((va + i, t, imm))
        confirmed = []
        for pos, t, imm in found:
            for back in range(1, 12):
                start = pos - back
                ins = next(iter(self.cs.disasm(self.mem[start:start + 16], self.base + start)), None)
                if ins and ins.size == back + 4 + imm and self.annotate(ins).find(f"rva {t:#x}") >= 0 or \
                        ins and ins.size == back + 4 + imm and t in self.imports and self.imports[t] in self.annotate(ins):
                    confirmed.append((start, t, ins))
                    break
        return confirmed


def main():
    img = Image()
    command, *args = sys.argv[1:]
    if command == "dis":
        rva, count = int(args[0], 16), int(args[1]) if len(args) > 1 else 40
        for ins in img.disasm(rva, count * 15)[:count]:
            print(img.annotate(ins))
    elif command == "func":
        rva = int(args[0], 16)
        f = img.function(rva)
        print(f"; function {f[0]:#x}-{f[1]:#x}" if f else "; no .pdata entry")
        begin, end = f if f else (rva, rva + 0x200)
        for ins in img.disasm(begin, end - begin):
            print(img.annotate(ins))
    elif command == "xref":
        for start, t, ins in img.xrefs([int(a, 16) for a in args]):
            f = img.function(start)
            print(f"{t:#x} <- {start:#x} in {f[0] if f else 0:#x}: {ins.mnemonic} {ins.op_str}")
    elif command == "str":
        pattern = re.compile(args[0].encode())
        hits = [(m.start(), m.group()) for m in re.finditer(rb"[\x20-\x7e]{4,}\x00", img.mem) if pattern.search(m.group())]
        for rva, s in hits:
            print(f"{rva:#x} {s[:-1].decode()!r}")
            for start, _, ins in img.xrefs([rva]):
                f = img.function(start)
                print(f"    <- {start:#x} in {f[0] if f else 0:#x}")


if __name__ == "__main__":
    main()
