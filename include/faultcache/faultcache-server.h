/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache client/server resolution, server side: derives chunk
 * content for one or more clients (see faultcache-client.h for the
 * client side and the full protocol/sharing description), each reached
 * over an already-connected AF_UNIX SOCK_SEQPACKET socket.
 */
#ifndef FAULTCACHE_SERVER_H
#define FAULTCACHE_SERVER_H

#include "faultcache.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * once per handoff -- see faultcache-client.h), turning that descriptor
 * into a chunk layout plus a local (function, user_data) pair used to
 * resolve the region's chunks -- same contract as the
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
typedef char *(*fc_region_factory_fn_t)(
    size_t descriptor_size, const void *descriptor,
    uint32_t *out_nchunks, size_t **out_chunk_sizes,
    fc_init_chunk_fn_t *out_init_chunk, void **out_init_chunk_user_data,
    fc_region_destroy_fn_t *out_destroy,
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

#endif /* FAULTCACHE_SERVER_H */
