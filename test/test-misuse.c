/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Exercises fc_misuse() call sites without killing the test binary (via
 * the debug misuse-hook), one real unhooked death test to confirm the
 * default print+abort() path still works, a genuine-SIGSEGV passthrough
 * check, and a couple of cheap-but-real edge cases (invalid chunk sizes,
 * pool destroy-ordering) that don't need any of that machinery.
 */
#include "faultcache/faultcache.h"
#include "faultcache/faultcache-client.h"
#include "faultcache/faultcache-debug.h"
#include "test-util.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static jmp_buf g_misuse_jmp;
static char g_misuse_what[256];

static void record_and_jump(const char *what) {
    snprintf(g_misuse_what, sizeof(g_misuse_what), "%s", what);
    longjmp(g_misuse_jmp, 1);
}

/* Triggers `call` (expected to invoke fc_misuse()) via the debug hook
 * instead of actually aborting, then checks the reported message. */
#define EXPECT_MISUSE(call, needle)                                          \
    do {                                                                     \
        fc_debug_set_misuse_hook(record_and_jump);                           \
        if (setjmp(g_misuse_jmp) == 0) {                                     \
            (void)(call);                                                    \
            CHECK(0 && "fc_misuse() did not fire for " #call);               \
        }                                                                    \
        fc_debug_set_misuse_hook(NULL);                                      \
        CHECK(strstr(g_misuse_what, needle) != NULL);                        \
    } while (0)

static void noop_init_chunk(uint32_t chunk, void *start, size_t size,
                             const void *user_data) {
    (void)chunk;
    (void)start;
    (void)size;
    (void)user_data;
}

static void test_hooked_misuse_cases(void) {
    EXPECT_MISUSE(fc_region_create(NULL, 0, NULL, NULL, NULL),
                  "fc_region_create");
    EXPECT_MISUSE(fc_region_base(NULL), "fc_region_base");
    EXPECT_MISUSE(fc_region_destroy(NULL), "fc_region_destroy");
    EXPECT_MISUSE(fc_region_size(NULL), "fc_region_size");
    EXPECT_MISUSE(fc_region_debug_stats(NULL, NULL), "fc_region_debug_stats");
    EXPECT_MISUSE(fc_client_region_destroy(NULL), "fc_client_region_destroy");
}

/* No hook installed here -- confirms the real default behavior (print +
 * abort()) still works, not just the hook escape hatch. */
static void test_default_abort_still_works(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fc_region_destroy(NULL);
        _exit(0); /* unreachable if fc_misuse() really abort()s */
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}

/* A genuine fault outside any known region must still crash the process
 * for real (chained to the prior disposition), not be silently
 * swallowed -- see the "not one of ours" path in segv_handler().
 *
 * Skipped under ASan/UBSan: they intercept the null-pointer store as
 * their own runtime error and abort() internally instead of letting the
 * kernel raise a genuine SIGSEGV, so the child wouldn't actually die via
 * WTERMSIG(status) == SIGSEGV under those builds. */
#if defined(__SANITIZE_ADDRESS__)
static void test_segv_passthrough(void) {
}
#else
static void test_segv_passthrough(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != NULL);
    size_t sizes[] = {64};
    fc_region_t *region = fc_region_create(pool, 1, sizes, noop_init_chunk, NULL);
    CHECK(region != NULL);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        volatile int *bad = NULL;
        *bad = 1; /* genuine fault, well outside `region` */
        _exit(0); /* unreachable */
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
}
#endif


static void test_invalid_chunk_sizes(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != NULL);

    size_t has_zero[] = {10, 0, 10};
    errno = 0;
    CHECK(fc_region_create(pool, 3, has_zero, noop_init_chunk, NULL) == NULL);
    CHECK(errno == EINVAL);

    size_t overflows[] = {SIZE_MAX - 5, 10};
    errno = 0;
    CHECK(fc_region_create(pool, 2, overflows, noop_init_chunk, NULL) == NULL);
    CHECK(errno == EINVAL);

    fc_pool_destroy(pool);
}

/* fc_pool_destroy() on a pool that isn't the head of the process-wide
 * pool list needs to walk past at least one other live pool first. */
static void test_pool_destroy_not_head(void) {
    fc_pool_t *p1 = fc_pool_create();
    fc_pool_t *p2 = fc_pool_create();
    CHECK(p1 != NULL && p2 != NULL);

    fc_pool_destroy(p1); /* p2 is head; walks past it to find p1 */
    fc_pool_destroy(p2);
    fc_pool_destroy(NULL); /* no-op, must not crash */
}

int main(void) {
    test_hooked_misuse_cases();
    test_invalid_chunk_sizes();
    test_pool_destroy_not_head();
    test_default_abort_still_works();
    test_segv_passthrough();
    return 0;
}
