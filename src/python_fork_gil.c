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
   module (python*_internals.h) do not have it.  CMake compiles it only if it compiles with the headers of the Python
   library (QORE_PYTHON_HAVE_FORK_GIL_CHECK), so that a Python version that changes them is built without it.
*/

#define Py_BUILD_CORE 1
#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_interp.h"
#endif
#if PY_VERSION_HEX < 0x030D0000
// _PyRuntime (the GIL of Python 3.11, and the list of interpreters of Python 3.11 and 3.12)
#include "internal/pycore_runtime.h"
#endif

#include <pthread.h>

#include "python_fork_gil.h"

#ifndef Py_GIL_DISABLED
/* returns the GIL that the interpreters of the module share: the main interpreter's, or NULL if there is none */
static struct _gil_runtime_state* qore_python_get_gil(void) {
#if PY_VERSION_HEX >= 0x030C0000
    PyInterpreterState* interp = PyInterpreterState_Main();
    return interp ? interp->ceval.gil : NULL;
#else
    return &_PyRuntime.ceval.gil;
#endif
}
#endif

int qore_python_fork_child_gil_lost(void) {
#ifdef Py_GIL_DISABLED
    // there is no GIL
    return 0;
#else
    struct _gil_runtime_state* gil = qore_python_get_gil();
    if (!gil) {
        return 0;
    }
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

#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030D0000
void qore_python_request_gil_switch(const struct qore_python_gil_waiter* waiters, size_t count) {
    struct _gil_runtime_state* gil = qore_python_get_gil();
    if (!gil || _Py_atomic_load_relaxed(&gil->locked) <= 0) {
        return;
    }
    PyThreadState* holder = (PyThreadState*)_Py_atomic_load_relaxed(&gil->last_holder);
    if (!holder) {
        return;
    }
    /* the interpreters share the GIL, but a thread that waits for it asks only its own interpreter to release it
       (gil_drop_request and eval_breaker are per interpreter with Python 3.11 and 3.12): a thread of another
       interpreter that runs Python code without blocking would never release it.  The request is made in the
       interpreter of the thread that holds the GIL, and only if that thread is not one of the waiting threads: a
       thread that releases the GIL for a request waits until another thread takes it.  The thread states cannot be
       freed while the list of interpreters is locked, so the holder is only used if it is found in it */
    PyThread_acquire_lock(_PyRuntime.interpreters.mutex, WAIT_LOCK);
    PyInterpreterState* holder_interp = NULL;
    unsigned long holder_id = 0;
    for (PyInterpreterState* interp = _PyRuntime.interpreters.head; interp && !holder_interp; interp = interp->next) {
        for (PyThreadState* ts = interp->threads.head; ts; ts = ts->next) {
            if (ts == holder) {
                holder_interp = interp;
                holder_id = ts->thread_id;
                break;
            }
        }
    }
    if (holder_interp) {
        /* the thread that holds a thread state is not known: a waiting thread holds the GIL with the state it holds
           when its section starts, or with the state with which it takes the GIL; if that state is not known (ex:
           the state of an interpreter that the thread creates), with a state that the thread created */
        int waiting = 0;
        for (size_t i = 0; i < count; ++i) {
            if (waiters[i].current == holder
                || (waiters[i].target ? waiters[i].target == holder : waiters[i].thread_id == holder_id)) {
                waiting = 1;
                break;
            }
        }
        /* the request is made under the GIL's mutex, like CPython does, so that the holder cannot release the GIL
           in the meantime: a request left in an interpreter after the GIL has changed hands would make the next
           thread of that interpreter that runs Python code release the GIL and wait for a thread that might never
           take it */
        if (!waiting) {
            pthread_mutex_lock(&gil->mutex);
            if (_Py_atomic_load_relaxed(&gil->locked) > 0
                && (PyThreadState*)_Py_atomic_load_relaxed(&gil->last_holder) == holder) {
                _Py_atomic_store_relaxed(&holder_interp->ceval.gil_drop_request, 1);
                _Py_atomic_store_relaxed(&holder_interp->ceval.eval_breaker, 1);
            }
            pthread_mutex_unlock(&gil->mutex);
        }
    }
    PyThread_release_lock(_PyRuntime.interpreters.mutex);
}
#endif

#ifdef DEBUG
#include <errno.h>
#include <unistd.h>

/* the thread holding the mutex of the GIL, and the pipes that tell it to release the mutex and that it holds it */
static pthread_t test_gil_mutex_thread;
static int test_gil_mutex_release[2] = {-1, -1};
static int test_gil_mutex_held[2] = {-1, -1};

static void test_close_pipes(void) {
    for (int i = 0; i < 2; ++i) {
        if (test_gil_mutex_release[i] != -1) {
            close(test_gil_mutex_release[i]);
            test_gil_mutex_release[i] = -1;
        }
        if (test_gil_mutex_held[i] != -1) {
            close(test_gil_mutex_held[i]);
            test_gil_mutex_held[i] = -1;
        }
    }
}

static void* test_hold_gil_mutex(void* arg) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)arg;
    char c = 1;
    pthread_mutex_lock(mutex);
    while (write(test_gil_mutex_held[1], &c, 1) == -1 && errno == EINTR) {
    }
    while (read(test_gil_mutex_release[0], &c, 1) == -1 && errno == EINTR) {
    }
    pthread_mutex_unlock(mutex);
    return NULL;
}

int qore_python_test_hold_gil_mutex(void) {
#ifdef Py_GIL_DISABLED
    errno = ENOTSUP;
    return -1;
#else
    struct _gil_runtime_state* gil = qore_python_get_gil();
    if (!gil || test_gil_mutex_release[0] != -1) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(test_gil_mutex_release) || pipe(test_gil_mutex_held)) {
        int err = errno;
        test_close_pipes();
        errno = err;
        return -1;
    }
    int rc = pthread_create(&test_gil_mutex_thread, NULL, test_hold_gil_mutex, &gil->mutex);
    if (rc) {
        test_close_pipes();
        errno = rc;
        return -1;
    }
    char c;
    ssize_t n;
    while ((n = read(test_gil_mutex_held[0], &c, 1)) == -1 && errno == EINTR) {
    }
    return n == 1 ? 0 : -1;
#endif
}

int qore_python_test_release_gil_mutex(void) {
    if (test_gil_mutex_release[1] == -1) {
        errno = EINVAL;
        return -1;
    }
    char c = 1;
    while (write(test_gil_mutex_release[1], &c, 1) == -1 && errno == EINTR) {
    }
    int rc = pthread_join(test_gil_mutex_thread, NULL);
    test_close_pipes();
    if (rc) {
        errno = rc;
        return -1;
    }
    return 0;
}
#endif
