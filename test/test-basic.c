/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <stdint.h>
#include <string.h>

static int init_count = 0;

static void init_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    CHECK(chunk == 0);
    init_count++;
    memset(start, 'A', size);
}

int main(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != NULL);

    size_t sizes[] = {4096 * 3 + 17};
    fc_region_t *region = fc_region_create(pool, 1, sizes, init_chunk, NULL);
    CHECK(region != NULL);
    CHECK(fc_region_size(region) == sizes[0]);

    const unsigned char *p = fc_region_base(region);
    CHECK(p[0] == 'A');
    CHECK(p[sizes[0] - 1] == 'A');
    CHECK(p[4096] == 'A'); /* second page, still the same chunk */
    CHECK(init_count == 1);

    /* Re-reading must not re-invoke the callback. */
    CHECK(p[10] == 'A');
    CHECK(init_count == 1);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
