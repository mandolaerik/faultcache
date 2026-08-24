/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * fc_pool_t: in-process, inline self-service fault handling. Each
 * not-yet-resolved chunk range starts out reserved unreadable; touching
 * it raises a fault that is resolved SYNCHRONOUSLY on the very thread
 * that touched it (no separate handler thread, no userfaultfd --
 * contrast with the client/server backend in client.c, which delegates
 * to a separate server process over uffd). How the OS delivers that
 * fault, and how the reservation is made, are the platform's business:
 * see fault.h and vm.h. Nothing below this comment mentions either.
 *
 * Resolving a chunk populates a private scratch mapping first, then
 * atomically installs it over the target range -- never making the
 * range writable and copying into it in place. The in-place approach is
 * racy: the permission change takes effect process-wide instantly, so
 * another thread can observe the page as readable-but-not-yet-written
 * in the gap before the content is actually copied in (empirically
 * confirmed during design -- see repo memory /
 * .github/copilot-notes/faultcache-zerocopy-design.md).
 *
 * Chunk boundaries need not be page-aligned, so a page can hold bytes
 * from several chunks and cannot be installed until all of them have
 * been filled. Each chunk's slice of a shared page is copied into a
 * staging page, and the page is installed when the last contributor
 * arrives -- see shared_page_t.
 *
 * A resident chunk is never touched again by this library, so reads of
 * it are invisible and the LRU queue would only ever record "filled
 * longest ago", not "coldest". To get real recency, a bounded pool
 * splits its queue in half by bytes and takes read access away from
 * everything past that midpoint (the contents stay); the next touch
 * faults, hands the access back and moves the chunk to the front. Only
 * the cold half pays for this, and only once per demotion.
 */
#include "faultcache/faultcache.h"
#include "faultcache/debug.h"
#include "fault.h"
#include "internal.h"
#include "vm.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chunk_lru {
    struct chunk_lru *next;
    struct chunk_lru *prev;
    fc_region_t *region;
    uint32_t chunk;
    bool resident;
    bool cold; /* resident but past the midpoint, so its pages are PROT_NONE */
} chunk_lru_t;

/*
 * A page holding bytes from more than one chunk. Such a page can only be
 * published once every contributing chunk has been filled, so earlier
 * contributions are held in a private staging page until the last one
 * arrives -- that is what keeps a fault on one chunk from dragging its
 * page-sharing neighbours in with it. Once published the page stays
 * resident for the region's lifetime: no single chunk owns it, so no
 * single eviction may take it away.
 */
typedef struct {
    size_t page_off;      /* page-aligned offset within the region */
    uint32_t first_chunk; /* contributors are the range [first, last] */
    uint32_t last_chunk;
    uint32_t staged;      /* contributors whose bytes are in `staging` */
    /* One page; .addr is nullptr before first use and once published. */
    vm_scratch_t staging;
    bool mapped;
} shared_page_t;

/*
 * The one true definition of the public fc_region_t (faultcache.h only
 * declares the tag). `next`/`prev` link it into its owning pool's region
 * list -- an intrusive circular doubly-linked list with a sentinel node
 * (pool->regions itself, see fc_pool_t below), so insertion/removal
 * never needs to special-case an empty list or the ends, and
 * fc_region_destroy() can unlink in O(1) given the handle directly (no
 * scan by address needed, unlike the fault path -- see
 * find_region_locked()).
 */
struct fc_region {
    fc_region_t *next;
    fc_region_t *prev;
    fc_pool_t *pool; /* owning pool, needed to find its lock */

    void *base;
    size_t total_size;  /* exact sum(chunk_sizes), reported by fc_region_size() */
    size_t mapped_size; /* total_size rounded up to a whole number of pages */
    size_t page_size;

    uint32_t nchunks;
    size_t *chunk_start; /* prefix sums, nchunks+1 entries */
    bool *initialized;   /* nchunks entries, guarded by the owning pool's lock */

    shared_page_t *shared_pages; /* nshared entries, sorted by page_off */
    uint32_t nshared;
    /* nchunks entries: has this chunk handed its bytes to its shared
     * page(s)? Staged bytes stay valid across eviction, so a refill must
     * not count itself twice. */
    bool *chunk_staged;

    /* LRU metadata: resident chunks are held in a doubly-linked queue
     * ordered by recency, with a per-chunk persistent fault counter. */
    chunk_lru_t *chunk_lru; /* nchunks entries */
    uint64_t *chunk_faults_total;  /* nchunks entries, survives eviction */
    size_t resident_bytes;
    uint32_t resident_chunks;
    uint64_t fault_events_total;

    fc_fill_chunk_fn_t fill_chunk;
    const void *user_data;

    /* Debug-only introspection (see debug.h), guarded by the
     * owning pool's lock, same as initialized[]. */
    uint32_t chunks_resolved;
    uint32_t faults_handled;
};

struct fc_pool {
    /* Guards both `regions` and fault resolution for every region in
     * this pool -- coarse (one pool-wide critical section per fault
     * rather than per-region/per-chunk), accepted for this first pass. */
    pthread_mutex_t lock;
    size_t target_size;
    size_t resident_bytes;
    /* Pool-global MRU/LRU queue. Nodes are region-owned chunks and may
     * freely interleave chunks from different regions. */
    chunk_lru_t lru_head; /* sentinel; next=MRU, prev=LRU */
    /* Last node of the hot half, i.e. the queue's midpoint by bytes;
     * &lru_head when the hot half is empty. hot_bytes is the size of
     * everything from lru_head.next through midpoint. */
    chunk_lru_t *midpoint;
    size_t hot_bytes;
    /* Sentinel node of the intrusive circular doubly-linked region
     * list: only .next/.prev are ever used on this node itself (all
     * other fields stay zeroed and untouched). Real regions run from
     * regions.next around to regions.prev; an empty pool has both
     * pointing back at &regions. */
    fc_region_t regions;
    fc_pool_t *next_pool; /* guarded by g_pools_lock, not lock */
};

/* Inserts `r` right after sentinel/list-head `head` -- O(1). */
static void region_list_insert(fc_region_t *head, fc_region_t *r) {
    r->next = head->next;
    r->prev = head;
    head->next->prev = r;
    head->next = r;
}

/* Unlinks `r` from whatever list it's in -- O(1), no head needed. */
static void region_list_remove(fc_region_t *r) {
    r->next->prev = r->prev;
    r->prev->next = r->next;
}

/*
 * Process-wide registry of live pools, so the one process-wide fault
 * handler can find which region (if any, across every fc_pool_t) a
 * given faulting address belongs to.
 */
static pthread_mutex_t g_pools_lock = PTHREAD_MUTEX_INITIALIZER;
static fc_pool_t *g_pools = nullptr;

/*
 * True for the duration of resolve_fault_locked() on this thread (set
 * around the call in fault_try_resolve(), below). Lets us detect
 * fill_chunk() touching a not-yet-resolved page of its own or another
 * region while already running -- the resolve in progress holds
 * pool->lock (and g_pools_lock), both plain non-recursive mutexes, so
 * naively recursing into the normal fault path here would deadlock this
 * thread against itself instead of making progress. Per-thread rather
 * than a single process-wide flag since faults on different threads are
 * legitimately concurrent and must not be confused with each other.
 */
static __thread bool g_resolving_fault = false;

static size_t page_floor(size_t x, size_t page_size) {
    return x - (x % page_size);
}

static size_t page_ceil(size_t x, size_t page_size) {
    return page_floor(x + page_size - 1, page_size);
}

/* Binary search for the chunk covering byte offset `off`. */
static uint32_t find_chunk(const fc_region_t *r, size_t off) {
    uint32_t lo = 0, hi = r->nchunks - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (r->chunk_start[mid] <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/*
 * Pages wholly owned by chunk `c`, i.e. everything except a first
 * and/or last page it shares with a neighbour. These are the only pages
 * a fault on `c` may install on its own, and the only ones evicting `c`
 * may take back. The range is empty when `c` fits inside shared pages.
 * The final chunk also owns the padding after it, up to mapped_size.
 */
static void chunk_exclusive_pages(const fc_region_t *r, uint32_t c,
                                  size_t *out_lo, size_t *out_hi) {
    size_t lo = page_ceil(r->chunk_start[c], r->page_size);
    size_t hi = (c + 1 == r->nchunks)
                    ? r->mapped_size
                    : page_floor(r->chunk_start[c + 1], r->page_size);
    *out_lo = lo;
    *out_hi = hi < lo ? lo : hi;
}

static shared_page_t *find_shared_page(const fc_region_t *r,
                                       size_t page_off) {
    uint32_t lo = 0, hi = r->nshared;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (r->shared_pages[mid].page_off < page_off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < r->nshared && r->shared_pages[lo].page_off == page_off)
        return &r->shared_pages[lo];
    return nullptr;
}

/*
 * Whether the page holding `off` can currently be read. Not the same as
 * "its chunk is initialized": a shared page needs every contributor (and
 * once published it stays put even after a contributor is evicted),
 * while a chunk demoted past the midpoint keeps its contents but not its
 * read access.
 */
static bool page_readable(const fc_region_t *r, size_t off) {
    size_t page = page_floor(off, r->page_size);
    const shared_page_t *sp = find_shared_page(r, page);
    if (sp)
        return sp->mapped;
    uint32_t c = find_chunk(r, page);
    return r->initialized[c] && !r->chunk_lru[c].cold;
}

/* Collects the pages holding a chunk boundary that is not page-aligned;
 * several boundaries may land in the same page, which is why this
 * dedupes rather than producing one entry per boundary. */
static void shared_pages_init(fc_region_t *r) {
    uint32_t n = 0;
    size_t prev = SIZE_MAX;
    for (uint32_t i = 1; i < r->nchunks; i++) {
        size_t p = page_floor(r->chunk_start[i], r->page_size);
        if (r->chunk_start[i] == p || p == prev)
            continue;
        prev = p;
        n++;
    }

    r->nshared = n;
    if (n == 0)
        return;
    r->shared_pages = calloc(n, sizeof(*r->shared_pages));
    /* Same survive-it-needs-a-test rationale as the other allocations in
     * fc_region_create(). */
    FC_ASSERT(r->shared_pages != nullptr);

    uint32_t k = 0;
    prev = SIZE_MAX;
    for (uint32_t i = 1; i < r->nchunks; i++) {
        size_t p = page_floor(r->chunk_start[i], r->page_size);
        if (r->chunk_start[i] == p || p == prev)
            continue;
        prev = p;

        size_t last_byte = p + r->page_size - 1;
        if (last_byte >= r->total_size)
            last_byte = r->total_size - 1;
        r->shared_pages[k++] = (shared_page_t) {
            .page_off = p,
            .first_chunk = find_chunk(r, p),
            .last_chunk = find_chunk(r, last_byte),
        };
    }
    FC_ASSERT(k == n);
}

static void lru_init(fc_region_t *r) {
    r->chunk_lru = calloc(r->nchunks, sizeof(*r->chunk_lru));
    r->chunk_faults_total = calloc(r->nchunks, sizeof(*r->chunk_faults_total));
    FC_ASSERT(r->chunk_lru && r->chunk_faults_total);
    r->resident_bytes = 0;
    r->resident_chunks = 0;
    r->fault_events_total = 0;

    for (uint32_t i = 0; i < r->nchunks; i++) {
        r->chunk_lru[i].region = r;
        r->chunk_lru[i].chunk = i;
        r->chunk_lru[i].resident = false;
    }
}

static size_t lru_node_size(const chunk_lru_t *node) {
    const fc_region_t *r = node->region;
    return r->chunk_start[node->chunk + 1] - r->chunk_start[node->chunk];
}

/* Only the chunk's exclusive pages: the ones it shares with a neighbour
 * belong to no single chunk, so no single chunk may hide them. */
static void chunk_set_readable(const fc_region_t *r, uint32_t chunk,
                               bool readable) {
    size_t lo, hi;
    chunk_exclusive_pages(r, chunk, &lo, &hi);
    if (hi > lo)
        vm_set_readable((char *)r->base + lo, hi - lo, readable);
}

static void lru_unlink(fc_pool_t *pool, chunk_lru_t *node) {
    FC_ASSERT(node->resident);
    if (pool->midpoint == node)
        pool->midpoint = node->prev;
    if (!node->cold)
        pool->hot_bytes -= lru_node_size(node);
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = nullptr;
    node->resident = false;
}

static void lru_insert_mru(fc_pool_t *pool, chunk_lru_t *node) {
    FC_ASSERT(!node->resident);
    node->next = pool->lru_head.next;
    node->prev = &pool->lru_head;
    pool->lru_head.next->prev = node;
    pool->lru_head.next = node;
    node->resident = true;
    node->cold = false;
    pool->hot_bytes += lru_node_size(node);
    if (pool->midpoint == &pool->lru_head)
        pool->midpoint = node;
}

/*
 * Walks the midpoint back towards the front until at most half the
 * resident bytes are still readable, taking read access away from every
 * node it passes. The MRU node is never demoted: it is the chunk whose
 * fault is being serviced right now, and hiding it again would just
 * fault straight back in.
 */
static void lru_rebalance(fc_pool_t *pool) {
    /* An unbounded pool never evicts, so it has nothing to learn from
     * paying for these faults. */
    if (pool->target_size == 0)
        return;

    while (pool->hot_bytes > pool->resident_bytes / 2
           && pool->midpoint != pool->lru_head.next) {
        chunk_lru_t *node = pool->midpoint;
        chunk_set_readable(node->region, node->chunk, false);
        node->cold = true;
        pool->hot_bytes -= lru_node_size(node);
        pool->midpoint = node->prev;
    }
}

/* Hands a demoted chunk its read access back and makes it the MRU. */
static void lru_promote(fc_pool_t *pool, chunk_lru_t *node) {
    FC_ASSERT(node->cold);
    chunk_set_readable(node->region, node->chunk, true);
    lru_unlink(pool, node);
    lru_insert_mru(pool, node);
}

static void lru_forget_chunk(fc_pool_t *pool, fc_region_t *r,
                             uint32_t chunk) {
    chunk_lru_t *node = &r->chunk_lru[chunk];
    if (!node->resident)
        return;

    size_t chunk_size = lru_node_size(node);
    lru_unlink(pool, node);
    r->initialized[chunk] = false;
    r->resident_bytes -= chunk_size;
    r->resident_chunks--;
    pool->resident_bytes -= chunk_size;
}

static void lru_evict_chunk(fc_pool_t *pool, fc_region_t *r,
                            uint32_t chunk) {
    lru_forget_chunk(pool, r, chunk);

    size_t lo, hi;
    chunk_exclusive_pages(r, chunk, &lo, &hi);
    if (hi == lo)
        return;

    vm_decommit((char *)r->base + lo, hi - lo);
}

static void evict_until_within_budget(fc_pool_t *pool,
                                      fc_region_t *protected_region,
                                      uint32_t protected_lo,
                                      uint32_t protected_hi) {
    if (pool->target_size == 0)
        return;

    while (pool->resident_bytes > pool->target_size) {
        chunk_lru_t *victim = pool->lru_head.prev;
        /* Nonzero resident bytes means at least one queued chunk. */
        FC_ASSERT(victim != &pool->lru_head);
        if (victim->region == protected_region
            && victim->chunk >= protected_lo && victim->chunk <= protected_hi)
            break;
        lru_evict_chunk(pool, victim->region, victim->chunk);
    }
}

static void install_shared_page(fc_region_t *r, shared_page_t *sp) {
    vm_scratch_seal(&sp->staging);
    vm_scratch_publish(&sp->staging, 0, r->page_size,
                       (char *)r->base + sp->page_off);
    sp->staging.addr = nullptr;
    sp->mapped = true;
}

/*
 * Copies chunk `c`'s slice of shared page `sp` out of `scratch` (which
 * covers [span_lo, ...)) and publishes the page if that was the last
 * missing contributor.
 */
static void stage_into_shared_page(fc_region_t *r,
                                   shared_page_t *sp, uint32_t c,
                                   const void *scratch, size_t span_lo) {
    FC_ASSERT(!sp->mapped);

    if (!sp->staging.addr)
        sp->staging = vm_scratch_create(r->page_size);

    size_t page_end = sp->page_off + r->page_size;
    size_t lo = r->chunk_start[c] > sp->page_off ? r->chunk_start[c]
                                                 : sp->page_off;
    size_t hi = r->chunk_start[c + 1] < page_end ? r->chunk_start[c + 1]
                                                 : page_end;
    memcpy((char *)sp->staging.addr + (lo - sp->page_off),
           (const char *)scratch + (lo - span_lo), hi - lo);

    r->chunk_staged[c] = true;
    sp->staged++;
    if (sp->staged == sp->last_chunk - sp->first_chunk + 1)
        install_shared_page(r, sp);
}

/*
 * Fills chunk `c` into a private scratch mapping, then installs its
 * exclusive pages atomically via mremap(MREMAP_MAYMOVE|MREMAP_FIXED) and
 * hands its slice of any shared boundary page to the staging machinery.
 * Called with the owning pool's lock held.
 */
static void fill_chunk_locked(fc_region_t *r, uint32_t c) {
    fc_pool_t *pool = r->pool;
    size_t start = r->chunk_start[c];
    size_t end = r->chunk_start[c + 1];
    size_t span_lo = page_floor(start, r->page_size);
    size_t span_hi = page_ceil(end, r->page_size);
    size_t span_len = span_hi - span_lo;

    /* Scratch comes zero-filled, so any tail padding past the last chunk
     * (or bytes not covered by any chunk on a shared boundary page)
     * reads back as 0 with no extra work. */
    vm_scratch_t scratch = vm_scratch_create(span_len);

    r->fill_chunk(c, (char *)scratch.addr + (start - span_lo), end - start,
                  r->user_data);
    r->initialized[c] = true;

    bool has_head = start % r->page_size != 0;
    size_t tail_page = page_floor(end, r->page_size);
    /* The last chunk owns the padding after it, so its trailing partial
     * page is exclusive rather than shared. */
    bool has_tail = end % r->page_size != 0 && c + 1 < r->nchunks;
    if (has_head && has_tail && tail_page == span_lo)
        has_tail = false; /* the whole chunk lives inside one shared page */

    /* A shared page cannot be published before every contributor -- this
     * chunk included -- has staged into it, so both of a chunk's shared
     * pages are always still pending on its very first fill, and never
     * want anything from it again afterwards. */
    if (!r->chunk_staged[c]) {
        if (has_head)
            stage_into_shared_page(r, find_shared_page(r, span_lo), c,
                                   scratch.addr, span_lo);
        if (has_tail)
            stage_into_shared_page(r, find_shared_page(r, tail_page), c,
                                   scratch.addr, span_lo);
    }

    /* Seal BEFORE publishing (see the file-level comment) -- the
     * region's public contract is that writes fault fatally, same as a
     * read-only file mapping. */
    vm_scratch_seal(&scratch);

    size_t excl_lo, excl_hi;
    chunk_exclusive_pages(r, c, &excl_lo, &excl_hi);
    if (excl_hi > excl_lo) {
        vm_scratch_publish(&scratch, excl_lo - span_lo, excl_hi - excl_lo,
                           (char *)r->base + excl_lo);
        /* Publishing only took the middle; the shared ends are still
         * sitting in the scratch buffer. */
        if (excl_lo > span_lo)
            vm_scratch_discard(&scratch, 0, excl_lo - span_lo);
        if (excl_hi < span_hi)
            vm_scratch_discard(&scratch, excl_hi - span_lo,
                               span_hi - excl_hi);
    } else {
        vm_scratch_discard(&scratch, 0, span_len);
    }

    r->chunk_faults_total[c]++;
    r->fault_events_total++;
    if (!r->chunk_lru[c].resident) {
        lru_insert_mru(pool, &r->chunk_lru[c]);
        r->resident_bytes += end - start;
        r->resident_chunks++;
        pool->resident_bytes += end - start;
    }
    r->chunks_resolved++;
}

/*
 * Makes the page covering `fault_off` readable again. That means filling
 * the chunk that owns it (or, for a page shared between chunks, every
 * contributor that is still missing, since the page cannot be published
 * half-populated), or -- if the chunk is still populated and merely
 * demoted past the midpoint -- just handing its read access back. Called
 * with `r`'s pool lock already held (kept held for the duration,
 * including while running `fill_chunk` -- see the fc_pool_t comment
 * above).
 */
static void resolve_fault_locked(fc_region_t *r, size_t fault_off) {
    fc_pool_t *pool = r->pool;
    size_t page = page_floor(fault_off, r->page_size);
    shared_page_t *sp = find_shared_page(r, page);
    uint32_t lo, hi;

    /* segv_handler() already re-checks !page_readable() under the same
     * held lock right before calling this, with no window for another
     * thread to intervene in between. */
    if (sp) {
        FC_ASSERT(!sp->mapped);
        lo = sp->first_chunk;
        hi = sp->last_chunk;
        for (uint32_t c = lo; c <= hi; c++) {
            if (!r->initialized[c])
                fill_chunk_locked(r, c);
        }
        /* Every contributor has now staged its slice, so the page is up. */
    } else {
        lo = hi = find_chunk(r, page);
        if (r->initialized[lo]) {
            lru_promote(pool, &r->chunk_lru[lo]);
            r->chunk_faults_total[lo]++;
            r->fault_events_total++;
        } else {
            fill_chunk_locked(r, lo);
        }
    }
    FC_ASSERT(page_readable(r, page));

    evict_until_within_budget(pool, r, lo, hi);
    lru_rebalance(pool);

    r->faults_handled++;
}

/*
 * Still a linear scan: the fault handler only has a faulting address,
 * not a region handle, so there's no way to avoid searching every
 * region of every pool here regardless of the list's O(1)-removal
 * shape (an interval tree would help if this ever shows up as hot;
 * not needed yet.
 */
static fc_region_t *find_region_locked(fc_pool_t *pool,
                                       uintptr_t addr) {
    for (fc_region_t *r = pool->regions.next; r != &pool->regions;
         r = r->next) {
        if (addr >= (uintptr_t)r->base &&
            addr < (uintptr_t)r->base + r->mapped_size)
            return r;
    }
    return nullptr;
}

bool fault_try_resolve(void *fault_addr) {
    uintptr_t addr = (uintptr_t)fault_addr;

    if (g_resolving_fault) {
        /* A fault occurred while already resolving another fault on this
         * thread -- either fill_chunk() touched a not-yet-resolved page
         * (of its own or another region), or something inside it crashed
         * outright. Either way, retrying the normal path here would
         * recurse into locks resolve_fault_locked already holds (plain,
         * non-recursive mutexes) instead of making progress, so treat it
         * as the caller bug it is rather than deadlocking.
         *
         * Tested via test_nested_fault_across_regions_is_fatal
         * (test/misuse.c), which forks and triggers this for real; the
         * child terminates via fc_misuse()'s abort(), which flushes
         * coverage counters itself before dying (see
         * fc_flush_coverage_before_death()). */
        fc_misuse("fault raised while fill_chunk() was still resolving "
                  "another fault -- nested/recursive faults are not "
                  "supported");
        return true; /* GCOVR_EXCL_LINE: only reached if a misuse hook
                      * survives fc_misuse() instead of the default
                      * abort() -- no test installs a surviving hook for
                      * this call site (would mean longjmp'ing out of a
                      * signal handler). */
    }

    pthread_mutex_lock(&g_pools_lock);
    for (fc_pool_t *pool = g_pools; pool; pool = pool->next_pool) {
        pthread_mutex_lock(&pool->lock);
        fc_region_t *r = find_region_locked(pool, addr);
        if (r) {
            size_t fault_off = addr - (uintptr_t)r->base;
            /* An address inside a known region whose page is already
             * readable is not a "chunk not populated yet" fault -- it's
             * a genuine violation (almost always a write to the now
             * read-only chunk). Don't swallow it: report it as not ours
             * so it reaches the crash path, same as an address outside
             * any region. */
            if (!page_readable(r, fault_off)) {
                g_resolving_fault = true;
                resolve_fault_locked(r, fault_off);
                g_resolving_fault = false;
                pthread_mutex_unlock(&pool->lock);
                pthread_mutex_unlock(&g_pools_lock);
                return true;
            }
        }
        pthread_mutex_unlock(&pool->lock);
    }
    pthread_mutex_unlock(&g_pools_lock);
    return false;
}

/* See fc_init()'s comment in faultcache.h for why the install point is an
 * explicit call rather than a lazy one at first use. */
void fc_init(void) {
    fault_arm();
}

void fc_rearm_handler(void) {
    if (!fault_armed())
        fc_misuse("fc_rearm_handler: fc_init() has not been called -- a "
                  "handler armed before fc_init() ends up below us anyway, "
                  "so there is nothing to displace yet");
    fault_rearm();
}

fc_pool_t *fc_pool_create(size_t target_size) {
    fc_pool_t *pool = malloc(sizeof(*pool));
    if (!pool)                 /* GCOVR_EXCL_LINE: OOM, needs fault injection */
        return nullptr;           /* GCOVR_EXCL_LINE */
    pthread_mutex_init(&pool->lock, nullptr);
    pool->target_size = target_size;
    pool->resident_bytes = 0;
    pool->lru_head.next = pool->lru_head.prev = &pool->lru_head;
    pool->midpoint = &pool->lru_head;
    pool->hot_bytes = 0;
    pool->regions.next = pool->regions.prev = &pool->regions;

    pthread_mutex_lock(&g_pools_lock);
    pool->next_pool = g_pools;
    g_pools = pool;
    pthread_mutex_unlock(&g_pools_lock);
    return pool;
}

static void region_free(fc_region_t *r) {
    vm_release(r->base, r->mapped_size);
    for (uint32_t i = 0; i < r->nshared; i++) {
        if (r->shared_pages[i].staging.addr)
            vm_scratch_discard(&r->shared_pages[i].staging, 0, r->page_size);
    }
    free(r->shared_pages);
    free(r->chunk_staged);
    free(r->chunk_start);
    free(r->initialized);
    free(r->chunk_lru);
    free(r->chunk_faults_total);
    free(r);
}

void fc_pool_destroy(fc_pool_t *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&g_pools_lock);
    fc_pool_t **p = &g_pools;
    while (*p && *p != pool)
        p = &(*p)->next_pool;
    if (*p == pool)
        *p = pool->next_pool;
    pthread_mutex_unlock(&g_pools_lock);

    while (pool->regions.next != &pool->regions) {
        fc_region_t *r = pool->regions.next;
        region_list_remove(r);
        region_free(r);
    }
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

FC_DIAG_PUSH
FC_DIAG_IGNORE_NONNULL_COMPARE
fc_region_t *fc_region_create(fc_pool_t *pool, uint32_t nchunks,
                               const size_t *chunk_sizes,
                               fc_fill_chunk_fn_t fill_chunk, const void *user_data) {
    if (!pool || nchunks == 0 || !chunk_sizes || !fill_chunk)
        fc_misuse("fc_region_create: invalid arguments");
    if (!fault_armed())
        fc_misuse("fc_region_create: fc_init() has not been called -- call it "
                  "from your library's init, before any region exists");

    size_t total_size = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        if (chunk_sizes[i] == 0 || total_size + chunk_sizes[i] < total_size) {
            errno = EINVAL;
            return nullptr;
        }
        total_size += chunk_sizes[i];
    }

    fc_region_t *r = calloc(1, sizeof(*r));
    /* Surviving OOM here would need a real (fault-injected) test to
     * prove; until then, aborting is good enough. */
    FC_ASSERT(r != nullptr);

    r->chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    r->initialized = calloc(nchunks, sizeof(bool));
    r->chunk_staged = calloc(nchunks, sizeof(bool));
    FC_ASSERT(r->chunk_start && r->initialized && r->chunk_staged);

    size_t acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        r->chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    r->chunk_start[nchunks] = acc;

    r->total_size = total_size;
    r->nchunks = nchunks;
    r->page_size = vm_page_size();
    r->fill_chunk = fill_chunk;
    r->user_data = user_data;
    r->mapped_size = page_ceil(total_size, r->page_size);

    r->base = vm_reserve(r->mapped_size);

    shared_pages_init(r);
    lru_init(r);

    r->pool = pool;
    pthread_mutex_lock(&pool->lock);
    region_list_insert(&pool->regions, r);
    pthread_mutex_unlock(&pool->lock);
    return r;
}
FC_DIAG_POP

FC_DIAG_PUSH
FC_DIAG_IGNORE_NONNULL_COMPARE
const void *fc_region_base(const fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_base: nullptr region");
    return region->base;
}
FC_DIAG_POP

FC_DIAG_PUSH
FC_DIAG_IGNORE_NONNULL_COMPARE
void fc_region_destroy(fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_destroy: nullptr region");

    fc_pool_t *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    for (uint32_t i = 0; i < region->nchunks; i++)
        lru_forget_chunk(pool, region, i);
    region_list_remove(region);
    pthread_mutex_unlock(&pool->lock);

    region_free(region);
}
FC_DIAG_POP

FC_DIAG_PUSH
FC_DIAG_IGNORE_NONNULL_COMPARE
size_t fc_region_size(const fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_size: nullptr region");
    /* total_size is set once at creation, before the handle is ever
     * published to a caller, and never changes again -- no lock needed. */
    return region->total_size;
}
FC_DIAG_POP

void fc_region_debug_stats(const fc_region_t *region,
                           struct fc_region_debug_stats *out) {
    if (!region || !out)
        fc_misuse("fc_region_debug_stats: nullptr region or out");

    fc_pool_t *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    out->nchunks = region->nchunks;
    out->chunks_resolved = region->chunks_resolved;
    out->faults_handled = region->faults_handled;
    pthread_mutex_unlock(&pool->lock);
}

void fc_region_debug_lru_stats(const fc_region_t *region,
                               struct fc_region_debug_lru_stats *out) {
    if (!region || !out)
        fc_misuse("fc_region_debug_lru_stats: nullptr region or out");

    fc_pool_t *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    out->resident_bytes = region->resident_bytes;
    out->resident_chunks = region->resident_chunks;
    out->fault_events_total = region->fault_events_total;
    pthread_mutex_unlock(&pool->lock);
}

void fc_pool_debug_lru_queue(const fc_pool_t *pool,
                             struct fc_pool_debug_lru_entry *out_entries,
                             uint32_t max_entries,
                             uint32_t *out_count) {
    if (!pool || !out_entries || !out_count)
        fc_misuse("fc_pool_debug_lru_queue: nullptr input");

    fc_pool_t *pool_mut = (fc_pool_t *)pool;
    pthread_mutex_lock(&pool_mut->lock);
    uint32_t total = 0;
    for (chunk_lru_t *node = pool_mut->lru_head.next;
         node != &pool_mut->lru_head; node = node->next) {
        uint32_t chunk = node->chunk;
        const fc_region_t *region = node->region;
        total++;
        if (total <= max_entries) {
            out_entries[total - 1] = (struct fc_pool_debug_lru_entry) {
                .region = region,
                .chunk = chunk,
                .size = (uint64_t)(region->chunk_start[chunk + 1]
                                   - region->chunk_start[chunk]),
                .faults_total = region->chunk_faults_total[chunk],
            };
        }
    }
    *out_count = total;
    pthread_mutex_unlock(&pool_mut->lock);
}

void fc_region_debug_lru_history(const fc_region_t *region,
                                 struct fc_region_debug_lru_entry *out_entries) {
    if (!region || !out_entries)
        fc_misuse("fc_region_debug_lru_history: nullptr input");

    fc_pool_t *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    for (uint32_t chunk = 0; chunk < region->nchunks; chunk++) {
        out_entries[chunk] = (struct fc_region_debug_lru_entry) {
            .chunk = chunk,
            .size = (uint64_t)(region->chunk_start[chunk + 1] - region->chunk_start[chunk]),
            .faults_total = region->chunk_faults_total[chunk],
            .resident = region->chunk_lru[chunk].resident ? 1 : 0,
            .cold = region->chunk_lru[chunk].cold ? 1 : 0,
        };
    }
    pthread_mutex_unlock(&pool->lock);
}
