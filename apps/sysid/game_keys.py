"""Game keys through SendInput scan codes (DirectInput sees them like real keys).

Bindings are the ETS_SysID profile's (controls.sii): B engine brake, ; and ' retarder
up and down, U lift axle, ` console. The game window must have focus.
"""
import ctypes, time
from ctypes import wintypes

SCAN = {}
for start, row in ((0x10, "qwertyuiop"), (0x1E, "asdfghjkl"), (0x2C, "zxcvbnm")):
    for i, c in enumerate(row):
        SCAN[c] = start + i
for i, c in enumerate("1234567890"):
    SCAN[c] = 0x02 + i
SCAN[" "], SCAN["-"] = 0x39, 0x0C
ENTER, GRAVE, SHIFT = 0x1C, 0x29, 0x2A
ENGINE_BRAKE, RETARDER_UP, RETARDER_DOWN, LIFT_AXLE = 0x30, 0x27, 0x28, 0x16


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def key(scan, down):
    event = INPUT(type=1)  # INPUT_KEYBOARD
    event.ki = KEYBDINPUT(0, scan, 0x0008 | (0 if down else 0x0002), 0, 0)  # SCANCODE, KEYUP
    ctypes.windll.user32.SendInput(1, ctypes.byref(event), ctypes.sizeof(INPUT))


def tap(scan, hold=0.05):
    key(scan, True); time.sleep(hold); key(scan, False); time.sleep(0.05)


def type_text(text, tick=lambda: None, ready=lambda: None):
    for c in text:
        ready()
        if c == "_":
            key(SHIFT, True); tap(SCAN["-"]); key(SHIFT, False)
        else:
            tap(SCAN[c])
        tick()


def console(*commands, focus=lambda: None, tick=lambda: None, focused=lambda: True, wait_s=600):
    """Open the game console, run the commands, close it. Letters are driving keys, so
    only type into a stopped truck that nobody drives. tick() runs between keys (a lease
    heartbeat: typing takes longer than its 5 s). Before every key, wait while focused()
    is false, so keys never go to another window."""
    def ready():
        until = time.monotonic() + wait_s
        while not focused():
            if time.monotonic() > until:
                raise TimeoutError(f"game window not in front for {wait_s} s while typing in the console")
            tick()
            time.sleep(0.05)
    focus()
    time.sleep(0.8)
    ready()
    tap(GRAVE)
    time.sleep(0.6)
    for command in commands:
        type_text(command, tick, ready)
        ready()
        tap(ENTER)
        time.sleep(0.3)
        tick()
    ready()
    tap(GRAVE)
    time.sleep(0.6)
    tick()


class Tapper:
    """Key taps inside a control loop without sleeping: press() puts a key down and
    update(), called every frame, lets it up after `hold` seconds."""

    def __init__(self, hold=0.06, gap=0.25):
        self.hold, self.gap = hold, gap
        self.down, self.since, self.last = None, 0.0, -1e9

    def ready(self):
        return self.down is None and time.monotonic() - self.last >= self.gap

    def press(self, scan):
        key(scan, True)
        self.down, self.since = scan, time.monotonic()

    def update(self):
        if self.down is not None and time.monotonic() - self.since >= self.hold:
            key(self.down, False)
            self.down, self.last = None, time.monotonic()

    def release(self):
        if self.down is not None:
            key(self.down, False)
            self.down = None


class Lever:
    """Steps a notched control (the retarder) toward a target position, one tap at a time,
    reading the position from a channel each frame."""

    def __init__(self, up, down, tapper=None):
        self.up, self.down = up, down
        self.tapper = tapper or Tapper()

    def update(self, position, target):
        self.tapper.update()
        if position is None or position == target or not self.tapper.ready():
            return
        self.tapper.press(self.up if position < target else self.down)
