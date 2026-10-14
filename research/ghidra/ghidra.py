"""Persistent Ghidra project of the pinned game executable, queried headless.

  py -3 research/ghidra/ghidra.py import            # once per game build; full auto-analysis
  py -3 research/ghidra/ghidra.py export            # out/eurotrucks2.functions.csv (ot.cmd hotspots names)
  py -3 research/ghidra/ghidra.py decompile 2B1A30 538A4E ...   # out/decomp/<entry>.c
  py -3 research/ghidra/ghidra.py refs 304B2E8 167F30          # every reference, out/refs.csv

Queries open the analyzed project read-only, so they never change it. The
executable is checked against ot/schema/<version>/game.json before import.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "research" / "ghidra"
# The game directory comes from ETS2_DIR, as in research/inspect_binary.py.
GAME = Path(os.environ.get("ETS2_DIR", r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")) / "bin" / "win_x64" / "eurotrucks2.exe"
VERSION = "1.61.1.1"
PROJECT = f"eurotrucks2_{VERSION}"


def headless():
    def version(path):  # ghidra_12.1.4_PUBLIC sorts after ghidra_9.2_PUBLIC
        return tuple(int(part) for part in re.findall(r"\d+", path.name.split("_")[1]))
    found = sorted((ROOT / "research" / "tools").glob("ghidra_*_PUBLIC"), key=version)
    if not found:
        raise SystemExit("Ghidra not found in research/tools (see research/README.md)")
    return found[-1] / "support" / "analyzeHeadless.bat"


def run(arguments, log):
    (HERE / "logs").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GHIDRA_HEADLESS_MAXMEM=os.environ.get("GHIDRA_HEADLESS_MAXMEM", "14G"))
    command = [str(headless()), str(HERE / "projects"), PROJECT, *arguments, "-log", str(HERE / "logs" / log)]
    result = subprocess.run(command, env=env, capture_output=True, text=True, errors="replace")
    for line in result.stdout.splitlines():
        if "(GhidraScript)" in line or "ERROR" in line or "SCRIPT ERROR" in line:
            print(line.strip())
    if result.returncode:
        print(result.stdout[-4000:], result.stderr[-2000:], sep="\n", file=sys.stderr)
        raise SystemExit(result.returncode)


def query(script, *arguments):
    run(["-process", GAME.name, "-noanalysis", "-readOnly", "-scriptPath", str(HERE / "scripts"),
         "-postScript", script, *arguments], f"{Path(script).stem}.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("import")
    sub.add_parser("export")
    decompile = sub.add_parser("decompile")
    decompile.add_argument("rvas", nargs="+", help="Image RVAs in hex; each is decompiled as its containing function")
    refs = sub.add_parser("refs", help="out/refs.csv: every reference to the given RVAs")
    refs.add_argument("rvas", nargs="+")
    args = parser.parse_args()
    if args.command == "import":
        expected = json.loads((ROOT / "ot" / "schema" / VERSION / "game.json").read_text(encoding="utf-8"))["exe_sha256"]
        actual = hashlib.sha256(GAME.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f"{GAME} is not {VERSION} (sha256 {actual})")
        (HERE / "projects").mkdir(parents=True, exist_ok=True)
        run(["-import", str(GAME), "-max-cpu", str(max(1, (os.cpu_count() or 2) - 2))], "import.log")
    elif args.command == "export":
        query("ExportFunctions.java", str(HERE / "out" / "eurotrucks2.functions.csv"))
    elif args.command == "refs":
        query("ListReferences.java", str(HERE / "out" / "refs.csv"), *args.rvas)
        print((HERE / "out" / "refs.csv").read_text(encoding="utf-8"), end="")
    else:
        query("DecompileFunctions.java", str(HERE / "out" / "decomp"), *args.rvas)


if __name__ == "__main__":
    main()
