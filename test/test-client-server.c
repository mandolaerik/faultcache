/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * End-to-end test of the client/server remote resolution split: a forked
 * child process acts as the server (fc_server_run()), while the parent
 * acts as the client, creating a region via fc_client_region_create() and
 * reading from it, letting the child resolve its faults.
 */
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-client.h"
#include "faultcache/faultcache-server.h"
#include "test-util.h"

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHUNK_SIZE 4096
#define NCHUNKS 3

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)chunk;
    uint8_t value = (uint8_t)(uintptr_t)user_data;
    memset(start, value, size);
}

/* Descriptor is a single byte: the fill value the factory should bind
 * fill_chunk()'s user_data to. Chunk layout is fixed for this test, but
 * decided entirely by the factory -- the client never states it. */
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
    out_layout->init_chunk = fill_chunk;
    out_layout->region_user_data =
        (void *)(uintptr_t) * (const uint8_t *)descriptor;
    out_layout->destroy_user_data = NULL;
    return NULL;
}

static void run_server(int conn_fd) {
    fc_server_t *server = fc_server_create(factory, NULL);
    CHECK(server != NULL);
    /* Returns once the client disconnects (see fc_server_run() docs). */
    CHECK(fc_server_run(server, conn_fd) == 0);
    fc_server_destroy(server);
    close(conn_fd);
    _exit(0);
}

int main(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        run_server(sv[1]);
        return 0; /* unreachable, run_server() calls _exit() */
    }
    close(sv[1]);

    fc_client_pool_t *pool = fc_client_pool_create();
    CHECK(pool != NULL);

    uint8_t descriptor = 0xAB;
    fc_client_region_t *region = fc_client_region_create(
        pool, sv[0], sizeof(descriptor), &descriptor, NULL);
    CHECK(region != NULL);
    CHECK(fc_client_region_size(region) == NCHUNKS * CHUNK_SIZE);

    const uint8_t *bytes = fc_client_region_base(region);
    for (size_t i = 0; i < NCHUNKS * CHUNK_SIZE; i++)
        CHECK(bytes[i] == 0xAB);

    fc_client_region_destroy(region);
    fc_client_pool_destroy(pool);

    close(sv[0]); /* server sees EOF, fc_server_run() returns */

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("test_client_server: OK\n");
    return 0;
}
