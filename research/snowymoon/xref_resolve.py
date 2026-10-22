"""Replay the string-xref address lookup of the snowymoon proxy (dxgi+0x6BD80) on eurotrucks2.exe.

The scan and lookups live in xref_engine.py (Capstone 5 x86-64 with DETAIL and SKIPDATA as in
dxgi+0x6B060). This script applies the selections of 0x6BD80 for the three values that the DLL
file does not contain: DAT_c78010 (composer), DAT_c78060/68/70 (persistent_drawable call sites)
and DAT_c77ff8/c78000/c78008 (scene range and the call inside it).

  py -3 research/snowymoon/xref_resolve.py       # game exe from ETS2_DIR or the Steam default
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xref_engine import Game, x86  # noqa: E402,F401


class Resolver(Game):
    def composer(self):
        """DAT_c78010: xref with >= 0x300 into its range, next RIP target after it, first E8 call in that range."""
        hit = self.find("transform_mvp_matrix", exact=True)
        for x in self.xrefs(hit):
            r = self.range_of(x)
            if r and x - r[0] >= 0x300:
                break
        else:
            return None
        for i in range(r[2], r[3] + 1):
            if self.sym_addr[i] > x and (t := self.rip_of(self.sym_addr[i])):
                r2 = self.range_of(t)
                if not r2:
                    return None
                for j in range(r2[2], r2[3] + 1):
                    if self.is_e8_call(j):
                        return dict(xref=x, range=r, rip_target=t, callee_range=r2, call=self.sym_addr[j],
                                    composer=self.call_target(self.sym_addr[j]))
                return None
        return None

    def persistent_drawable(self, american=False):
        """DAT_c78068/60/70: branch sites to the range start of the first xref; [0], [1], [3] ([2] for ATS)."""
        hit = self.find("persistent_drawable", exact=True)
        x = self.xrefs(hit)[0]
        r = self.range_of(x)
        sites = self.branch_sites(r[0])
        return dict(xref=x, range=r, sites=sites,
                    c78068=sites[0], c78060=sites[1], c78070=sites[2 if american else 3])

    def scene(self):
        """DAT_c77ff8/c78008/c78000: last xref's range, last E8 call whose callee holds a lone 0xFFFF immediate."""
        hit = self.find("?element_at@?$array_t@Uqueue_data_t@render_queue_set_data_t@prism", exact=False)
        x = self.xrefs(hit)[-1]
        r = self.range_of(x)
        chosen = callee = None
        for i in range(r[2], r[3] + 1):
            if not self.is_e8_call(i):
                continue
            t = self.call_target(self.sym_addr[i])
            r2 = self.range_of(t)
            if r2 and any(self.imms(self.sym_addr[j]) == [0xFFFF] for j in range(r2[2], r2[3] + 1)):
                chosen, callee = self.sym_addr[i], t
        return dict(xref=x, range=r, call=chosen, callee=callee,
                    c77ff8=r[0], c78008=chosen - r[0] if chosen else None, c78000=r[1] - r[0])


def main():
    g = Resolver()
    rva = lambda v: f"{v - g.base:#x}" if v else "0"
    g.load()

    print("range check (first xref -> range start):")
    for text, exact in [("[dx11] Mapping buffer with unsupported usage flags!", False),
                        ("[cvar] Skipping store of cvars to %s - store is disabled.", False),
                        ("[dx11] Backbuffer acquired", False), ("<undefined>", True),
                        ("[rfx] Cannot tokenize semantics name '%s' in effect", False),
                        ("[dxgi] Present failed (0x%lx)", False), ("[cmd] '%s' - unknown command", False)]:
        hit = g.find(text, exact)
        rows = [(x, g.range_of(x)) for x in g.xrefs(hit)]
        print(f"  {text[:40]!r:44} string {rva(hit)}: " +
              ", ".join(f"xref {rva(x)} range {rva(r[0]) if r else '-'}" for x, r in rows))

    c = g.composer()
    print("composer DAT_c78010:", {k: (rva(v) if isinstance(v, int) else tuple(rva(e) for e in v[:2])) for k, v in c.items()} if c else None)
    p = g.persistent_drawable()
    print("persistent_drawable: xref", rva(p["xref"]), "range", rva(p["range"][0]), rva(p["range"][1]),
          "sites", [rva(s) for s in p["sites"]])
    print("  DAT_c78068", rva(p["c78068"]), "DAT_c78060", rva(p["c78060"]), "DAT_c78070", rva(p["c78070"]))
    s = g.scene()
    print("scene: xref", rva(s["xref"]), "range", rva(s["range"][0]), rva(s["range"][1]),
          "call", rva(s["call"]), "callee", rva(s["callee"]))
    print(f"  DAT_c77ff8 {rva(s['c77ff8'])} DAT_c78008 {s['c78008']:#x} DAT_c78000 {s['c78000']:#x}"
          if s["c78008"] is not None else "  no call matched")


if __name__ == "__main__":
    main()
