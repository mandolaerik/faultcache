/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Confirms a rejected descriptor fails fc_client_region_create() with
 * errno == ECONNREFUSED and relays the factory's rejection message to
 * the client, malloc'd and owned by the caller.
 */
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-client.h"
#include "faultcache/faultcache-server.h"
#include "test-util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static char *factory(size_t descriptor_size, const void *descriptor,
                      fc_region_recipe_t *out_layout,
                      void *factory_user_data) {
    (void)descriptor;
    (void)out_layout;
    (void)factory_user_data;
    CHECK(descriptor_size == 1);
    return strdup("descriptor rejected: unknown fill value");
}

static void run_server(int conn_fd) {
    fc_server_t *server = fc_server_create(factory, NULL);
    CHECK(server != NULL);
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

    uint8_t descriptor = 0xFF;
    char *error = NULL;
    const void *addr = fc_client_region_create(pool, sv[0], sizeof(descriptor),
                                                &descriptor, &error);
    CHECK(addr == NULL);
    CHECK(errno == ECONNREFUSED);
    CHECK(error != NULL);
    CHECK(strcmp(error, "descriptor rejected: unknown fill value") == 0);
    free(error);

    fc_client_pool_destroy(pool);
    close(sv[0]); /* server sees EOF, fc_server_run() returns */

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("test_client_server_reject: OK\n");
    return 0;
}
