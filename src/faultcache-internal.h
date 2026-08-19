/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Shared between both backends (faultcache-sigsegv.c, faultcache.c) --
 * not installed, not part of any public header.
 */
#ifndef FAULTCACHE_INTERNAL_H
#define FAULTCACHE_INTERNAL_H

/* Reports a caller bug (see fc_debug_set_misuse_hook() in
 * faultcache-debug.h for the test-hook escape hatch); by default prints
 * `what` to stderr and abort()s. Never returns unless a misbehaving
 * hook does. */
void fc_misuse(const char *what);

#endif
