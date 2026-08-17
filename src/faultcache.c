/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE
#include "faultcache/faultcache.h"
#include "faultcache/faultcache_debug.h"
#include "faultcache/faultcache_remote.h"
#include "faultcache_wire.h"

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
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

struct fc_region {
    struct fc_region *next;
    struct fc_pool_impl *pool; /* back-pointer, for locking debug counters below */

    void *base;
    size_t total_size;  /* exact sum(chunk_sizes), reported by *_region_size() */
    size_t mapped_size; /* total_size rounded up to a whole number of pages */
    size_t page_size;

    uint32_t nchunks;
    size_t *chunk_start;   /* prefix sums, nchunks+1 entries */
    bool *initialized;     /* nchunks entries, handler thread only */

    fc_init_chunk_fn_t init_chunk;
    void *user_data;

    int memfd;     /* shared backing storage for `base`, MAP_SHARED */
    int uffd;
    int wake_fd;   /* eventfd used to ask the handler thread to stop */
    pthread_t thread;

    /* Debug-only introspection (see faultcache_debug.h), guarded by
     * pool->lock rather than by the handler thread's usual lock-free
     * ownership of initialized[], since these are read cross-thread. */
    uint32_t chunks_resolved;
    uint32_t faults_handled;
};

/*
 * Shared region-tracking bookkeeping for both in-process and client
 * pools -- identical mechanics either way (a mutex plus a linked list of
 * regions), but wrapped in the two distinct fc_pool/fc_client_pool tags
 * below so the two APIs can never be mixed up at compile time, even
 * though their implementation is one and the same.
 */
struct fc_pool_impl {
    pthread_mutex_t lock;
    struct fc_region *regions;
};

struct fc_pool {
    struct fc_pool_impl impl;
};

struct fc_client_pool {
    struct fc_pool_impl impl;
};

static void pool_impl_init(struct fc_pool_impl *impl) {
    pthread_mutex_init(&impl->lock, NULL);
    impl->regions = NULL;
}

static void region_teardown(struct fc_region *r);

static void pool_impl_teardown(struct fc_pool_impl *impl) {
    while (impl->regions) {
        struct fc_region *r = impl->regions;
        impl->regions = r->next;
        region_teardown(r);
    }
    pthread_mutex_destroy(&impl->lock);
}

fc_pool_t *fc_pool_create(void) {
    struct fc_pool *pool = malloc(sizeof(*pool));
    if (!pool)
        return NULL;
    pool_impl_init(&pool->impl);
    return pool;
}

fc_client_pool_t *fc_client_pool_create(void) {
    struct fc_client_pool *pool = malloc(sizeof(*pool));
    if (!pool)
        return NULL;
    pool_impl_init(&pool->impl);
    return pool;
}

static void region_teardown(struct fc_region *r) {
    /* Remote regions (fc_client_region_create()) have no local handler
     * thread or wake_fd: their faults are serviced by a server process
     * instead, via the uffd fd handed off at creation time. */
    if (r->wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t unused = write(r->wake_fd, &one, sizeof(one));
        (void)unused;
        pthread_join(r->thread, NULL);
        close(r->wake_fd);
    }

    close(r->uffd); /* implicitly unregisters the range */
    munmap(r->base, r->mapped_size);
    close(r->memfd);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
}

void fc_pool_destroy(fc_pool_t *pool) {
    if (!pool)
        return;
    pool_impl_teardown(&pool->impl);
    free(pool);
}

void fc_client_pool_destroy(fc_client_pool_t *pool) {
    if (!pool)
        return;
    pool_impl_teardown(&pool->impl);
    free(pool);
}

static void pool_add(struct fc_pool_impl *pool, struct fc_region *r) {
    pthread_mutex_lock(&pool->lock);
    r->next = pool->regions;
    pool->regions = r;
    pthread_mutex_unlock(&pool->lock);
}

static struct fc_region *pool_remove(struct fc_pool_impl *pool, const void *addr) {
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

static struct fc_region *pool_find(struct fc_pool_impl *pool, const void *addr) {
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

fc_region_t fc_region_create(fc_pool_t *pool,
                                          uint32_t nchunks,
                                          const size_t *chunk_sizes,
                                          fc_init_chunk_fn_t init_chunk,
                                          void *user_data) {
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
    r->memfd = -1;

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

    /* memfd + MAP_SHARED (not private anonymous) so a future fork()/spawn()
     * resolver process can be handed a mapping of the same physical pages. */
    r->memfd = memfd_create("faultcache-region", MFD_CLOEXEC);
    if (r->memfd < 0)
        goto fail_alloc;
    if (ftruncate(r->memfd, (off_t)r->mapped_size) < 0)
        goto fail_alloc;

    r->base = mmap(NULL, r->mapped_size, PROT_READ, MAP_SHARED, r->memfd, 0);
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

    r->pool = &pool->impl;
    if (pthread_create(&r->thread, NULL, handler_thread_main, r) != 0)
        goto fail_wakefd;

    pool_add(&pool->impl, r);
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
    if (r->memfd >= 0)
        close(r->memfd);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
    errno = saved_errno;
    return NULL;
}
}

/*
 * Phase 1 of remote region creation: asks the server to resolve
 * `descriptor` into a chunk layout (see faultcache_remote.h -- the
 * server's factory owns chunk layout now, not the caller). On success,
 * returns a malloc'd array of `*out_nchunks` chunk sizes (caller frees
 * with free()); returns NULL on failure (errno set; ECONNREFUSED if the
 * server rejected the descriptor). If `out_error` is non-NULL, *out_error
 * is always set: NULL on success or if no message was available,
 * otherwise a malloc()'d, NUL-terminated string (caller frees with
 * free()) from the server's rejection reply.
 */
static size_t *resolve_descriptor(int server_fd, size_t descriptor_size,
                                   const void *descriptor,
                                   uint32_t *out_nchunks, char **out_error) {
    if (out_error)
        *out_error = NULL;

    size_t total = sizeof(struct fc_resolve_req_hdr) + descriptor_size;
    if (total > FC_WIRE_MSG_MAX) {
        errno = EMSGSIZE;
        return NULL;
    }

    void *msg = malloc(total);
    if (!msg)
        return NULL;
    struct fc_resolve_req_hdr req = {
        .descriptor_size = (uint32_t)descriptor_size,
        .reserved = 0,
    };
    memcpy(msg, &req, sizeof(req));
    if (descriptor_size)
        memcpy((char *)msg + sizeof(req), descriptor, descriptor_size);

    ssize_t sent = send(server_fd, msg, total, 0);
    free(msg);
    if (sent < 0)
        return NULL;

    char *reply_buf = malloc(FC_WIRE_MSG_MAX);
    if (!reply_buf)
        return NULL;
    ssize_t rd = recv(server_fd, reply_buf, FC_WIRE_MSG_MAX, 0);
    if (rd < 0) {
        free(reply_buf);
        return NULL;
    }

    struct fc_resolve_resp_hdr resp;
    bool ok = (size_t)rd >= sizeof(resp);
    if (ok) {
        memcpy(&resp, reply_buf, sizeof(resp));
        ok = resp.status == 0
                 ? resp.nchunks > 0 &&
                       (size_t)rd == sizeof(resp) +
                                         (size_t)resp.nchunks * sizeof(uint64_t)
                 : (size_t)rd == sizeof(resp) + (size_t)resp.error_len;
    }
    if (!ok) {
        free(reply_buf);
        errno = ECONNREFUSED;
        return NULL;
    }
    if (resp.status != 0) {
        if (out_error && resp.error_len) {
            char *msg2 = malloc((size_t)resp.error_len + 1);
            if (msg2) {
                memcpy(msg2, reply_buf + sizeof(resp), resp.error_len);
                msg2[resp.error_len] = '\0';
                *out_error = msg2;
            }
        }
        free(reply_buf);
        errno = ECONNREFUSED;
        return NULL;
    }

    size_t *chunk_sizes = malloc((size_t)resp.nchunks * sizeof(size_t));
    if (!chunk_sizes) {
        free(reply_buf);
        return NULL;
    }
    /* reply_buf + sizeof(resp) isn't guaranteed 8-byte aligned (sizeof(resp)
     * is no longer a multiple of 8 now that it carries error_len too), so
     * copy each size out by value rather than indexing a uint64_t* over it. */
    const char *wire_sizes = reply_buf + sizeof(resp);
    for (uint32_t i = 0; i < resp.nchunks; i++) {
        uint64_t v;
        memcpy(&v, wire_sizes + (size_t)i * sizeof(v), sizeof(v));
        chunk_sizes[i] = (size_t)v;
    }
    free(reply_buf);

    *out_nchunks = resp.nchunks;
    return chunk_sizes;
}

/*
 * Phase 2 of remote region creation: hands off `uffd` (via SCM_RIGHTS)
 * and `base` for the region resolve_descriptor() just agreed on with the
 * server, re-stating the same descriptor bytes so the server can find
 * it. Blocks for the single-byte ack/nack reply. Returns 0 on success,
 * -1 on failure (errno set; ECONNREFUSED if rejected).
 */
static int send_attach(int server_fd, int uffd, const void *base,
                        size_t descriptor_size, const void *descriptor) {
    size_t total = sizeof(struct fc_attach_req_hdr) + descriptor_size;
    if (total > FC_WIRE_MSG_MAX) {
        errno = EMSGSIZE;
        return -1;
    }

    void *msg = malloc(total);
    if (!msg)
        return -1;

    struct fc_attach_req_hdr hdr = {
        .base = (uint64_t)(uintptr_t)base,
        .descriptor_size = (uint32_t)descriptor_size,
        .reserved = 0,
    };
    memcpy(msg, &hdr, sizeof(hdr));
    if (descriptor_size)
        memcpy((char *)msg + sizeof(hdr), descriptor, descriptor_size);

    struct iovec iov = {.iov_base = msg, .iov_len = total};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cbuf,
        .msg_controllen = sizeof(cbuf),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &uffd, sizeof(int));
    mh.msg_controllen = cmsg->cmsg_len;

    ssize_t sent = sendmsg(server_fd, &mh, 0);
    free(msg);
    if (sent < 0)
        return -1;

    uint8_t ack = 1;
    ssize_t rd = recv(server_fd, &ack, 1, 0);
    if (rd != 1 || ack != 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}

fc_client_region_t fc_client_region_create(fc_client_pool_t *pool,
                                            int server_fd,
                                            size_t descriptor_size,
                                            const void *descriptor,
                                            char **out_error) {
    if (!pool || (!descriptor && descriptor_size > 0)) {
        errno = EINVAL;
        return NULL;
    }

    uint32_t nchunks = 0;
    size_t *chunk_sizes = resolve_descriptor(server_fd, descriptor_size,
                                              descriptor, &nchunks,
                                              out_error);
    if (!chunk_sizes)
        return NULL;

    size_t total_size = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        if (chunk_sizes[i] == 0 || total_size + chunk_sizes[i] < total_size) {
            free(chunk_sizes);
            errno = EINVAL;
            return NULL;
        }
        total_size += chunk_sizes[i];
    }

    struct fc_region *r = calloc(1, sizeof(*r));
    if (!r) {
        free(chunk_sizes);
        return NULL;
    }
    r->memfd = -1;

    r->chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    r->initialized = calloc(nchunks, sizeof(bool));
    if (!r->chunk_start || !r->initialized) {
        free(chunk_sizes);
        goto fail_alloc;
    }

    size_t acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        r->chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    r->chunk_start[nchunks] = acc;
    free(chunk_sizes);
    chunk_sizes = NULL;

    r->total_size = total_size;
    r->nchunks = nchunks;
    r->page_size = (size_t)sysconf(_SC_PAGESIZE);
    /* Resolved remotely: no local init_chunk/user_data, and no local
     * handler thread (see the wake_fd == -1 check in region_teardown()). */
    r->init_chunk = NULL;
    r->user_data = NULL;
    r->uffd = -1;
    r->wake_fd = -1;

    r->mapped_size = page_ceil(total_size, r->page_size);

    r->memfd = memfd_create("faultcache-region", MFD_CLOEXEC);
    if (r->memfd < 0)
        goto fail_alloc;
    if (ftruncate(r->memfd, (off_t)r->mapped_size) < 0)
        goto fail_alloc;

    r->base = mmap(NULL, r->mapped_size, PROT_READ, MAP_SHARED, r->memfd, 0);
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

    if (send_attach(server_fd, r->uffd, r->base, descriptor_size,
                     descriptor) < 0) {
        int saved_errno = errno;
        ioctl(r->uffd, UFFDIO_UNREGISTER, &reg.range);
        errno = saved_errno;
        goto fail_uffd;
    }

    r->pool = &pool->impl;
    pool_add(&pool->impl, r);
    return r->base;

fail_uffd:
    close(r->uffd);
fail_map:
    munmap(r->base, r->mapped_size);
fail_alloc: {
    int saved_errno = errno ? errno : ENOMEM;
    if (r->memfd >= 0)
        close(r->memfd);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
    errno = saved_errno;
    return NULL;
}
}

int fc_region_destroy(fc_pool_t *pool, fc_region_t region) {
    if (!pool) {
        errno = EINVAL;
        return -1;
    }
    struct fc_region *r = pool_remove(&pool->impl, region);
    if (!r) {
        errno = EINVAL;
        return -1;
    }
    region_teardown(r);
    return 0;
}

size_t fc_region_size(fc_pool_t *pool, fc_region_t region) {
    if (!pool)
        return 0;
    struct fc_region *r = pool_find(&pool->impl, region);
    return r ? r->total_size : 0;
}

int fc_client_region_destroy(fc_client_pool_t *pool,
                              fc_client_region_t region) {
    if (!pool) {
        errno = EINVAL;
        return -1;
    }
    struct fc_region *r = pool_remove(&pool->impl, region);
    if (!r) {
        errno = EINVAL;
        return -1;
    }
    region_teardown(r);
    return 0;
}

size_t fc_client_region_size(fc_client_pool_t *pool,
                              fc_client_region_t region) {
    if (!pool)
        return 0;
    struct fc_region *r = pool_find(&pool->impl, region);
    return r ? r->total_size : 0;
}

int fc_region_debug_stats(fc_pool_t *pool, fc_region_t region,
                                 struct fc_region_debug_stats *out) {
    if (!pool || !out) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->impl.lock);
    struct fc_region *r = pool->impl.regions;
    while (r && r->base != region)
        r = r->next;
    if (!r) {
        pthread_mutex_unlock(&pool->impl.lock);
        errno = EINVAL;
        return -1;
    }
    out->nchunks = r->nchunks;
    out->chunks_resolved = r->chunks_resolved;
    out->faults_handled = r->faults_handled;
    pthread_mutex_unlock(&pool->impl.lock);
    return 0;
}
