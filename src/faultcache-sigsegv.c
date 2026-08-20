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
     * rather than per-region/per-chunk), accepted for this first pass;
     * see TODO.md. */
    pthread_mutex_t lock;
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
    uint32_t c0 = find_chunk(r, fault_off);
    /* segv_handler() already re-checks !initialized[c0] under the same
     * held lock right before calling this, with no window for another
     * thread to intervene in between. */
    FC_ASSERT(!r->initialized[c0]);

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
    }

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
 * not needed yet -- see TODO.md).
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

    /* Not one of ours -- a genuine fault. Restore whatever disposition
     * was in effect before we installed ours (rather than silently
     * swallowing real crashes) and chain to it. Flush coverage last,
     * after the chain attempt: from here the process either dies
     * inside the old handler, or returns and the kernel retries the
     * faulting instruction against the now-restored disposition -- either
     * way, no application code runs again, so this is the last chance to
     * flush counters for the lines below (a dump placed before them
     * would miss their counters, since gcov only credits a line once
     * its block is actually entered). */
    sigaction(sig, &g_old_action, nullptr);
    if ((g_old_action.sa_flags & SA_SIGINFO) && g_old_action.sa_sigaction)
        g_old_action.sa_sigaction(sig, info, ucontext);
    /* else: default/ignore disposition is now restored; returning lets
     * the faulting instruction retry, which raises against it for real. */
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
    sigaction(SIGSEGV, &sa, &g_old_action);
}

fc_pool_t *fc_pool_create(void) {
    pthread_once(&g_handler_once, install_handler);

    struct fc_pool *pool = malloc(sizeof(*pool));
    if (!pool)                 /* GCOVR_EXCL_LINE: OOM, needs fault injection */
        return nullptr;           /* GCOVR_EXCL_LINE */
    pthread_mutex_init(&pool->lock, nullptr);
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

fc_region_t *fc_region_create(fc_pool_t *pool, uint32_t nchunks,
                               const size_t *chunk_sizes,
                               fc_fill_chunk_fn_t fill_chunk, const void *user_data) {
    if (!pool || nchunks == 0 || !chunk_sizes || !fill_chunk)
        fc_misuse("fc_region_create: invalid arguments");

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

    r->pool = pool;
    pthread_mutex_lock(&pool->lock);
    region_list_insert(&pool->regions, r);
    pthread_mutex_unlock(&pool->lock);
    return r;
}

const void *fc_region_base(const fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_base: nullptr region");
    return region->base;
}

void fc_region_destroy(fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_destroy: nullptr region");

    struct fc_pool *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    region_list_remove(region);
    pthread_mutex_unlock(&pool->lock);

    region_free(region);
}

size_t fc_region_size(const fc_region_t *region) {
    if (!region)
        fc_misuse("fc_region_size: nullptr region");
    /* total_size is set once at creation, before the handle is ever
     * published to a caller, and never changes again -- no lock needed. */
    return region->total_size;
}

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
