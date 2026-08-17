/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <string.h>

#define NCHUNKS 5
static int counts[NCHUNKS];

static void init_chunk(uint32_t chunk, void *start, size_t size,
                        void *user_data) {
    (void)user_data;
    counts[chunk]++;
    memset(start, 'a' + (int)chunk, size);
}

int main(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != NULL);

    size_t page = 4096;
    size_t sizes[NCHUNKS] = {page, page * 2, page, page * 3, page};
    const void *base = fc_region_create(pool, NCHUNKS, sizes, init_chunk, NULL);
    CHECK(base != NULL);

    size_t offsets[NCHUNKS];
    offsets[0] = 0;
    for (int i = 1; i < NCHUNKS; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];

    const unsigned char *p = base;

    /* Touching one chunk must not touch any other (all chunks here are
     * page-aligned, so there is no sharing between them). */
    CHECK(p[offsets[2]] == 'c');
    CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 1 &&
          counts[3] == 0 && counts[4] == 0);

    CHECK(p[offsets[0]] == 'a');
    CHECK(p[offsets[4] + sizes[4] - 1] == 'e');
    CHECK(counts[0] == 1 && counts[4] == 1);

    /* Re-reading an already-resolved chunk must not re-invoke init_chunk. */
    CHECK(p[offsets[2] + sizes[2] - 1] == 'c');
    CHECK(counts[2] == 1);

    /* The whole (multi-page) chunk 3 must be filled consistently once
     * touched. */
    for (size_t i = 0; i < sizes[3]; i++)
        CHECK(p[offsets[3] + i] == 'd');
    CHECK(counts[3] == 1);

    CHECK(fc_region_destroy(pool, base) == 0);
    fc_pool_destroy(pool);
    return 0;
}
