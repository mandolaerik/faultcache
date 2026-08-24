/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * POSIX side of the fault seam (src/fault.h): SIGSEGV delivery, and
 * coexisting with everyone else who wants the same signal.
 */
#include "fault.h"
#include "internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

static pthread_once_t g_handler_once = PTHREAD_ONCE_INIT;
static struct sigaction g_old_action;
/* Read without the once-guard, so it must be set inside install_handler(),
 * which pthread_once() publishes to other threads for us. */
static bool g_handler_installed = false;
/* True while this thread is passing a fault down g_old_action. */
static __thread bool g_chaining = false;

static void segv_handler(int sig, siginfo_t *info, void *ucontext) {
    /* Saved across everything below: the interrupted code is somewhere
     * between a failing call and its errno check, and both the resolve
     * path (mmap/mremap) and the diagnostics down here set errno. */
    int saved_errno = errno;

    if (fault_try_resolve(info->si_addr)) {
        errno = saved_errno;
        return; /* retry the faulting instruction */
    }
    errno = saved_errno;

    /* Not one of ours -- chain to whatever was installed before us, but
     * do NOT uninstall ourselves to do it. That handler may resolve its
     * own fault and return (another lazy-mapping library, a JIT, a GC
     * write barrier), in which case the retried instruction succeeds and
     * execution continues -- and it has to continue with our regions
     * still working. Restoring is only right for a default/ignore
     * disposition, where there is nothing to call and returning is
     * precisely what lets the kernel act on it.
     *
     * Flush coverage last, after the chain attempt: from here the process
     * either dies inside the old handler, or returns and the kernel
     * retries the faulting instruction -- either way this is the last
     * chance to flush counters for the lines below (a dump placed before
     * them would miss their counters, since gcov only credits a line once
     * its block is actually entered).
     *
     * g_chaining breaks a cycle: after fc_rearm_handler(), a library that
     * had saved us and chains back can route the same fault into us a
     * second time, and following g_old_action again would bounce it
     * between the two until the stack ran out. Reaching here twice for one
     * fault means the chain loops, so end it at the default disposition
     * instead -- a crash, which is what an unhandled fault should be. */
    if (g_chaining) {
        signal(sig, SIG_DFL);
    } else {
        g_chaining = true;
        if (g_old_action.sa_flags & SA_SIGINFO)
            g_old_action.sa_sigaction(sig, info, ucontext);
        else if (g_old_action.sa_handler != SIG_DFL
                 && g_old_action.sa_handler != SIG_IGN)
            g_old_action.sa_handler(sig);
        else
            sigaction(sig, &g_old_action, nullptr);
        g_chaining = false;
    }
    fc_flush_coverage_before_death();
}

static void install_handler(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = segv_handler;
    /* SA_NODEFER: without it, SIGSEGV is auto-blocked for this thread
     * for the handler's duration, and a synchronous fault (e.g.
     * fill_chunk touching another unresolved page) raised while it's
     * blocked can't be delivered to us at all -- the kernel forces the
     * signal's default disposition instead, killing the process with a
     * raw, undiagnosed SIGSEGV. With it, such a fault re-enters this
     * handler, and fault_try_resolve() turns it into a clear
     * fc_misuse() diagnostic instead. */
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    struct sigaction prev;
    sigaction(SIGSEGV, &sa, &prev);
    /* On a re-arm we may already be the installed handler; keeping our
     * own action as the chain target would make the tail call itself. */
    if (!((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == segv_handler))
        g_old_action = prev;
    g_handler_installed = true;
}

void fault_arm(void) {
    pthread_once(&g_handler_once, install_handler);
}

void fault_rearm(void) {
    install_handler();
}

bool fault_armed(void) {
    return g_handler_installed;
}
