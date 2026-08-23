/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "util.h"

#include <string.h>
#include <unistd.h>

/*
 * Layout (page size assumed to be 4096, checked in main()):
 *   chunk0 [0,100)         chunk1 [100,150)       chunk2 [150,180)
 *   chunk3 [180,4096)      <- ends exactly on a page boundary
 *   chunk4 [4096,8222)     <- starts page-aligned, spans two pages
 *   chunk5 [8222,8242)     <- shares its first (and only) page with chunk4
 *
 * Chunks 0-3 all share page 0 and must resolve together; chunk3 ending
 * exactly on a page boundary must stop the group from cascading into
 * chunk4. Chunk4 owns page 1 outright and must resolve alone, even though
 * it shares page 2 with chunk5.
 */
#define NCHUNKS 6
static int counts[NCHUNKS];
static const size_t sizes[NCHUNKS] = {100, 50, 30, 4096 - 180, 8222 - 4096,
                                       20};

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    CHECK(size == sizes[chunk]);
    counts[chunk]++;
    memset(start, 'A' + (int)chunk, size);
}

int main(void) {
    fc_init();
    CHECK(sysconf(_SC_PAGESIZE) == 4096);

    fc_pool_t *pool = fc_pool_create(0);
    CHECK(pool != nullptr);

    size_t offsets[NCHUNKS];
    offsets[0] = 0;
    for (int i = 1; i < NCHUNKS; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];
    CHECK(offsets[3] == 180);
    CHECK(offsets[4] == 4096);
    CHECK(offsets[5] == 8222);

    fc_region_t *region = fc_region_create(pool, NCHUNKS, sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);
    const unsigned char *p = fc_region_base(region);

    /* Touch chunk1: must resolve chunks 0-3 (sharing page 0) in one go,
     * and must NOT touch chunks 4-5. */
    CHECK(p[offsets[1] + 10] == 'B');
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1 &&
          counts[3] == 1);
    CHECK(counts[4] == 0 && counts[5] == 0);

    /* Touch chunk4's own page: it shares page 2 with chunk5, but that page
     * is not what faulted, so chunk5 must stay untouched. */
    CHECK(p[offsets[4] + 10] == 'E');
    CHECK(counts[4] == 1 && counts[5] == 0);
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1 &&
          counts[3] == 1);

    /* Touch the page they do share: chunk4 already contributed its half,
     * so only chunk5 is still missing. */
    CHECK(p[offsets[5]] == 'F');
    CHECK(counts[4] == 1 && counts[5] == 1);

    /* Every byte of every chunk must match its own fill pattern. */
    for (int c = 0; c < NCHUNKS; c++) {
        for (size_t i = 0; i < sizes[c]; i++)
            CHECK(p[offsets[c] + i] == (unsigned char)('A' + c));
        CHECK(counts[c] == 1);
    }

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
