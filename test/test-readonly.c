/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 */

#include "faultcache/faultcache.h"
#include "test-util.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void init_chunk(uint32_t chunk, void *start, size_t size,
                        void *user_data) {
    (void)chunk;
    (void)user_data;
    memset(start, 'X', size);
}

int main(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != NULL);

    size_t sizes[] = {4096};
    const void *base = fc_region_create(pool, 1, sizes, init_chunk, NULL);
    CHECK(base != NULL);
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

    CHECK(fc_region_destroy(pool, base) == 0);
    fc_pool_destroy(pool);
    return 0;
}
