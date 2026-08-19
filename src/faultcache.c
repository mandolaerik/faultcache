/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-debug.h"
#include "faultcache/faultcache-client.h"
#include "faultcache-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

/*
 * The client half of the client/server split (see
 * faultcache-client.h): resolution happens in a separate server
 * process, reached via UFFDIO_COPY over the uffd fd handed off at
 * creation time -- unlike fc_pool_t (src/faultcache-sigsegv.c), a
 * client region has no local handler thread of its own.
 */
struct fc_client_region {
    struct fc_client_region *next;

    void *base;
    size_t total_size;  /* exact sum(chunk_sizes), reported by *_region_size() */
    size_t mapped_size; /* total_size rounded up to a whole number of pages */
    size_t page_size;

    uint32_t nchunks;
    size_t *chunk_start;   /* prefix sums, nchunks+1 entries */
    bool *initialized;     /* nchunks entries */

    int memfd;     /* shared backing storage for `base`, MAP_SHARED */
    int uffd;
};

/* Region-tracking bookkeeping for fc_client_pool_t: a mutex plus a
 * linked list of regions. */
struct fc_pool_impl {
    pthread_mutex_t lock;
    struct fc_client_region *regions;
};

struct fc_client_pool {
    struct fc_pool_impl impl;
};

static void pool_impl_init(struct fc_pool_impl *impl) {
    pthread_mutex_init(&impl->lock, NULL);
    impl->regions = NULL;
}

static void region_teardown(struct fc_client_region *r);

static void pool_impl_teardown(struct fc_pool_impl *impl) {
    while (impl->regions) {
        struct fc_client_region *r = impl->regions;
        impl->regions = r->next;
        region_teardown(r);
    }
    pthread_mutex_destroy(&impl->lock);
}

fc_client_pool_t *fc_client_pool_create(void) {
    struct fc_client_pool *pool = malloc(sizeof(*pool));
    if (!pool)
        return NULL;
    pool_impl_init(&pool->impl);
    return pool;
}

static void region_teardown(struct fc_client_region *r) {
    close(r->uffd); /* implicitly unregisters the range */
    munmap(r->base, r->mapped_size);
    close(r->memfd);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
}

void fc_client_pool_destroy(fc_client_pool_t *pool) {
    if (!pool)
        return;
    pool_impl_teardown(&pool->impl);
    free(pool);
}

static void pool_add(struct fc_pool_impl *pool, struct fc_client_region *r) {
    pthread_mutex_lock(&pool->lock);
    r->next = pool->regions;
    pool->regions = r;
    pthread_mutex_unlock(&pool->lock);
}

static struct fc_client_region *pool_remove(struct fc_pool_impl *pool, const void *addr) {
    pthread_mutex_lock(&pool->lock);
    struct fc_client_region **p = &pool->regions;
    while (*p) {
        if ((*p)->base == addr) {
            struct fc_client_region *found = *p;
            *p = found->next;
            pthread_mutex_unlock(&pool->lock);
            return found;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static struct fc_client_region *pool_find(struct fc_pool_impl *pool, const void *addr) {
    pthread_mutex_lock(&pool->lock);
    struct fc_client_region *r = pool->regions;
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

/*
 * Phase 1 of remote region creation: asks the server to resolve
 * `descriptor` into a chunk layout (see faultcache-server.h -- the
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

    struct fc_client_region *r = calloc(1, sizeof(*r));
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
    r->uffd = -1;

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

int fc_client_region_destroy(fc_client_pool_t *pool,
                              fc_client_region_t region) {
    if (!pool) {
        errno = EINVAL;
        return -1;
    }
    struct fc_client_region *r = pool_remove(&pool->impl, region);
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
    struct fc_client_region *r = pool_find(&pool->impl, region);
    return r ? r->total_size : 0;
}
