"""Separate Ghidra project of the snowymoon dxgi.dll proxy, queried headless (read-only analysis).

  py -3 -I research/snowymoon/ghidra.py import                 # copy the DLL to work/, import, full auto-analysis
  py -3 -I research/snowymoon/ghidra.py decompile 1A890 ...    # ghidra/out/decomp/<entry>.c
  py -3 -I research/snowymoon/ghidra.py refs 9850E0 ...        # ghidra/out/refs.csv

It reuses the scripts of research/ghidra but never touches the eurotrucks2 projects.
The DLL is not loaded or run; the game folder copy is only read. The copy, the project
and the outputs stay out of Git (.gitignore).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / "research" / "snowymoon"
GHIDRA = HERE / "ghidra"
SCRIPTS = ROOT / "research" / "ghidra" / "scripts"
SOURCE = Path(os.environ.get("ETS2_DIR", r"D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2")) / "bin" / "win_x64" / "dxgi.dll.off"
COPY = HERE / "work" / "snowymoon_dxgi.bin"
SHA256 = "d819c16805a9a0ca17f34ef46cc445c5847f32308d10e69bf08f3c1b4779f4dc"
PROJECT = "snowymoon_dxgi"


def headless():
    found = sorted((ROOT / "research" / "tools").glob("ghidra_*_PUBLIC"))
    if not found:
        raise SystemExit("Ghidra not found in research/tools (see research/README.md)")
    return found[-1] / "support" / "analyzeHeadless.bat"


def run(arguments, log):
    (GHIDRA / "logs").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GHIDRA_HEADLESS_MAXMEM=os.environ.get("GHIDRA_HEADLESS_MAXMEM", "12G"))
    command = [str(headless()), str(GHIDRA / "projects"), PROJECT, *arguments, "-log", str(GHIDRA / "logs" / log)]
    result = subprocess.run(command, env=env, capture_output=True, text=True, errors="replace")
    for line in result.stdout.splitlines():
        if "(GhidraScript)" in line or "ERROR" in line:
            print(line.strip())
    if result.returncode:
        print(result.stdout[-4000:], result.stderr[-2000:], sep="\n", file=sys.stderr)
        raise SystemExit(result.returncode)


def query(script, *arguments):
    run(["-process", COPY.name, "-noanalysis", "-readOnly", "-scriptPath", str(SCRIPTS),
         "-postScript", script, *arguments], f"{Path(script).stem}.log")


def main():
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("import")
    decompile = sub.add_parser("decompile")
    decompile.add_argument("rvas", nargs="+")
    refs = sub.add_parser("refs")
    refs.add_argument("rvas", nargs="+")
    args = parser.parse_args()
    out = GHIDRA / "out"
    if args.command == "import":
        if not COPY.exists():
            COPY.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE, COPY)
        if hashlib.sha256(COPY.read_bytes()).hexdigest() != SHA256:
            raise SystemExit(f"{COPY} is not the analyzed build (sha256 {SHA256[:16]}...)")
        (GHIDRA / "projects").mkdir(parents=True, exist_ok=True)
        run(["-import", str(COPY), "-loader", "PeLoader", "-max-cpu", str(max(1, (os.cpu_count() or 2) - 2))], "import.log")
    elif args.command == "refs":
        query("ListReferences.java", str(out / "refs.csv"), *args.rvas)
        print((out / "refs.csv").read_text(encoding="utf-8"), end="")
    else:
        query("DecompileFunctions.java", str(out / "decomp"), *args.rvas)


if __name__ == "__main__":
    main()
