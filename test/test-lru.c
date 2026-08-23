/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <string.h>

#define PAGE 4096

static int counts[3];

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    CHECK(chunk < 3);
    counts[chunk]++;
    memset(start, 'a' + (int)chunk, size);
}

int main(void) {
    fc_init();
    fc_pool_t *pool = fc_pool_create(PAGE);
    CHECK(pool != nullptr);

    size_t sizes[] = {PAGE, PAGE};
    fc_region_t *region = fc_region_create(pool, 2, sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    const unsigned char *p = fc_region_base(region);

    CHECK(p[0] == 'a');
    CHECK(counts[0] == 1 && counts[1] == 0);

    CHECK(p[PAGE] == 'b');
    CHECK(counts[0] == 1 && counts[1] == 1);

    /* The first chunk should now have been evicted to satisfy the
     * PAGE-sized budget, so touching it again must re-run fill_chunk(). */
    CHECK(p[0] == 'a');
    CHECK(counts[0] == 2 && counts[1] == 1);

    fc_region_destroy(region);
    fc_pool_destroy(pool);

    memset(counts, 0, sizeof(counts));

    pool = fc_pool_create(PAGE);
    CHECK(pool != nullptr);

    /* The first two chunks share the first page; the third starts on the
     * next page and acts as the eviction trigger. */
    size_t shared_sizes[] = {100, PAGE - 100, PAGE};
    region = fc_region_create(pool, 3, shared_sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    p = fc_region_base(region);

    CHECK(p[0] == 'a');
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 0);

    CHECK(p[PAGE] == 'c');
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1);

    /* Chunks 0 and 1 own no page of their own, so evicting them frees
     * nothing and the page they share stays mapped -- re-reading it must
     * not fault, let alone re-fill. */
    fc_region_destroy(region);
    fc_pool_destroy(pool);

    memset(counts, 0, sizeof(counts));

    /* A chunk bigger than the whole budget: the chunk that was just
     * resolved is protected from eviction, so the pool stays over its
     * target rather than throwing away what the caller is reading. */
    pool = fc_pool_create(PAGE);
    CHECK(pool != nullptr);

    size_t big_sizes[] = {4 * PAGE};
    region = fc_region_create(pool, 1, big_sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    p = fc_region_base(region);

    CHECK(p[0] == 'a');
    CHECK(p[4 * PAGE - 1] == 'a');
    CHECK(counts[0] == 1);

    fc_region_destroy(region);
    fc_pool_destroy(pool);

    memset(counts, 0, sizeof(counts));

    /* Destroying a region while a shared page is still half-staged (only
     * one of its two contributors has been filled) must release the
     * staging page too. */
    pool = fc_pool_create(0);
    CHECK(pool != nullptr);

    size_t partial_sizes[] = {100, 2 * PAGE};
    region = fc_region_create(pool, 2, partial_sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    p = fc_region_base(region);

    CHECK(p[PAGE] == 'b');
    CHECK(counts[0] == 0 && counts[1] == 1);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
