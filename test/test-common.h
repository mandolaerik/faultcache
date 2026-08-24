/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef FAULTCACHE_TEST_COMMON_H
#define FAULTCACHE_TEST_COMMON_H

#ifdef _WIN32
#define FC_TEST_PAGE_SIZE 65536
#else
#define FC_TEST_PAGE_SIZE 4096
#endif

#endif
