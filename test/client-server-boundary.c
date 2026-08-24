/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * End-to-end boundary test for the client/server path: verifies that
 * server-side resolve grouping follows page-sharing boundaries exactly,
 * including tiny chunks in the middle of a page.
 */
#include "faultcache/faultcache.h"
#include "faultcache/client.h"
#include "faultcache/server.h"
#include "test-common.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define NCHUNKS 6

static int counter_write_fd;
static const size_t sizes[NCHUNKS] = {
    100,
    50,
    30,
    FC_TEST_PAGE_SIZE - 180,
    FC_TEST_PAGE_SIZE + 30,
    20,
};

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)user_data;
    CHECK(chunk < NCHUNKS);
    CHECK(size == sizes[chunk]);
    memset(start, 'A' + (int)chunk, size);

    uint8_t id = (uint8_t)(chunk + 1);
    ssize_t unused = write(counter_write_fd, &id, 1);
    (void)unused;
}

static char *factory(size_t descriptor_size, const void *descriptor,
                      fc_region_recipe_t *out_layout,
                      void *factory_user_data) {
    (void)factory_user_data;
    CHECK(descriptor_size == 1);
    CHECK(*(const uint8_t *)descriptor == 0x11);

    size_t *layout = malloc((size_t)NCHUNKS * sizeof(size_t));
    if (!layout)
        return strdup("out of memory");
    for (uint32_t i = 0; i < NCHUNKS; i++)
        layout[i] = sizes[i];

    out_layout->nchunks = NCHUNKS;
    out_layout->chunk_sizes = layout;
    out_layout->fill_chunk = fill_chunk;
    out_layout->region_user_data = nullptr;
    out_layout->destroy_user_data = nullptr;
    return nullptr;
}

static void run_server(int conn_fd) {
    fc_server_t *server = fc_server_create(factory, nullptr, 0);
    CHECK(server != nullptr);
    CHECK(fc_server_run(server, conn_fd) == 0);
    fc_server_destroy(server);
    close(conn_fd);
    _exit(0);
}

static int read_count_for_chunk(int counts[NCHUNKS], uint8_t id) {
    CHECK(id >= 1 && id <= NCHUNKS);
    int idx = (int)id - 1;
    counts[idx]++;
    return idx;
}

int main(void) {
    int sv[2], counter_pipe[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    CHECK(pipe(counter_pipe) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        close(counter_pipe[0]);
        counter_write_fd = counter_pipe[1];
        run_server(sv[1]);
        return 0; /* unreachable */
    }

    close(sv[1]);
    close(counter_pipe[1]);

    fc_client_pool_t *pool = fc_client_pool_create(0);
    CHECK(pool != nullptr);

    uint8_t descriptor = 0x11;
    fc_client_region_t *region =
        fc_client_region_create(pool, sv[0], sizeof(descriptor), &descriptor,
                                nullptr);
    CHECK(region != nullptr);

    size_t offsets[NCHUNKS];
    offsets[0] = 0;
    for (int i = 1; i < NCHUNKS; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];
    CHECK(offsets[3] == 180);
    CHECK(offsets[4] == FC_TEST_PAGE_SIZE);
    CHECK(offsets[5] == 8222);

    const uint8_t *p = fc_client_region_base(region);

    /* Touch chunk1: must resolve chunks 0..3 only. */
    CHECK(p[offsets[1] + 10] == 'B');

    /* Touch chunk4: must resolve chunks 4..5 only. */
    CHECK(p[offsets[4] + 10] == 'E');

    /* Read back entire region for content correctness. */
    for (int c = 0; c < NCHUNKS; c++) {
        for (size_t i = 0; i < sizes[c]; i++)
            CHECK(p[offsets[c] + i] == (uint8_t)('A' + c));
    }

    fc_client_region_destroy(region);
    fc_client_pool_destroy(pool);
    close(sv[0]);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int counts[NCHUNKS] = {0};
    uint8_t id;
    while (read(counter_pipe[0], &id, 1) == 1)
        (void)read_count_for_chunk(counts, id);
    close(counter_pipe[0]);

    for (int c = 0; c < NCHUNKS; c++)
        CHECK(counts[c] == 1);

    printf("test_client_server_boundary: OK\n");
    return 0;
}
