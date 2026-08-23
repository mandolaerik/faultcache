/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef FAULTCACHE_DEBUG_H
#define FAULTCACHE_DEBUG_H

/*
 * Introspection into a region's fault-handling activity, for tests and
 * debugging. Not part of the stable API: shape and semantics may change
 * without notice, and it is not installed alongside faultcache.h.
 */

#include "faultcache.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fc_region_debug_stats {
    uint32_t nchunks;         /* total chunks in the region */
    uint32_t chunks_resolved; /* chunks initialized so far */
    uint32_t faults_handled;  /* resolve passes (mremap installs) done so far */
};

struct fc_region_debug_lru_entry {
    uint32_t chunk;
    uint64_t size;
    uint64_t faults_total;
    uint8_t resident;
    uint8_t cold; /* resident, but demoted past the pool's midpoint */
};

struct fc_pool_debug_lru_entry {
    const fc_region_t *region;
    uint32_t chunk;
    uint64_t size;
    uint64_t faults_total;
};

struct fc_region_debug_lru_stats {
    uint64_t resident_bytes;
    uint32_t resident_chunks;
    uint64_t fault_events_total;
};

/*
 * Fills *out with a snapshot of region's current fault-handling state.
 * Never triggers a fault itself. Safe to call from any thread.
 *
 * `region` and `out` must be non-nullptr -- violating that is a caller
 * bug, not a recoverable error, and aborts the process.
 */
void fc_region_debug_stats(const fc_region_t *region,
                           struct fc_region_debug_stats *out);

/* Fills *out with a snapshot of region-local LRU counters. */
void fc_region_debug_lru_stats(const fc_region_t *region,
                               struct fc_region_debug_lru_stats *out);

/*
 * Copies the pool-global resident queue from MRU to LRU into `out_entries`.
 * Each entry is exposed as (region pointer, chunk index).
 * The caller passes `max_entries` as the size of `out_entries`; `*out_count`
 * receives the total number of resident chunks in the pool.
 *
 * `pool`, `out_entries`, and `out_count` must be non-nullptr.
 */
void fc_pool_debug_lru_queue(const fc_pool_t *pool,
                             struct fc_pool_debug_lru_entry *out_entries,
                             uint32_t max_entries,
                             uint32_t *out_count);

/*
 * Copies the per-chunk persistent fault-history table into `out_entries`.
 * `out_entries` must have exactly region->nchunks entries.
 */
void fc_region_debug_lru_history(const fc_region_t *region,
                                 struct fc_region_debug_lru_entry *out_entries);

/*
 * Overrides what happens when the library detects a caller bug (nullptr/
 * invalid handle, bad arguments -- see e.g. fc_region_destroy()) across
 * BOTH faultcache.h and faultcache-client.h. By default (hook == nullptr)
 * this prints a diagnostic to stderr and abort()s the process; tests
 * can install a hook to observe/verify a specific misuse case without
 * killing the test binary.
 *
 * The hook must NOT return normally -- every call site assumes control
 * never comes back (it goes on to dereference the very handle that was
 * just rejected) -- so a hook has to divert control flow itself, e.g.
 * via siglongjmp(). If a hook is installed but returns anyway, the
 * library falls back to the default print+abort() as a safety net.
 *
 * Debug-only, not part of the stable API. Not thread-safe (a single
 * process-wide slot) -- for single-threaded test use only.
 */
typedef void (*fc_misuse_hook_t)(const char *what);
void fc_debug_set_misuse_hook(fc_misuse_hook_t hook);

#ifdef __cplusplus
}
#endif

#endif
