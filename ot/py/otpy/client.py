"""Named-pipe commands and ownership-based shared state reads (Windows x64)."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct
import time
import uuid


class _Overlapped(C.Structure):
    _fields_ = [("Internal", C.c_size_t), ("InternalHigh", C.c_size_t),
                ("Offset", W.DWORD), ("OffsetHigh", W.DWORD), ("hEvent", W.HANDLE)]


_k = C.WinDLL("kernel32", use_last_error=True)


def _api(name, result, *arguments):
    function = getattr(_k, name)
    function.restype, function.argtypes = result, list(arguments)
    return function


_create_file = _api("CreateFileW", W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD,
                    C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
_close = _api("CloseHandle", W.BOOL, W.HANDLE)
_wait_pipe = _api("WaitNamedPipeW", W.BOOL, W.LPCWSTR, W.DWORD)
_event = _api("CreateEventW", W.HANDLE, C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR)
_wait = _api("WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)
_read = _api("ReadFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
             C.POINTER(W.DWORD), C.POINTER(_Overlapped))
_write = _api("WriteFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
              C.POINTER(W.DWORD), C.POINTER(_Overlapped))
_result = _api("GetOverlappedResult", W.BOOL, W.HANDLE, C.POINTER(_Overlapped),
               C.POINTER(W.DWORD), W.BOOL)
_cancel = _api("CancelIoEx", W.BOOL, W.HANDLE, C.POINTER(_Overlapped))
_open_mapping = _api("OpenFileMappingW", W.HANDLE, W.DWORD, W.BOOL, W.LPCWSTR)
_map = _api("MapViewOfFile", C.c_void_p, W.HANDLE, W.DWORD, W.DWORD, W.DWORD, C.c_size_t)
_unmap = _api("UnmapViewOfFile", W.BOOL, C.c_void_p)
_free_library = _api("FreeLibrary", W.BOOL, W.HMODULE)
_INVALID = C.c_void_p(-1).value


def _io(handle, function, buffer, size, deadline):
    event = _event(None, True, False, None)
    if not event:
        raise C.WinError(C.get_last_error())
    op = _Overlapped(hEvent=event)
    count = W.DWORD()
    pending = False
    try:
        if function(handle, buffer, size, C.byref(count), C.byref(op)):
            return count.value
        error = C.get_last_error()
        if error != 997:  # ERROR_IO_PENDING
            raise C.WinError(error)
        pending = True
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("ot pipe response timed out")
            status = _wait(event, max(1, min(50, int(left * 1000))))
            if status == 0:
                if not _result(handle, C.byref(op), C.byref(count), False):
                    raise C.WinError(C.get_last_error())
                pending = False
                return count.value
            if status != 258:  # WAIT_TIMEOUT
                raise C.WinError(C.get_last_error())
    finally:
        if pending:
            _cancel(handle, C.byref(op))
            # Keep the OVERLAPPED storage alive until cancellation completes.
            _result(handle, C.byref(op), C.byref(count), True)
        _close(event)


class Client:
    """Each command owns one pipe connection and closes it after its reply."""

    def __init__(self, timeout=5.0, pipe_name=r"\\.\pipe\ot"):
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.timeout = timeout
        self.pipe_name = pipe_name

    def request(self, command, **arguments):
        payload = (json.dumps({"cmd": command, **arguments}, allow_nan=False) + "\n").encode()
        if len(payload) > 65536:
            raise ValueError("ot request exceeds 65536 bytes")
        deadline = time.monotonic() + self.timeout
        name = self.pipe_name
        while True:
            # OVERLAPPED + SECURITY_SQOS_PRESENT + SECURITY_IDENTIFICATION.
            handle = _create_file(name, 0xC0000000, 0, None, 3, 0x40110000, None)
            if handle != _INVALID:
                break
            error = C.get_last_error()
            if error == 2:
                raise FileNotFoundError(f"Command pipe is absent: {name}; plugin is not loaded or initialization failed")
            if error != 231:  # ERROR_PIPE_BUSY
                raise C.WinError(error)
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("ot pipe is busy")
            _wait_pipe(name, max(1, min(50, int(left * 1000))))
        try:
            offset = 0
            while offset < len(payload):
                remaining = payload[offset:]
                buffer = C.create_string_buffer(remaining)
                n = _io(handle, _write, buffer, len(remaining), deadline)
                if not n:
                    raise ConnectionError("ot pipe closed during request")
                offset += n
            reply = bytearray()
            while len(reply) <= 1024 * 1024:
                buffer = C.create_string_buffer(4096)
                n = _io(handle, _read, buffer, len(buffer), deadline)
                if not n:
                    raise ConnectionError("ot pipe closed without a complete response")
                reply.extend(buffer.raw[:n])
                if b"\n" in reply:
                    response = json.loads(reply.split(b"\n", 1)[0])
                    if not isinstance(response, dict) or not isinstance(response.get("ok"), bool):
                        raise ValueError("Invalid ot response envelope")
                    if not response["ok"]:
                        raise RuntimeError(response.get("error", "ot command failed"))
                    return response["result"]
            raise ValueError("ot response exceeds 1 MiB")
        finally:
            _close(handle)

    def read(self, field):
        return self.request("read", field=field)

    def snapshot(self):
        return self.request("snapshot")

    def schema(self):
        return self.request("schema")

    def tier(self, value=None):
        return self.request("tier", **({} if value is None else {"value": value}))

    def panic(self):
        return self.request("panic")


class LoaderClient(Client):
    """Resident loader; commands finish at a game SDK frame boundary."""

    def __init__(self, timeout=5.0):
        super().__init__(timeout=timeout, pipe_name=r"\\.\pipe\ot_loader")

    def status(self):
        return self.request("status")

    def control(self, action):
        request_id = str(uuid.uuid4())
        # Submit once. A lost response is not an instruction to repeat a reload.
        try:
            result = self.request(action, request_id=request_id)
        except (OSError, RuntimeError) as error:
            raise RuntimeError(f"Loader request {request_id}: inspect loader status before retrying: {error}") from error
        deadline = time.monotonic() + max(8.0, self.timeout)
        while True:
            operation = result.get("operation") or {}
            if operation.get("request_id") != request_id:
                raise RuntimeError(f"Loader operation {request_id} was superseded; inspect loader status")
            if operation["state"] == "done":
                return result
            if operation["state"] in ("failed", "expired", "cancelled"):
                raise RuntimeError(operation.get("error", operation["state"]))
            if time.monotonic() >= deadline:
                raise TimeoutError(f"Loader operation {request_id} is {operation['state']}; inspect loader status before retrying")
            time.sleep(0.05)
            result = self.status()


class StateReader:
    """One consumer per mapping; read_latest() intentionally skips older frames.

    Use as a context manager. ot_ipc.dll atomically claims/copies/releases slots;
    Python never accesses mutable slot payloads without ownership.
    """
    _SLOTS, _PAYLOAD = 8, 65536
    _SIZE = 64 + _SLOTS * (64 + _PAYLOAD)

    def __init__(self, helper=None):
        self._handle = self._view = self._native = self._copy = None
        self.sequence = 0
        if C.sizeof(C.c_void_p) != 8:
            raise RuntimeError("ot requires 64-bit Python")
        try:
            path = Path(helper) if helper else Path(__file__).resolve().parents[2] / "dist" / "ot_ipc.dll"
            self._native = C.WinDLL(str(path.resolve()))
            self._copy = self._native.ot_state_copy
            self._copy.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint64, C.POINTER(C.c_uint64)]
            self._copy.restype = C.c_int
            self._handle = _open_mapping(6, False, r"Local\OT_State")  # FILE_MAP_READ|WRITE for slot ownership
            if not self._handle:
                raise C.WinError(C.get_last_error())
            self._view = _map(self._handle, 6, 0, 0, self._SIZE)
            if not self._view:
                raise C.WinError(C.get_last_error())
            magic, abi, slots, size, self.producer_pid = struct.unpack("<8sIIII", C.string_at(self._view, 24))
            if magic != b"OTSTATE1" or abi != 1 or slots != self._SLOTS or size != self._PAYLOAD:
                raise ValueError("Unsupported OT_State shared-memory ABI")
            self._buffer = C.create_string_buffer(self._PAYLOAD)
        except BaseException:
            self.close()
            raise

    def read_latest(self):
        if not self._view:
            raise RuntimeError("StateReader is closed")
        sequence = C.c_uint64()
        n = self._copy(self._view, self._buffer, self._PAYLOAD, self.sequence, C.byref(sequence))
        if n < 0:
            raise ValueError("Invalid OT_State slot length")
        if not n:
            return None
        self.sequence = sequence.value
        return json.loads(self._buffer.raw[:n])

    def close(self):
        if self._view:
            _unmap(self._view)
            self._view = None
        if self._handle:
            _close(self._handle)
            self._handle = None
        self._copy = None
        if self._native:
            _free_library(self._native._handle)
            self._native = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
