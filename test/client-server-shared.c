/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Two client connections handing off regions with the SAME descriptor to
 * one fc_server_t (serviced by two threads, one fc_server_run() call
 * each) must resolve to the same server-side region: each chunk is
 * derived by fill_chunk at most once, no matter which connection (or how
 * many) faults on it, yet every connection reads back correct content.
 */
#include "faultcache/faultcache.h"
#include "faultcache/client.h"
#include "faultcache/server.h"
#include "test-common.h"
#include "util.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHUNK_SIZE FC_TEST_PAGE_SIZE
#define NCHUNKS 3

/* Server-side only: counts actual fill_chunk invocations, via a pipe
 * inherited across fork() so the client (parent) can observe it after
 * the server (child) has exited. */
static int counter_write_fd;

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)chunk;
    uint8_t value = (uint8_t)(uintptr_t)user_data;
    memset(start, value, size);
    uint8_t one = 1;
    ssize_t unused = write(counter_write_fd, &one, 1);
    (void)unused;
}

static char *factory(size_t descriptor_size, const void *descriptor,
                      fc_region_recipe_t *out_layout,
                      void *factory_user_data) {
    (void)factory_user_data;
    if (descriptor_size != 1)
        return strdup("expected a 1-byte descriptor");
    size_t *sizes = malloc(NCHUNKS * sizeof(size_t));
    if (!sizes)
        return strdup("out of memory");
    for (uint32_t i = 0; i < NCHUNKS; i++)
        sizes[i] = CHUNK_SIZE;
    out_layout->nchunks = NCHUNKS;
    out_layout->chunk_sizes = sizes;
    out_layout->fill_chunk = fill_chunk;
    out_layout->region_user_data =
        (void *)(uintptr_t) * (const uint8_t *)descriptor;
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

    uint8_t descriptor = 0x42; /* same descriptor on both connections */

    fc_client_region_t *region_a = fc_client_region_create(
        pool, svA[0], sizeof(descriptor), &descriptor, nullptr);
    CHECK(region_a != nullptr);
    fc_client_region_t *region_b = fc_client_region_create(
        pool, svB[0], sizeof(descriptor), &descriptor, nullptr);
    CHECK(region_b != nullptr);
    CHECK(region_a != region_b); /* distinct client-side mappings */

    const uint8_t *bytes_a = fc_client_region_base(region_a);
    for (size_t i = 0; i < NCHUNKS * CHUNK_SIZE; i++)
        CHECK(bytes_a[i] == 0x42);

    const uint8_t *bytes_b = fc_client_region_base(region_b);
    for (size_t i = 0; i < NCHUNKS * CHUNK_SIZE; i++)
        CHECK(bytes_b[i] == 0x42);

    fc_client_region_destroy(region_a);
    fc_client_region_destroy(region_b);
    fc_client_pool_destroy(pool);

    close(svA[0]);
    close(svB[0]);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Server process has exited: no more writes to the pipe can happen,
     * so the byte count read now is final. Must be exactly NCHUNKS (not
     * 2*NCHUNKS) -- proof that the second region's chunks were served
     * from the shared cache, never re-derived. */
    uint8_t marker;
    int count = 0;
    while (read(counter_pipe[0], &marker, 1) == 1)
        count++;
    close(counter_pipe[0]);
    CHECK(count == NCHUNKS);

    printf("test_client_server_shared: OK (fill_chunk called %d times for %d "
           "chunks x 2 connections)\n",
           count, NCHUNKS);
    return 0;
}
