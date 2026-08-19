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
            abort();                                                         \
        }                                                                    \
    } while (0)

#endif
