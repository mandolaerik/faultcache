/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "util.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)chunk;
    (void)user_data;
    memset(start, 'X', size);
}

int main(void) {
    fc_init();
    fc_pool_t *pool = fc_pool_create(0);
    CHECK(pool != nullptr);

    size_t sizes[] = {4096};
    fc_region_t *region = fc_region_create(pool, 1, sizes, fill_chunk, nullptr);
    CHECK(region != nullptr);
    const void *base = fc_region_base(region);
    CHECK(((const unsigned char *)base)[0] == 'X');

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* Child: the mapping is read-only, this write must be fatal. */
        *(volatile unsigned char *)base = 'Y';
        _exit(0); /* must not be reached */
    }

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    return 0;
}
