/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache - transparent cache for cheaply-derived memory content.
 *
 * fc_region_create() reserves a contiguous read-only address range,
 * divided into "chunks", tracked by a pool. No chunk is populated
 * until it is first accessed: touching any byte inside a chunk triggers
 * a page fault that is resolved by invoking the user-supplied
 * fill_chunk() callback, which fills that chunk's bytes. Once resolved,
 * a chunk behaves exactly like a page cached by mmap(PROT_READ):
 * subsequent reads are free, and writes are illegal (they fault fatally,
 * just as they would on a read-only file mapping).
 *
 * This is useful for lazily materializing content that is cheap to derive
 * on demand, e.g. decompressing blocks of a compressed file only as they
 * are actually read.
 *
 * This header covers the in-process API: chunk content is derived
 * in-process by fill_chunk(). See faultcache-client.h/faultcache-server.h
 * for the split client/server variant, where content is derived by a
 * separate server process.
 *
 * Implemented on Linux via mmap(PROT_NONE) + a process-wide SIGSEGV
 * handler: touching an unresolved chunk faults synchronously on the
 * accessing thread, which resolves it inline before retrying the access.
 */
#ifndef FAULTCACHE_H
#define FAULTCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tracks the regions carved out of it (see fc_client_pool_t in
 * faultcache-client.h for the split client/server equivalent). A pool is
 * expected to eventually also own a shared, size-bounded cache of
 * resident chunks (possibly spanning several regions) with LRU eviction;
 * for now it just tracks region lifetimes.
 */
typedef struct fc_pool fc_pool_t;

/* Returns nullptr on failure (errno is set). */
fc_pool_t *fc_pool_create(void);

/* Destroys the pool, tearing down any regions still alive within it. */
void fc_pool_destroy(fc_pool_t *pool);

/*
 * Called once per chunk, the first time any byte of that chunk is
 * accessed. Must fill exactly `size` bytes starting at `start`.
 *
 * `start`/`size` always describe the entire chunk (as given via
 * chunk_sizes[chunk]), never a partial page-aligned fragment of it, even
 * when several chunks are resolved together because they share a page.
 *
 * Called synchronously, inline, on whichever thread first accesses the
 * chunk -- from within the library's SIGSEGV handler, on that thread's
 * own stack. It must not access any unresolved memory of any region
 * (that would recursively fault and deadlock; nested/recursive faults
 * are not yet supported) and should stick to simple, reentrant work,
 * since it runs in a signal handler's context.
 */
typedef void (*fc_fill_chunk_fn_t)(uint32_t chunk, void *start, size_t size,
                                   const void *user_data);

/* Opaque handle to a region created by fc_region_create(), tracked by
 * its owning pool in an intrusive doubly-linked list (see
 * src/faultcache-sigsegv.c) so fc_region_destroy() is O(1). Not a
 * pointer to the region's own memory -- use fc_region_base() for that. */
typedef struct fc_region fc_region_t;

/*
 * Reserve a read-only, lazily-populated address range made of `nchunks`
 * consecutive chunks whose sizes are given by chunk_sizes[0..nchunks-1],
 * tracked by `pool`.
 *
 * `pool`/`chunk_sizes`/`fill_chunk` must be non-nullptr and `nchunks` must
 * be > 0 -- violating that is a caller bug, not a recoverable error,
 * and aborts the process.
 *
 * Returns an opaque handle on success, or nullptr on failure (errno set to
 * EINVAL) if chunk_sizes contains an invalid entry (zero, or one whose
 * running total overflows) -- this can happen even with correct calling
 * code. A resource allocation (malloc()/mmap()) failure currently aborts
 * the process instead of returning nullptr.
 * Use fc_region_base() to get the mapping's base address (of total
 * size sum(chunk_sizes)).
 *
 * The returned handle must be released with fc_region_destroy().
 */
fc_region_t *fc_region_create(fc_pool_t *pool,
                               uint32_t nchunks,
                               const size_t *chunk_sizes,
                               fc_fill_chunk_fn_t fill_chunk,
                               const void *user_data);

/*
 * Release a mapping previously returned by fc_region_create(). `region`
 * must not be used again afterwards (including passing it to
 * fc_region_base()/fc_region_size()): it is freed by this call.
 *
 * `region` must be a valid, live handle -- passing nullptr (or reusing an
 * already-destroyed handle) is a caller bug, not a recoverable error,
 * and aborts the process.
 */
void fc_region_destroy(fc_region_t *region);

/* Total size in bytes of a mapping previously returned by
 * fc_region_create(). `region` must be a valid, live handle (see
 * fc_region_destroy()). */
size_t fc_region_size(const fc_region_t *region);

/* The region's mapped base address (of total size fc_region_size()
 * bytes) -- may be dereferenced/read directly; writes fault fatally.
 * Valid for as long as `region` itself is (i.e. until
 * fc_region_destroy()). `region` must be a valid, live handle. Never
 * fails. */
const void *fc_region_base(const fc_region_t *region);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_H */
