/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */
#include "faultcache-internal.h"
#include "faultcache/faultcache-debug.h"

#include <stdio.h>
#include <stdlib.h>

static fc_misuse_hook_t g_misuse_hook = NULL;

void fc_debug_set_misuse_hook(fc_misuse_hook_t hook) {
    g_misuse_hook = hook;
}

void fc_misuse(const char *what) {
    if (g_misuse_hook)
        g_misuse_hook(what);

    /* Tested for real (test_default_abort_still_works in
     * test-misuse.c, with no hook installed), but that test forks and
     * the child necessarily terminates via SIGABRT rather than a
     * normal exit() -- gcov only flushes counters at normal exit, so
     * these two lines never show as covered despite genuinely running.
     * GCOVR_EXCL_START */
    fprintf(stderr, "faultcache: misuse: %s\n", what);
    abort();
    /* GCOVR_EXCL_STOP */
}
