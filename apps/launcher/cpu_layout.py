"""CPU placement that keeps the game's main thread on a physical core of its own.

ETS2 renders on one main thread, and every sensor view adds to that thread.
Measured 2026-10-12 on a Ryzen 7 3700X standstill scene with the Foxglove
viewer open: moving the viewer to the other L3 group gained 2.8 ms per frame,
and pinning the main thread with its SMT sibling left empty gained 5.9 ms in
total. Helpers (relay, Foxglove server, recorder, viewer) go to the other L3
group; the game keeps the first one. Everything is restored on stop.
"""
import ctypes as C
from ctypes import wintypes as W

K = C.WinDLL("kernel32", use_last_error=True)
K.OpenProcess.restype = K.OpenThread.restype = K.CreateToolhelp32Snapshot.restype = W.HANDLE
K.SetThreadAffinityMask.restype = C.c_size_t
K.SetThreadAffinityMask.argtypes = [W.HANDLE, C.c_size_t]
K.SetProcessAffinityMask.argtypes = [W.HANDLE, C.c_size_t]
K.GetProcessAffinityMask.argtypes = [W.HANDLE, C.POINTER(C.c_size_t), C.POINTER(C.c_size_t)]
K.GetCurrentProcess.restype = W.HANDLE
NORMAL, BELOW_NORMAL = 0x20, 0x4000
VIEWER = "Foxglove.exe"


def system_mask():
    process, system = C.c_size_t(), C.c_size_t()
    K.GetProcessAffinityMask(K.GetCurrentProcess(), C.byref(process), C.byref(system))
    return system.value


def topology():
    """(physical core masks, L3 group masks) of processor group 0."""
    length = W.DWORD(0)
    K.GetLogicalProcessorInformationEx(0xFFFF, None, C.byref(length))
    buffer = C.create_string_buffer(length.value)
    if not K.GetLogicalProcessorInformationEx(0xFFFF, buffer, C.byref(length)):
        raise C.WinError(C.get_last_error())
    cores, caches, offset = [], [], 0
    while offset < length.value:
        relation, size = C.cast(C.byref(buffer, offset), C.POINTER(W.DWORD * 2)).contents
        body = offset + 8
        if relation == 0:  # RelationProcessorCore: GROUP_AFFINITY after 24 bytes of header fields
            cores.append(C.c_size_t.from_buffer_copy(buffer.raw[body + 24:body + 32]).value)
        elif relation == 2 and buffer.raw[body] == 3:  # RelationCache, level 3
            caches.append(C.c_size_t.from_buffer_copy(buffer.raw[body + 32:body + 40]).value)
        offset += size
    return sorted(cores), sorted(set(caches))


class Plan:
    def __init__(self):
        cores, caches = topology()
        everything = system_mask()
        if len(caches) >= 2:
            game = next(c for c in caches if c & 1)
        else:
            game = 0
            for core in cores[:max(1, len(cores) // 2)]:
                game |= core
        game_cores = [c for c in cores if c & game == c]
        self.all = everything
        self.game = game
        self.helpers = everything & ~game or everything
        # Core 0 takes most interrupts; the next core holds the main thread alone.
        self.main_core = game_cores[1] if len(game_cores) > 1 else game_cores[0]
        self.main = self.main_core & -self.main_core
        self.game_rest = game & ~self.main_core or game

    def describe(self):
        return f"game {self.game:#x} (main {self.main:#x}), helpers {self.helpers:#x}"


def _process(pid, mask=None, priority=None):
    handle = K.OpenProcess(0x0200 | 0x0400, False, pid)
    if not handle:
        return False
    ok = True
    if mask is not None:
        ok &= bool(K.SetProcessAffinityMask(handle, mask))
    if priority is not None:
        ok &= bool(K.SetPriorityClass(handle, priority))
    K.CloseHandle(handle)
    return ok


class _Thread(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("thread", W.DWORD), ("owner", W.DWORD),
                ("base", W.LONG), ("delta", W.LONG), ("flags", W.DWORD)]


def _threads(pid):
    snapshot = K.CreateToolhelp32Snapshot(0x4, 0)
    entry, ids = _Thread(size=C.sizeof(_Thread)), []
    more = K.Thread32First(snapshot, C.byref(entry))
    while more:
        if entry.owner == pid:
            ids.append(entry.thread)
        more = K.Thread32Next(snapshot, C.byref(entry))
    K.CloseHandle(snapshot)
    return ids


def _thread(tid, mask):
    handle = K.OpenThread(0x0020 | 0x0040, False, tid)
    if not handle:
        return False
    ok = bool(K.SetThreadAffinityMask(handle, mask))
    K.CloseHandle(handle)
    return ok


class _Process(C.Structure):
    _fields_ = [("size", W.DWORD), ("usage", W.DWORD), ("pid", W.DWORD), ("heap", C.c_size_t),
                ("module", W.DWORD), ("threads", W.DWORD), ("parent", W.DWORD), ("base", W.LONG),
                ("flags", W.DWORD), ("image", W.WCHAR * 260)]


def pids(image):
    """Process ids by image name, without spawning tasklist every refresh."""
    snapshot = K.CreateToolhelp32Snapshot(0x2, 0)
    entry, ids = _Process(size=C.sizeof(_Process)), []
    more = K.Process32FirstW(snapshot, C.byref(entry))
    while more:
        if entry.image.lower() == image.lower():
            ids.append(entry.pid)
        more = K.Process32NextW(snapshot, C.byref(entry))
    K.CloseHandle(snapshot)
    return ids


class Layout:
    """Applies the plan to the game and the viewer; refresh() picks up new threads and windows."""
    def __init__(self, game_pid, main_tid):
        self.plan = Plan()
        self.game_pid, self.main_tid = game_pid, main_tid
        self.placed_threads, self.viewers = set(), set()

    def refresh(self):
        if not self.placed_threads:
            _process(self.game_pid, self.plan.game)
        for tid in _threads(self.game_pid):
            if tid not in self.placed_threads:
                _thread(tid, self.plan.main if tid == self.main_tid else self.plan.game_rest)
                self.placed_threads.add(tid)
        for pid in pids(VIEWER):
            if pid not in self.viewers and _process(pid, self.plan.helpers, BELOW_NORMAL):
                self.viewers.add(pid)

    def restore(self):
        for tid in _threads(self.game_pid):
            _thread(tid, self.plan.all)
        _process(self.game_pid, self.plan.all)
        for pid in self.viewers:
            _process(pid, self.plan.all, NORMAL)
        self.placed_threads.clear()
        self.viewers.clear()


if __name__ == "__main__":
    print(Plan().describe())
