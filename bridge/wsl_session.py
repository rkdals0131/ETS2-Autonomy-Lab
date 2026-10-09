"""Compatibility entry point for existing WSL session commands."""
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().parents[1] / "apps" / "launcher" / "wsl_session.py"), run_name="__main__")
