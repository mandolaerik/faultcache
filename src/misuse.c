/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */
#include "internal.h"
#include "faultcache/debug.h"

#include <stdio.h>
#include <stdlib.h>

static fc_misuse_hook_t g_misuse_hook = nullptr;

void fc_debug_set_misuse_hook(fc_misuse_hook_t hook) {
    g_misuse_hook = hook;
}

void fc_misuse(const char *what) {
    if (g_misuse_hook)
        g_misuse_hook(what);

    fprintf(stderr, "faultcache: misuse: %s\n", what);
    fc_flush_coverage_before_death();
    abort(); /* GCOVR_EXCL_LINE: gcov never marks a noreturn call's own
              * line as executed (no fall-through edge to attribute it
              * to) -- unrelated to fork/signal-death; confirmed with a
              * plain non-forked abort() too. */
}
