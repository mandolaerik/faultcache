/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE
#include "faultcache/faultcache_remote.h"
#include "faultcache_wire.h"

#include <errno.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* One per client handoff attached to a region (possibly several per
 * region, when multiple handoffs share a descriptor -- see
 * faultcache_remote.h). */
struct fc_server_mapping {
    struct fc_server_mapping *next;
    int uffd;
    uintptr_t base_addr; /* opaque number, never dereferenced here */
};

/*
 * A region's identity is its descriptor. `content`/`resolved` are the
 * server-side resolved-content cache, shared by every mapping attached to
 * this region: a chunk is derived (init_chunk called) at most once no
 * matter how many mappings fault on it, in whatever order.
 */
struct fc_server_region {
    struct fc_server_region *next;

    void *descriptor;
    size_t descriptor_size;

    size_t page_size;
    uint32_t nchunks;
    size_t *chunk_start;  /* prefix sums, nchunks+1 entries */
    bool *resolved;       /* nchunks entries */
    uint8_t *content;     /* mapped_size bytes; content[chunk_start[i]..]
                            * valid once resolved[i] */

    fc_init_chunk_fn_t init_chunk;
    void *user_data;
    fc_region_destroy_fn_t destroy_user_data; /* may be NULL */

    struct fc_server_mapping *mappings;
};

struct fc_server {
    fc_region_factory_fn_t factory;
    void *factory_user_data;
    /* Guards region lookup/creation and the resolved[]/content cache,
     * since several fc_server_run() calls (one per connection) may share
     * one fc_server_t concurrently. */
    pthread_mutex_t lock;
    struct fc_server_region *regions;
    int stop_fd; /* eventfd; a write asks fc_server_run() to return */
};

static size_t page_floor(size_t x, size_t page_size) {
    return x - (x % page_size);
}

static size_t page_ceil(size_t x, size_t page_size) {
    return page_floor(x + page_size - 1, page_size);
}

static uint32_t find_chunk(const struct fc_server_region *r, size_t off) {
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

/* Mirrors resolve_fault() in faultcache.c -- see the comment there for why
 * pages that straddle chunk boundaries are resolved as a group. Unlike
 * that one, the resolved content is cached in `r->content` rather than a
 * throwaway buffer, since other mappings of the same region may need it. */
static void resolve_fault(struct fc_server *server, struct fc_server_region *r,
                           struct fc_server_mapping *m, size_t fault_off) {
    uint32_t c0 = find_chunk(r, fault_off);

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

    pthread_mutex_lock(&server->lock);
    for (uint32_t i = lo; i <= hi; i++) {
        if (r->resolved[i])
            continue; /* already derived by an earlier fault on any mapping */
        size_t chunk_size = r->chunk_start[i + 1] - r->chunk_start[i];
        r->init_chunk(i, r->content + r->chunk_start[i], chunk_size,
                       r->user_data);
        r->resolved[i] = true;
    }
    pthread_mutex_unlock(&server->lock);

    /* Every mapping still needs its own UFFDIO_COPY into its own mm, even
     * when the content was already cached by another mapping's fault. */
    struct uffdio_copy copy = {
        .dst = (unsigned long)(m->base_addr + page_lo),
        .src = (unsigned long)(r->content + page_lo),
        .len = page_hi - page_lo,
        .mode = 0,
    };
    if (ioctl(m->uffd, UFFDIO_COPY, &copy) < 0 && errno != EEXIST) {
        /* Nothing better to do: this mapping's client stays blocked. */
    }
}

static struct fc_server_region *find_region(struct fc_server *server,
                                             size_t descriptor_size,
                                             const void *descriptor) {
    for (struct fc_server_region *r = server->regions; r; r = r->next)
        if (r->descriptor_size == descriptor_size &&
            memcmp(r->descriptor, descriptor, descriptor_size) == 0)
            return r;
    return NULL;
}

/*
 * Builds a brand-new region for a descriptor not seen before, by calling
 * the factory to turn it into a chunk layout plus a (init_chunk,
 * user_data, destroy_user_data) triple. Returns NULL on failure (factory
 * rejection, an invalid layout, or OOM), in which case destroy_user_data
 * (if the factory set one) has already been invoked -- the caller never
 * needs to clean up user_data itself. On failure, *out_error is set to a
 * malloc()'d message (the factory's own, or one describing an invalid
 * layout) or left NULL if there's nothing to say; the caller owns it.
 */
static struct fc_server_region *create_region(fc_region_factory_fn_t factory,
                                               size_t descriptor_size,
                                               const void *descriptor,
                                               void *factory_user_data,
                                               char **out_error) {
    uint32_t nchunks = 0;
    size_t *chunk_sizes = NULL;
    fc_init_chunk_fn_t init_chunk = NULL;
    void *user_data = NULL;
    fc_region_destroy_fn_t destroy_user_data = NULL;
    char *error = factory(descriptor_size, descriptor, &nchunks, &chunk_sizes,
                           &init_chunk, &user_data, &destroy_user_data,
                           factory_user_data);
    if (error) {
        free(chunk_sizes);
        *out_error = error;
        return NULL;
    }

    bool layout_ok = nchunks > 0 && chunk_sizes && init_chunk;
    size_t acc = 0;
    for (uint32_t i = 0; layout_ok && i < nchunks; i++) {
        if (chunk_sizes[i] == 0 || acc + chunk_sizes[i] < acc)
            layout_ok = false;
        else
            acc += chunk_sizes[i];
    }
    if (!layout_ok) {
        free(chunk_sizes);
        if (destroy_user_data)
            destroy_user_data(user_data);
        *out_error = strdup("factory returned an invalid chunk layout");
        return NULL;
    }

    struct fc_server_region *r = calloc(1, sizeof(*r));
    void *desc_copy = descriptor_size ? malloc(descriptor_size) : NULL;
    size_t *chunk_start = malloc((size_t)(nchunks + 1) * sizeof(size_t));
    bool *resolved = calloc(nchunks, sizeof(bool));
    if (!r || (descriptor_size && !desc_copy) || !chunk_start || !resolved) {
        free(r);
        free(desc_copy);
        free(chunk_start);
        free(resolved);
        free(chunk_sizes);
        if (destroy_user_data)
            destroy_user_data(user_data);
        return NULL;
    }
    if (descriptor_size)
        memcpy(desc_copy, descriptor, descriptor_size);

    acc = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        chunk_start[i] = acc;
        acc += chunk_sizes[i];
    }
    chunk_start[nchunks] = acc;
    free(chunk_sizes);

    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *content = calloc(1, page_ceil(acc, page_size));
    if (!content) {
        free(r);
        free(desc_copy);
        free(chunk_start);
        free(resolved);
        if (destroy_user_data)
            destroy_user_data(user_data);
        return NULL;
    }

    r->descriptor = desc_copy;
    r->descriptor_size = descriptor_size;
    r->page_size = page_size;
    r->nchunks = nchunks;
    r->chunk_start = chunk_start;
    r->resolved = resolved;
    r->content = content;
    r->init_chunk = init_chunk;
    r->user_data = user_data;
    r->destroy_user_data = destroy_user_data;
    return r;
}

static void region_free(struct fc_server_region *r) {
    while (r->mappings) {
        struct fc_server_mapping *m = r->mappings;
        r->mappings = m->next;
        close(m->uffd);
        free(m);
    }
    if (r->destroy_user_data)
        r->destroy_user_data(r->user_data);
    free(r->descriptor);
    free(r->chunk_start);
    free(r->resolved);
    free(r->content);
    free(r);
}

/*
 * Receives one resolve-request message on `conn_fd`: just descriptor
 * bytes. Finds the descriptor's existing region, or builds one via the
 * factory if this is the first time it's been seen. Replies with the
 * resulting chunk layout, or a rejection.
 *
 * Returns 1 if accepted, 0 if rejected (nacked, but the connection stays
 * open for further resolves), 2 if the peer disconnected cleanly, -1 on
 * a fatal I/O error on conn_fd (errno set).
 */
static int handle_resolve(struct fc_server *server, int conn_fd) {
    char *buf = malloc(FC_WIRE_MSG_MAX);
    if (!buf)
        return -1;

    ssize_t rd = recv(conn_fd, buf, FC_WIRE_MSG_MAX, 0);
    if (rd < 0) {
        free(buf);
        return -1;
    }
    if (rd == 0) { /* peer closed before sending anything more */
        free(buf);
        return 2;
    }

    struct fc_resolve_req_hdr req;
    bool ok = (size_t)rd >= sizeof(req);
    if (ok) {
        memcpy(&req, buf, sizeof(req));
        ok = (size_t)rd == sizeof(req) + req.descriptor_size;
    }

    struct fc_resolve_resp_hdr resp = {.status = -1, .nchunks = 0, .error_len = 0};
    uint64_t *reply_sizes = NULL;
    char *error = NULL;
    if (ok) {
        const void *descriptor = buf + sizeof(req);
        pthread_mutex_lock(&server->lock);
        struct fc_server_region *r =
            find_region(server, req.descriptor_size, descriptor);
        if (!r) {
            r = create_region(server->factory, req.descriptor_size, descriptor,
                               server->factory_user_data, &error);
            if (r) {
                r->next = server->regions;
                server->regions = r;
            }
        }
        if (r) {
            reply_sizes = malloc((size_t)r->nchunks * sizeof(uint64_t));
            if (reply_sizes) {
                for (uint32_t i = 0; i < r->nchunks; i++)
                    reply_sizes[i] =
                        (uint64_t)(r->chunk_start[i + 1] - r->chunk_start[i]);
                resp.status = 0;
                resp.nchunks = r->nchunks;
            }
        } else if (error) {
            resp.error_len = (uint32_t)strlen(error);
        }
        pthread_mutex_unlock(&server->lock);
    }
    free(buf);

    size_t trailer_bytes = resp.status == 0
                                ? (size_t)resp.nchunks * sizeof(uint64_t)
                                : (size_t)resp.error_len;
    size_t reply_bytes = sizeof(resp) + trailer_bytes;
    void *reply_buf = malloc(reply_bytes);
    bool sent_ok = false;
    if (reply_buf) {
        memcpy(reply_buf, &resp, sizeof(resp));
        if (resp.status == 0) {
            if (resp.nchunks)
                memcpy((char *)reply_buf + sizeof(resp), reply_sizes,
                       trailer_bytes);
        } else if (resp.error_len) {
            memcpy((char *)reply_buf + sizeof(resp), error, trailer_bytes);
        }
        sent_ok =
            send(conn_fd, reply_buf, reply_bytes, 0) == (ssize_t)reply_bytes;
    }
    free(reply_sizes);
    free(reply_buf);
    free(error);

    if (!sent_ok)
        return -1;
    return resp.status == 0 ? 1 : 0;
}

/*
 * Receives one attach-request message on `conn_fd`: the same descriptor
 * bytes a preceding handle_resolve() call just resolved, plus the
 * client's base address and uffd fd (via SCM_RIGHTS). Attaches a new
 * mapping to that region and registers the uffd with `ep`. Sends the
 * ack/nack reply on conn_fd.
 *
 * Returns 1 if a mapping was accepted, 0 if rejected (nacked, but the
 * connection stays open for further resolves), 2 if the peer disconnected
 * cleanly, -1 on a fatal I/O error on conn_fd (errno set).
 */
static int handle_attach(struct fc_server *server, int ep, int conn_fd) {
    char *buf = malloc(FC_WIRE_MSG_MAX);
    if (!buf)
        return -1;

    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = buf, .iov_len = FC_WIRE_MSG_MAX};
    struct msghdr mh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cbuf,
        .msg_controllen = sizeof(cbuf),
    };
    ssize_t rd = recvmsg(conn_fd, &mh, 0);
    if (rd < 0) {
        free(buf);
        return -1;
    }
    if (rd == 0) { /* peer closed before sending anything more */
        free(buf);
        return 2;
    }

    int uffd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mh);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&uffd, CMSG_DATA(cmsg), sizeof(int));

    struct fc_attach_req_hdr hdr;
    bool ok = (size_t)rd >= sizeof(hdr) && uffd >= 0;
    if (ok) {
        memcpy(&hdr, buf, sizeof(hdr));
        ok = (size_t)rd == sizeof(hdr) + hdr.descriptor_size;
    }

    struct fc_server_mapping *m = NULL;
    if (ok) {
        const void *descriptor = buf + sizeof(hdr);
        pthread_mutex_lock(&server->lock);
        struct fc_server_region *r =
            find_region(server, hdr.descriptor_size, descriptor);
        if (!r) {
            /* Attach for a descriptor this connection never resolved --
             * a protocol violation by the client. */
            ok = false;
        } else {
            m = calloc(1, sizeof(*m));
            if (!m) {
                ok = false;
            } else {
                m->uffd = uffd;
                m->base_addr = (uintptr_t)hdr.base;
                m->next = r->mappings;
                r->mappings = m;
                struct epoll_event ev = {.events = EPOLLIN, .data.fd = uffd};
                epoll_ctl(ep, EPOLL_CTL_ADD, uffd, &ev);
            }
        }
        pthread_mutex_unlock(&server->lock);
    }
    free(buf);

    uint8_t reply = ok ? 0 : 1;
    ssize_t sent = send(conn_fd, &reply, 1, 0);
    if (!ok || sent != 1) {
        /* m (if any) stays owned by its region and gets cleaned up by
         * fc_server_destroy(); only the just-received fd needs closing
         * here if it was never attached to a mapping. */
        if (!m && uffd >= 0)
            close(uffd);
        return sent == 1 ? 0 : -1; /* rejected-but-open vs. fatal */
    }
    return 1;
}

fc_server_t *fc_server_create(fc_region_factory_fn_t factory,
                               void *factory_user_data) {
    if (!factory) {
        errno = EINVAL;
        return NULL;
    }
    struct fc_server *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->factory = factory;
    s->factory_user_data = factory_user_data;
    pthread_mutex_init(&s->lock, NULL);
    s->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->stop_fd < 0) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}

int fc_server_run(fc_server_t *server, int conn_fd) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return -1;

    struct epoll_event ev = {.events = EPOLLIN, .data.fd = conn_fd};
    if (epoll_ctl(ep, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
        close(ep);
        return -1;
    }
    ev.data.fd = server->stop_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, server->stop_fd, &ev) < 0) {
        close(ep);
        return -1;
    }

    /* Each fc_client_region_create() call is a strict resolve-then-attach
     * ping-pong on this connection (see faultcache_wire.h); this tracks
     * which message conn_fd's next readability event should be parsed
     * as. */
    enum { CONN_WAIT_RESOLVE, CONN_WAIT_ATTACH } conn_state = CONN_WAIT_RESOLVE;

    int ret = 0;
    for (;;) {
        struct epoll_event events[16];
        int n = epoll_wait(ep, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ret = -1;
            break;
        }

        bool stop = false;
        bool disconnected = false;
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server->stop_fd) {
                stop = true;
                continue;
            }
            if (fd == conn_fd) {
                int rc = conn_state == CONN_WAIT_RESOLVE
                             ? handle_resolve(server, conn_fd)
                             : handle_attach(server, ep, conn_fd);
                if (rc == 2 || rc < 0) { /* EOF, or a fatal error */
                    disconnected = true;
                    if (rc < 0)
                        ret = -1;
                } else if (conn_state == CONN_WAIT_RESOLVE) {
                    /* Only proceed to attach if resolve was accepted;
                     * otherwise the client's call already failed and it
                     * won't send an attach for this descriptor. */
                    conn_state = rc == 1 ? CONN_WAIT_ATTACH : CONN_WAIT_RESOLVE;
                } else {
                    conn_state = CONN_WAIT_RESOLVE; /* attach settled */
                }
                continue;
            }

            pthread_mutex_lock(&server->lock);
            struct fc_server_region *r = NULL;
            struct fc_server_mapping *m = NULL;
            for (struct fc_server_region *rr = server->regions; rr && !m;
                 rr = rr->next) {
                for (struct fc_server_mapping *mm = rr->mappings; mm;
                     mm = mm->next) {
                    if (mm->uffd == fd) {
                        r = rr;
                        m = mm;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&server->lock);
            if (!m)
                continue;

            struct uffd_msg msg;
            ssize_t rd = read(fd, &msg, sizeof(msg));
            if (rd == sizeof(msg) && msg.event == UFFD_EVENT_PAGEFAULT) {
                size_t fault_addr = (size_t)msg.arg.pagefault.address;
                resolve_fault(server, r, m, fault_addr - m->base_addr);
            }
        }
        if (stop || disconnected)
            break;
    }

    close(ep);
    return ret;
}

void fc_server_stop(fc_server_t *server) {
    uint64_t one = 1;
    ssize_t unused = write(server->stop_fd, &one, sizeof(one));
    (void)unused;
}

void fc_server_destroy(fc_server_t *server) {
    if (!server)
        return;
    while (server->regions) {
        struct fc_server_region *r = server->regions;
        server->regions = r->next;
        region_free(r);
    }
    pthread_mutex_destroy(&server->lock);
    close(server->stop_fd);
    free(server);
}


