/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Windows side of the VM seam (src/vm.h).
 *
 * The seam exists because publishing has to be atomic against other
 * threads, and Windows offers exactly one way to do that:
 * MapViewOfFile3(MEM_REPLACE_PLACEHOLDER) swapping a section view into
 * a reservation. Hence the placeholder dance -- a plain
 * VirtualAlloc(MEM_RESERVE) region cannot have parts of it replaced,
 * and VirtualProtect-then-copy has the same race that rules out
 * mprotect-then-copy on POSIX.
 *
 * Consequence for the whole library: a view can only be placed at an
 * allocation-granularity boundary (64K), not a page boundary --
 * measured, not assumed: a page-granular base fails with
 * ERROR_MAPPED_ALIGNMENT while a page-granular *length* is accepted.
 * vm_page_size() therefore reports the allocation granularity, which
 * makes every target inproc.c computes 64K-aligned by construction.
 * Faults are coarser here than on Linux; nothing else changes.
 */
#include "vm.h"
#include "internal.h"

#include <stdint.h>
#include <windows.h>

size_t vm_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    /* Not dwPageSize: see the file comment. */
    return si.dwAllocationGranularity;
}

void *vm_reserve(size_t size) {
    void *p = VirtualAlloc2(nullptr, nullptr, size,
                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                            PAGE_NOACCESS, nullptr, 0);
    FC_ASSERT(p != nullptr);
    return p;
}

void vm_release(void *base, size_t size) {
    /* Splitting turned the one reservation into a run of independent
     * pieces, some of them views by now, so there is nothing to free in
     * a single call -- walk and release each piece for what it is. */
    char *p = base;
    char *end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        FC_ASSERT(VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi));
        SIZE_T step = mbi.RegionSize;
        if (mbi.State != MEM_FREE) {
            if (mbi.Type == MEM_MAPPED)
                FC_ASSERT(UnmapViewOfFile(p));
            else
                FC_ASSERT(VirtualFree(p, 0, MEM_RELEASE));
        }
        p += step;
    }
}

void vm_set_readable(void *addr, size_t size, bool readable) {
    DWORD old;
    FC_ASSERT(VirtualProtect(addr, size,
                             readable ? PAGE_READONLY : PAGE_NOACCESS, &old));
}

void vm_decommit(void *addr, size_t size) {
    /* MEM_PRESERVE_PLACEHOLDER is what makes this the counterpart of the
     * POSIX MAP_FIXED/PROT_NONE overwrite: the pages go back to the
     * pagefile-backed section and stop counting against the working set,
     * while the address range stays reserved and inaccessible so the
     * next touch faults here again rather than landing on someone
     * else's allocation. */
    char *p = addr;
    char *end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        FC_ASSERT(VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi));
        SIZE_T step = mbi.RegionSize;
        if (mbi.Type == MEM_MAPPED)
            FC_ASSERT(UnmapViewOfFileEx(p, MEM_PRESERVE_PLACEHOLDER));
        p += step;
    }
}

vm_scratch_t vm_scratch_create(size_t size) {
    /* Pagefile-backed rather than private memory, because publishing
     * hands this exact storage to the region: the target ends up as a
     * second view of this section, which is what makes the swap atomic
     * instead of a copy. */
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                    PAGE_READWRITE,
                                    (DWORD)((uint64_t)size >> 32),
                                    (DWORD)(size & 0xFFFFFFFFu), nullptr);
    FC_ASSERT(sec != nullptr);
    void *addr = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, size);
    FC_ASSERT(addr != nullptr);
    return (vm_scratch_t) {.addr = addr, .size = size, .section = sec};
}

void vm_scratch_seal(const vm_scratch_t *s) {
    DWORD old;
    FC_ASSERT(VirtualProtect(s->addr, s->size, PAGE_READONLY, &old));
}

/* Releases the staging view once every byte has been published or
 * discarded. Published ranges are views of the same section and keep it
 * alive on their own, so the handle can go here too. */
static void scratch_consume(vm_scratch_t *s, size_t len) {
    s->consumed += len;
    FC_ASSERT(s->consumed <= s->size);
    if (s->consumed == s->size) {
        FC_ASSERT(UnmapViewOfFile(s->addr));
        FC_ASSERT(CloseHandle(s->section));
        s->addr = nullptr;
    }
}

void vm_scratch_publish(vm_scratch_t *s, size_t off, size_t len,
                        void *target) {
    /* Preserve the original placeholder only when replacing it whole. */
    MEMORY_BASIC_INFORMATION mbi;
    FC_ASSERT(VirtualQuery(target, &mbi, sizeof(mbi)) == sizeof(mbi));
    if (mbi.BaseAddress != target || mbi.RegionSize != len ||
        mbi.AllocationBase != target)
        FC_ASSERT(VirtualFree(target, len,
                              MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));

    void *got = MapViewOfFile3(s->section, GetCurrentProcess(), target, off, len,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READONLY,
                               nullptr, 0);
    FC_ASSERT(got == target);
    scratch_consume(s, len);
}

void vm_scratch_discard(vm_scratch_t *s, size_t off, size_t len) {
    (void)off; /* the section is freed as a whole, once fully consumed */
    scratch_consume(s, len);
}
