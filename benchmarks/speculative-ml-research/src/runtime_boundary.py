"""ctypes wrapper for the cooperative runtime boundary hook.

The C call forks internally: the ACTIVE runtime returns here with rc 0; the
parked TEMPLATE never returns (it parks in a C poll loop and _exits); a
restored GRANDCHILD is the only process that returns from the same call a
second time, with a bumped generation. Callers detect restore by comparing
generation() before/after.
"""
import ctypes
import os
from pathlib import Path

_LIB_ENV = "AGENTVFS_RUNTIME_CLIENT_LIB"
_DEFAULT = (Path(__file__).resolve().parents[3] / "build"
            / "libagentvfs_runtime_client.so")
_lib = None


def _load():
    global _lib
    if _lib is None:
        path = os.environ.get(_LIB_ENV) or str(_DEFAULT)
        lib = ctypes.CDLL(path)
        lib.agentvfs_runtime_boundary.argtypes = [
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.agentvfs_runtime_boundary.restype = ctypes.c_int
        lib.agentvfs_runtime_current_generation.argtypes = []
        lib.agentvfs_runtime_current_generation.restype = ctypes.c_uint64
        _lib = lib
    return _lib


def boundary(kind: str = "manual") -> int:
    """Block at a quiescence boundary; returns the generation AFTER the call
    (a change vs. before means this process is a restored generation)."""
    lib = _load()
    err = ctypes.create_string_buffer(512)
    rc = lib.agentvfs_runtime_boundary(kind.encode(), err, len(err))
    if rc != 0:
        raise RuntimeError(
            f"runtime boundary failed: {err.value.decode(errors='replace')}")
    return generation()


def generation() -> int:
    return int(_load().agentvfs_runtime_current_generation())
