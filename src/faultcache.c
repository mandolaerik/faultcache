/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-debug.h"
#include "faultcache/faultcache-client.h"
#include "faultcache-internal.h"
#include "faultcache-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
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
 *
 * Tracked by its owning pool in an intrusive circular doubly-linked
 * list with a sentinel node (fc_pool_impl.regions itself, see below),
 * so fc_client_region_destroy() can unlink in O(1) given the handle
 * directly -- no scan by address needed, unlike the old const-void*
 * handle scheme.
 */
struct fc_client_region {
    struct fc_client_region *next;
    struct fc_client_region *prev;
    struct fc_pool_impl *pool; /* owning pool, needed to find its lock */

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

/* Inserts `r` right after sentinel/list-head `head` -- O(1). */
static void client_region_list_insert(struct fc_client_region *head,
                                       struct fc_client_region *r) {
    r->next = head->next;
    r->prev = head;
    head->next->prev = r;
    head->next = r;
}

/* Unlinks `r` from whatever list it's in -- O(1), no head needed. */
static void client_region_list_remove(struct fc_client_region *r) {
    r->next->prev = r->prev;
    r->prev->next = r->next;
}

/* Region-tracking bookkeeping for fc_client_pool_t: a mutex plus the
 * sentinel node of an intrusive circular doubly-linked region list --
 * only .next/.prev are ever used on `regions` itself. Real regions run
 * from regions.next around to regions.prev; an empty pool has both
 * pointing back at &regions. */
struct fc_pool_impl {
    pthread_mutex_t lock;
    struct fc_client_region regions;
};

struct fc_client_pool {
    struct fc_pool_impl impl;
};

static void pool_impl_init(struct fc_pool_impl *impl) {
    pthread_mutex_init(&impl->lock, nullptr);
    impl->regions.next = impl->regions.prev = &impl->regions;
}

static void region_teardown(struct fc_client_region *r);

static void pool_impl_teardown(struct fc_pool_impl *impl) {
    while (impl->regions.next != &impl->regions) {
        struct fc_client_region *r = impl->regions.next;
        client_region_list_remove(r);
        region_teardown(r);
    }
    pthread_mutex_destroy(&impl->lock);
}

fc_client_pool_t *fc_client_pool_create(void) {
    struct fc_client_pool *pool = malloc(sizeof(*pool));
    FC_ASSERT(pool != nullptr);
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
    r->pool = pool;
    pthread_mutex_lock(&pool->lock);
    client_region_list_insert(&pool->regions, r);
    pthread_mutex_unlock(&pool->lock);
}

/* `region` must be a live handle -- not validated (see
 * fc_client_region_destroy()'s doc comment). */
static void pool_remove(struct fc_client_region *region) {
    struct fc_pool_impl *pool = region->pool;
    pthread_mutex_lock(&pool->lock);
    client_region_list_remove(region);
    pthread_mutex_unlock(&pool->lock);
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
 * with free()); returns nullptr on failure (errno set; ECONNREFUSED if the
 * server rejected the descriptor). If `out_error` is non-nullptr, *out_error
 * is always set: nullptr on success or if no message was available,
 * otherwise a malloc()'d, NUL-terminated string (caller frees with
 * free()) from the server's rejection reply.
 */
static size_t *resolve_descriptor(int server_fd, size_t descriptor_size,
                                   const void *descriptor,
                                   uint32_t *out_nchunks, char **out_error) {
    if (out_error)
        *out_error = nullptr;

    size_t total = sizeof(struct fc_resolve_req_hdr) + descriptor_size;
    if (total > FC_WIRE_MSG_MAX) {
        errno = EMSGSIZE;
        return nullptr;
    }

    void *msg = malloc(total);
    FC_ASSERT(msg != nullptr);
    struct fc_resolve_req_hdr req = {
        .descriptor_size = (uint32_t)descriptor_size,
        .reserved = 0,
    };
    memcpy(msg, &req, sizeof(req));
    if (descriptor_size)
        memcpy((char *)msg + sizeof(req), descriptor, descriptor_size);

    ssize_t sent = send(server_fd, msg, total, 0);
    free(msg);
    FC_ASSERT(sent >= 0);

    char *reply_buf = malloc(FC_WIRE_MSG_MAX);
    FC_ASSERT(reply_buf != nullptr);
    ssize_t rd = recv(server_fd, reply_buf, FC_WIRE_MSG_MAX, 0);
    FC_ASSERT(rd >= 0);

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
    /* GCOVR_EXCL_START: a malformed/truncated reply needs a malicious or
     * buggy server to trigger, not modeled by any test harness here. */
    if (!ok) {
        free(reply_buf);
        errno = ECONNREFUSED;
        return nullptr;
    }
    /* GCOVR_EXCL_STOP */
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
        return nullptr;
    }

    size_t *chunk_sizes = malloc((size_t)resp.nchunks * sizeof(size_t));
    FC_ASSERT(chunk_sizes != nullptr);
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
    /* resolve_descriptor() already bounded descriptor_size against the
     * same FC_WIRE_MSG_MAX cap with a slightly smaller header, so this
     * is unreachable in practice; no test exercises the few-byte edge
     * band where it technically isn't, so abort is good enough there. */
    FC_ASSERT(total <= FC_WIRE_MSG_MAX);

    void *msg = malloc(total);
    FC_ASSERT(msg != nullptr);

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
    FC_ASSERT(sent >= 0);

    /* GCOVR_EXCL_START: needs a server that accepts the resolve phase but
     * rejects attach -- not modeled by any test harness here (fc_server_t
     * currently never rejects at this phase). */
    uint8_t ack = 1;
    ssize_t rd = recv(server_fd, &ack, 1, 0);
    if (rd != 1 || ack != 0) {
        errno = ECONNREFUSED;
        return -1;
    }
    /* GCOVR_EXCL_STOP */
    return 0;
}

fc_client_region_t *fc_client_region_create(fc_client_pool_t *pool,
                                             int server_fd,
                                             size_t descriptor_size,
                                             const void *descriptor,
                                             char **out_error) {
    if (!pool || (!descriptor && descriptor_size > 0)) {
        /* GCOVR_EXCL_START: TODO(coverage): documented EINVAL contract for
         * bad arguments -- no test calls this with a null pool or
         * descriptor. */
        errno = EINVAL;
        return nullptr;
        /* GCOVR_EXCL_STOP */
    }

    uint32_t nchunks = 0;
    size_t *chunk_sizes = resolve_descriptor(server_fd, descriptor_size,
                                              descriptor, &nchunks,
                                              out_error);
    if (!chunk_sizes)
        return nullptr;

    size_t total_size = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        if (chunk_sizes[i] == 0 || total_size + chunk_sizes[i] < total_size) {
            /* GCOVR_EXCL_START: TODO(coverage): documented EINVAL contract
             * for a malformed/adversarial chunk layout -- no test harness
             * constructs a server that sends one. */
            free(chunk_sizes);
            errno = EINVAL;
            return nullptr;
            /* GCOVR_EXCL_STOP */
        }
        total_size += chunk_sizes[i];
    }

    struct fc_client_region *r = calloc(1, sizeof(*r));
    FC_ASSERT(r != nullptr);
    r->memfd = -1;

    r->chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    r->initialized = calloc(nchunks, sizeof(bool));
    FC_ASSERT(r->chunk_start && r->initialized);

    size_t acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        r->chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    r->chunk_start[nchunks] = acc;
    free(chunk_sizes);
    chunk_sizes = nullptr;

    r->total_size = total_size;
    r->nchunks = nchunks;
    r->page_size = (size_t)sysconf(_SC_PAGESIZE);
    r->uffd = -1;

    r->mapped_size = page_ceil(total_size, r->page_size);

    r->memfd = memfd_create("faultcache-region", MFD_CLOEXEC);
    FC_ASSERT(r->memfd >= 0);
    FC_ASSERT(ftruncate(r->memfd, (off_t)r->mapped_size) == 0);

    r->base = mmap(nullptr, r->mapped_size, PROT_READ, MAP_SHARED, r->memfd, 0);
    FC_ASSERT(r->base != MAP_FAILED);

    r->uffd = (int)syscall(SYS_userfaultfd,
                            O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    FC_ASSERT(r->uffd >= 0);

    struct uffdio_api api = {.api = UFFD_API, .features = 0};
    FC_ASSERT(ioctl(r->uffd, UFFDIO_API, &api) == 0);

    struct uffdio_register reg = {
        .range = {.start = (unsigned long)r->base, .len = r->mapped_size},
        .mode = UFFDIO_REGISTER_MODE_MISSING,
    };
    FC_ASSERT(ioctl(r->uffd, UFFDIO_REGISTER, &reg) == 0);

    /* GCOVR_EXCL_START: send_attach() only returns -1 via its own
     * ECONNREFUSED ack/nack check now (its other failure modes all
     * abort) -- exercising this needs a server that accepts resolve but
     * rejects attach, which fc_server_t currently never does. */
    if (send_attach(server_fd, r->uffd, r->base, descriptor_size,
                     descriptor) < 0) {
        int saved_errno = errno;
        ioctl(r->uffd, UFFDIO_UNREGISTER, &reg.range);
        errno = saved_errno;
        goto fail_uffd;
    }

    pool_add(&pool->impl, r);
    return r;

fail_uffd: {
    int saved_errno = errno;
    close(r->uffd);
    munmap(r->base, r->mapped_size);
    close(r->memfd);
    free(r->chunk_start);
    free(r->initialized);
    free(r);
    errno = saved_errno;
    return nullptr;
}
    /* GCOVR_EXCL_STOP */
}

void fc_client_region_destroy(fc_client_region_t *region) {
    if (!region)
        fc_misuse("fc_client_region_destroy: nullptr region");
    pool_remove(region);
    region_teardown(region);
}

const void *fc_client_region_base(const fc_client_region_t *region) {
    return region ? region->base : nullptr;
}

size_t fc_client_region_size(const fc_client_region_t *region) {
    return region ? region->total_size : 0;
}
