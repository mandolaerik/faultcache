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
#include "faultcache/faultcache-client-server.h"
#include "test-util.h"

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
                        void *user_data) {
    (void)chunk;
    struct user_ctx *ctx = user_data;
    memset(start, ctx->fill_value, size);
}

static void destroy_user_data(void *user_data) {
    free(user_data);
    g_destroy_calls++;
}

static char *factory(size_t descriptor_size, const void *descriptor,
                      uint32_t *out_nchunks, size_t **out_chunk_sizes,
                      fc_init_chunk_fn_t *out_init_chunk, void **out_user_data,
                      fc_region_destroy_fn_t *out_destroy_user_data,
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

    *out_nchunks = NCHUNKS;
    *out_chunk_sizes = sizes;
    *out_init_chunk = fill_chunk;
    *out_user_data = ctx;
    *out_destroy_user_data = destroy_user_data;
    return NULL;
}

struct server_thread_arg {
    fc_server_t *server;
    int conn_fd;
};

static void *server_thread_main(void *arg) {
    struct server_thread_arg *a = arg;
    CHECK(fc_server_run(a->server, a->conn_fd) == 0);
    return NULL;
}

int main(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);

    fc_server_t *server = fc_server_create(factory, NULL);
    CHECK(server != NULL);

    struct server_thread_arg arg = {server, sv[1]};
    pthread_t server_thread;
    CHECK(pthread_create(&server_thread, NULL, server_thread_main, &arg) == 0);

    fc_client_pool_t *pool = fc_client_pool_create();
    CHECK(pool != NULL);

    uint8_t descriptor = 0x7C;
    const void *addr = fc_client_region_create(pool, sv[0], sizeof(descriptor),
                                                &descriptor, NULL);
    CHECK(addr != NULL);

    const uint8_t *bytes = addr;
    for (size_t i = 0; i < NCHUNKS * CHUNK_SIZE; i++)
        CHECK(bytes[i] == 0x7C);

    CHECK(fc_client_region_destroy(pool, addr) == 0);
    fc_client_pool_destroy(pool);

    close(sv[0]); /* server thread sees EOF, fc_server_run() returns */
    pthread_join(server_thread, NULL);
    close(sv[1]);

    CHECK(g_destroy_calls == 0); /* not freed while the server was alive */
    fc_server_destroy(server);
    CHECK(g_destroy_calls == 1); /* freed exactly once at teardown */

    printf("test_client_server_destroy: OK\n");
    return 0;
}
