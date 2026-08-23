/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Confirms fc_region_factory_fn_t's destroy_user_data out-param is
 * invoked exactly once by fc_server_destroy(), releasing a heap object
 * the factory allocated. Client and "server" run as two threads of one
 * process (fc_server_run() blocks, so it gets its own thread) purely so
 * the destroy-call counter can be an ordinary shared variable.
 */
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-client.h"
#include "faultcache/faultcache-server.h"
#include "util.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHUNK_SIZE 4096
#define NCHUNKS 3

struct user_ctx {
    uint8_t fill_value;
};

static int g_destroy_calls = 0;

static void fill_chunk(uint32_t chunk, void *start, size_t size,
                        const void *user_data) {
    (void)chunk;
    const struct user_ctx *ctx = user_data;
    memset(start, ctx->fill_value, size);
}

static void destroy_user_data(void *user_data) {
    free(user_data);
    g_destroy_calls++;
}

static char *factory(size_t descriptor_size, const void *descriptor,
                      fc_region_recipe_t *out_layout,
                      void *factory_user_data) {
    (void)factory_user_data;
    if (descriptor_size != 1)
        return strdup("expected a 1-byte descriptor");

    struct user_ctx *ctx = malloc(sizeof(*ctx));
    size_t *sizes = malloc(NCHUNKS * sizeof(size_t));
    if (!ctx || !sizes) {
        free(ctx);
        free(sizes);
        return strdup("out of memory");
    }
    ctx->fill_value = *(const uint8_t *)descriptor;
    for (uint32_t i = 0; i < NCHUNKS; i++)
        sizes[i] = CHUNK_SIZE;

    out_layout->nchunks = NCHUNKS;
    out_layout->chunk_sizes = sizes;
    out_layout->fill_chunk = fill_chunk;
    out_layout->region_user_data = ctx;
    out_layout->destroy_user_data = destroy_user_data;
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

int main(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);

    fc_server_t *server = fc_server_create(factory, nullptr, 0);
    CHECK(server != nullptr);

    struct server_thread_arg arg = {server, sv[1]};
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, nullptr, server_thread_main, &arg) == 0);

    fc_client_pool_t *pool = fc_client_pool_create(0);
    CHECK(pool != nullptr);

    uint8_t descriptor = 0x7C;
    fc_client_region_t *region = fc_client_region_create(
        pool, sv[0], sizeof(descriptor), &descriptor, nullptr);
    CHECK(region != nullptr);

    const uint8_t *bytes = fc_client_region_base(region);
    for (size_t i = 0; i < NCHUNKS * CHUNK_SIZE; i++)
        CHECK(bytes[i] == 0x7C);

    fc_client_pool_destroy(pool); /* also covers cleanup of a still-live region */

    close(sv[0]); /* server thread sees EOF, fc_server_run() returns */
    pthread_join(server_thread, nullptr);
    close(sv[1]);

    CHECK(g_destroy_calls == 0); /* not freed while the server was alive */
    fc_server_destroy(server);
    CHECK(g_destroy_calls == 1); /* freed exactly once at teardown */

    printf("test_client_server_destroy: OK\n");
    return 0;
}
