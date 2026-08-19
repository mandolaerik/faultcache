/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache client/server resolution, client side: a pool whose
 * accesses fault locally, but whose chunk content is supplied by a
 * separate server process (see faultcache-server.h), reached over an
 * already-connected AF_UNIX SOCK_SEQPACKET socket. Same-machine IPC, not
 * networked -- "client/server" describes the fault/derive role split,
 * not a network protocol.
 *
 * No code crosses the client/server boundary: only a caller-defined
 * descriptor (opaque bytes) does. Within one fc_server_t, a descriptor is
 * a region's identity AND its chunk layout: the first client to hand off
 * a given descriptor causes the server to create a region for it (via
 * the factory, turning the descriptor into a chunk layout plus a local
 * fc_fill_chunk_fn_t/user_data pair, exactly as if chunk_sizes/fill_chunk
 * had been supplied directly to fc_region_create()). The client
 * never declares a chunk layout itself -- it asks the server to resolve
 * a descriptor first, and only then creates a mapping sized to match.
 * This also means there is no client/server layout-agreement invariant
 * to maintain: the server is the sole authority on a descriptor's shape.
 * (For instance, a descriptor could just be a path to a zip file
 * containing a manifest plus payload entries; the factory would open it,
 * read the manifest, and hand back one chunk per payload entry --
 * entirely the factory's own logic, nothing faultcache-specific.)
 *
 * Any later handoff of the SAME descriptor -- from that client again, or
 * from a different one -- attaches to that same server-side region
 * instead of creating another one, so chunks already resolved for one
 * client are never re-derived for another. This is the natural,
 * dependency-free way to share resolved content across client processes:
 * no explicit "is this the same region" API, just the same descriptor
 * bytes. (Each client still has its own uffd/mapping of the memory --
 * this dedups the resolution work and its cached result, not the
 * physical pages themselves.)
 *
 * No code (function pointers/closures) ever crosses the process
 * boundary, which is not portable or safe in general -- only the
 * descriptor bytes do.
 *
 * The underlying userfaultfd fd is shared with the server via SCM_RIGHTS;
 * UFFDIO_COPY issued by the server still resolves faults in the client's
 * address space (uffd operations are scoped to the mm that registered
 * the range, not to whichever process currently holds the fd) -- so the
 * server never needs its own mapping of the region's memory.
 */
#ifndef FAULTCACHE_CLIENT_H
#define FAULTCACHE_CLIENT_H

#include "faultcache.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tracks the client regions carved out of it (see fc_pool_t in
 * faultcache.h for the in-process equivalent).
 */
typedef struct fc_client_pool fc_client_pool_t;

/* Currently aborts the process on allocation failure (rather than returning
 * NULL) */
fc_client_pool_t *fc_client_pool_create(void);

/* Destroys the pool, tearing down any regions still alive within it. */
void fc_client_pool_destroy(fc_client_pool_t *pool);

/* Opaque handle to a region created by fc_client_region_create(), tracked
 * by its owning pool in an intrusive doubly-linked list (see
 * src/faultcache.c) so fc_client_region_destroy() is O(1). Not a
 * pointer to the region's own memory -- use fc_client_region_base() for
 * that. */
typedef struct fc_client_region fc_client_region_t;

/*
 * Reserve a region whose chunk layout and content are entirely decided
 * by the server reachable via `server_fd` (a connected AF_UNIX
 * SOCK_SEQPACKET socket), from `descriptor_size`/`descriptor` alone --
 * see the file-level comment. A server seeing the same descriptor bytes
 * again (from this client or another) reuses its existing region and
 * chunk layout rather than resolving it again.
 *
 * Blocks until the server has resolved the descriptor and accepted the
 * resulting mapping, so failures are reported synchronously just like
 * fc_region_create(): returns NULL on failure (errno is set) if
 * `descriptor`/`descriptor_size` are locally invalid, or if the server
 * rejected the descriptor or the connection otherwise misbehaved
 * (reported as errno = ECONNREFUSED). A local resource allocation or
 * syscall failure currently aborts the process instead of returning NULL.
 *
 * If `out_error` is non-NULL, *out_error is always set: NULL if the call
 * succeeded or no message was available, otherwise a malloc()'d,
 * human-readable string (relayed from the factory's rejection message,
 * see fc_region_factory_fn_t in faultcache-server.h) that the caller
 * must free(). Pass NULL if uninterested.
 *
 * Returns an opaque handle on success, which must be released with
 * fc_client_region_destroy(). Use fc_client_region_base() to get the
 * mapping's base address.
 */
fc_client_region_t *fc_client_region_create(fc_client_pool_t *pool,
                                             int server_fd,
                                             size_t descriptor_size,
                                             const void *descriptor,
                                             char **out_error);

/*
 * Release a mapping previously returned by fc_client_region_create().
 * `region` must not be used again afterwards (including passing it to
 * fc_client_region_base()/fc_client_region_size()): it is freed by this
 * call.
 *
 * `region` must be a valid, live handle -- passing NULL (or reusing an
 * already-destroyed handle) is a caller bug, not a recoverable error,
 * and aborts the process.
 */
void fc_client_region_destroy(fc_client_region_t *region);

/* The region's mapped base address (of total size
 * fc_client_region_size() bytes) -- may be dereferenced/read directly;
 * writes fault fatally. Valid for as long as `region` itself is (i.e.
 * until fc_client_region_destroy()). */
const void *fc_client_region_base(const fc_client_region_t *region);

/* Total size in bytes of a mapping previously returned by
 * fc_client_region_create(). */
size_t fc_client_region_size(const fc_client_region_t *region);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_CLIENT_H */

