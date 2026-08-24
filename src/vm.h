/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * The virtual-memory seam: every page-level kernel call the inline
 * backend (inproc.c) makes goes through here, so that a port is a new
 * vm-<platform>.c rather than #ifdefs scattered through the fault
 * logic. Failures abort -- surviving them would need fault injection to
 * test, and the callers have nowhere to put an error anyway, since most
 * of them run inside a fault handler.
 *
 * The contract that actually matters is publishing. A chunk is filled
 * into a scratch buffer and only then installed over the target range,
 * and the install must be atomic with respect to other threads: there
 * must be no instant at which the target is readable but not yet
 * populated. mprotect()-then-memcpy() in place does NOT qualify -- the
 * protection change lands process-wide immediately, so another thread
 * can read the gap (empirically confirmed during design). POSIX gets
 * this from mremap(MREMAP_FIXED); a port must find its own equivalent
 * (on Windows, MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)) rather than
 * fall back to copying into place.
 */
#ifndef FAULTCACHE_VM_H
#define FAULTCACHE_VM_H

#include <stdbool.h>
#include <stddef.h>

size_t vm_page_size(void);

/*
 * Address space that reads as inaccessible until something is published
 * over it. `size` is a whole number of pages.
 */
void *vm_reserve(size_t size);
void vm_release(void *base, size_t size);

/* Takes read access away from a populated range, or hands it back. The
 * contents survive either way -- this is the midpoint demotion, not
 * eviction. */
void vm_set_readable(void *addr, size_t size, bool readable);

/*
 * Returns a published range to the reserved-but-inaccessible state and
 * releases its physical pages. Merely hiding it would leave the pages
 * charged to RSS, which would defeat the point of evicting.
 */
void vm_decommit(void *addr, size_t size);

/*
 * A writable staging buffer that can later be published, atomically and
 * piecewise, into a reservation. Zero-filled on creation, so a caller
 * that fills only part of it still gets defined bytes for the rest.
 */
typedef struct {
    void *addr;
    size_t size;
} vm_scratch_t;

vm_scratch_t vm_scratch_create(size_t size);

/* Drops write access. Callers publish read-only memory, so this happens
 * before the range is reachable by anyone else rather than after. */
void vm_scratch_seal(const vm_scratch_t *s);

/* Moves [off, off+len) to `target`, atomically replacing whatever the
 * reservation had there. The published bytes leave the scratch buffer;
 * whatever remains is still the caller's to discard. */
void vm_scratch_publish(const vm_scratch_t *s, size_t off, size_t len,
                        void *target);

/* Throws away a piece that will never be published. */
void vm_scratch_discard(const vm_scratch_t *s, size_t off, size_t len);

#endif
