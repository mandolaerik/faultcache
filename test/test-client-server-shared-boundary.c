/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Two client connections, same descriptor, boundary-heavy layout.
 * The first access happens in different chunks on each connection but
 * within the same shared page-group (chunks 0..3). This must still
 * derive each chunk at most once server-side.
 */
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-client.h"
#include "faultcache/faultcache-server.h"
#include "test-util.h"

#include <pthread.h>
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
    4096 - 180,
    8222 - 4096,
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
    CHECK(*(const uint8_t *)descriptor == 0x22);

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

struct server_thread_arg {
    fc_server_t *server;
    int conn_fd;
};

static void *server_thread_main(void *arg) {
    struct server_thread_arg *a = arg;
    CHECK(fc_server_run(a->server, a->conn_fd) == 0);
    return nullptr;
}

static void run_server(int conn_fd_a, int conn_fd_b) {
    fc_server_t *server = fc_server_create(factory, nullptr, 0);
    CHECK(server != nullptr);

    struct server_thread_arg arg_a = {server, conn_fd_a};
    struct server_thread_arg arg_b = {server, conn_fd_b};
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, nullptr, server_thread_main, &arg_a) == 0);
    CHECK(pthread_create(&tb, nullptr, server_thread_main, &arg_b) == 0);
    pthread_join(ta, nullptr);
    pthread_join(tb, nullptr);

    fc_server_destroy(server);
    close(conn_fd_a);
    close(conn_fd_b);
    _exit(0);
}

int main(void) {
    int svA[2], svB[2], counter_pipe[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, svA) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, svB) == 0);
    CHECK(pipe(counter_pipe) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(svA[0]);
        close(svB[0]);
        close(counter_pipe[0]);
        counter_write_fd = counter_pipe[1];
        run_server(svA[1], svB[1]);
        return 0; /* unreachable */
    }

    close(svA[1]);
    close(svB[1]);
    close(counter_pipe[1]);

    fc_client_pool_t *pool = fc_client_pool_create(0);
    CHECK(pool != nullptr);

    uint8_t descriptor = 0x22;
    fc_client_region_t *region_a =
        fc_client_region_create(pool, svA[0], sizeof(descriptor), &descriptor,
                                nullptr);
    CHECK(region_a != nullptr);
    fc_client_region_t *region_b =
        fc_client_region_create(pool, svB[0], sizeof(descriptor), &descriptor,
                                nullptr);
    CHECK(region_b != nullptr);

    size_t offsets[NCHUNKS];
    offsets[0] = 0;
    for (int i = 1; i < NCHUNKS; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];

    const uint8_t *a = fc_client_region_base(region_a);
    const uint8_t *b = fc_client_region_base(region_b);

    /* Asymmetric first touches within the same shared page group [0..3]. */
    CHECK(a[offsets[0] + 5] == 'A');
    CHECK(b[offsets[2] + 5] == 'C');

    /* Then touch chunk4 from one connection, which must also pull chunk5. */
    CHECK(a[offsets[4] + 7] == 'E');

    /* Verify full content on both client mappings. */
    for (int c = 0; c < NCHUNKS; c++) {
        for (size_t i = 0; i < sizes[c]; i++) {
            CHECK(a[offsets[c] + i] == (uint8_t)('A' + c));
            CHECK(b[offsets[c] + i] == (uint8_t)('A' + c));
        }
    }

    fc_client_region_destroy(region_a);
    fc_client_region_destroy(region_b);
    fc_client_pool_destroy(pool);
    close(svA[0]);
    close(svB[0]);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    int counts[NCHUNKS] = {0};
    uint8_t id;
    while (read(counter_pipe[0], &id, 1) == 1) {
        CHECK(id >= 1 && id <= NCHUNKS);
        counts[(int)id - 1]++;
    }
    close(counter_pipe[0]);

    for (int c = 0; c < NCHUNKS; c++)
        CHECK(counts[c] == 1);

    printf("test_client_server_shared_boundary: OK\n");
    return 0;
}
