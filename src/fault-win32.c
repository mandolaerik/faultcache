/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Windows side of the fault seam (src/fault.h).
 *
 * Much smaller than the POSIX side, because a vectored exception
 * handler is a chain by construction: returning
 * EXCEPTION_CONTINUE_SEARCH passes the fault to the next handler and
 * ultimately to the OS. There is no displaced handler to save, no
 * disposition to restore, and no cycle to break -- the whole of
 * g_old_action, g_chaining and SA_NODEFER on the POSIX side exists to
 * rebuild what Windows already provides.
 */
#include "fault.h"
#include "internal.h"

#include <pthread.h>
#include <windows.h>

static pthread_once_t g_handler_once = PTHREAD_ONCE_INIT;
static bool g_handler_installed = false;
static PVOID g_handle = nullptr;

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    /* [0] is the access type (read/write/execute), [1] the address. A
     * write to a resolved chunk arrives here too, and is not ours to
     * resolve -- fault_try_resolve() reports that by returning false,
     * same as an address outside every region. */
    void *addr = (void *)er->ExceptionInformation[1];
    return fault_try_resolve(addr) ? EXCEPTION_CONTINUE_EXECUTION
                                   : EXCEPTION_CONTINUE_SEARCH;
}

static void install_handler(void) {
    /* First: we must see the fault before a debugger's or a crash
     * reporter's handler decides it is fatal. */
    g_handle = AddVectoredExceptionHandler(1, veh_handler);
    FC_ASSERT(g_handle != nullptr);
    g_handler_installed = true;
}

void fault_arm(void) {
    pthread_once(&g_handler_once, install_handler);
}

void fault_rearm(void) {
    /* Adding again would leave two registrations of the same handler,
     * so drop the old one first; the new one goes back to the front. */
    FC_ASSERT(RemoveVectoredExceptionHandler(g_handle) != 0);
    install_handler();
}

bool fault_armed(void) {
    return g_handler_installed;
}
