/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * faultcache - transparent cache for cheaply-derived memory content.
 *
 * fc_region_create() reserves a contiguous read-only address range,
 * divided into "chunks", tracked by a pool. No chunk is populated
 * until it is first accessed: touching any byte inside a chunk triggers
 * a page fault that is resolved by invoking the user-supplied
 * fill_chunk() callback, which fills that chunk's bytes. Once resolved,
 * a chunk behaves exactly like a page cached by mmap(PROT_READ):
 * subsequent reads are free, and writes are illegal (they fault fatally,
 * just as they would on a read-only file mapping).
 *
 * This is useful for lazily materializing content that is cheap to derive
 * on demand, e.g. decompressing blocks of a compressed file only as they
 * are actually read.
 *
 * This header covers the in-process API: chunk content is derived
 * in-process by fill_chunk(). See faultcache-client.h/faultcache-server.h
 * for the split client/server variant, where content is derived by a
 * separate server process.
 *
 * Implemented on Linux via mmap(PROT_NONE) + a process-wide SIGSEGV
 * handler: touching an unresolved chunk faults synchronously on the
 * accessing thread, which resolves it inline before retrying the access.
 * fc_init() installs that handler; see its comment for how to coexist
 * with other SIGSEGV users.
 */
#ifndef FAULTCACHE_H
#define FAULTCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fc_init() must be called before any region exists, to arm a SIGSEGV handler.
 * The function is idempotent, so calling it transitively from multiple
 * libraries is safe. If additional SIGSEGV handlers are armed by other parties
 * -- a crash reporter, Python's `faulthandler.enable` -- then you may need to
 * call `fc_rearm_handler()` right after that to ensure the two handlers are
 * chained.
 *
 * Arming is thread-safe: the first call to fc_init installs, every later one is
 * a no-op. The requirement is enforced rather than papered over --
 * fc_region_create() aborts with a diagnostic if it was forgotten, instead of
 * quietly arming on your behalf.
 *
 * Call it from your own library's init, not lazily at first use. Signal
 * dispositions form a single chain built by install order: the handler
 * installed last is the one the kernel calls, and the ones below it are reached
 * only if those above pass faults down. faultcache does pass them down, and
 * stays installed while doing so, so it composes in either direction. Not every
 * library does -- a crash reporter that dumps and exits, or Python's
 * faulthandler, ends the chain where it stands. So faultcache wants to sit
 * *above* those, and where it sits is decided by when this is called.
 *
 * Calling it from init is what leaves that arrangeable from the outside. Given
 * libraries A and B that both use faultcache and a library C that installs a
 * handler of its own, an application that can modify none of the three still
 * controls the outcome with the ordering it already has:
 *
 *     c_init();     // C takes SIGSEGV first
 *     a_init();     // A's fc_init() puts faultcache on top of it
 *     b_init();     // B's fc_init() is a no-op
 *
 * Nothing there mentions faultcache, which is the point -- A's and B's users
 * should not have to know about a transitive dependency. Had the handler gone
 * in lazily at first use instead, that ordering would have no effect at all,
 * since the install would happen somewhere inside A's later operation rather
 * than at a_init().
 *
 * The faultcache handler is never uninstalled, not even when the last pool is
 * destroyed: another thread can be inside the handler at that moment with no
 * way to find out, so restoring the old disposition could drop a fault in
 * flight. With no live regions it costs nothing anyway -- every fault just
 * falls through to the same chain it would have hit otherwise.
 */
void fc_init(void);

/*
 * Re-installs faultcache's handler on top of whatever holds SIGSEGV now and
 * makes that the new chain target, so faults outside live unresolved regions
 * are passed on to it. Displacing our own handler leaves the chain target
 * alone, so repeated calls can't route us through ourselves. Implies fc_init().
 * Call it where you control the threads, as with init: it rewrites the target
 * that faulting threads read.
 *
 * You need this function when two things coincide: something else arms
 * SIGSEGV without passing on the faults it did not cause, and it does so after
 * fc_init() has run. Neither half hurts alone -- a handler that chains properly
 * still delivers our faults to us, and one armed before fc_init() ends up below
 * us anyway -- but together they strand our regions under a handler that will
 * not hand them back. Assume the first half unless you know otherwise; chaining
 * is the rarer discipline.
 *
 * The second half is the one you can rarely arrange away: something arms
 * lazily rather than in its init (Python's faulthandler, a crash reporter on
 * first use), an init has to run after yours for unrelated reasons, or a plugin
 * is `dlopen()`ed later. Concretely: if your code calls faulthandler.enable(),
 * or installs a handler of its own, call this right afterwards.
 *
 * The convention in the general case is that whoever brings the conflict into
 * the process repairs it. It is only visible where both sides are in the same
 * dependency set, and either side can arrive transitively: a library depending
 * on faultcache and on something that arms SIGSEGV calls this itself, rather
 * than documenting two transitive dependencies for its users; where one library
 * brings each side, only the application above them sees both, so it calls
 * this. The corollary is not to re-arm speculatively -- if you cannot name the
 * handler you are displacing, the conflict is not yours, and whoever owns it is
 * calling this too.
 *
 * Displacing a handler that had saved ours leaves the two pointing at each
 * other. faultcache detects that a fault has come back around to it and ends
 * the chain at the default disposition, so the result is an ordinary crash
 * rather than a bounce until the stack runs out.
 */
void fc_rearm_handler(void);

/*
 * Tracks the regions carved out of it (see fc_client_pool_t in
 * faultcache-client.h for the split client/server equivalent). A pool is
 * expected to eventually also own a shared, size-bounded cache of
 * resident chunks (possibly spanning several regions) with LRU eviction;
 * for now it just tracks region lifetimes.
 */
typedef struct fc_pool fc_pool_t;

/* Returns nullptr on failure (errno is set). */
fc_pool_t *fc_pool_create(void);

/* Destroys the pool, tearing down any regions still alive within it. */
void fc_pool_destroy(fc_pool_t *pool);

/*
 * Called once per chunk, the first time any byte of that chunk is
 * accessed. Must fill exactly `size` bytes starting at `start`.
 *
 * `start`/`size` always describe the entire chunk (as given via
 * chunk_sizes[chunk]), never a partial page-aligned fragment of it, even
 * when several chunks are resolved together because they share a page.
 *
 * Called synchronously, inline, on whichever thread first accesses the
 * chunk -- from within the library's SIGSEGV handler, on that thread's
 * own stack. It must not access any unresolved memory of any region
 * (that would recursively fault and deadlock; nested/recursive faults
 * are not yet supported) and should stick to simple, reentrant work,
 * since it runs in a signal handler's context.
 */
typedef void (*fc_fill_chunk_fn_t)(uint32_t chunk, void *start, size_t size,
                                   const void *user_data);

/* Opaque handle to a region created by fc_region_create(), tracked by
 * its owning pool in an intrusive doubly-linked list (see
 * src/faultcache-sigsegv.c) so fc_region_destroy() is O(1). Not a
 * pointer to the region's own memory -- use fc_region_base() for that. */
typedef struct fc_region fc_region_t;

/*
 * Reserve a read-only, lazily-populated address range made of `nchunks`
 * consecutive chunks whose sizes are given by chunk_sizes[0..nchunks-1],
 * tracked by `pool`.
 *
 * `pool`/`chunk_sizes`/`fill_chunk` must be non-nullptr and `nchunks` must
 * be > 0 -- violating that is a caller bug, not a recoverable error,
 * and aborts the process.
 *
 * Returns an opaque handle on success, or nullptr on failure (errno set to
 * EINVAL) if chunk_sizes contains an invalid entry (zero, or one whose
 * running total overflows) -- this can happen even with correct calling
 * code. A resource allocation (malloc()/mmap()) failure currently aborts
 * the process instead of returning nullptr.
 * Use fc_region_base() to get the mapping's base address (of total
 * size sum(chunk_sizes)).
 *
 * The returned handle must be released with fc_region_destroy().
 */
fc_region_t *fc_region_create(fc_pool_t *pool,
                               uint32_t nchunks,
                               const size_t *chunk_sizes,
                               fc_fill_chunk_fn_t fill_chunk,
                               const void *user_data);

/*
 * Release a mapping previously returned by fc_region_create(). `region`
 * must not be used again afterwards (including passing it to
 * fc_region_base()/fc_region_size()): it is freed by this call.
 *
 * `region` must be a valid, live handle -- passing nullptr (or reusing an
 * already-destroyed handle) is a caller bug, not a recoverable error,
 * and aborts the process.
 */
void fc_region_destroy(fc_region_t *region);

/* Total size in bytes of a mapping previously returned by
 * fc_region_create(). `region` must be a valid, live handle (see
 * fc_region_destroy()). */
size_t fc_region_size(const fc_region_t *region);

/* The region's mapped base address (of total size fc_region_size()
 * bytes) -- may be dereferenced/read directly; writes fault fatally.
 * Valid for as long as `region` itself is (i.e. until
 * fc_region_destroy()). `region` must be a valid, live handle. Never
 * fails. */
const void *fc_region_base(const fc_region_t *region);

#ifdef __cplusplus
}
#endif

#endif /* FAULTCACHE_H */
