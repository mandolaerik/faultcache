/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Shared between both backends (faultcache-sigsegv.c, faultcache.c) --
 * not installed, not part of any public header.
 */
#ifndef FAULTCACHE_INTERNAL_H
#define FAULTCACHE_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>

/* Reports a caller bug (see fc_debug_set_misuse_hook() in
 * faultcache-debug.h for the test-hook escape hatch); by default prints
 * `what` to stderr and abort()s. Never returns unless a misbehaving
 * hook does. */
void fc_misuse(const char *what);

/* Internal invariant check: aborts on failure regardless of NDEBUG
 * (unlike libc assert()). Not routed through fc_misuse()/its test hook
 * -- a violation here is a bug in this library, not caller misuse, so
 * nothing should be able to intercept and survive it. */
#define FC_ASSERT(cond)                                                      \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "faultcache: internal error: %s:%d: %s\n",       \
                    __FILE__, __LINE__, #cond);                              \
            fc_flush_coverage_before_death();                                \
            abort();                                                         \
        }                                                                    \
    } while (0)

/*
 * Several death paths in this library are only exercisable by a test
 * that forks and lets the child terminate via a real signal (SIGABRT
 * from an abort() below, or a chained/re-raised SIGSEGV) rather than a
 * normal exit() -- gcov only writes .gcda files at normal exit, via an
 * atexit-registered hook, so lines reached only inside such a death
 * would otherwise never show as covered despite genuinely running.
 * __gcov_dump() (libgcov, GCC >= 8) flushes them manually. Only
 * declared/called when FC_COVERAGE_BUILD is defined (src/meson.build,
 * gated on -Db_coverage=true) -- a plain weak reference isn't enough to
 * pull __gcov_dump's definition out of libgcov (nothing else forces the
 * linker to resolve it, so it silently resolves to NULL and is never
 * called, confirmed empirically), and a normal build must not need to
 * link libgcov at all. Not generally async-signal-safe (it does file
 * I/O), but every call site here is immediately before an
 * already-inevitable death in a test-only scenario. */
#ifdef FC_COVERAGE_BUILD
extern void __gcov_dump(void);
static inline void fc_flush_coverage_before_death(void) {
    __gcov_dump();
}
#else
static inline void fc_flush_coverage_before_death(void) {
}
#endif

#endif
