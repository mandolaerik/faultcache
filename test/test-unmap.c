/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)chunk;
    (void)user_data;
    memset(start, 'Z', size);
}

int main(void) {
    fc_init();
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != nullptr);

    size_t sizes[] = {4096, 8192};
    fc_region_t *region = fc_region_create(pool, 2, sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);
    CHECK(fc_region_size(region) == 4096 + 8192);
    const void *base = fc_region_base(region);
    CHECK(((const unsigned char *)base)[0] == 'Z');

    /* region is freed by this call; querying it afterwards isn't
     * possible any more (it's an opaque handle, not an address to look
     * up) -- the fork()+access below is what actually proves the
     * mapping is gone. */
    fc_region_destroy(region);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* Child: the mapping was released, this access must be fatal. */
        volatile unsigned char v = *(const volatile unsigned char *)base;
        (void)v;
        _exit(0); /* must not be reached */
    }

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    fc_pool_destroy(pool);
    return 0;
}
