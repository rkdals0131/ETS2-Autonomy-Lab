"""Compatibility import for the launcher-owned recorder."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps.launcher.bag_recording import BagRecording, STATE_TOPICS
