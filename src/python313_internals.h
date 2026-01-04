/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python313_internals.h

    Qore Programming Language

    Copyright 2020 - 2026 Qore Technologies, s.r.o.

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

#ifndef _QORE_PYTHON313_INTERNALS_H
#define _QORE_PYTHON313_INTERNALS_H

#include <fileobject.h>

// Python 3.13 uses global recursion limits instead of per-thread
inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    (void)state;  // unused in Python 3.13+
    return Py_GetRecursionLimit();
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    (void)state;  // unused in Python 3.13+
    Py_SetRecursionLimit(new_limit);
}

// For Python 3.13, use our own thread-local state to track current thread state
// This mirrors the approach used in Python 3.14 GIL-enabled internals
// Using inline thread_local ensures a single instance shared across all compilation units (C++17)
inline thread_local PyThreadState* _qore_tss_tstate = nullptr;

// Get the current thread state from our thread-local tracking
DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return _qore_tss_tstate;
}

// Set this thread's state in thread-local storage
DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    _qore_tss_tstate = state;
}

// GIL status check - use our own tracking since PyGILState_Check() is unreliable
// in Python 3.13 after GIL transitions with sub-interpreters
DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    // Use our own tracking - if we have a thread state set, we consider ourselves
    // to be holding the GIL for the purposes of our API
    return _qore_tss_tstate != nullptr;
}

// Get the thread state that holds the GIL
// In Python 3.13, PyGILState_Check() and TSS can be inconsistent during GIL transitions
// with sub-interpreters. Use our own tracking for reliability.
DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    return _qore_tss_tstate;
}

// Swap thread state for ceval purposes - use our thread-local tracking
DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    PyThreadState* old = _qore_tss_tstate;
    _qore_tss_tstate = new_state;
    return old;
}

#define _QORE_PYTHON_REENABLE_GIL_CHECK /* no-op in Python 3.13 - check is always enabled */

// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

// gilstate_counter access macros - these are available in GIL-enabled Python
#define _QORE_GILSTATE_COUNTER_INC(tstate) (++(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_DEC(tstate) (--(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_GET(tstate) ((tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_ASSERT_ONE(tstate) assert((tstate)->gilstate_counter == 1)

// _PyGILState_GetInterpreterStateUnsafe was an internal function
// Use PyInterpreterState_Main() as a safe replacement
inline PyInterpreterState* _PyGILState_GetInterpreterStateUnsafe() {
    return PyInterpreterState_Main();
}

// Thread state management functions for GIL-enabled Python
DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    return PyGILState_Check();
}

DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    _qore_tss_tstate = tstate;
    PyEval_AcquireThread(tstate);
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
    _qore_tss_tstate = nullptr;
}

#endif
