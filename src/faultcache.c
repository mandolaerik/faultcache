/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE
#include "faultcache/faultcache.h"
#include "faultcache/faultcache_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

struct fc_region {
    struct fc_region *next;
    struct fc_pool *pool; /* back-pointer, for locking debug counters below */

    void *base;
    size_t total_size;  /* exact sum(chunk_sizes), reported by fc_region_size() */
    size_t mapped_size; /* total_size rounded up to a whole number of pages */
    size_t page_size;

    uint32_t nchunks;
    size_t *chunk_start;   /* prefix sums, nchunks+1 entries */
    bool *initialized;     /* nchunks entries, handler thread only */

    fc_init_chunk_fn_t init_chunk;
    void *user_data;

    int uffd;
    int wake_fd;   /* eventfd used to ask the handler thread to stop */
    pthread_t thread;

    /* Debug-only introspection (see faultcache_debug.h), guarded by
     * pool->lock rather than by the handler thread's usual lock-free
     * ownership of initialized[], since these are read cross-thread. */
    uint32_t chunks_resolved;
    uint32_t faults_handled;
};

struct fc_pool {
    pthread_mutex_t lock;
    struct fc_region *regions;
};

fc_pool_t *fc_pool_create(void) {
    struct fc_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

static void region_teardown(struct fc_region *r) {
    uint64_t one = 1;
    ssize_t unused = write(r->wake_fd, &one, sizeof(one));
    (void)unused;
    pthread_join(r->thread, NULL);

    close(r->wake_fd);
    close(r->uffd); /* implicitly unregisters the range */
    munmap(r->base, r->mapped_size);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
}

void fc_pool_destroy(fc_pool_t *pool) {
    if (!pool)
        return;
    while (pool->regions) {
        struct fc_region *r = pool->regions;
        pool->regions = r->next;
        region_teardown(r);
    }
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

static void pool_add(struct fc_pool *pool, struct fc_region *r) {
    pthread_mutex_lock(&pool->lock);
    r->next = pool->regions;
    pool->regions = r;
    pthread_mutex_unlock(&pool->lock);
}

static struct fc_region *pool_remove(struct fc_pool *pool, const void *addr) {
    pthread_mutex_lock(&pool->lock);
    struct fc_region **p = &pool->regions;
    while (*p) {
        if ((*p)->base == addr) {
            struct fc_region *found = *p;
            *p = found->next;
            pthread_mutex_unlock(&pool->lock);
            return found;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static struct fc_region *pool_find(struct fc_pool *pool, const void *addr) {
    pthread_mutex_lock(&pool->lock);
    struct fc_region *r = pool->regions;
    while (r && r->base != addr)
        r = r->next;
    pthread_mutex_unlock(&pool->lock);
    return r;
}

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
 * Resolve a single page-fault. Because chunk boundaries need not be
 * page-aligned, a page can straddle two (or more) chunks; whenever that
 * happens every chunk touching the affected pages is initialized and
 * committed together in one UFFDIO_COPY, so no chunk is ever left
 * half-initialized on a shared page. Only ever called from the region's
 * single handler thread, so `initialized[]` needs no locking.
 */
static void resolve_fault(struct fc_region *r, size_t fault_off) {
    uint32_t c0 = find_chunk(r, fault_off);
    if (r->initialized[c0])
        return; /* stale duplicate fault notification */

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
    /* zeroed so that any tail padding past the last chunk (or bytes not
     * covered by any chunk on a shared boundary page) reads back as 0. */
    void *buf = calloc(1, buf_len);
    if (!buf)
        return; /* faulting thread stays blocked; nothing sane to do here */

    uint32_t newly_resolved = 0;
    for (uint32_t i = lo; i <= hi; i++) {
        if (r->initialized[i])
            continue;
        size_t chunk_size = r->chunk_start[i + 1] - r->chunk_start[i];
        void *dst = (char *)buf + (r->chunk_start[i] - page_lo);
        r->init_chunk(i, dst, chunk_size, r->user_data);
        r->initialized[i] = true;
        newly_resolved++;
    }

    pthread_mutex_lock(&r->pool->lock);
    r->chunks_resolved += newly_resolved;
    r->faults_handled++;
    pthread_mutex_unlock(&r->pool->lock);

    struct uffdio_copy copy = {
        .dst = (unsigned long)((char *)r->base + page_lo),
        .src = (unsigned long)buf,
        .len = buf_len,
        .mode = 0,
    };
    if (ioctl(r->uffd, UFFDIO_COPY, &copy) < 0 && errno != EEXIST) {
        /* Nothing better to do: leave the faulting thread blocked rather
         * than risk handing back undefined memory. */
    }

    free(buf);
}

static void *handler_thread_main(void *arg) {
    struct fc_region *r = arg;
    struct pollfd pfds[2] = {
        {.fd = r->uffd, .events = POLLIN},
        {.fd = r->wake_fd, .events = POLLIN},
    };

    for (;;) {
        int n = poll(pfds, 2, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[1].revents & POLLIN)
            break; /* fc_region_destroy() asked us to stop */
        if (!(pfds[0].revents & POLLIN))
            continue;

        struct uffd_msg msg;
        ssize_t rd = read(r->uffd, &msg, sizeof(msg));
        if (rd < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            break;
        }
        if (rd != sizeof(msg) || msg.event != UFFD_EVENT_PAGEFAULT)
            continue;

        size_t fault_addr = (size_t)msg.arg.pagefault.address;
        size_t fault_off = fault_addr - (size_t)r->base;
        resolve_fault(r, fault_off);
    }
    return NULL;
}

const void *fc_region_create(fc_pool_t *pool, uint32_t nchunks,
                              const size_t *chunk_sizes,
                              fc_init_chunk_fn_t init_chunk, void *user_data) {
    if (!pool || nchunks == 0 || !chunk_sizes || !init_chunk) {
        errno = EINVAL;
        return NULL;
    }

    size_t total_size = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        if (chunk_sizes[i] == 0 || total_size + chunk_sizes[i] < total_size) {
            errno = EINVAL;
            return NULL;
        }
        total_size += chunk_sizes[i];
    }

    struct fc_region *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    r->chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    r->initialized = calloc(nchunks, sizeof(bool));
    if (!r->chunk_start || !r->initialized)
        goto fail_alloc;

    size_t acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        r->chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    r->chunk_start[nchunks] = acc;

    r->total_size = total_size;
    r->nchunks = nchunks;
    r->page_size = (size_t)sysconf(_SC_PAGESIZE);
    r->init_chunk = init_chunk;
    r->user_data = user_data;
    r->uffd = -1;
    r->wake_fd = -1;

    r->mapped_size = page_ceil(total_size, r->page_size);

    r->base = mmap(NULL, r->mapped_size, PROT_READ,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r->base == MAP_FAILED)
        goto fail_alloc;

    r->uffd = (int)syscall(SYS_userfaultfd,
                            O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (r->uffd < 0)
        goto fail_map;

    struct uffdio_api api = {.api = UFFD_API, .features = 0};
    if (ioctl(r->uffd, UFFDIO_API, &api) < 0)
        goto fail_uffd;

    struct uffdio_register reg = {
        .range = {.start = (unsigned long)r->base, .len = r->mapped_size},
        .mode = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl(r->uffd, UFFDIO_REGISTER, &reg) < 0)
        goto fail_uffd;

    r->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (r->wake_fd < 0)
        goto fail_register;

    r->pool = pool;
    if (pthread_create(&r->thread, NULL, handler_thread_main, r) != 0)
        goto fail_wakefd;

    pool_add(pool, r);
    return r->base;

fail_wakefd:
    close(r->wake_fd);
fail_register:
    ioctl(r->uffd, UFFDIO_UNREGISTER, &reg.range);
fail_uffd:
    close(r->uffd);
fail_map:
    munmap(r->base, r->mapped_size);
fail_alloc: {
    int saved_errno = errno ? errno : ENOMEM;
    free(r->chunk_start);
    free(r->initialized);
    free(r);
    errno = saved_errno;
    return NULL;
}
}

int fc_region_destroy(fc_pool_t *pool, const void *addr) {
    if (!pool) {
        errno = EINVAL;
        return -1;
    }
    struct fc_region *r = pool_remove(pool, addr);
    if (!r) {
        errno = EINVAL;
        return -1;
    }
    region_teardown(r);
    return 0;
}

size_t fc_region_size(fc_pool_t *pool, const void *addr) {
    if (!pool)
        return 0;
    struct fc_region *r = pool_find(pool, addr);
    return r ? r->total_size : 0;
}

int fc_region_debug_stats(fc_pool_t *pool, const void *addr,
                          struct fc_region_debug_stats *out) {
    if (!pool || !out) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);
    struct fc_region *r = pool->regions;
    while (r && r->base != addr)
        r = r->next;
    if (!r) {
        pthread_mutex_unlock(&pool->lock);
        errno = EINVAL;
        return -1;
    }
    out->nchunks = r->nchunks;
    out->chunks_resolved = r->chunks_resolved;
    out->faults_handled = r->faults_handled;
    pthread_mutex_unlock(&pool->lock);
    return 0;
}
