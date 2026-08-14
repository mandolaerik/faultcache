/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef FAULTCACHE_TEST_UTIL_H
#define FAULTCACHE_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>

/* Minimal assertion macro: unlike assert(), it is never compiled out and
 * always prints which condition failed. */
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#endif /* FAULTCACHE_TEST_UTIL_H */
