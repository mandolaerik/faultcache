/* © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0 */

/* Minimal C shim wrapping faultcache's fill_chunk callback.
 *
 * It exists for one reason: fill_chunk runs inside a SIGSEGV handler, and a
 * cyclic GC pass triggered there can finalize an unrelated object that
 * touches unresolved region memory, faulting again -- fatal by design (see
 * src/faultcache-sigsegv.c). A ctypes callback cannot prevent that, because
 * its trampoline allocates the argument tuple and the buffer wrapper before
 * any Python statement of ours could run. Here PyGC_Disable() is the first
 * thing that happens after acquiring the GIL.
 *
 * Deliberately limited to the callback: everything the shim needs is in the
 * stable ABI at 3.10, so it ships as a single abi3 binary, whereas a full C
 * Region with the buffer protocol would not be.
 */

/* Py_LIMITED_API is set by the build (meson's limited_api: kwarg). */
#include <Python.h>

#include <stdint.h>

#ifndef PyBUF_WRITE
/* Header oversight: PyBUF_READ/PyBUF_WRITE only became visible under
 * Py_LIMITED_API >= 3.11, even though PyMemoryView_FromMemory -- their only
 * consumer -- has been in the stable ABI since 3.3, which freezes the value. */
#  define PyBUF_WRITE 0x200
#endif

#define CAPSULE_NAME "faultcache._filler"

struct filler {
    PyObject *callable;
};

/* Both GIL-guarded. A fill may release the GIL (it runs arbitrary user
 * code), letting another thread enter its own fill, so the process-global GC
 * flag has to be depth-counted rather than saved and restored per call. */
static long g_fill_depth;
static int g_gc_was_enabled;

static void fill_trampoline(uint32_t chunk, void *start, size_t size,
                            const void *user_data)
{
    const struct filler *f = user_data;

    /* Nothing can precede this; PyGC_Disable() needs the GIL. Ensure itself
     * only allocates when a thread has no thread state yet. */
    PyGILState_STATE gil = PyGILState_Ensure();

    if (g_fill_depth++ == 0)
        g_gc_was_enabled = PyGC_IsEnabled();
    PyGC_Disable();

    PyObject *view = PyMemoryView_FromMemory((char *)start, (Py_ssize_t)size,
                                             PyBUF_WRITE);
    if (view) {
        PyObject *index = PyLong_FromUnsignedLong(chunk);
        if (index) {
            PyObject *result = PyObject_CallFunctionObjArgs(f->callable, index,
                                                            view, NULL);
            Py_XDECREF(result);
            Py_DECREF(index);
        }
        /* Exceptions can't cross the C callback boundary, and the fault is
         * resolved regardless with whatever fill_chunk managed to write. */
        if (PyErr_Occurred())
            PyErr_WriteUnraisable(f->callable);

        /* start points into a scratch mapping the caller unmaps as soon as we
         * return, so a view the callback stashed away must stop working. */
        PyObject *released = PyObject_CallMethod(view, "release", NULL);
        Py_XDECREF(released);
        Py_DECREF(view);
    }
    if (PyErr_Occurred())
        PyErr_WriteUnraisable(f->callable);

    if (--g_fill_depth == 0 && g_gc_was_enabled)
        PyGC_Enable();

    PyGILState_Release(gil);
}

static void filler_destructor(PyObject *capsule)
{
    struct filler *f = PyCapsule_GetPointer(capsule, CAPSULE_NAME);

    if (!f)
        return;
    Py_DECREF(f->callable);
    PyMem_Free(f);
}

static PyObject *make_filler(PyObject *module, PyObject *callable)
{
    (void)module;

    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "fill_chunk must be callable");
        return NULL;
    }

    struct filler *f = PyMem_Malloc(sizeof(*f));
    if (!f)
        return PyErr_NoMemory();
    Py_INCREF(callable);
    f->callable = callable;

    PyObject *capsule = PyCapsule_New(f, CAPSULE_NAME, filler_destructor);
    if (!capsule) {
        Py_DECREF(callable);
        PyMem_Free(f);
        return NULL;
    }

    return Py_BuildValue("(NKK)", capsule,
                         (unsigned long long)(uintptr_t)fill_trampoline,
                         (unsigned long long)(uintptr_t)f);
}

PyDoc_STRVAR(make_filler_doc,
"make_filler(callable) -> (capsule, fill_chunk_addr, user_data_addr)\n"
"\n"
"Wrap a Python fill_chunk callable as an fc_fill_chunk_fn_t. The two\n"
"addresses are passed straight to fc_region_create(); the capsule owns the\n"
"callable and must outlive the region.");

static PyMethodDef shim_methods[] = {
    {"make_filler", make_filler, METH_O, make_filler_doc},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef shim_module = {
    PyModuleDef_HEAD_INIT,
    "_faultcache",
    "Internal callback shim for the faultcache bindings.",
    -1,
    shim_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC PyInit__faultcache(void)
{
    return PyModule_Create(&shim_module);
}
