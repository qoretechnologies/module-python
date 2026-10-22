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

/* Clears the thread state bound to the calling thread for the GIL state API (PyGILState_GetThisThreadState()), as
   CPython does when it unbinds a state; it does not change the state itself (its bound_gilstate flag).
*/
void qore_python_gilstate_tss_clear(void);

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
