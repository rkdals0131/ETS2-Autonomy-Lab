"""D3D11 device and context call sites of eurotrucks2.exe, and which of them the render pass
recording closure reaches (research/snowymoon/multicore.md). Reads the exe only; never runs it.

  py -3 research/snowymoon/d3d_sites.py                 # every hooked-slot site, reachable ones marked
  py -3 research/snowymoon/d3d_sites.py --all-slots     # every site, hooked by snowymoon or not
  py -3 research/snowymoon/d3d_sites.py --path <rva>    # one call chain from a closure root to a function

A site is "mov R, [X + 0x363FD18]" (ID3D11Device*), "[X + 0x363FCF0]" (immediate context) or
"[X + 0x363FCF8]" (ID3D11DeviceContext1*), then "mov V, [R]" and "call [V + d]" a few instructions
later (48 instructions); the slot is d / 8. The closure is a breadth-first walk over direct calls from the recording
roots below, plus the engine device vtable slots that the walk meets as "call [reg + slot]" (an
over-approximation: any object with a method at that offset counts). capstone is in the user
site-packages, so run it without -I, as sm.py.
"""
import argparse
import bisect
import os
import struct
from collections import deque
from pathlib import Path

import capstone
import capstone.x86 as x86
import pefile

EXE = Path(os.environ.get("ETS2_DIR", r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")) / "bin" / "win_x64" / "eurotrucks2.exe"
FIELDS = {0x363FD18: "device", 0x363FCF0: "context", 0x363FCF8: "context1"}
# snowymoon slots (README section 4.3): device 0x17B70, context 0x17C90.
HOOKED = {"device": {3, 5, 7, 8, 9, 10, 11, 12, 15, 21},
          "context": {8, 9, 11, 12, 13, 14, 15, 17, 20, 21, 33, 35, 36, 44, 54, 69},
          "context1": {8, 9, 11, 12, 13, 14, 15, 17, 20, 21, 33, 35, 36, 44, 54, 69, 119, 123}}
DEVICE = {3: "CreateBuffer", 4: "CreateTexture1D", 5: "CreateTexture2D", 6: "CreateTexture3D",
          7: "CreateShaderResourceView", 8: "CreateUnorderedAccessView", 9: "CreateRenderTargetView",
          10: "CreateDepthStencilView", 11: "CreateInputLayout", 12: "CreateVertexShader",
          15: "CreatePixelShader", 20: "CreateBlendState", 21: "CreateDepthStencilState",
          22: "CreateRasterizerState", 23: "CreateSamplerState", 24: "CreateQuery"}
CONTEXT = {7: "VSSetConstantBuffers", 8: "PSSetShaderResources", 9: "PSSetShader", 10: "PSSetSamplers",
           11: "VSSetShader", 12: "DrawIndexed", 13: "Draw", 14: "Map", 15: "Unmap",
           16: "PSSetConstantBuffers", 17: "IASetInputLayout", 20: "DrawIndexedInstanced",
           21: "DrawInstanced", 33: "OMSetRenderTargets", 35: "OMSetBlendState",
           36: "OMSetDepthStencilState", 44: "RSSetViewports", 46: "CopySubresourceRegion",
           47: "CopyResource", 48: "UpdateSubresource", 54: "GenerateMips", 69: "CSSetShader",
           115: "CopySubresourceRegion1", 116: "UpdateSubresource1", 119: "VSSetConstantBuffers1",
           123: "PSSetConstantBuffers1"}
# Recording closure roots, 1.61.1.9 (docs/28 section 11; 1.61.1.1 + 0x1E30 above 0x1400000).
ROOTS = {
    0x1475C90: "scene pass callback", 0x151D9C0: "pass callback 0x151BB90", 0x16109B0: "pass callback 0x160EB80",
    0x166F120: "pass callback 0x166D2F0", 0x1467090: "pass callback 0x1465260", 0x151DDB0: "blit pass callback",
    0x2E6060: "draw batch", 0x2E50B0: "bundle", 0x2E6B60: "uniform segment switch", 0x1E0650: "frame allocator",
    0x160E7C0: "queue set prepare", 0x2DECF0: "draw item emit", 0x32E540: "resource ready",
    0x2B9520: "SRV cache", 0x2BEAE0: "sampler cache", 0x2AE6B0: "frame view registry",
    0x2E8100: "buffer record acquire", 0x2E84B0: "buffer record release",
}
# Engine device vtable slots followed by the walk: the 0x398-byte dx11_device_t vtable from 0x100 on
# ([0x120] maintenance, [0x128] buffer create, [0x148] flush, [0x158] upload, [0x278] binding ...).
# Lower offsets also name COM and common virtuals (Release is [0x10]) and would link everything.
ENGINE_SLOTS = tuple(range(0x100, 0x398, 8))


class Exe:
    def __init__(self, path=EXE):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b"\0").decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in self.pe.sections}
        runtime = sorted({(e.struct.BeginAddress, e.struct.EndAddress) for e in self.pe.DIRECTORY_ENTRY_EXCEPTION})
        self.starts = [b for b, _ in runtime]
        self.ranges = runtime
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.cs.detail = True

    def function_of(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.ranges[i][0] <= rva < self.ranges[i][1]:
            return self.ranges[i][0]
        return None

    def instructions(self, begin, end):
        return list(self.cs.disasm(self.mem[begin:end], self.base + begin))

    def engine_vtable(self):
        """The dx11_device_t vtable: the .rdata table whose [0x108] is cmd_buffer_compile 0x2B1AA0."""
        va, size = self.sections[".rdata"]
        want = struct.pack("<Q", self.base + 0x2B1AA0)
        at = self.mem.find(want, va, va + size)
        while at != -1:
            table = at - 0x108
            if struct.unpack_from("<Q", self.mem, table + 0x110)[0] == self.base + 0x2B3AF0:
                return table
            at = self.mem.find(want, at + 8, va + size)
        raise SystemExit("engine device vtable not found")


def scan(exe):
    """Per function: direct call targets, engine-slot indirect calls, D3D sites."""
    calls, slots, sites = {}, {}, []
    for begin, end in exe.ranges:
        ins = exe.instructions(begin, end)
        out, ind = set(), set()
        for k, i in enumerate(ins):
            branch = i.group(capstone.CS_GRP_JUMP) or i.id == x86.X86_INS_CALL
            if branch and i.operands and i.operands[0].type == x86.X86_OP_IMM:
                # Calls, and jumps out of this .pdata range: a split function continues in another entry.
                target = i.operands[0].imm - exe.base
                if i.id == x86.X86_INS_CALL or not begin <= target < end:
                    out.add(target)
            elif branch and i.operands and i.operands[0].type == x86.X86_OP_MEM:
                m = i.operands[0].mem
                if m.base not in (0, x86.X86_REG_RIP) and m.index == 0 and m.disp in ENGINE_SLOTS:
                    ind.add(m.disp)
            if i.id != x86.X86_INS_MOV or len(i.operands) != 2 or i.operands[1].type != x86.X86_OP_MEM:
                continue
            field = FIELDS.get(i.operands[1].mem.disp)
            if not field or i.operands[0].type != x86.X86_OP_REG:
                continue
            obj, vt = {i.operands[0].reg}, set()
            # Decode on from the load itself: split .pdata entries can cut the sequence.
            after = i.address - exe.base + i.size
            for j in exe.cs.disasm(exe.mem[after:after + 0x200], exe.base + after, 48):
                ops = j.operands
                if j.id == x86.X86_INS_MOV and len(ops) == 2 and ops[0].type == x86.X86_OP_REG:
                    dst = ops[0].reg
                    if ops[1].type == x86.X86_OP_REG and ops[1].reg in obj:
                        obj.add(dst); vt.discard(dst); continue
                    if ops[1].type == x86.X86_OP_MEM and ops[1].mem.base in obj and ops[1].mem.disp == 0 and ops[1].mem.index == 0:
                        vt.add(dst); obj.discard(dst); continue
                    obj.discard(dst); vt.discard(dst)
                if j.id == x86.X86_INS_CALL and ops and ops[0].type == x86.X86_OP_MEM and ops[0].mem.base in vt:
                    sites.append((j.address - exe.base, begin, field, ops[0].mem.disp // 8))
                    break
        if ins and ins[-1].id not in (x86.X86_INS_RET, x86.X86_INS_JMP, x86.X86_INS_INT3):
            out.add(end)  # falls through into the next .pdata entry (a split function)
        calls[begin], slots[begin] = out, ind
    return calls, slots, sites


def closure(exe, calls, slots):
    table = exe.engine_vtable()
    engine = {d: struct.unpack_from("<Q", exe.mem, table + d)[0] - exe.base for d in ENGINE_SLOTS}
    parent = {r: (None, None) for r in ROOTS}
    todo = deque(ROOTS)
    while todo:
        f = todo.popleft()
        nexts = [(exe.function_of(t), None) for t in sorted(calls.get(f, ()))]
        nexts += [(engine[d], d) for d in sorted(slots.get(f, ()))]
        for g, via in nexts:
            if g is not None and g not in parent:
                parent[g] = (f, via)
                todo.append(g)
    return parent, engine, table


def chain(parent, f):
    """f <- caller <- ... <- root; "<-[0x158]-" marks an engine vtable slot instead of a direct call."""
    out = f"{f:X}"
    while parent[f][0] is not None:
        f, via = parent[f]
        out += (f" <-[{via:#x}]- " if via is not None else " <- ") + f"{f:X}"
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--all-slots", action="store_true")
    parser.add_argument("--path", type=lambda s: int(s, 16))
    args = parser.parse_args()
    exe = Exe()
    missing = [f"{r:X}" for r in ROOTS if exe.function_of(r) != r]
    if missing:
        raise SystemExit(f"roots that are not .pdata function starts: {missing}")
    calls, slots, sites = scan(exe)
    parent, engine, table = closure(exe, calls, slots)
    if args.path is not None:
        f = exe.function_of(args.path)
        print(chain(parent, f) if f in parent else "not reached")
        return
    print(f"engine device vtable .rdata {table:X}: " + ", ".join(f"[{d:#x}]={engine[d]:X}" for d in (0x120, 0x128, 0x148, 0x158, 0x278)))
    print(f"closure: {len(parent)} functions from {len(ROOTS)} roots")
    print("site,function,interface,slot,method,hooked,reached,chain")
    for site, func, field, slot in sorted(sites, key=lambda s: (s[2], s[3], s[0])):
        hooked = slot in HOOKED[field]
        if not (hooked or args.all_slots):
            continue
        name = (DEVICE if field == "device" else CONTEXT).get(slot, "?")
        reached = func in parent
        print(f"{site:X},{func:X},{field},{slot},{name},{'yes' if hooked else 'no'},{'yes' if reached else 'no'},{chain(parent, func) if reached else ''}")


if __name__ == "__main__":
    main()
