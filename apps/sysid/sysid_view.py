"""Progress viewer for sysid_drive.py: step list, step and overall progress, live truck state.

Usage: py -3 sysid_view.py <run_dir>/status.json  (sysid_drive.py --view opens it in a new window)
"""
import ctypes, json, os, sys, threading, time
from pathlib import Path
from rich.console import Group
from rich.live import Live
from rich.panel import Panel
from rich.progress_bar import ProgressBar
from rich.table import Table
from rich.text import Text


def load(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return None


def fmt(v, spec, none="-"):
    return none if v is None else format(v, spec)


def render(st):
    steps, idx, state = st["steps"], st["index"], st["state"]
    total = sum(s["seconds"] for s in steps) or 1
    done_s = sum(s["seconds"] for s in steps[:max(idx, 0)])
    cur = steps[idx] if 0 <= idx < len(steps) else None
    cur_frac = min(1.0, st["step_elapsed"] / cur["seconds"]) if cur else (1.0 if state == "done" else 0.0)
    overall = min(1.0, (done_s + (cur["seconds"] * cur_frac if cur else 0)) / total)

    table = Table(expand=True, show_edge=False, pad_edge=False)
    table.add_column("", width=2)
    table.add_column("group", style="dim", width=13)
    table.add_column("step")
    table.add_column("est", justify="right", width=6)
    lo = max(0, idx - 6)
    hi = min(len(steps), lo + 18)
    for i in range(lo, hi):
        s = steps[i]
        if i < idx or state == "done":
            mark, style = "ok", "green"
        elif i == idx:
            mark, style = (">>", "bold yellow") if state == "running" else ("!!", "bold red")
        else:
            mark, style = "", "white"
        table.add_row(mark, s["group"], Text(s["label"], style=style), f"{s['seconds']:.0f}s")
    if hi < len(steps):
        table.add_row("", "", Text(f"... {len(steps) - hi} more", style="dim"), "")

    live = st["live"]
    info = Table.grid(padding=(0, 2))
    info.add_row("speed", f"{fmt(live['speed_kmh'], '5.1f')} km/h", "gear", fmt(live["gear"], "d"),
                 "armed", "yes" if live["armed"] else "no")
    info.add_row("steer cmd", fmt(live["cmd_steer"], "+.3f"), "applied", fmt(live["eff_steer"], "+.3f"),
                 "throttle/brake", f"{live['throttle']:.2f} / {live['brake']:.2f}")
    info.add_row("to pad margin", f"{fmt(live['edge_m'], '.0f')} m", "phase", st["phase"], "commands", str(live["frames"]))

    color = {"running": "yellow", "done": "green", "stopped": "red"}.get(state, "cyan")
    head = Text.assemble((f" {state.upper()} ", f"bold black on {color}"), "  ", st["message"])
    bars = Table.grid(padding=(0, 1))
    bars.add_column(width=9)
    bars.add_column(ratio=1)
    bars.add_column(width=24)
    bars.add_row("step", ProgressBar(total=1, completed=cur_frac),
                 f"{cur_frac*100:3.0f}%  {cur['label'][:16] if cur else ''}")
    bars.add_row("overall", ProgressBar(total=1, completed=overall),
                 f"{overall*100:3.0f}%  {max(idx, 0)}/{len(steps)} steps")
    return Panel(Group(head, Text(""), bars, Text(""), table, Text(""), info),
                 title=f"OT sysid  {Path(st['run_dir']).name}", border_style=color)


# Seconds the final screen stays up before the window closes itself; a stop keeps it
# longer so the reason can be read (it is also in the run log).
LINGER = {"done": 5, "stopped": 30}
# A running status that stops changing for this long means the driver process is gone.
STALE_S = 20


def no_quick_edit():
    """A click into a console with QuickEdit starts a text selection that blocks every
    write, which froze the viewer before it could close itself."""
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.GetStdHandle(-10)  # STD_INPUT_HANDLE
    mode = ctypes.c_uint32()
    if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
        kernel32.SetConsoleMode(handle, (mode.value & ~0x0040) | 0x0080)  # QUICK_EDIT off, EXTENDED_FLAGS on


def watchdog(path):
    """Close the window from a separate thread, even if a console write is blocked: after
    the linger time once the run has ended, or when the status stops changing."""
    def run():
        start, ended = time.time(), None
        while True:
            st = load(path)
            if st:
                if st["state"] in LINGER:
                    ended = ended or time.time() + LINGER[st["state"]] + 1
                elif time.time() - st.get("updated", start) > STALE_S:
                    ended = ended or time.time() + LINGER["stopped"] + 1
            if ended and time.time() > ended:
                os._exit(0)
            time.sleep(0.5)
    threading.Thread(target=run, daemon=True).start()


def main():
    no_quick_edit()
    path = sys.argv[1]
    watchdog(path)
    st, start = None, time.time()
    with Live(Text("waiting for status.json ..."), refresh_per_second=8, screen=False) as live:
        while True:
            st = load(path) or st
            if st:
                live.update(render(st))
                if st["state"] in LINGER:
                    break
                if time.time() - st.get("updated", start) > STALE_S:
                    st["state"], st["message"] = "stopped", f"no status update for {STALE_S} s (driver exited?)"
                    live.update(render(st))
                    break
            time.sleep(0.12)
    end = time.monotonic() + LINGER[st["state"]]
    while (left := end - time.monotonic()) > 0:
        print(f"\rclosing in {left:3.0f} s (Ctrl+C closes now)", end="", flush=True)
        time.sleep(0.25)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
