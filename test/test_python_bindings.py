# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""Exercises the ctypes bindings in python/faultcache against libfaultcache."""
import os
import sys

import faultcache

PAGE = 4096


def test_pool_maxsize_not_implemented():
    try:
        faultcache.Pool(maxsize=64 * 1024 * 1024)
    except NotImplementedError:
        return
    raise AssertionError("Pool(maxsize=...) should not be implemented yet")


def test_basic_region():
    # Page-aligned chunks: touching one must not touch its neighbours.
    counts = [0, 0, 0]
    sizes = [PAGE, PAGE, PAGE]

    def init_chunk(chunk, buf):
        counts[chunk] += 1
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.Region(sizes, init_chunk)
        assert len(region) == sum(sizes)
        assert counts == [0, 0, 0]

        assert region[PAGE + 5] == ord('b')
        assert counts == [0, 1, 0]

        assert region[2 * PAGE:2 * PAGE + 3] == bytes([ord('c')]) * 3
        assert counts == [0, 1, 1]

        # Re-reading must not re-invoke init_chunk.
        assert region[PAGE:PAGE + 3] == bytes([ord('b')]) * 3
        assert counts == [0, 1, 1]

        assert region[-1] == ord('c')
        region.close()
        assert region.closed


def test_debug_stats():
    # Same laziness check as test_basic_region, but verified via the C
    # library's own counters (fc_region_debug_stats) instead of relying on
    # a Python-side callback counter.
    def init_chunk(chunk, buf):
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.Region([PAGE, PAGE, PAGE], init_chunk)
        stats = region.debug_stats()
        assert stats.nchunks == 3
        assert stats.chunks_resolved == 0
        assert stats.faults_handled == 0

        region[PAGE]
        stats = region.debug_stats()
        assert stats.chunks_resolved == 1
        assert stats.faults_handled == 1

        region[PAGE]  # re-reading must not count as another fault
        stats = region.debug_stats()
        assert stats.chunks_resolved == 1
        assert stats.faults_handled == 1

        region[2 * PAGE]
        stats = region.debug_stats()
        assert stats.chunks_resolved == 2
        assert stats.faults_handled == 2

        region.close()
        try:
            region.debug_stats()
        except ValueError:
            return
        raise AssertionError("debug_stats() on a closed region should raise")


def test_boundary_sharing_group():
    # Mirrors test/test-boundary.c: chunks not aligned to page boundaries
    # must resolve together with every other chunk sharing that page.
    counts = [0, 0, 0]
    sizes = [100, 50, PAGE * 2]  # chunk2 starts at 150, mid-page.

    def init_chunk(chunk, buf):
        counts[chunk] += 1
        buf[:] = bytes([ord('a') + chunk]) * len(buf)

    with faultcache.Pool() as pool:
        region = pool.Region(sizes, init_chunk)
        assert region[120] == ord('b')  # inside chunk1
        # chunk2 starts within the same page group and gets pulled in too.
        assert counts == [1, 1, 1]


def test_slice_step():
    def init_chunk(chunk, buf):
        buf[:] = bytes(range(len(buf)))

    with faultcache.Pool() as pool:
        region = pool.Region([16], init_chunk)
        assert region[0:16:2] == bytes(range(0, 16, 2))


def test_pool_close_closes_regions():
    def init_chunk(chunk, buf):
        buf[:] = b"\0" * len(buf)

    pool = faultcache.Pool()
    region = pool.Region([PAGE], init_chunk)
    assert not region.closed
    pool.close()
    assert region.closed
    assert pool.closed


def test_write_is_fatal():
    def init_chunk(chunk, buf):
        buf[:] = b"X" * len(buf)

    with faultcache.Pool() as pool:
        region = pool.Region([PAGE], init_chunk)
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
    def init_chunk(chunk, buf):
        buf[:] = b"Z" * len(buf)

    pool = faultcache.Pool()
    region = pool.Region([PAGE], init_chunk)
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


def main():
    tests = [
        test_pool_maxsize_not_implemented,
        test_basic_region,
        test_debug_stats,
        test_boundary_sharing_group,
        test_slice_step,
        test_pool_close_closes_regions,
        test_write_is_fatal,
        test_access_after_unmap_is_fatal,
    ]
    for t in tests:
        t()
        print(f"ok: {t.__name__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
