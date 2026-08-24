/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * The fault-delivery seam: how the OS tells us a thread touched a page
 * we have not populated yet. POSIX delivers it as SIGSEGV, Windows as a
 * vectored exception; both are "run on the faulting thread, then either
 * retry the instruction or let the fault continue to whoever is next".
 * That shape is what makes one seam possible.
 *
 * Traffic goes both ways across this header, which is the point:
 *
 *   fault_arm/fault_rearm/fault_armed  are implemented per platform,
 *   because registering a handler, keeping the displaced one and
 *   deciding what "let it continue" means are all platform-specific.
 *
 *   fault_try_resolve  is implemented once, in inproc.c, because
 *   finding which region owns an address and populating it is the same
 *   everywhere. The platform handler calls it and does nothing else.
 *
 * Nothing about signals, exception records or chaining crosses into the
 * shared side, and nothing about pools, regions or locks crosses into
 * the platform side.
 */
#ifndef FAULTCACHE_FAULT_H
#define FAULTCACHE_FAULT_H

#include <stdbool.h>

/* Arms the platform's fault handler. Idempotent and thread-safe; the
 * first call is the one that captures whatever was installed before us
 * as the chain target. */
void fault_arm(void);

/* Arms again on top of whatever has taken the fault handler since, for
 * the cases install order cannot fix (a lazily-arming library, a
 * dlopen'd plugin). Never captures our own handler as the chain target,
 * which would make the chain call itself. */
void fault_rearm(void);

/* Whether fault_arm() has run, so region creation can refuse to hand
 * out memory nothing would populate. */
bool fault_armed(void);

/*
 * Called by the platform handler for every fault it sees, with the
 * faulting address. Returns true if the fault was ours and the address
 * is now populated, meaning the faulting instruction should be retried;
 * false if the address belongs to no region of ours (or is a genuine
 * violation on one, such as a write to a resolved read-only chunk), in
 * which case the platform must pass the fault on.
 *
 * Runs on the faulting thread with the fault still pending, so it may
 * only use what is safe in that context. errno is the platform side's
 * responsibility -- this function may clobber it freely.
 */
bool fault_try_resolve(void *addr);

#endif
