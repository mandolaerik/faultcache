/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache client/server resolution: split a pool's client (the process
 * whose accesses fault) from its server (a separate process that supplies
 * chunk content), connected over an already-connected AF_UNIX
 * SOCK_SEQPACKET socket.
 *
 * No code crosses the client/server boundary: only a caller-defined
 * descriptor (opaque bytes) does. Within one fc_server_t, a descriptor is
 * a region's identity AND its chunk layout: the first client to hand off
 * a given descriptor causes the server to create a region for it (via
 * the factory, turning the descriptor into a chunk layout plus a local
 * fc_init_chunk_fn_t/user_data pair, exactly as if chunk_sizes/init_chunk
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
#ifndef FAULTCACHE_REMOTE_H
#define FAULTCACHE_REMOTE_H

#include "faultcache.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tracks the client regions carved out of it (see fc_pool_t in
 * faultcache.h for the in-process equivalent).
 */
typedef struct fc_client_pool fc_client_pool_t;

/* Returns NULL on failure (errno is set). */
fc_client_pool_t *fc_client_pool_create(void);

/* Destroys the pool, tearing down any regions still alive within it. */
void fc_client_pool_destroy(fc_client_pool_t *pool);

/* A region's base address, as returned by fc_client_region_create(). Just
 * a `const void *` in disguise -- may be dereferenced/read directly. */
typedef const void *fc_client_region_t;

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
 * fc_region_create(): returns NULL on failure (errno is set; a
 * rejected descriptor -- unknown to the factory, or a resource failure
 * on the server -- is reported as errno = ECONNREFUSED).
 *
 * If `out_error` is non-NULL, *out_error is always set: NULL if the call
 * succeeded or no message was available, otherwise a malloc()'d,
 * human-readable string (relayed from the factory's rejection message,
 * see fc_region_factory_fn_t) that the caller must free(). Pass NULL if
 * uninterested.
 *
 * The returned pointer must be released with fc_client_region_destroy(),
 * passing the same pool.
 */
fc_client_region_t fc_client_region_create(fc_client_pool_t *pool,
                                            int server_fd,
                                            size_t descriptor_size,
                                            const void *descriptor,
                                            char **out_error);

/*
 * Release a mapping previously returned by fc_client_region_create() on
 * `pool`. Returns 0 on success, -1 on failure (errno is set).
 */
int fc_client_region_destroy(fc_client_pool_t *pool,
                              fc_client_region_t region);

/* Total size in bytes of a mapping previously returned by
 * fc_client_region_create() on `pool`, or 0 if `region` is not a live
 * region of that pool. */
size_t fc_client_region_size(fc_client_pool_t *pool,
                              fc_client_region_t region);

/*
 * Called once user_data (as set by the factory) is no longer needed --
 * currently only when the owning fc_server_t is torn down via
 * fc_server_destroy(); freeing it earlier, e.g. once a region's last
 * attached client disconnects, is a known follow-up not implemented yet
 * (see TODO.md). May be NULL if user_data needs no cleanup (e.g. it
 * isn't a pointer to anything owned, as in fc_init_chunk_fn_t's own
 * user_data contract).
 */
typedef void (*fc_region_destroy_fn_t)(void *user_data);

/*
 * Called the first time a descriptor is seen by a given fc_server_t (not
 * once per handoff -- see the file-level comment), turning that
 * descriptor into a chunk layout plus a local (function, user_data) pair
 * used to resolve the region's chunks -- same contract as the
 * chunk_sizes/init_chunk/user_data passed directly to
 * fc_region_create(), except the layout is now the factory's
 * decision, not the caller's.
 *
 * On success, must set every out parameter and return NULL:
 *   - *out_nchunks: the number of chunks, > 0.
 *   - *out_chunk_sizes: a malloc()'d array of *out_nchunks sizes, each
 *     > 0. Ownership transfers to the caller (the library), which
 *     free()s it once it has copied the sizes out, immediately after
 *     this call returns -- do not retain or reuse the pointer.
 *   - *out_init_chunk, *out_user_data: as for fc_region_create().
 *   - *out_destroy_user_data: see fc_region_destroy_fn_t. May be left
 *     NULL.
 * To reject the descriptor, return a malloc()'d, human-readable message
 * explaining why (never NULL, which means success -- return e.g.
 * strdup("") if there's nothing more specific to say); ownership
 * transfers to the caller (the library), which free()s it after relaying
 * it to the client's fc_client_region_create() call. This fails that
 * call synchronously; any of *out_chunk_sizes already allocated is still
 * free()'d by the caller in this case, but *out_user_data is not --
 * release it yourself before returning.
 *
 * Called from whichever thread's fc_server_run() first observes the
 * descriptor; like init_chunk, must not block on anything that could
 * deadlock against that loop.
 *
 * `factory_user_data` is passed through unchanged from fc_server_create()
 * -- one server-wide value shared by every call, e.g. so a language
 * binding can wrap a single caller-supplied callback/context pair in one
 * C trampoline instead of generating one per registration.
 */
typedef char *(*fc_region_factory_fn_t)(size_t descriptor_size,
                                         const void *descriptor,
                                         uint32_t *out_nchunks,
                                         size_t **out_chunk_sizes,
                                         fc_init_chunk_fn_t *out_init_chunk,
                                         void **out_user_data,
                                         fc_region_destroy_fn_t *out_destroy_user_data,
                                         void *factory_user_data);

typedef struct fc_server fc_server_t;

/*
 * `factory_user_data` is passed through unchanged as the factory's final
 * argument on every call. Returns NULL on failure (errno is set).
 * The caller retains ownership of the factory pointer and must not free it
 * until after fc_server_destroy() returns.
 */
fc_server_t *fc_server_create(fc_region_factory_fn_t factory,
                               void *factory_user_data);

/*
 * Services resolve/attach requests and page faults on `conn_fd`, an
 * already-connected AF_UNIX SOCK_SEQPACKET socket to one client. A
 * client may hand off any number of regions over the same connection,
 * one after another; each is serviced for as long as the region is
 * alive.
 *
 * Returns once `conn_fd` reaches EOF (the client disconnected) or
 * fc_server_stop() is called from another thread. Returns 0 on either of
 * those, -1 on a fatal error (errno is set).
 *
 * There is no accept()/listen() handling in faultcache itself: a server
 * wanting to handle several concurrent clients should accept() its own
 * listening socket and run one fc_server_run() (e.g. per thread or
 * process) per accepted connection. Concurrent fc_server_run() calls on
 * the same fc_server_t are safe (needed for descriptor-based sharing
 * across connections) and share the same regions/factory.
 */
int fc_server_run(fc_server_t *server, int conn_fd);

/* Asks a concurrently-running fc_server_run() to return. */
void fc_server_stop(fc_server_t *server);

/* Tears down the server and every region it was servicing, calling each
 * region's destroy_user_data (if any -- see fc_region_factory_fn_t).
 * Must not be called while fc_server_run() is still running on another
 * thread. */
void fc_server_destroy(fc_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_REMOTE_H */

