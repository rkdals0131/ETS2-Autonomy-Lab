"""Read an ETS2 PE file and compare the pinned ETS2LA Windows byte patterns.

This never opens a process or executes the game. A match is a static location,
not proof that the pointed-to runtime object or its layout is correct.
"""
import csv
import json
import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parent
GAME = pathlib.Path(r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")
exe = GAME / "bin/win_x64/eurotrucks2.exe"
data = exe.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
machine, nsects, timestamp, _, _, optional_size, flags = struct.unpack_from("<HHIIIHH", data, pe + 4)
optional = pe + 24
image_base = struct.unpack_from("<Q", data, optional + 24)[0]
sections = []
for i in range(nsects):
    off = optional + optional_size + i * 40
    name, virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<8sIIII", data, off)
    sections.append(dict(name=name.rstrip(b"\0").decode(), rva=rva, virtual_size=virtual_size,
                         raw_size=raw_size, raw_offset=raw_offset))

def raw(rva):
    for sec in sections:
        if sec['rva'] <= rva < sec['rva'] + sec['raw_size']:
            return sec['raw_offset'] + rva - sec['rva']
    raise ValueError(f"RVA outside file-backed sections: {rva:x}")

imports = []
import_rva = struct.unpack_from('<I', data, optional + 120)[0]
off = raw(import_rva)
while any(data[off:off+20]):
    name_rva = struct.unpack_from('<I', data, off+12)[0]
    start = raw(name_rva)
    imports.append(data[start:data.index(b'\0', start)].decode())
    off += 20

text = next(s for s in sections if s['name'] == '.text')
code = data[text['raw_offset']:text['raw_offset']+text['raw_size']]
source = ROOT / 'sources/ETS2LA_plugin-3b01d90b5be2/src/patterns.win32.hpp'
namespace = []
rows = []
for lineno, line in enumerate(source.read_text().splitlines(), 1):
    ns = re.search(r'namespace ([\w:]+)', line)
    if ns:
        namespace.append(ns[1])
    if line.strip() == '}':
        namespace.pop()
    m = re.search(r'auto (\w*pattern)\s*=\s*"([0-9a-fA-F? ]+)"', line)
    if not m:
        continue
    pattern = b''.join(b'.' if '?' in t else re.escape(bytes([int(t, 16)])) for t in m[2].split())
    matches = [x.start() for x in re.finditer(pattern, code, re.DOTALL)]
    rows.append(dict(symbol='::'.join(namespace + [m[1]]), source_line=lineno, pattern=m[2],
                     match_count=len(matches),
                     file_offsets=';'.join(hex(text['raw_offset']+x) for x in matches),
                     rvas=';'.join(hex(text['rva']+x) for x in matches)))
findings = ROOT / 'findings'
with (findings/'ets2la_pattern_matches.csv').open('w', newline='', encoding='utf-8-sig') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)

strings = []
terms = re.compile(rb'mirror|parking.camera|camera_manager|render_target|r_device|r_deferred|r_mirror|scs_telemetry|d3d11|d3d12', re.I)
for m in re.finditer(rb'[\x20-\x7e]{5,}', data):
    if terms.search(m[0]):
        strings.append({'file_offset':hex(m.start()), 'text':m[0].decode()})
with (findings/'binary_camera_render_strings.csv').open('w', newline='', encoding='utf-8-sig') as f:
    w=csv.DictWriter(f,fieldnames=['file_offset','text']);w.writeheader();w.writerows(strings)
summary = dict(executable=str(exe), machine=hex(machine), preferred_image_base=hex(image_base),
               sections=sections, imports=imports, patterns=rows, relevant_string_count=len(strings))
(findings/'binary_summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps({'machine':hex(machine),'imports':imports,'patterns':[{k:r[k] for k in ['symbol','match_count','rvas']} for r in rows], 'strings':len(strings)},indent=2))
