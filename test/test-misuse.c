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
        fc_debug_set_misuse_hook(nullptr);                                      \
        CHECK(strstr(g_misuse_what, needle) != nullptr);                        \
    } while (0)

static void noop_fill_chunk(uint32_t chunk, void *start, size_t size,
                             const void *user_data) {
    (void)chunk;
    (void)start;
    (void)size;
    (void)user_data;
}

static void test_hooked_misuse_cases(void) {
    EXPECT_MISUSE(fc_region_create(nullptr, 0, nullptr, nullptr, nullptr),
                  "fc_region_create");
    EXPECT_MISUSE(fc_region_base(nullptr), "fc_region_base");
    EXPECT_MISUSE(fc_region_destroy(nullptr), "fc_region_destroy");
    EXPECT_MISUSE(fc_region_size(nullptr), "fc_region_size");
    EXPECT_MISUSE(fc_region_debug_stats(nullptr, nullptr), "fc_region_debug_stats");
    EXPECT_MISUSE(fc_client_region_destroy(nullptr), "fc_client_region_destroy");
}

/* No hook installed here -- confirms the real default behavior (print +
 * abort()) still works, not just the hook escape hatch. */
static void test_default_abort_still_works(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fc_region_destroy(nullptr);
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
static void test_segv_chains_to_prior_siginfo_handler(void) {
}
#else
static void test_segv_passthrough(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != nullptr);
    size_t sizes[] = {64};
    fc_region_t *region = fc_region_create(pool, 1, sizes, noop_fill_chunk, nullptr);
    CHECK(region != nullptr);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        volatile int *bad = nullptr;
        *bad = 1; /* genuine fault, well outside `region` */
        _exit(0); /* unreachable */
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);

    /* Destroying the pool (not the region directly) also exercises
     * fc_pool_destroy()'s "clean up any regions still alive" path. */
    fc_pool_destroy(pool);
}

static int g_prior_handler_pipe_wr;

static void prior_segv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;
    char c = 'x';
    ssize_t ignored = write(g_prior_handler_pipe_wr, &c, 1);
    (void)ignored;
    /* Restore default disposition and return: the kernel retries the
     * faulting instruction, raising SIGSEGV for real against SIG_DFL --
     * proves this is a genuine chain, not a swallow. */
    signal(SIGSEGV, SIG_DFL);
}

/* If a SA_SIGINFO SIGSEGV handler was already installed before
 * faultcache's own (e.g. another library's crash handler), a genuine
 * fault outside any region must chain to *that* handler, not just fall
 * through to the default disposition -- see the SA_SIGINFO branch in
 * segv_handler()'s "not one of ours" tail.
 *
 * Must run before any other test's first fc_pool_create() call:
 * faultcache installs its own handler exactly once per process (via
 * pthread_once) and captures whatever was in effect at that moment as
 * the "prior" disposition to chain to. Everything below happens inside
 * a forked child so that this one-time capture happens there (with our
 * handler already installed) rather than in the shared test process. */
static void test_segv_chains_to_prior_siginfo_handler(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        g_prior_handler_pipe_wr = pipefd[1];

        struct sigaction sa = {0};
        sa.sa_sigaction = prior_segv_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        CHECK(sigaction(SIGSEGV, &sa, nullptr) == 0);

        fc_pool_t *pool = fc_pool_create(); /* first ever: captures ours */
        CHECK(pool != nullptr);

        volatile int *bad = nullptr;
        *bad = 1; /* genuine fault, well outside any region */
        _exit(1); /* unreachable */
    }
    close(pipefd[1]);

    char c = 0;
    CHECK(read(pipefd[0], &c, 1) == 1); /* prior handler really ran */
    close(pipefd[0]);

    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);
}
#endif



static fc_region_t *g_inner_region;

/* Mimics layering faultcache in two levels (an outer region's fill_chunk
 * deriving its content from an inner, still-unresolved region) -- not
 * supported, and deliberately not worth supporting: faultcache is a
 * low-level mechanism, and merging both derivation steps into one
 * fill_chunk is simpler and faster than layering two caches. Must
 * trigger segv_handler()'s g_resolving_fault check (fc_misuse() +
 * abort()), not a deadlock or an unexplained crash. */
static void fill_chunk_touches_other_region(uint32_t chunk, void *start,
                                             size_t size,
                                             const void *user_data) {
    (void)chunk;
    (void)user_data;
    memset(start, 0, size);
    const unsigned char *inner = fc_region_base(g_inner_region);
    volatile unsigned char v = inner[0]; /* g_inner_region, not yet resolved */
    (void)v;
}

static void test_nested_fault_across_regions_is_fatal(void) {
    fc_pool_t *outer_pool = fc_pool_create();
    fc_pool_t *inner_pool = fc_pool_create();
    CHECK(outer_pool != nullptr && inner_pool != nullptr);

    size_t inner_size[] = {64};
    fc_region_t *inner_region = fc_region_create(inner_pool, 1, inner_size,
                                                  noop_fill_chunk, nullptr);
    CHECK(inner_region != nullptr);
    g_inner_region = inner_region;

    size_t outer_size[] = {64};
    fc_region_t *outer_region = fc_region_create(
        outer_pool, 1, outer_size, fill_chunk_touches_other_region, nullptr);
    CHECK(outer_region != nullptr);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        const unsigned char *base = fc_region_base(outer_region);
        volatile unsigned char v = base[0]; /* resolves outer, touching inner */
        (void)v;
        _exit(0); /* unreachable: fc_misuse() aborts first */
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    fc_pool_destroy(outer_pool);
    fc_pool_destroy(inner_pool);
}

static void test_invalid_chunk_sizes(void) {
    fc_pool_t *pool = fc_pool_create();
    CHECK(pool != nullptr);

    size_t has_zero[] = {10, 0, 10};
    errno = 0;
    CHECK(fc_region_create(pool, 3, has_zero, noop_fill_chunk, nullptr) == nullptr);
    CHECK(errno == EINVAL);

    size_t overflows[] = {SIZE_MAX - 5, 10};
    errno = 0;
    CHECK(fc_region_create(pool, 2, overflows, noop_fill_chunk, nullptr) == nullptr);
    CHECK(errno == EINVAL);

    fc_pool_destroy(pool);
}

/* fc_pool_destroy() on a pool that isn't the head of the process-wide
 * pool list needs to walk past at least one other live pool first. */
static void test_pool_destroy_not_head(void) {
    fc_pool_t *p1 = fc_pool_create();
    fc_pool_t *p2 = fc_pool_create();
    CHECK(p1 != nullptr && p2 != nullptr);

    fc_pool_destroy(p1); /* p2 is head; walks past it to find p1 */
    fc_pool_destroy(p2);
    fc_pool_destroy(nullptr); /* no-op, must not crash */
}

static void test_client_pool_destroy_null(void) {
    fc_client_pool_destroy(nullptr); /* no-op, must not crash */
}

/* Descriptor too large to ever fit in a wire message: resolve_descriptor()
 * must reject it (EMSGSIZE) before touching server_fd or dereferencing
 * descriptor at all, so this needs neither a real server nor a valid fd. */
static void test_client_region_create_oversized_descriptor(void) {
    fc_client_pool_t *pool = fc_client_pool_create();
    CHECK(pool != nullptr);

    char dummy = 0;
    errno = 0;
    fc_client_region_t *region = fc_client_region_create(
        pool, -1, SIZE_MAX / 2, &dummy, nullptr);
    CHECK(region == nullptr);
    CHECK(errno == EMSGSIZE);

    fc_client_pool_destroy(pool);
}

int main(void) {
    /* Must run first: captures our handler as faultcache's "prior
     * disposition" via install_handler()'s one-time pthread_once, before
     * any other test's fc_pool_create() call does so with the default
     * disposition instead. */
    test_segv_chains_to_prior_siginfo_handler();
    test_hooked_misuse_cases();
    test_invalid_chunk_sizes();
    test_pool_destroy_not_head();
    test_client_pool_destroy_null();
    test_client_region_create_oversized_descriptor();
    test_default_abort_still_works();
    test_segv_passthrough();
    test_nested_fault_across_regions_is_fatal();
    return 0;
}
