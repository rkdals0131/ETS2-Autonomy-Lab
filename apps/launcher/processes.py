"""Release only POSIX process groups created by the ROS session."""
import os
import signal
import subprocess


def signal_group(process, sig):
    if process and process.poll() is None:
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            pass


def stop_group(process, timeout):
    if process is None:
        return
    signal_group(process, signal.SIGINT)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        signal_group(process, signal.SIGKILL)
        process.wait(timeout=2)
