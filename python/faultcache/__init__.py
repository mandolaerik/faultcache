# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""ctypes bindings for libfaultcache.

Mirrors the mmap module's shape: a Pool tracks Regions, and a Region is a
read-only, lazily-populated byte range that primarily supports slicing.
Only the bytes actually sliced are ever faulted in and passed to the
init_chunk callback - unaccessed chunks are never touched.

    pool = faultcache.Pool()
    region = pool.Region([100, 200, 4096], init_chunk)
    region[50:150]   # only touches chunk 0 and chunk 1

Env var FAULTCACHE_LIBRARY overrides the shared library path/name used to
locate libfaultcache (useful to point at a build directory before install).
"""
import ctypes
import ctypes.util
import os
import traceback
import weakref
from typing import Callable, NamedTuple, Optional, Sequence, Union

__all__ = ["Pool", "Region", "DebugStats"]


class DebugStats(NamedTuple):
    """Snapshot of a region's fault-handling activity.

    Mirrors struct fc_region_debug_stats from faultcache-debug.h - a
    debug/test-only API, not part of the stable ABI.
    """
    nchunks: int
    chunks_resolved: int
    faults_handled: int


def _load_library() -> ctypes.CDLL:
    name = os.environ.get("FAULTCACHE_LIBRARY") or ctypes.util.find_library(
        "faultcache"
    ) or "libfaultcache.so"
    return ctypes.CDLL(name, use_errno=True)


_lib = _load_library()

_InitChunkFn = ctypes.CFUNCTYPE(
    None, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p
)

_lib.fc_pool_create.argtypes = []
_lib.fc_pool_create.restype = ctypes.c_void_p

_lib.fc_pool_destroy.argtypes = [ctypes.c_void_p]
_lib.fc_pool_destroy.restype = None

_lib.fc_region_create.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_size_t),
    _InitChunkFn,
    ctypes.c_void_p,
]
_lib.fc_region_create.restype = ctypes.c_void_p

_lib.fc_region_destroy.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.fc_region_destroy.restype = ctypes.c_int

_lib.fc_region_size.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_lib.fc_region_size.restype = ctypes.c_size_t


class _CDebugStats(ctypes.Structure):
    _fields_ = [
        ("nchunks", ctypes.c_uint32),
        ("chunks_resolved", ctypes.c_uint32),
        ("faults_handled", ctypes.c_uint32),
    ]


_lib.fc_region_debug_stats.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(_CDebugStats)
]
_lib.fc_region_debug_stats.restype = ctypes.c_int

# ctypes.string_at()/memmove() are implemented in-process and never release
# the GIL. Reading a not-yet-resolved page blocks in the kernel until the
# handler thread services the fault, and its callback trampoline needs the
# GIL (via PyGILState_Ensure) to call back into Python - so reading through
# those builtins can deadlock the whole process. A real foreign-function
# call made through a CDLL (like this libc memcpy) does release the GIL for
# the duration of the call, so route all reads through it instead.
_libc = ctypes.CDLL(None, use_errno=True)
_libc.memcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_libc.memcpy.restype = ctypes.c_void_p


def _read(addr: int, length: int) -> bytes:
    buf = ctypes.create_string_buffer(length)
    _libc.memcpy(buf, addr, length)
    return buf.raw


InitChunkFn = Callable[[int, memoryview], None]


class Region:
    """A read-only, lazily-populated byte range. Create via Pool.Region()."""

    def __init__(self, pool: "Pool", chunk_sizes: Sequence[int],
                 init_chunk: InitChunkFn):
        if pool._handle is None:
            raise ValueError("pool is closed")

        n = len(chunk_sizes)
        sizes_arr = (ctypes.c_size_t * n)(*chunk_sizes)

        def trampoline(chunk, start, size, _user_data):
            buf = (ctypes.c_uint8 * size).from_address(start)
            # cast() drops the '<' byte-order prefix ctypes attaches to the
            # format string; memoryview's slice-assignment fast path only
            # accepts unprefixed single-byte formats.
            view = memoryview(buf).cast('B')
            try:
                init_chunk(chunk, view)
            except Exception:
                # Can't propagate across the C callback boundary; the fault
                # is still resolved with whatever init_chunk wrote (if
                # anything) before raising.
                traceback.print_exc()

        # Kept alive on self: ctypes does not keep the trampoline/closure
        # alive on the C side, only the CFUNCTYPE wrapper object matters.
        self._cb = _InitChunkFn(trampoline)

        addr = _lib.fc_region_create(pool._handle, n, sizes_arr, self._cb, None)
        if not addr:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))

        self._pool = pool
        self._addr: Optional[int] = addr
        self._size = _lib.fc_region_size(pool._handle, addr)
        pool._regions.add(self)

    @property
    def closed(self) -> bool:
        return self._addr is None

    def debug_stats(self) -> DebugStats:
        """Snapshot of resolved-chunk/fault counters straight from the C
        library, without touching any region memory. Debug/test-only, see
        faultcache-debug.h."""
        if self._addr is None:
            raise ValueError("Region is closed")
        stats = _CDebugStats()
        rc = _lib.fc_region_debug_stats(self._pool._handle, self._addr,
                                         ctypes.byref(stats))
        if rc != 0:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))
        return DebugStats(stats.nchunks, stats.chunks_resolved,
                           stats.faults_handled)

    def close(self) -> None:
        if self._addr is None:
            return
        if self._pool._handle is not None:
            _lib.fc_region_destroy(self._pool._handle, self._addr)
        self._addr = None
        self._size = 0

    def __len__(self) -> int:
        return self._size

    def __getitem__(self, key: Union[int, slice]) -> Union[int, bytes]:
        if self._addr is None:
            raise ValueError("Region is closed")
        n = self._size

        if isinstance(key, slice):
            start, stop, step = key.indices(n)
            if step == 1:
                length = max(0, stop - start)
                return _read(self._addr + start, length)
            # One native read for the whole covered span (still safe re the
            # GIL note above), then subsample in pure Python - no further
            # native calls needed per byte.
            lo, hi = (start, stop) if step > 0 else (stop + 1, start + 1)
            span = _read(self._addr + lo, max(0, hi - lo))
            return bytes(span[i - lo] for i in range(start, stop, step))

        if isinstance(key, int):
            i = key + n if key < 0 else key
            if not 0 <= i < n:
                raise IndexError("Region index out of range")
            return _read(self._addr + i, 1)[0]

        raise TypeError(
            f"Region indices must be integers or slices, not {type(key).__name__}"
        )

    def __enter__(self) -> "Region":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


class Pool:
    """Tracks the lifetime of Regions carved out of it.

    `maxsize` will eventually bound total resident chunk memory with LRU
    eviction across regions; not implemented yet.
    """

    def __init__(self, maxsize: Optional[int] = None):
        self._handle: Optional[int] = None
        if maxsize is not None:
            raise NotImplementedError(
                "bounded pools (LRU chunk eviction) are not implemented yet"
            )
        handle = _lib.fc_pool_create()
        if not handle:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))
        self._handle: Optional[int] = handle
        self._regions: "weakref.WeakSet[Region]" = weakref.WeakSet()
        self.Region: Callable[[Sequence[int], InitChunkFn], Region] = (
            lambda chunk_sizes, init_chunk: Region(self, chunk_sizes, init_chunk)
        )

    @property
    def closed(self) -> bool:
        return self._handle is None

    def close(self) -> None:
        if self._handle is None:
            return
        for region in list(self._regions):
            region.close()
        _lib.fc_pool_destroy(self._handle)
        self._handle = None

    def __enter__(self) -> "Pool":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
