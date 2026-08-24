/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache client/server resolution, server side: derives chunk
 * content for one or more clients (see client.h for the
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
 * A chunk layout plus the (function, user_data) pair used to resolve it,
 * as decided by fc_region_factory_fn_t -- same contract as the
 * chunk_sizes/fill_chunk/user_data passed directly to
 * fc_region_create(), except the layout is now the factory's decision,
 * not the caller's. Zeroed by the caller (the library) before the
 * factory is invoked.
 */
typedef struct {
    uint32_t nchunks;    /* > 0 */
    size_t *chunk_sizes; /* malloc()'d, nchunks entries, each > 0;
                           * ownership transfers to the caller, which
                           * free()s it once it has copied the sizes out,
                           * immediately after the factory call returns --
                           * do not retain or reuse the pointer */
    fc_fill_chunk_fn_t fill_chunk;
    void *region_user_data;
    /*
    * Called when region is destroyed.
    * Currently only when the owning fc_server_t is torn down via
    * fc_server_destroy(), but may happen later. May be nullptr if user_data
    * needs no cleanup.
    */
    void (*destroy_user_data)(void *region_user_data);
} fc_region_recipe_t;

/*
 * Called the first time a descriptor is seen by a given fc_server_t (not
 * once per handoff -- see client.h), turning that descriptor
 * into an `out_layout`.
 *
 * On success, must set every field of `*out_layout` and return nullptr.
 * To reject the descriptor, return a malloc()'d, human-readable message
 * explaining why (never nullptr, which means success -- return e.g.
 * strdup("") if there's nothing more specific to say); ownership
 * transfers to the caller (the library), which free()s it after relaying
 * it to the client's fc_client_region_create() call. This fails that
 * call synchronously; any of out_layout->chunk_sizes already allocated
 * is still free()'d by the caller in this case, but
 * out_layout->region_user_data is not -- release it yourself before
 * returning.
 *
 * Called from whichever thread's fc_server_run() first observes the
 * descriptor; like fill_chunk, must not block on anything that could
 * deadlock against that loop.
 *
 * `factory_user_data` is passed through unchanged from fc_server_create()
 * -- one server-wide value shared by every call, e.g. so a language
 * binding can wrap a single caller-supplied callback/context pair in one
 * C trampoline instead of generating one per registration.
 */
typedef char *(*fc_region_factory_fn_t)(size_t descriptor_size,
                                         const void *descriptor,
                                         fc_region_recipe_t *out_recipe,
                                         void *factory_user_data);

typedef struct fc_server fc_server_t;

/*
 * `factory_user_data` is passed through unchanged as the factory's final
 * argument on every call.
 * `target_size == 0` means "unbounded" (no size limit yet).
 * Any other value is rejected for now, until the LRU cache is implemented.
 * `factory` must be non-nullptr -- passing nullptr is caller misuse and
 * aborts the process.
 * Returns nullptr on failure (errno is set).
 * The caller retains ownership of the factory pointer and must not free it
 * until after fc_server_destroy() returns.
 */
FC_API fc_server_t *fc_server_create(fc_region_factory_fn_t factory,
                              void *factory_user_data,
                              size_t target_size) FC_NOTNULL(1);

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
 * `server` must be non-nullptr -- passing nullptr is caller misuse and
 * aborts the process.
 *
 * There is no accept()/listen() handling in faultcache itself: a server
 * wanting to handle several concurrent clients should accept() its own
 * listening socket and run one fc_server_run() (e.g. per thread or
 * process) per accepted connection. Concurrent fc_server_run() calls on
 * the same fc_server_t are safe (needed for descriptor-based sharing
 * across connections) and share the same regions/factory.
 */
FC_API int fc_server_run(fc_server_t *server, int conn_fd) FC_NOTNULL(1);

/* Asks a concurrently-running fc_server_run() to return.
 * `server` must be non-nullptr -- passing nullptr is caller misuse and
 * aborts the process. */
FC_API void fc_server_stop(fc_server_t *server) FC_NOTNULL(1);

/* Tears down the server and every region it was servicing, calling each
 * region's destroy_user_data (if any -- see fc_region_factory_fn_t).
 * Must not be called while fc_server_run() is still running on another
 * thread. */
FC_API void fc_server_destroy(fc_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_SERVER_H */
