"""Compatibility entry point; application ownership lives in apps/launcher."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps.launcher.launcher import *

if __name__ == "__main__":
    main()
