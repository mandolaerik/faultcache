# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""ctypes bindings for libfaultcache.

Mirrors the mmap module's shape: a Pool tracks Regions, and a Region is a
read-only, lazily-populated byte range that primarily supports slicing.
Only the bytes actually sliced are ever faulted in and passed to the
fill_chunk callback - unaccessed chunks are never touched.

    pool = faultcache.Pool()
    region = pool.Region([100, 200, 4096], fill_chunk)
    region[50:150]   # only touches chunk 0 and chunk 1

Region.view() returns a zero-copy memoryview instead of a bytes copy - see
its docstring for important lifetime/safety caveats.

fill_chunk runs inside a SIGSEGV handler and is subject to real
restrictions - see Region's docstring before writing one.

Importing this module arms that handler, so there is nothing to set up.
The one thing to remember: if something arms SIGSEGV *after* the import --
`faulthandler.enable()` being the common case -- it takes the signal away
from us, and a lazy read that we could have resolved dies as a fatal
"Segmentation fault" instead. The `rearm_handler` function can be used to
put us back on top right afterwards.

The environment variable `FAULTCACHE_LIBRARY` overrides the shared library used
to locate libfaultcache. This is useful to point at a build directory before
install.
"""
import ctypes
import ctypes.util
import os
import weakref
from typing import Callable, NamedTuple, Optional, Sequence, Union

__all__ = ["Pool", "Region", "DebugStats", "rearm_handler"]


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

try:
    import _faultcache as _shim
except ImportError as exc:
    # No pure-ctypes fallback on purpose: a ctypes callback cannot suppress
    # the cyclic collector around fill_chunk, and fill_chunk runs inside a
    # SIGSEGV handler where a GC pass can be fatal (see TODO.md section 1).
    raise ImportError(
        "faultcache requires its compiled callback shim (_faultcache)"
    ) from exc

_lib.fc_init.argtypes = []
_lib.fc_init.restype = None

_lib.fc_rearm_handler.argtypes = []
_lib.fc_rearm_handler.restype = None

# Import time is this binding's library init: the earliest point we control,
# and the one the application orders by placing its imports. Deferring to
# the first Pool would put the install after arbitrary application code and
# make the handler topology depend on which thread got there first.
_lib.fc_init()

_lib.fc_pool_create.argtypes = [ctypes.c_size_t]
_lib.fc_pool_create.restype = ctypes.c_void_p

_lib.fc_pool_destroy.argtypes = [ctypes.c_void_p]
_lib.fc_pool_destroy.restype = None

_lib.fc_region_create.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_void_p,  # fc_fill_chunk_fn_t, built by the shim
    ctypes.c_void_p,
]
_lib.fc_region_create.restype = ctypes.c_void_p

_lib.fc_region_base.argtypes = [ctypes.c_void_p]
_lib.fc_region_base.restype = ctypes.c_void_p

_lib.fc_region_destroy.argtypes = [ctypes.c_void_p]
_lib.fc_region_destroy.restype = None

_lib.fc_region_size.argtypes = [ctypes.c_void_p]
_lib.fc_region_size.restype = ctypes.c_size_t


class _CDebugStats(ctypes.Structure):
    _fields_ = [
        ("nchunks", ctypes.c_uint32),
        ("chunks_resolved", ctypes.c_uint32),
        ("faults_handled", ctypes.c_uint32),
    ]


_lib.fc_region_debug_stats.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(_CDebugStats)
]
_lib.fc_region_debug_stats.restype = None

_libc = ctypes.CDLL(None, use_errno=True)
_libc.memcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_libc.memcpy.restype = ctypes.c_void_p


def _read(addr: int, length: int) -> bytes:
    buf = ctypes.create_string_buffer(length)
    _libc.memcpy(buf, addr, length)
    return buf.raw


def rearm_handler() -> None:
    """Put `faultcache`'s SIGSEGV handler back on top of the chain. In particular, this is needed after `faulthandler.enable()` is called, which arms its own handler and takes the signal away from us.

    Re-installs the handler of `faultcache` over whatever holds SIGSEGV now and
    makes that the new chain target, so faults outside live unresolved regions
    are passed on to it. Displacing `faultcache`'s own handler leaves the chain
    target alone, so repeated calls can't route us through ourselves. Call it
    from a thread you control, with no faults in flight.

    You need it when two things coincide: something else arms SIGSEGV
    without passing on the faults it did not cause, and it does so after
    faultcache was imported. Neither half hurts alone - a handler that
    chains properly still delivers our faults to us, and one armed before
    the import ends up below us anyway - but together they strand our
    regions under a handler that will not hand them back. Assume the first
    half unless you know otherwise; chaining is the rarer discipline.
    faulthandler is the case to watch: enable() is normally called from
    application code, long after any import, and a resolvable lazy read
    underneath it dies as a fatal "Segmentation fault" traceback.

    The convention in the general case is that whoever brings the conflict
    into the process repairs it. It is only visible where both sides are
    in the same dependency set, and either side can arrive transitively: a
    library depending on faultcache and on something that arms SIGSEGV
    calls this itself, rather than documenting two transitive dependencies
    for its users; where one library brings each side, only the
    application above them sees both, so it calls this. The corollary is
    not to rearm speculatively - if you cannot name the handler you are
    displacing, the conflict is not yours, and whoever owns it is calling
    this too.
    """
    _lib.fc_rearm_handler()


FillChunkFn = Callable[[int, memoryview], None]


class Region:
    """A read-only, lazily-populated byte range. Create via Pool.Region().

    The fill_chunk callback
    ~~~~~~~~~~~~~~~~~~~~~~~
    ``fill_chunk(chunk_index, buf)`` must fill ``buf`` (a writable
    memoryview of exactly that chunk's size) with the chunk's bytes. It is
    called from inside a SIGSEGV handler, on whichever thread touched the
    memory, so two invariants apply:

    1. ``buf`` is valid ONLY for the duration of the call. It points into a
       scratch mapping that gets moved into place - and thereby unmapped -
       as soon as fill_chunk returns. The view is released for you, so a
       retained one raises ValueError instead of corrupting memory.

    2. Nothing reachable from fill_chunk may read an unresolved part of any
       Region of any Pool. That faults again while the handler is still
       running, which faultcache reports and aborts on; it cannot be
       recovered from. The trap is that "reachable" includes code you never
       call directly: a ``__del__`` or ``weakref.finalize`` callback on an
       object whose last reference happens to go away during the fill.

       The cyclic collector is suppressed for the duration of the call, so
       unrelated garbage can no longer be finalized at an arbitrary
       allocation point. Refcount-driven finalization cannot be deferred by
       CPython and still fires immediately. A sufficient rule that avoids
       all of it: don't let fill_chunk drop the last reference to anything
       but its own locals - no clearing shared containers, no rebinding
       attributes, no closing objects.

    Exceptions cannot cross the handler boundary: one raised out of
    fill_chunk is reported via sys.unraisablehook, and the fault is
    resolved with whatever was written before it raised.
    """

    def __init__(self, pool: "Pool", chunk_sizes: Sequence[int],
                 fill_chunk: FillChunkFn):
        if pool._handle is None:
            raise ValueError("pool is closed")

        n = len(chunk_sizes)
        sizes_arr = (ctypes.c_size_t * n)(*chunk_sizes)

        # The capsule owns the callable and must outlive the region: it is
        # what fill_chunk_addr/user_data point into.
        self._filler, fill_fn, user_data = _shim.make_filler(fill_chunk)

        region = _lib.fc_region_create(pool._handle, n, sizes_arr, fill_fn,
                                       user_data)
        if not region:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))

        self._pool = pool
        self._region: Optional[int] = region  # opaque fc_region_t*, not an address
        self._addr: Optional[int] = _lib.fc_region_base(region)
        self._size = _lib.fc_region_size(region)
        self._view_count = 0
        pool._regions.add(self)

    @property
    def closed(self) -> bool:
        return self._region is None

    def debug_stats(self) -> DebugStats:
        """Snapshot of resolved-chunk/fault counters straight from the C
        library, without touching any region memory. Debug/test-only, see
        faultcache-debug.h."""
        if self._addr is None:
            raise ValueError("Region is closed")
        stats = _CDebugStats()
        _lib.fc_region_debug_stats(self._region, ctypes.byref(stats))
        return DebugStats(stats.nchunks, stats.chunks_resolved,
                           stats.faults_handled)

    @staticmethod
    def _view_released(region: "Region") -> None:
        region._view_count -= 1

    def view(self, start: int = 0, stop: Optional[int] = None,
             step: int = 1) -> memoryview:
        """Zero-copy memoryview over region[start:stop:step].

        Unlike __getitem__ (which always returns a fresh bytes copy,
        matching mmap's convention), this shares the region's own memory
        directly via ctypes.from_address() - no copy is made, so it's
        cheap for large ranges. With the mprotect+SIGSEGV backend (see
        src/faultcache-sigsegv.c), any access to the returned view -
        including from unrelated C code such as numpy - resolves
        unresolved chunks lazily and safely on whichever thread touches
        them, same as __getitem__. There is no separate "populate first"
        step to take here, unlike an earlier userfaultfd-based design
        that needed one to dodge a GIL-deadlock class of bug specific to
        that backend (see repo notes) - resolving synchronously on the
        faulting thread itself, as this backend does, is what removes
        that requirement.

        SAFETY: plain ctypes buffers (and the memoryviews wrapping them)
        carry no reference back to this Region - ctypes has no way to
        hook CPython's real buffer-export refcounting from pure Python
        (PyMemoryView_FromBuffer(), notably, silently discards any
        'obj' owner reference passed to it - it is not a substitute for
        a true buffer-protocol exporter). To catch the most common
        misuse, this Region tracks outstanding view() results via
        weakref.finalize() and close() raises if any are still alive;
        this keeps the Region object itself alive for as long as a
        view() result is (finalize holds a strong reference until it
        fires), which also prevents __del__ from firing prematurely.
        However, further slicing of the returned memoryview (e.g.
        region.view()[10:20]) produces additional, UNTRACKED aliases of
        the same memory that do not keep anything alive on their own -
        keep the original view() return value (or the Region itself)
        referenced for as long as any such further slice is in use.
        Using a view after the owning Region has been closed is
        undefined behavior (use-after-free).
        """
        if self._addr is None:
            raise ValueError("Region is closed")
        arr = (ctypes.c_uint8 * self._size).from_address(self._addr)
        mv = memoryview(arr).cast("B")[start:stop:step]
        self._view_count += 1
        weakref.finalize(mv, Region._view_released, self)
        return mv

    def close(self) -> None:
        if self._addr is None:
            return
        if self._view_count > 0:
            raise BufferError(
                f"cannot close Region: {self._view_count} outstanding "
                "view() result(s)"
            )
        if not self._pool.closed:
            _lib.fc_region_destroy(self._region)
        self._region = None
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

    `maxsize` is the target size in bytes. `0` means unbounded for now; any
    non-zero value is rejected until the LRU cache is implemented.
    """

    def __init__(self, maxsize: int = 0):
        self._handle: Optional[int] = None
        if maxsize != 0:
            raise NotImplementedError(
                "bounded pools (LRU chunk eviction) are not implemented yet; "
                "target_size must be 0"
            )
        handle = _lib.fc_pool_create(maxsize)
        if not handle:
            errno = ctypes.get_errno()
            raise OSError(errno, os.strerror(errno))
        self._handle: Optional[int] = handle
        self._regions: "weakref.WeakSet[Region]" = weakref.WeakSet()
        self.Region: Callable[[Sequence[int], FillChunkFn], Region] = (
            lambda chunk_sizes, fill_chunk: Region(self, chunk_sizes, fill_chunk)
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
