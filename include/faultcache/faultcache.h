/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache - transparent cache for cheaply-derived memory content.
 *
 * fc_region_create() reserves a contiguous read-only address range, divided
 * into "chunks", tracked by a pool. No chunk is populated until it is first
 * accessed: touching any byte inside a chunk triggers a page fault that is
 * resolved by invoking the user-supplied init_chunk() callback, which fills
 * that chunk's bytes. Once resolved, a chunk behaves exactly like a page
 * cached by mmap(PROT_READ): subsequent reads are free, and writes are
 * illegal (they fault fatally, just as they would on a read-only file
 * mapping).
 *
 * This is useful for lazily materializing content that is cheap to derive
 * on demand, e.g. decompressing blocks of a compressed file only as they
 * are actually read.
 *
 * Implemented on Linux using userfaultfd(2).
 */
#ifndef FAULTCACHE_H
#define FAULTCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tracks the regions carved out of it. A pool is expected to eventually
 * also own a shared, size-bounded cache of resident chunks (possibly
 * spanning several regions) with LRU eviction; for now it just tracks
 * region lifetimes.
 */
typedef struct fc_pool fc_pool_t;

/* Returns NULL on failure (errno is set). */
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
 * May be called concurrently from a background thread while the calling
 * application is running; it must not access the region itself (that
 * would deadlock), and should be safe to run off the main thread.
 */
typedef void (*fc_init_chunk_fn_t)(uint32_t chunk, void *start, size_t size,
                                    void *user_data);

/*
 * Reserve a read-only, lazily-populated address range made of `nchunks`
 * consecutive chunks whose sizes are given by chunk_sizes[0..nchunks-1],
 * tracked by `pool`.
 *
 * Returns the base address of the mapping (of total size
 * sum(chunk_sizes)) on success, or NULL on failure (errno is set).
 *
 * The returned pointer must be released with fc_region_destroy(), passing
 * the same pool.
 */
const void *fc_region_create(fc_pool_t *pool, uint32_t nchunks,
                              const size_t *chunk_sizes,
                              fc_init_chunk_fn_t init_chunk, void *user_data);

/*
 * Release a mapping previously returned by fc_region_create() on `pool`.
 * Returns 0 on success, -1 on failure (errno is set).
 */
int fc_region_destroy(fc_pool_t *pool, const void *addr);

/* Total size in bytes of a mapping previously returned by fc_region_create()
 * on `pool`, or 0 if addr is not a live region of that pool. */
size_t fc_region_size(fc_pool_t *pool, const void *addr);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_H */
