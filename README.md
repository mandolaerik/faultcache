# faultcache

Transparent, lazy-loading memory cache for cheaply-derived content.

Imagine a huge spreadsheet with fifty columns, stored on disk one column at a
time with each column compressed on its own — the same trick columnar file
formats like parquet use so that reading two of the fifty columns doesn't mean
decompressing (or even reading) the other forty-eight. `faultcache` lets you ask
for one contiguous address range covering the *whole decompressed table* up
front, and only pay to decompress each column the first time it's actually read
— touching column 3 never touches column 47. To the rest of your program, the
whole table just looks like ordinary memory once it's been touched.

This is the same idea as a read-only `mmap` of a file, taken one step further.
`mmap` already gives you an address range that's lazily backed by a file's
*existing* bytes — page 3 isn't read off disk until something touches it.
`faultcache` lazily backs an address range with *derived* bytes instead,
computed by your own callback the first time they're touched, for content that
doesn't already exist as a flat byte range on disk to be paged in as-is.

`fc_region_create()` reserves a contiguous, read-only address range split
into **chunks**. No chunk is populated until it is first touched: reading
any byte inside it triggers a page fault, which faultcache resolves
inline by calling a user-supplied `fill_chunk()` callback that derives
just that chunk's bytes. From then on the chunk behaves like ordinary
`mmap(PROT_READ)` memory — reads are free, writes fault fatally.

This is useful whenever content is cheap to *derive* but wasteful to
materialize eagerly or keep resident forever: decompressing blocks of a
compressed file only as they're actually read, decoding one column of a
columnar file format on demand, etc. The `examples/` directory has
worked examples for both.

## How it works

- **Linux**: `mmap(PROT_NONE)` + a process-wide `SIGSEGV` handler.
  Touching an unresolved chunk faults synchronously on the accessing
  thread, which resolves it inline (via a scratch mapping installed
  atomically with `mremap(MREMAP_FIXED)`) before retrying the access.
- **Windows** (mingw-w64 only): the same in-process model, built on
  `VirtualAlloc2`/`MapViewOfFile3` placeholder mappings instead of
  `mmap`/`mremap`. Views can only land on 64K allocation-granularity
  boundaries rather than a page boundary, so faults are coarser than on
  Linux; nothing else changes. See Limitations below for what's not
  ported yet.

## Features

- **Chunked regions**: a region is a flat list of independently-lazy
  chunks (`fc_region_create(pool, nchunks, chunk_sizes, fill_chunk,
  user_data)`). Touching any byte of a chunk resolves that whole chunk
  (and any neighbor forced in by a shared, non-page-aligned boundary),
  never more.
- **Basic LRU based cache eviction**: When resident chunks exceed a user-defined
  threshold, old chunks are evicted from the cache. Some trickery is done to
  ensure cold chunks are evicted before hot chunks.
- **Performance**: What sets this library apart is that performance lands in the
  same ballpark as a read-only `mmap` view of a file: The first access within a
  page costs an exception, but accessing the bytes of a region is free once the
  surrounding chunk is loaded. A re-implementation of `mmap` through
  `faultcache` would give worse performance than vanilla `mmap` because we are
  limited to userspace APIs; however, by most metrics `faultcache` with
  `maxsize=N` should not be worse off than a corresponding `mmap` utilizing
  `N*2` bytes of shared memory.
- **In-process backend**: content is derived by your own callback,
  running inline in the same process — no IPC, no extra process.
- **Client/server backend** (Linux-only, **experimental**): a pool whose
  faults are resolved by a separate server process over `userfaultfd(2)`
  + `SCM_RIGHTS` (see `client.h`/`server.h`).
  Content derivation is keyed by a caller-defined opaque descriptor, so
  multiple client processes handing off the same descriptor share one
  underlying resolved region instead of re-deriving it independently.
- **Python bindings** (`python/faultcache`): a small `ctypes`-based
  binding mirroring the `mmap` module's shape (`Pool`, `Region`,
  slicing, a zero-copy `Region.view()`), plus a mandatory compiled
  callback shim (`_faultcache`) that makes the GC/reentrancy semantics of
  running `fill_chunk` inside a signal handler safe from Python.
- **OS support**: Linux is the primary, fully-supported platform (both
  backends). Windows (mingw-w64 only) runs the in-process backend; the
  client/server backend is Linux-only by design (see Limitations).

## Requirements

- A C compiler (GCC or Clang) and [Meson](https://mesonbuild.com/) + Ninja.
  On Windows, mingw-w64 is currently the only supported toolchain.
- Linux with a kernel supporting `userfaultfd(2)` if you want the
  client/server backend (the in-process backend needs nothing special,
  and is what's available on Windows).
- Python 3.10+ with development headers, only if you want the Python
  bindings (optional — the C library builds fine without them).

## Building

Build everything in multiple ways, and run all regression tests:
```sh
python3 scripts/ci.sh all
```

To build only the C library:

```sh
meson setup build
meson test -C build
```

Useful variants:

```sh
# ASan/UBSan
meson setup build-asan -Db_sanitize=address,undefined
meson test -C build-asan

# Line coverage (gcovr)
meson setup build-coverage -Db_coverage=true
meson test -C build-coverage
ninja -C build-coverage coverage-html   # or: gcovr -r . build-coverage

# Build and test the Python bindings against a specific interpreter
meson setup build-py3.12 -Dpython=/usr/bin/python3.12
meson test -C build-py3.12
```

`meson install` installs the shared library, pkg-config file, and the
public headers (`faultcache.h`, `client.h`,
`server.h`).

## Using it from C

```c
#include <faultcache/faultcache.h>

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    /* derive this chunk's `size` bytes into `start`, e.g. decompress
     * the corresponding block of a compressed file */
}

int main(void) {
    fc_init();  /* arm the fault handler once, before any region exists */

    fc_pool_t *pool = fc_pool_create(0);  /* 0 = unbounded */
    size_t sizes[] = {4096, 4096, 4096};
    fc_region_t *region = fc_region_create(pool, 3, sizes, fill_chunk, nullptr);

    const unsigned char *base = fc_region_base(region);
    unsigned char first_byte = base[0];  /* faults chunk 0 into existence */

    fc_region_destroy(region);
    fc_pool_destroy(pool);
}
```

Link against `faultcache` (via pkg-config:
`pkg-config --cflags --libs faultcache`, or `faultcache_dep` if
you're consuming this as a Meson subproject).

## Using it from Python

```python
import faultcache

def fill_chunk(chunk: int, buf: memoryview):
    buf[:] = ...  # derive this chunk's bytes into buf (a writable memoryview)

# maxsize=0 (default) is unbounded
pool = faultcache.Pool()
# three chunks: [0:10000], [10000:30000], [30000:70000]
region = pool.create_region([10000, 20000, 40000], fill_chunk)
# returns bytes, only touches/fills chunks 0 and 1
region[5000:15000]
# returns zero-copy memoryview: does not touch/fill any chunks until
# view[N] is read
view = region.view(5000, 15000)
```

See the docstrings in `python/faultcache/__init__.py` for the full
contract `fill_chunk` must honor (it runs inside a signal handler) and
`Region.view()`'s lifetime caveats.

## Examples

- `examples/compressed_chunks/` — lazily decompresses blocks of a gzip
  file as they're read.
- `examples/parquet_column/` — lazily decodes one column of a Parquet
  file on access (`lazy_column.py`), verified in
  `test/test_parquet_example.py` against files written by `pyarrow`
  (opt-in: `-Dpyarrow=true`).

## Limitations / TODO

- **Windows support covers the in-process backend only.** mingw-w64 is
  the only targeted toolchain (not MSVC/clang-cl). The client/server
  backend has no Windows equivalent (`userfaultfd`/`SCM_RIGHTS` are
  Linux-specific) and isn't built there. Some POSIX-only C tests
  (`boundary`, `readonly`, `unmap`, `misuse`) haven't been ported yet, so
  those edge cases are less exercised on Windows than on Linux. The
  Python bindings aren't wired up for Windows library discovery yet
  either (`FAULTCACHE_LIBRARY` must point at the built DLL explicitly).
- **The client/server backend is experimental** and Linux-only. It works
  and is tested, but its API/wire protocol may still change.
- **Shared pages are not evicted.** A page shared by multiple chunks
  (non-page-aligned chunk boundaries) is pinned for the region's lifetime once
  resolved — there's no partial-chunk or partial-page eviction. This is only a
  problem if many chunks are misaligned and short. Therefore: Keep your chunks
  page aligned if possible, in particular if your chunks must be short.
- **Cache eviction is chunk-granular and simplistic.** The cache only keeps
  track of usage with chunk granularity, so if only a handful of pages are hot
  within a large chunk, then this keeps the whole chunk resident. (e.g.
  frequency, historical fault counters).
- **Locking is coarse.** Each pool serializes fault resolution (and, for
  bounded pools, eviction) behind one pool-wide lock, not per-region or
  per-chunk.
- **Pool consumes RSS memory.** A plain read-only `mmap` of a regular file is
  backed by the kernel's page cache or shared memory (`Cached` or `Shmem` in
  `/proc/meminfo`), which is reclaimable under memory pressure and shared across
  every process mapping that file. `faultcache`'s anonymous scratch/resident
  pages are ordinary private RSS instead: not reclaimable by the kernel and not
  shared with other processes. Two alternatives worth considering:
  - Dynamically adjust pool size depending on overall memory load
  - Instead of filling an in-memory anonymous `mmap` block, we could let each
    resident chunk be represented by an actual file in `/tmp`; this way, we
    could use an actual read-only `mmap` of that file. This would give us some
    of the benefits of `mmap`: memory consumption would land in the
    (reclaimable) page cache instead of private RSS, and we could get a more
    reliable recency heuristic through `mincore()`. This would however come at
    the expense of disk space consumption, which can be a real problem on
    systems where `/tmp` has limited capacity.
- **Debug/introspection API is unstable.** `debug.h` is
  build-tree-only (not installed), exists for tests, and its shape may
  change without notice.

## License

MPL-2.0 — see [LICENSE](LICENSE).
