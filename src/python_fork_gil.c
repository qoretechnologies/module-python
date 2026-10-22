/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2026 Qore Technologies, s.r.o.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* Reads the state of the GIL in the child of fork().

   This file is C and is compiled with CPython's internal headers, which are C headers: they give the layout of the
   GIL of the Python library the module is built with, for every version, where the bundled C++ definitions of the
   module (python*_internals.h) do not have it.
*/

#define Py_BUILD_CORE 1
#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_interp.h"
#else
#include "internal/pycore_runtime.h"
#endif

#include <pthread.h>

#include "python_fork_gil.h"

int qore_python_fork_child_gil_lost(void) {
#ifdef Py_GIL_DISABLED
    // there is no GIL
    return 0;
#else
#if PY_VERSION_HEX >= 0x030C0000
    // the GIL that the interpreters of the module share: the main interpreter's
    PyInterpreterState* interp = PyInterpreterState_Main();
    struct _gil_runtime_state* gil = interp ? interp->ceval.gil : NULL;
    if (!gil) {
        return 0;
    }
#else
    struct _gil_runtime_state* gil = &_PyRuntime.ceval.gil;
#endif
    // the mutexes protect the state of the GIL while a thread takes or drops it; a mutex held at fork() by another
    // thread is never released in the child
    if (pthread_mutex_trylock(&gil->mutex)) {
        return 1;
    }
    pthread_mutex_unlock(&gil->mutex);
#ifdef FORCE_SWITCHING
    if (pthread_mutex_trylock(&gil->switch_mutex)) {
        return 1;
    }
    pthread_mutex_unlock(&gil->switch_mutex);
#endif

    // only the thread that called fork() exists in the child, so the state cannot change while it is read
#if PY_VERSION_HEX >= 0x030D0000
    int locked = gil->locked;
    PyThreadState* holder = gil->last_holder;
#else
    int locked = _Py_atomic_load_relaxed(&gil->locked);
    PyThreadState* holder = (PyThreadState*)_Py_atomic_load_relaxed(&gil->last_holder);
#endif
    // -1: not created yet, 0: free
    if (locked <= 0) {
        return 0;
    }
    // while the GIL is held and its mutex is free, the last holder is the thread that holds it
    return holder && holder->thread_id == PyThread_get_thread_ident() ? 0 : 1;
#endif
}
