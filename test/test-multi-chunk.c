/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <string.h>

#define NCHUNKS 5
static int counts[NCHUNKS];

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    counts[chunk]++;
    memset(start, 'a' + (int)chunk, size);
}

int main(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != nullptr);

    size_t page = 4096;
    size_t sizes[NCHUNKS] = {page, page * 2, page, page * 3, page};
    fc_region_t *region = fc_region_create(pool, NCHUNKS, sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);

    size_t offsets[NCHUNKS];
    offsets[0] = 0;
    for (int i = 1; i < NCHUNKS; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];

    const unsigned char *p = fc_region_base(region);

    /* Touching one chunk must not touch any other (all chunks here are
     * page-aligned, so there is no sharing between them). */
    CHECK(p[offsets[2]] == 'c');
    CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 1 &&
          counts[3] == 0 && counts[4] == 0);

    CHECK(p[offsets[0]] == 'a');
    CHECK(p[offsets[4] + sizes[4] - 1] == 'e');
    CHECK(counts[0] == 1 && counts[4] == 1);

    /* Re-reading an already-resolved chunk must not re-invoke fill_chunk. */
    CHECK(p[offsets[2] + sizes[2] - 1] == 'c');
    CHECK(counts[2] == 1);

    /* The whole (multi-page) chunk 3 must be filled consistently once
     * touched. */
    for (size_t i = 0; i < sizes[3]; i++)
        CHECK(p[offsets[3] + i] == 'd');
    CHECK(counts[3] == 1);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
