/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * POSIX implementation of the virtual-memory seam (see vm.h). Linux
 * today: publishing needs mremap(MREMAP_FIXED), which is not in POSIX,
 * so this file is Linux-specific despite the name describing the
 * flavour of the APIs rather than the kernel.
 */
#define _GNU_SOURCE
#include "vm.h"
#include "internal.h"

#include <sys/mman.h>
#include <unistd.h>

size_t vm_page_size(void) {
    return (size_t)sysconf(_SC_PAGESIZE);
}

void *vm_reserve(size_t size) {
    void *base = mmap(nullptr, size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FC_ASSERT(base != MAP_FAILED);
    return base;
}

void vm_release(void *base, size_t size) {
    FC_ASSERT(munmap(base, size) == 0);
}

void vm_set_readable(void *addr, size_t size, bool readable) {
    FC_ASSERT(mprotect(addr, size, readable ? PROT_READ : PROT_NONE) == 0);
}

void vm_decommit(void *addr, size_t size) {
    /* Fresh anonymous PROT_NONE pages over the range both hide it and
     * free what it was holding; mprotect() alone would only hide it. */
    FC_ASSERT(mmap(addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
              != MAP_FAILED);
}

vm_scratch_t vm_scratch_create(size_t size) {
    /* Kernel-supplied anonymous pages are already zero-filled. */
    void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FC_ASSERT(addr != MAP_FAILED);
    return (vm_scratch_t) {.addr = addr, .size = size};
}

void vm_scratch_seal(const vm_scratch_t *s) {
    FC_ASSERT(mprotect(s->addr, s->size, PROT_READ) == 0);
}

void vm_scratch_publish(const vm_scratch_t *s, size_t off, size_t len,
                        void *target) {
    /* One syscall, so the target goes from inaccessible straight to
     * populated with no observable state in between. */
    FC_ASSERT(mremap((char *)s->addr + off, len, len,
                     MREMAP_MAYMOVE | MREMAP_FIXED, target) != MAP_FAILED);
}

void vm_scratch_discard(const vm_scratch_t *s, size_t off, size_t len) {
    FC_ASSERT(munmap((char *)s->addr + off, len) == 0);
}
