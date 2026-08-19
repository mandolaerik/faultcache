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

    fprintf(stderr, "faultcache: misuse: %s\n", what);
    abort();
}
