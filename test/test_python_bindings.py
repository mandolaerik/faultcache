# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""Exercises the ctypes bindings in python/faultcache against libfaultcache."""
import os
import sys
import traceback

import faultcache

PAGE = 4096


def test_pool_maxsize_default_is_unbounded():
    with faultcache.Pool() as pool:
        assert not pool.closed

    try:
        faultcache.Pool(maxsize=0)
    except Exception as exc:  # pragma: no cover - sanity check for the API stub
        raise AssertionError(f"Pool(maxsize=0) should be valid, got {exc!r}")

    try:
        faultcache.Pool(maxsize=64 * 1024 * 1024)
    except NotImplementedError:
        return
    raise AssertionError("Pool(maxsize>0) should be rejected until LRU is implemented")


def test_basic_region():
    # Page-aligned chunks: touching one must not touch its neighbours.
    counts = [0, 0, 0]
    sizes = [PAGE, PAGE, PAGE]

    def fill_chunk(chunk, buf):
        counts[chunk] += 1
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region(sizes, fill_chunk)
        assert len(region) == sum(sizes)
        assert counts == [0, 0, 0]

        assert region[PAGE + 5] == ord('b')
        assert counts == [0, 1, 0]

        assert region[2 * PAGE:2 * PAGE + 3] == bytes([ord('c')]) * 3
        assert counts == [0, 1, 1]

        # Re-reading must not re-invoke fill_chunk.
        assert region[PAGE:PAGE + 3] == bytes([ord('b')]) * 3
        assert counts == [0, 1, 1]

        assert region[-1] == ord('c')
        region.close()
        assert region.closed


def test_debug_stats():
    # Same laziness check as test_basic_region, but verified via the C
    # library's own counters (fc_region_debug_stats) instead of relying on
    # a Python-side callback counter.
    def fill_chunk(chunk, buf):
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region([PAGE, PAGE, PAGE], fill_chunk)
        stats = region._debug_stats()
        assert stats.nchunks == 3
        assert stats.chunks_resolved == 0
        assert stats.faults_handled == 0

        region[PAGE]
        stats = region._debug_stats()
        assert stats.chunks_resolved == 1
        assert stats.faults_handled == 1

        region[PAGE]  # re-reading must not count as another fault
        stats = region._debug_stats()
        assert stats.chunks_resolved == 1
        assert stats.faults_handled == 1

        region[2 * PAGE]
        stats = region._debug_stats()
        assert stats.chunks_resolved == 2
        assert stats.faults_handled == 2

        region.close()
        try:
            region._debug_stats()
        except ValueError:
            return
        raise AssertionError("debug_stats() on a closed region should raise")


def test_debug_lru_queue_and_history() -> None:
    # Pool-level queue is global across regions and exposes (Region, chunk).
    sizes = [PAGE, PAGE]

    def fill_chunk(chunk: int, buf: memoryview) -> None:
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region_a = pool.create_region(sizes, fill_chunk)
        region_b = pool.create_region(sizes, fill_chunk)

        assert region_a[0] == ord('a')
        assert region_b[0] == ord('a')
        assert region_a[PAGE] == ord('b')

        stats_a = region_a._debug_lru_stats()
        stats_b = region_b._debug_lru_stats()
        assert stats_a.resident_chunks == 2
        assert stats_b.resident_chunks == 1
        assert stats_a.fault_events_total == 2
        assert stats_b.fault_events_total == 1

        queue = pool._debug_lru_queue()
        assert [(entry.region, entry.chunk) for entry in queue] == [
            (region_a, 1),
            (region_b, 0),
            (region_a, 0),
        ]
        assert [entry.faults_total for entry in queue] == [1, 1, 1]

        history = region_a._debug_lru_history()
        assert len(history) == 2
        assert [entry.faults_total for entry in history] == [1, 1]


def test_region_identity_hash_behavior() -> None:
    def fill_chunk(chunk: int, buf: memoryview) -> None:
        buf[:] = b"x" * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region([PAGE], fill_chunk)
        alias = object.__new__(faultcache.Region)
        alias._region = None
        alias._addr = None
        alias._size = 0
        alias._view_count = 0
        alias._pool = pool

        assert alias is not region
        assert alias != region
        assert hash(alias) != hash(region)

        d = {region: "ok"}
        assert d[region] == "ok"

        s = {region, alias}
        assert len(s) == 2


def test_boundary_sharing_group():
    # Mirrors test/test-boundary.c: chunks not aligned to page boundaries
    # must resolve together with every other chunk sharing that page.
    counts = [0, 0, 0]
    sizes = [100, 50, PAGE * 2]  # chunk2 starts at 150, mid-page.

    def fill_chunk(chunk, buf):
        counts[chunk] += 1
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region(sizes, fill_chunk)
        assert region[120] == ord('b')  # inside chunk1
        # chunk2 starts within the same page group and gets pulled in too.
        assert counts == [1, 1, 1]


def test_slice_step():
    def fill_chunk(chunk, buf):
        buf[:] = bytes(range(len(buf)))

    with faultcache.Pool() as pool:
        region = pool.create_region([16], fill_chunk)
        assert region[0:16:2] == bytes(range(0, 16, 2))


def test_pool_close_closes_regions():
    def fill_chunk(chunk, buf):
        buf[:] = b"\0" * len(buf)

    pool = faultcache.Pool()
    region = pool.create_region([PAGE], fill_chunk)
    assert not region.closed
    pool.close()
    assert region.closed
    assert pool.closed


def test_pool_keeps_region_alive_without_user_reference():
    import gc
    import weakref

    def fill_chunk(chunk, buf):
        buf[:] = b"\0" * len(buf)

    pool = faultcache.Pool()
    region = pool.create_region([PAGE], fill_chunk)
    wr = weakref.ref(region)

    del region
    gc.collect()
    assert wr() is not None
    assert len(pool._regions) == 1

    pool.close()
    gc.collect()
    assert wr() is None


def test_write_is_fatal():
    def fill_chunk(chunk, buf):
        buf[:] = b"X" * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region([PAGE], fill_chunk)
        assert region[0] == ord('X')

        pid = os.fork()
        if pid == 0:
            # Child: the mapping is read-only, this write must be fatal.
            import ctypes
            ctypes.memset(region._addr, ord('Y'), 1)
            os._exit(0)  # must not be reached

        _, status = os.waitpid(pid, 0)
        assert os.WIFSIGNALED(status), "write to read-only region did not crash"


def test_access_after_unmap_is_fatal():
    def fill_chunk(chunk, buf):
        buf[:] = b"Z" * len(buf)

    pool = faultcache.Pool()
    region = pool.create_region([PAGE], fill_chunk)
    assert region[0] == ord('Z')
    addr = region._addr
    region.close()

    pid = os.fork()
    if pid == 0:
        import ctypes
        ctypes.string_at(addr, 1)
        os._exit(0)  # must not be reached

    _, status = os.waitpid(pid, 0)
    assert os.WIFSIGNALED(status), "access after close did not crash"
    pool.close()


def test_view_zero_copy():
    def fill_chunk(chunk, buf):
        buf[:] = bytes(i % 256 for i in range(len(buf)))

    with faultcache.Pool() as pool:
        region = pool.create_region([PAGE, PAGE], fill_chunk)

        v = region.view(0, 8)
        assert isinstance(v, memoryview)
        assert bytes(v) == bytes(range(8))

        # step != 1, spanning into the second (still-unresolved) chunk.
        v2 = region.view(PAGE - 8, PAGE + 8, 2)
        assert bytes(v2) == bytes(region[PAGE - 8:PAGE + 8:2])

        stats = region._debug_stats()
        assert stats.chunks_resolved == 2  # touched by v2 above

        # Drop the outstanding views before the Pool context manager
        # closes the region on exit (close() refuses otherwise).
        del v, v2


def test_view_lifetime_tracking():
    import gc
    import weakref

    def fill_chunk(chunk, buf):
        buf[:] = b"\0" * len(buf)

    pool = faultcache.Pool()
    region = pool.create_region([PAGE], fill_chunk)

    v = region.view(0, 10)
    assert region._view_count == 1

    # Pool keeps explicit ownership of created regions until close().
    # Dropping the caller reference must therefore not collect Region yet.
    alive = []
    wr = weakref.ref(region, lambda r: alive.append(True))
    del region
    gc.collect()
    assert not alive, "Region was collected while a view() result is alive"

    assert bytes(v[:4]) == b"\0\0\0\0"
    del v
    gc.collect()
    assert not alive, "Region was collected before pool.close()"

    pool.close()
    gc.collect()
    assert alive, "Region was not collected after pool.close()"


def test_view_blocks_close():
    def fill_chunk(chunk, buf):
        buf[:] = b"\0" * len(buf)

    with faultcache.Pool() as pool:
        region = pool.create_region([PAGE], fill_chunk)
        v = region.view(0, 10)
        try:
            region.close()
            raise AssertionError("close() should refuse while a view() "
                                  "result is outstanding")
        except BufferError:
            pass
        del v
        region.close()
        assert region.closed


def test_gc_is_suppressed_during_fill():
    """Deterministic reproducer for the cyclic-GC reentrancy hazard.

    A finalizer sitting on *cyclic* garbage (refcounting alone can never
    free it, only the cyclic collector) reads a still-unresolved region.
    Allocating past gen0's threshold inside fill_chunk forces that
    collector to run on the faulting thread while it is still inside the
    SIGSEGV handler, so the finalizer faults again -> nested fault -> fatal.

    Determinism comes from gc.collect() zeroing gen0's counter immediately
    before the cycle is built (so nothing collects it too early) and from
    fill_chunk then allocating several times the threshold.

    Without the C shim's PyGC_Disable this kills the child with SIGABRT.
    The child also checks that the collector is back on afterwards and that
    the cycle really was collectable, so the test cannot pass merely
    because the reproducer went stale.
    """
    import gc

    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_fd)
        os.dup2(write_fd, 2)
        try:
            def victim_fill(chunk, buf):
                buf[:] = b"V" * len(buf)

            def trigger_fill(chunk, buf):
                buf[:] = b"T" * len(buf)
                junk = [[] for _ in range(gc.get_threshold()[0] * 3)]
                junk.clear()

            pool = faultcache.Pool()
            victim = pool.create_region([PAGE], victim_fill)
            trigger = pool.create_region([PAGE], trigger_fill)

            gc.collect()

            touched = []

            class Toucher:
                def __init__(self):
                    self.loop = self

                def __del__(self):
                    touched.append(victim[0])  # faults if still unresolved

            Toucher()  # dropped, but self-referential: only gc can free it

            trigger[0]

            assert gc.isenabled(), "fill_chunk left the collector disabled"
            gc.collect()
            assert touched, "the cycle was never collected; test went stale"
            os._exit(0)
        except BaseException:
            traceback.print_exc()
            os._exit(1)

    os.close(write_fd)
    with os.fdopen(read_fd, "rb") as f:
        err = f.read()
    _, status = os.waitpid(pid, 0)

    assert not os.WIFSIGNALED(status), (
        f"a GC pass inside fill_chunk reached the unresolved region "
        f"(signal {os.WTERMSIG(status)}, stderr={err!r})")
    assert os.WEXITSTATUS(status) == 0, (
        f"child failed (exit {os.WEXITSTATUS(status)}), stderr={err!r}")


def test_refcount_finalizer_during_fill_touching_region_is_fatal():
    """Pins the tighter boundary the GC suppression deliberately stops at.

    PyGC_Disable() only silences the cyclic collector. A refcount reaching
    zero inside fill_chunk still runs __del__/weakref callbacks right
    there, and CPython offers no way to defer those - so the documented
    rule is the stricter "fill_chunk must not drop the last reference to
    anything but its own locals", not merely "must not read a region".

    This asserts the violation stays loud: faultcache's own nested-fault
    diagnostic plus SIGABRT, never a silent crash or corruption.
    """
    import signal

    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_fd)
        os.dup2(write_fd, 2)
        try:
            def victim_fill(chunk, buf):
                buf[:] = b"V" * len(buf)

            holder = []

            def trigger_fill(chunk, buf):
                buf[:] = b"T" * len(buf)
                holder.clear()  # last reference -> __del__ runs right here

            pool = faultcache.Pool()
            victim = pool.create_region([PAGE], victim_fill)
            trigger = pool.create_region([PAGE], trigger_fill)

            class Toucher:
                def __del__(self):
                    victim[0]  # unresolved -> faults

            holder.append(Toucher())

            trigger[0]
            os._exit(0)  # must not be reached
        except BaseException:
            traceback.print_exc()
            os._exit(1)

    os.close(write_fd)
    with os.fdopen(read_fd, "rb") as f:
        err = f.read()
    _, status = os.waitpid(pid, 0)

    assert os.WIFSIGNALED(status), (
        f"a finalizer run inside fill_chunk did not reach the unresolved "
        f"region (status={status}, stderr={err!r})")
    assert os.WTERMSIG(status) == signal.SIGABRT, (
        f"died from {os.WTERMSIG(status)}, not faultcache's own diagnosed "
        f"abort (stderr={err!r})")
    assert b"faultcache: misuse" in err, (
        f"crashed without faultcache's nested-fault diagnostic: {err!r}")


def test_fill_buffer_is_invalidated_after_fill_returns():
    """fill_chunk's buffer must stop working once the callback returns.

    The buffer fill_chunk receives points into the scratch mapping that
    resolve_fault_locked() mremap()s into place - which unmaps it. Reading
    a retained view would dereference an unmapped address (verified pre-fix:
    SIGSEGV, no diagnostic) and, since the buffer is writable, silently
    corrupt whatever gets mapped there later. The shim release()s the
    memoryview once fill_chunk returns, turning this into a ValueError.
    """
    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_fd)
        os.dup2(write_fd, 2)
        stash = []

        def fill_chunk(chunk, buf):
            stash.append(buf)
            buf[:] = b"S" * len(buf)

        try:
            with faultcache.Pool() as pool:
                region = pool.create_region([PAGE], fill_chunk)
                assert region[0] == ord('S')
                try:
                    bytes(stash[0][:4])
                except ValueError:
                    os._exit(0)
                os._exit(2)
        except BaseException:
            traceback.print_exc()
            os._exit(1)

    os.close(write_fd)
    with os.fdopen(read_fd, "rb") as f:
        err = f.read()
    _, status = os.waitpid(pid, 0)

    assert not os.WIFSIGNALED(status), (
        f"retained fill_chunk buffer still dereferences the unmapped scratch "
        f"page (died from signal {os.WTERMSIG(status)})")
    code = os.WEXITSTATUS(status)
    assert code != 2, "retained fill_chunk buffer was still readable"
    assert code == 0, f"unexpected failure (exit {code}), stderr={err!r}"


def main():
    tests = [
        test_pool_maxsize_default_is_unbounded,
        test_basic_region,
        test_debug_stats,
        test_debug_lru_queue_and_history,
        test_region_identity_hash_behavior,
        test_boundary_sharing_group,
        test_slice_step,
        test_view_zero_copy,
        test_view_lifetime_tracking,
        test_view_blocks_close,
        test_pool_close_closes_regions,
        test_write_is_fatal,
        test_access_after_unmap_is_fatal,
        test_gc_is_suppressed_during_fill,
        test_refcount_finalizer_during_fill_touching_region_is_fatal,
        test_fill_buffer_is_invalidated_after_fill_returns,
    ]
    for t in tests:
        t()
        print(f"ok: {t.__name__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
