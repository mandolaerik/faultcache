/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-common.h"
#include "util.h"

#include <string.h>

#define PAGE FC_TEST_PAGE_SIZE
#define NCHUNKS 5

static int counts[NCHUNKS];

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    CHECK(chunk < NCHUNKS);
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

    memset(counts, 0, sizeof(counts));

    /* Midpoint protection: reads of a resident chunk are invisible to
     * the library, so recency can only be observed by demoting the cold
     * half of the queue and waiting for it to fault back in. Fill four
     * page-sized chunks to fill the budget exactly -- that leaves chunks
     * 0 and 1 demoted -- then read chunk 0 again and add a fifth chunk to
     * force one eviction. */
    pool = fc_pool_create(4 * PAGE);
    CHECK(pool != nullptr);

    size_t even_sizes[] = {PAGE, PAGE, PAGE, PAGE, PAGE};
    region = fc_region_create(pool, NCHUNKS, even_sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    p = fc_region_base(region);

    for (uint32_t c = 0; c < 4; c++)
        CHECK(p[c * PAGE] == 'a' + (int)c);
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1
          && counts[3] == 1 && counts[4] == 0);

    /* Touching chunk 0 faults on its demoted pages and makes it the most
     * recently used one, without re-running fill_chunk(). */
    CHECK(p[0] == 'a');
    CHECK(counts[0] == 1);

    CHECK(p[4 * PAGE] == 'e');
    CHECK(counts[4] == 1);

    /* Chunk 1 is now the coldest and was evicted in chunk 4's favour,
     * while the freshly touched chunk 0 survived. */
    CHECK(p[0] == 'a');
    CHECK(counts[0] == 1);

    CHECK(p[PAGE] == 'b');
    CHECK(counts[1] == 2);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
