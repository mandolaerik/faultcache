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
    uint32_t faults_handled;  /* UFFDIO_COPY commits done so far */
};

/*
 * Fills *out with a snapshot of region's current fault-handling state.
 * Never triggers a fault itself. Safe to call from any thread.
 * Returns 0 on success, -1 (errno = EINVAL) if region is not currently
 * registered with pool.
 */
int fc_region_debug_stats(fc_pool_t *pool, fc_region_t region,
                                 struct fc_region_debug_stats *out);

#ifdef __cplusplus
}
#endif

#endif
