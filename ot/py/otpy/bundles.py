"""OT_Bundles reader and Python-owned image recording (Windows x64)."""
import ctypes as C
import json
from pathlib import Path
import struct

from .client import W, _api, _open_mapping, _map, _unmap, _close, _free_library, _wait

_mutex = _api("CreateMutexW", W.HANDLE, C.c_void_p, W.BOOL, W.LPCWSTR)
_release_mutex = _api("ReleaseMutex", W.BOOL, W.HANDLE)


def _component(value):
    # File paths cross the shared-memory boundary. Accept one Windows path
    # component, never a path, drive, alternate stream, or parent traversal.
    if (not isinstance(value, str) or not value or value in (".", "..")
            or any(c in value for c in '\\/:\0') or value.endswith((".", " "))):
        raise ValueError("Invalid bundle path component")
    return value


class BundleReader:
    """Single consumer; close on the same thread that opens the reader.

    read_next returns the oldest queued bundle. Binary views own immutable
    Python bytes, so a subsequent read cannot overwrite a previous result.
    """
    def __init__(self, helper=None):
        self._handle = self._view = self._native = self._copy = self._mutex = None
        self._owns_mutex = False
        self.sequence = 0
        if C.sizeof(C.c_void_p) != 8:
            raise RuntimeError("ot requires 64-bit Python")
        try:
            self._mutex = _mutex(None, False, r"Local\OT_Bundles_Reader")
            if not self._mutex:
                raise C.WinError(C.get_last_error())
            if _wait(self._mutex, 0) not in (0, 0x80):  # acquired or abandoned
                raise RuntimeError("Another OT_Bundles reader owns the queue")
            self._owns_mutex = True
            self._handle = _open_mapping(6, False, r"Local\OT_Bundles")
            if not self._handle:
                raise C.WinError(C.get_last_error())
            self._view = _map(self._handle, 6, 0, 0, 64)
            if not self._view:
                raise C.WinError(C.get_last_error())
            magic, abi, slots, capacity, self.producer_pid = struct.unpack("<8sIIII", C.string_at(self._view, 24))
            if magic != b"OTBNDL01" or abi != 1 or slots != 3 or not 0 < capacity <= 256*1024*1024 or capacity % 64:
                raise ValueError("Unsupported OT_Bundles shared-memory ABI")
            _unmap(self._view)
            self._view = _map(self._handle, 6, 0, 0, 64 + slots*(64+capacity))
            if not self._view:
                raise C.WinError(C.get_last_error())
            self.capacity = capacity
            path = Path(helper) if helper else Path(__file__).resolve().parents[2] / "dist" / "ot_ipc.dll"
            self._native = C.WinDLL(str(path.resolve()))
            self._copy = self._native.ot_bundle_copy
            self._copy.argtypes = [C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint64, C.POINTER(C.c_uint64)]
            self._copy.restype = C.c_int
            self._buffer = C.create_string_buffer(capacity)
        except BaseException:
            self.close()
            raise

    def read_next(self):
        if not self._view:
            raise RuntimeError("BundleReader is closed")
        sequence = C.c_uint64()
        size = self._copy(self._view, self._buffer, self.capacity, self.sequence, C.byref(sequence))
        if size < 0:
            raise ValueError("Invalid OT_Bundles slot length")
        if not size:
            return None
        self.sequence = sequence.value
        raw = C.string_at(self._buffer, size)
        if len(raw) < 8:
            raise ValueError("Bundle has no manifest length")
        metadata_bytes, = struct.unpack_from("<Q", raw)
        if metadata_bytes > len(raw)-8:
            raise ValueError("Bundle manifest exceeds the payload")
        manifest = json.loads(raw[8:8+metadata_bytes])
        payload = memoryview(raw)[8+metadata_bytes:]
        cameras = {_component(view["camera"]) for view in manifest["views"]}
        files, seen = [], set()
        for item in manifest["files"]:
            camera, name = _component(item["camera"]), _component(item["file"])
            offset, length = item["offset"], item["length"]
            if (camera not in cameras or (camera, name) in seen or name == "images.json"
                    or not isinstance(offset, int) or not isinstance(length, int)
                    or offset < 0 or length < 0 or offset + length > len(payload)):
                raise ValueError("Invalid bundle file range or destination")
            seen.add((camera, name))
            files.append({"camera": camera, "file": name, "data": payload[offset:offset+length]})
        return {"sequence": self.sequence, "manifest": manifest, "files": files}

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
        if self._mutex:
            if self._owns_mutex:
                _release_mutex(self._mutex)
                self._owns_mutex = False
            _close(self._mutex)
            self._mutex = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def save_bundle(bundle, directory):
    """Save a decoded bundle. Existing captures/files are never overwritten."""
    directory = Path(directory)
    directory.mkdir()
    manifest = bundle["manifest"]
    for view in manifest["views"]:
        (directory / view["camera"]).mkdir()
    for item in bundle["files"]:
        with open(directory / item["camera"] / item["file"], "xb") as stream:
            stream.write(item["data"])
    for view in manifest["views"]:
        with open(directory / view["camera"] / "images.json", "x", encoding="utf-8") as stream:
            json.dump(view["metadata"], stream, ensure_ascii=False, indent=2)
    with open(directory / "bundle.json", "x", encoding="utf-8") as stream:
        json.dump({"sequence": bundle["sequence"], **manifest}, stream, ensure_ascii=False, indent=2)
    return directory
