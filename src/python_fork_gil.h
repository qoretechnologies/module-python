/* -*- mode: c++; indent-tabs-mode: nil -*- */
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

#ifndef _QORE_PYTHON_FORK_GIL_H

#define _QORE_PYTHON_FORK_GIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if, in the child of fork(), the GIL can never be acquired, because a thread that does not exist in the
   child held it (or one of the mutexes that protect it) when the process was copied; returns 0 if the GIL is free or
   held by the calling thread.

   Must be called in the child by the thread that called fork(), before Python is used in the child; it only reads
   memory and tries the GIL's mutexes, so it can be called from a fork handler.
*/
int qore_python_fork_child_gil_lost(void);

#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030D0000
#include <stddef.h>
/* a thread that can wait for the GIL */
struct qore_python_gil_waiter {
    /* the Python thread identifier of the thread */
    unsigned long thread_id;
    /* the thread state that the thread holds when the section starts, if any */
    PyThreadState* current;
    /* the thread state with which the thread takes the GIL, if known; if not, the thread is assumed to hold any thread
       state that it created */
    PyThreadState* target;
};

/* Asks the thread that holds the GIL to release it, unless it is one of the waiting threads given, which cannot
   release it for a request: a thread that releases the GIL for a request waits until another thread takes it.  The
   holder is identified by its thread state, which can be created by another thread than the one that uses it, so it
   is compared with the thread states of the waiting threads as well as with their thread identifiers.  With Python
   3.11 and 3.12, the interpreters share the GIL, and a waiting thread asks only its own interpreter to release it, so
   that a thread of another interpreter that runs Python code would keep it. */
void qore_python_request_gil_switch(const struct qore_python_gil_waiter* waiters, size_t count);

/* Returns 1 if the interpreter of the given thread state (or the interpreters of the module, for NULL) uses the GIL
   that the interpreters share, 0 if it has its own GIL (an isolated sub-interpreter, PEP 684, Python 3.12) */
int qore_python_uses_shared_gil(PyThreadState* tstate);
#endif

#ifdef DEBUG
/* Test hooks (debug builds only): a thread of the module holds the mutex of the GIL, as a thread waiting for the GIL
   does from time to time, until qore_python_test_release_gil_mutex() is called.  Returns 0 once the mutex is held,
   or -1 for an error, with errno set.  Called with the GIL held; the thread holding the GIL must neither release it
   nor take it again while the mutex is held, since both need the mutex.
*/
int qore_python_test_hold_gil_mutex(void);

/* Releases the mutex held since qore_python_test_hold_gil_mutex(), and waits for the thread to end; returns 0, or
   -1 for an error, with errno set.
*/
int qore_python_test_release_gil_mutex(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
