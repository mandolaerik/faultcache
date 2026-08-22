/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * fc_pool_t: in-process, inline self-service fault handling. Each
 * not-yet-resolved chunk range starts out mmap(PROT_NONE); touching it
 * raises SIGSEGV, caught by a process-wide handler installed on first
 * use, which resolves the fault SYNCHRONOUSLY on the very thread that
 * touched it (no separate handler thread, no userfaultfd -- contrast
 * with the client/server backend in faultcache.c, which delegates to a
 * separate server process over uffd).
 *
 * Resolving a chunk populates a private scratch mapping first, then
 * atomically installs it over the target range via
 * mremap(MREMAP_MAYMOVE|MREMAP_FIXED) -- never mprotect(PROT_READ|WRITE)
 * then memmove() in place. The in-place approach is racy: mprotect takes
 * effect process-wide instantly, so another thread can observe the page
 * as readable-but-not-yet-written in the gap before the content is
 * actually copied in (empirically confirmed during design -- see repo
 * memory / .github/copilot-notes/faultcache-zerocopy-design.md).
 */
#define _GNU_SOURCE
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-debug.h"
#include "faultcache-internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Also the public fc_region_t (see faultcache.h): the header only
 * forward-declares the tag, this is its one true definition. `next`/
 * `prev` link it into its owning pool's region list -- an intrusive
 * circular doubly-linked list with a sentinel node (pool->regions
 * itself, see struct fc_pool below), so insertion/removal never needs
 * to special-case an empty list or the ends, and fc_region_destroy()
 * can unlink in O(1) given the handle directly (no scan by address
 * needed, unlike the fault path -- see find_region_locked()).
 */
struct fc_region;

struct fc_chunk_lru {
    struct fc_chunk_lru *next;
    struct fc_chunk_lru *prev;
    struct fc_region *region;
    uint32_t chunk;
    bool resident;
};

struct fc_region {
    struct fc_region *next;
    struct fc_region *prev;
    struct fc_pool *pool; /* owning pool, needed to find its lock */

    void *base;
    size_t total_size;  /* exact sum(chunk_sizes), reported by fc_region_size() */
    size_t mapped_size; /* total_size rounded up to a whole number of pages */
    size_t page_size;

    uint32_t nchunks;
    size_t *chunk_start; /* prefix sums, nchunks+1 entries */
    bool *initialized;   /* nchunks entries, guarded by the owning pool's lock */

    /* LRU metadata: resident chunks are held in a doubly-linked queue
     * ordered by recency, with a per-chunk persistent fault counter.
     * Chunks that share a page are evicted as one page group. */
    struct fc_chunk_lru *chunk_lru; /* nchunks entries */
    uint64_t *chunk_faults_total;  /* nchunks entries, survives eviction */
    size_t resident_bytes;
    uint32_t resident_chunks;
    uint64_t fault_events_total;

    fc_fill_chunk_fn_t fill_chunk;
    const void *user_data;

    /* Debug-only introspection (see faultcache-debug.h), guarded by the
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
    struct fc_chunk_lru lru_head; /* sentinel; next=MRU, prev=LRU */
    /* Sentinel node of the intrusive circular doubly-linked region
     * list: only .next/.prev are ever used on this node itself (all
     * other fields stay zeroed and untouched). Real regions run from
     * regions.next around to regions.prev; an empty pool has both
     * pointing back at &regions. */
    struct fc_region regions;
    struct fc_pool *next_pool; /* guarded by g_pools_lock, not lock */
};

/* Inserts `r` right after sentinel/list-head `head` -- O(1). */
static void region_list_insert(struct fc_region *head, struct fc_region *r) {
    r->next = head->next;
    r->prev = head;
    head->next->prev = r;
    head->next = r;
}

/* Unlinks `r` from whatever list it's in -- O(1), no head needed. */
static void region_list_remove(struct fc_region *r) {
    r->next->prev = r->prev;
    r->prev->next = r->next;
}

/*
 * Process-wide registry of live pools, so the one process-wide SIGSEGV
 * handler can find which region (if any, across every fc_pool_t) a
 * given faulting address belongs to.
 */
static pthread_mutex_t g_pools_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fc_pool *g_pools = nullptr;

static pthread_once_t g_handler_once = PTHREAD_ONCE_INIT;
static struct sigaction g_old_action;
/* Read without the once-guard, so it must be set inside install_handler(),
 * which pthread_once() publishes to other threads for us. */
static bool g_handler_installed = false;
/* True while this thread is passing a fault down g_old_action. */
static __thread bool g_chaining = false;

/*
 * True for the duration of resolve_fault_locked() on this thread (set
 * around the call in segv_handler(), below). Lets segv_handler() detect
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
static uint32_t find_chunk(const struct fc_region *r, size_t off) {
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

static void chunk_group_bounds(const struct fc_region *r, uint32_t c0,
                               uint32_t *out_lo, uint32_t *out_hi,
                               size_t *out_page_lo, size_t *out_page_hi) {
    uint32_t lo = c0, hi = c0;
    size_t page_lo = page_floor(r->chunk_start[lo], r->page_size);
    size_t page_hi = page_ceil(r->chunk_start[hi + 1], r->page_size);

    while (lo > 0 && r->chunk_start[lo] > page_lo) {
        lo--;
        page_lo = page_floor(r->chunk_start[lo], r->page_size);
    }
    while (hi + 1 < r->nchunks && r->chunk_start[hi + 1] < page_hi) {
        hi++;
        page_hi = page_ceil(r->chunk_start[hi + 1], r->page_size);
    }

    *out_lo = lo;
    *out_hi = hi;
    *out_page_lo = page_lo;
    *out_page_hi = page_hi;
}

static void lru_init(struct fc_region *r) {
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

static void lru_unlink(struct fc_chunk_lru *node) {
    if (!node->resident)
        return;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node->prev = nullptr;
    node->resident = false;
}

static void lru_insert_mru(struct fc_pool *pool, struct fc_chunk_lru *node) {
    if (node->resident)
        lru_unlink(node);
    node->next = pool->lru_head.next;
    node->prev = &pool->lru_head;
    pool->lru_head.next->prev = node;
    pool->lru_head.next = node;
    node->resident = true;
}

static void lru_forget_chunk(struct fc_pool *pool, struct fc_region *r,
                             uint32_t chunk) {
    struct fc_chunk_lru *node = &r->chunk_lru[chunk];
    if (!node->resident)
        return;

    size_t chunk_size = r->chunk_start[chunk + 1] - r->chunk_start[chunk];
    lru_unlink(node);
    r->initialized[chunk] = false;
    r->resident_bytes -= chunk_size;
    r->resident_chunks--;
    pool->resident_bytes -= chunk_size;
}

static void lru_evict_chunk_group(struct fc_pool *pool, struct fc_region *r,
                                  uint32_t c0) {
    uint32_t lo, hi;
    size_t page_lo, page_hi;
    chunk_group_bounds(r, c0, &lo, &hi, &page_lo, &page_hi);

    for (uint32_t i = lo; i <= hi; i++)
        lru_forget_chunk(pool, r, i);

    /* mprotect(PROT_NONE) would hide the pages but leave them charged to
     * RSS; remapping fresh anonymous PROT_NONE pages over the range also
     * frees them, which is the whole point of evicting. */
    FC_ASSERT(mmap((char *)r->base + page_lo, page_hi - page_lo, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
              != MAP_FAILED);
}

static bool lru_chunk_in_group(const struct fc_region *r, uint32_t chunk,
                               uint32_t lo, uint32_t hi) {
    (void)r;
    return chunk >= lo && chunk <= hi;
}

static void evict_until_within_budget(struct fc_pool *pool,
                                      struct fc_region *protected_region,
                                      uint32_t protected_lo,
                                      uint32_t protected_hi) {
    if (pool->target_size == 0)
        return;

    while (pool->resident_bytes > pool->target_size) {
        struct fc_chunk_lru *victim = pool->lru_head.prev;
        if (victim == &pool->lru_head)
            break;
        if (victim->region == protected_region
            && lru_chunk_in_group(victim->region, victim->chunk,
                                  protected_lo, protected_hi))
            break;
        lru_evict_chunk_group(pool, victim->region, victim->chunk);
    }
}

/*
 * Resolves the chunk (and any page-sharing neighbors, since chunk
 * boundaries need not be page-aligned) covering `fault_off` within `r`,
 * atomically installing the result via mremap(). Called with `r`'s
 * pool lock already held (kept held for the duration, including while
 * running `fill_chunk` -- see the struct fc_pool comment above). A
 * no-op if `fault_off` turns out to already be resolved (e.g. another
 * thread won the race for the same chunk/page before this call acquired
 * the lock) -- either way, the caller just returns from the signal
 * handler and lets the faulting access retry.
 */
static void resolve_fault_locked(struct fc_region *r, size_t fault_off) {
    struct fc_pool *pool = r->pool;
    uint32_t c0 = find_chunk(r, fault_off);
    /* segv_handler() already re-checks !initialized[c0] under the same
     * held lock right before calling this, with no window for another
     * thread to intervene in between. */
    FC_ASSERT(!r->initialized[c0]);

    uint32_t lo, hi;
    size_t page_lo, page_hi;
    chunk_group_bounds(r, c0, &lo, &hi, &page_lo, &page_hi);

    size_t buf_len = page_hi - page_lo;
    /* Fresh anonymous pages are already zero-filled by the kernel, so
     * any tail padding past the last chunk (or bytes not covered by any
     * chunk on a shared boundary page) reads back as 0 with no extra
     * work. */
    void *scratch = mmap(nullptr, buf_len, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* Surviving this would need a real (fault-injected) test to prove;
     * until then, aborting is good enough. */
    FC_ASSERT(scratch != MAP_FAILED);

    uint32_t newly_resolved = 0;
    for (uint32_t i = lo; i <= hi; i++) {
        /* The page-sharing extension above is transitive/exhaustive
         * within one call, so a chunk in [lo,hi] can't already be
         * initialized here. */
        FC_ASSERT(!r->initialized[i]);
        size_t chunk_size = r->chunk_start[i + 1] - r->chunk_start[i];
        void *dst = (char *)scratch + (r->chunk_start[i] - page_lo);
        r->fill_chunk(i, dst, chunk_size, r->user_data);
        r->initialized[i] = true;
        newly_resolved++;

        r->chunk_faults_total[i]++;
        r->fault_events_total++;
        if (!r->chunk_lru[i].resident) {
            lru_insert_mru(pool, &r->chunk_lru[i]);
            r->resident_bytes += chunk_size;
            r->resident_chunks++;
            pool->resident_bytes += chunk_size;
        }
    }

    evict_until_within_budget(pool, r, lo, hi);

    /* Drop to read-only BEFORE installing at the target address (see
     * the file-level comment) -- the region's public contract is that
     * writes fault fatally, same as a read-only file mapping. Same
     * survive-it-needs-a-test rationale as the mmap above. */
    FC_ASSERT(mprotect(scratch, buf_len, PROT_READ) == 0);

    FC_ASSERT(mremap(scratch, buf_len, buf_len, MREMAP_MAYMOVE | MREMAP_FIXED,
                      (char *)r->base + page_lo) != MAP_FAILED);

    r->chunks_resolved += newly_resolved;
    r->faults_handled++;
}

/*
 * Still a linear scan: the SIGSEGV handler only has a faulting address,
 * not a region handle, so there's no way to avoid searching every
 * region of every pool here regardless of the list's O(1)-removal
 * shape (an interval tree would help if this ever shows up as hot;
 * not needed yet.
 */
static struct fc_region *find_region_locked(struct fc_pool *pool,
                                             uintptr_t addr) {
    for (struct fc_region *r = pool->regions.next; r != &pool->regions;
         r = r->next) {
        if (addr >= (uintptr_t)r->base &&
            addr < (uintptr_t)r->base + r->mapped_size)
            return r;
    }
    return nullptr;
}

static void segv_handler(int sig, siginfo_t *info, void *ucontext) {
    int saved_errno = errno;
    uintptr_t addr = (uintptr_t)info->si_addr;

    if (g_resolving_fault) {
        /* A fault occurred while already resolving another fault on this
         * thread -- either fill_chunk() touched a not-yet-resolved page
         * (of its own or another region), or something inside it crashed
         * outright. Either way, retrying the normal path here would
         * recurse into locks resolve_fault_locked already holds (plain,
         * non-recursive mutexes) instead of making progress, so treat it
         * as the caller bug it is rather than deadlocking. errno is
         * restored first since fc_misuse()'s default path calls
         * fprintf().
         *
         * Tested via test_nested_fault_across_regions_is_fatal
         * (test-misuse.c), which forks and triggers this for real; the
         * child terminates via fc_misuse()'s abort(), which flushes
         * coverage counters itself before dying (see
         * fc_flush_coverage_before_death()). */
        errno = saved_errno;
        fc_misuse("fault raised while fill_chunk() was still resolving "
                  "another fault -- nested/recursive faults are not "
                  "supported");
        return; /* GCOVR_EXCL_LINE: only reached if a misuse hook survives
                 * fc_misuse() instead of the default abort() -- no test
                 * installs a surviving hook for this call site (would
                 * mean longjmp'ing out of a signal handler). */
    }

    pthread_mutex_lock(&g_pools_lock);
    for (struct fc_pool *pool = g_pools; pool; pool = pool->next_pool) {
        pthread_mutex_lock(&pool->lock);
        struct fc_region *r = find_region_locked(pool, addr);
        if (r) {
            size_t fault_off = addr - (uintptr_t)r->base;
            /* An address inside a known region but already resolved is
             * not a "chunk not populated yet" fault -- it's a genuine
             * violation (almost always a write to the now-read-only
             * chunk). Don't swallow it: fall through to the crash path
             * below, same as an address outside any region. */
            if (!r->initialized[find_chunk(r, fault_off)]) {
                g_resolving_fault = true;
                resolve_fault_locked(r, fault_off);
                g_resolving_fault = false;
                pthread_mutex_unlock(&pool->lock);
                pthread_mutex_unlock(&g_pools_lock);
                errno = saved_errno;
                return; /* retry the faulting instruction */
            }
        }
        pthread_mutex_unlock(&pool->lock);
    }
    pthread_mutex_unlock(&g_pools_lock);
    errno = saved_errno;

    /* Not one of ours -- chain to whatever was installed before us, but
     * do NOT uninstall ourselves to do it. That handler may resolve its
     * own fault and return (another lazy-mapping library, a JIT, a GC
     * write barrier), in which case the retried instruction succeeds and
     * execution continues -- and it has to continue with our regions
     * still working. Restoring is only right for a default/ignore
     * disposition, where there is nothing to call and returning is
     * precisely what lets the kernel act on it.
     *
     * Flush coverage last, after the chain attempt: from here the process
     * either dies inside the old handler, or returns and the kernel
     * retries the faulting instruction -- either way this is the last
     * chance to flush counters for the lines below (a dump placed before
     * them would miss their counters, since gcov only credits a line once
     * its block is actually entered).
     *
     * g_chaining breaks a cycle: after fc_rearm_handler(), a library that
     * had saved us and chains back can route the same fault into us a
     * second time, and following g_old_action again would bounce it
     * between the two until the stack ran out. Reaching here twice for one
     * fault means the chain loops, so end it at the default disposition
     * instead -- a crash, which is what an unhandled fault should be. */
    if (g_chaining) {
        signal(sig, SIG_DFL);
    } else {
        g_chaining = true;
        if (g_old_action.sa_flags & SA_SIGINFO)
            g_old_action.sa_sigaction(sig, info, ucontext);
        else if (g_old_action.sa_handler != SIG_DFL
                 && g_old_action.sa_handler != SIG_IGN)
            g_old_action.sa_handler(sig);
        else
            sigaction(sig, &g_old_action, nullptr);
        g_chaining = false;
    }
    fc_flush_coverage_before_death();
}

static void install_handler(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = segv_handler;
    /* SA_NODEFER: without it, SIGSEGV is auto-blocked for this thread
     * for the handler's duration, and a synchronous fault (e.g.
     * fill_chunk touching another unresolved page) raised while it's
     * blocked can't be delivered to us at all -- the kernel forces the
     * signal's default disposition instead, killing the process with a
     * raw, undiagnosed SIGSEGV. With it, such a fault re-enters this
     * handler, where g_resolving_fault turns it into a clear
     * fc_misuse() diagnostic instead. */
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev;
    sigaction(SIGSEGV, &sa, &prev);
    /* On a re-arm we may already be the installed handler; keeping our
     * own action as the chain target would make the tail call itself. */
    if (!((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == segv_handler))
        g_old_action = prev;
    g_handler_installed = true;
}

/* See fc_init()'s comment in faultcache.h for why the install point is an
 * explicit call rather than a lazy one at first use. */
void fc_init(void) {
    pthread_once(&g_handler_once, install_handler);
}

void fc_rearm_handler(void) {
    fc_init();
    install_handler();
}

fc_pool_t *fc_pool_create(size_t target_size) {
    struct fc_pool *pool = malloc(sizeof(*pool));
    if (!pool)                 /* GCOVR_EXCL_LINE: OOM, needs fault injection */
        return nullptr;           /* GCOVR_EXCL_LINE */
    pthread_mutex_init(&pool->lock, nullptr);
    pool->target_size = target_size;
    pool->resident_bytes = 0;
    pool->lru_head.next = pool->lru_head.prev = &pool->lru_head;
    pool->regions.next = pool->regions.prev = &pool->regions;

    pthread_mutex_lock(&g_pools_lock);
    pool->next_pool = g_pools;
    g_pools = pool;
    pthread_mutex_unlock(&g_pools_lock);
    return pool;
}

static void region_free(struct fc_region *r) {
    munmap(r->base, r->mapped_size);
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
    struct fc_pool **p = &g_pools;
    while (*p && *p != pool)
        p = &(*p)->next_pool;
    if (*p == pool)
        *p = pool->next_pool;
    pthread_mutex_unlock(&g_pools_lock);

    while (pool->regions.next != &pool->regions) {
        struct fc_region *r = pool->regions.next;
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
    if (!g_handler_installed)
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

    struct fc_region *r = calloc(1, sizeof(*r));
    /* Surviving OOM here would need a real (fault-injected) test to
     * prove; until then, aborting is good enough. */
    FC_ASSERT(r != nullptr);

    r->chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    r->initialized = calloc(nchunks, sizeof(bool));
    FC_ASSERT(r->chunk_start && r->initialized);

    size_t acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        r->chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    r->chunk_start[nchunks] = acc;

    r->total_size = total_size;
    r->nchunks = nchunks;
    r->page_size = (size_t)sysconf(_SC_PAGESIZE);
    r->fill_chunk = fill_chunk;
    r->user_data = user_data;
    r->mapped_size = page_ceil(total_size, r->page_size);

    r->base = mmap(nullptr, r->mapped_size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* Same survive-it-needs-a-test rationale as the calloc/malloc above. */
    FC_ASSERT(r->base != MAP_FAILED);

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

    struct fc_pool *pool = region->pool;
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

    struct fc_pool *pool = region->pool;
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

    struct fc_pool *pool = region->pool;
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

    struct fc_pool *pool_mut = (struct fc_pool *)pool;
    pthread_mutex_lock(&pool_mut->lock);
    uint32_t total = 0;
    for (struct fc_chunk_lru *node = pool_mut->lru_head.next;
         node != &pool_mut->lru_head; node = node->next) {
        uint32_t chunk = node->chunk;
        const struct fc_region *region = node->region;
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

    struct fc_pool *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    for (uint32_t chunk = 0; chunk < region->nchunks; chunk++) {
        out_entries[chunk] = (struct fc_region_debug_lru_entry) {
            .chunk = chunk,
            .size = (uint64_t)(region->chunk_start[chunk + 1] - region->chunk_start[chunk]),
            .faults_total = region->chunk_faults_total[chunk],
            .resident = region->chunk_lru[chunk].resident ? 1 : 0,
        };
    }
    pthread_mutex_unlock(&pool->lock);
}
