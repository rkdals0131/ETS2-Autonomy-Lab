"""Map snowymoon_logs.txt breadcrumbs "(file:line)" to the call sites of the log function.

The proxy logs only (file id, line) pairs through one function (0x83200 in the analyzed build:
ecx = file id, edx = line, appended to snowymoon_logs.txt under a lock). Every call site in
.text is found by decoding each .pdata function linearly and tracking the last constants
moved into ecx/edx.

  py -3 research/snowymoon/log_sites.py [<snowymoon_logs.txt>]
"""
import re
import struct
import sys
from pathlib import Path

import capstone

from sm import Image

LOG_FUNCTION = 0x83200
DEFAULT_LOG = Path(r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\bin\win_x64\snowymoon_logs.txt")


def sites(img):
    va, size = img.sections[".text"]
    data = img.mem[va:va + size]
    callers = set()
    for i in range(size - 5):
        if data[i] == 0xE8 and va + i + 5 + struct.unpack_from("<i", data, i + 1)[0] == LOG_FUNCTION:
            f = img.function(va + i)
            if f:
                callers.add(f)
    found = {}
    for begin, end in sorted(callers):
        regs = {}
        for ins in img.disasm(begin, end - begin):
            rva = ins.address - img.base
            ops = ins.operands
            if ins.mnemonic in ("mov", "lea") and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG:
                reg = ins.reg_name(ops[0].reg)
                if ops[1].type == capstone.x86.X86_OP_IMM:
                    regs[reg[-2:]] = ops[1].imm
                elif ops[1].type == capstone.x86.X86_OP_MEM and ins.mnemonic == "lea" and ops[1].mem.index == 0 and ops[1].mem.base:
                    # lea ecx, [rdx - 6] after mov edx, 0x4f; or lea ecx, [rdi + 0x46] with rdi zeroed earlier
                    base = ins.reg_name(ops[1].mem.base)[-2:]
                    if base in regs:
                        regs[reg[-2:]] = regs[base] + ops[1].mem.disp
                    elif 0 < ops[1].mem.disp < 0x100:
                        regs[reg[-2:]] = ops[1].mem.disp
                    else:
                        regs.pop(reg[-2:], None)
                else:
                    regs.pop(reg[-2:], None)
            elif ins.mnemonic == "xor" and len(ops) == 2 and ops[0].type == ops[1].type == capstone.x86.X86_OP_REG and ops[0].reg == ops[1].reg:
                regs[ins.reg_name(ops[0].reg)[-2:]] = 0
            if ins.mnemonic == "call" and ops and ops[0].type == capstone.x86.X86_OP_IMM and ops[0].imm - img.base == LOG_FUNCTION:
                key = (regs.get("cx"), regs.get("dx"))
                found.setdefault(key, []).append((rva, begin))
            elif ins.mnemonic == "call":
                regs = {}
    return found


def main():
    img = Image()
    table = sites(img)
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOG
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"\((\d+):(\d+)\)", line.strip())
        if not m:
            print(f"{line.strip():<24} (not a breadcrumb)")
            continue
        key = (int(m.group(1)), int(m.group(2)))
        hits = table.get(key, [])
        where = ", ".join(f"{rva:#x} in {fn:#x}" for rva, fn in hits) or "not found"
        print(f"{line.strip():<12} {where}")
    print(f"\n{sum(map(len, table.values()))} call sites, file ids {sorted({k[0] for k in table if k[0] is not None})}")


if __name__ == "__main__":
    main()
