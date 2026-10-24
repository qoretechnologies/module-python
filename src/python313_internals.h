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

// Python 3.13+ GIL state
// ======================
// The thread state that the thread has attached (PyThreadState_GetUnchecked(), public since 3.13) tells whether and
// with which state the thread holds the GIL: the module keeps no tracking of its own, which could differ from it (ex:
// after code like JNI released or acquired the GIL without the module's API).  PyGILState_Check() and
// PyGILState_GetThisThreadState() are not used for this: they report the state bound to the thread for the GIL
// state API, which is not necessarily the attached state with sub-interpreters.

DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return PyThreadState_GetUnchecked();
}

//! no tracking to update: the attachment is changed with the CPython API
DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    (void)state;
}

DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    return PyThreadState_GetUnchecked() != nullptr;
}

//! returns the attached state of a thread that holds the GIL without a context of the module (ex: a thread created
//! by Python), or nullptr
DLLLOCAL static inline PyThreadState* _qore_check_python_created_thread_gil() {
    return PyThreadState_GetUnchecked();
}

DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    return PyThreadState_GetUnchecked();
}

//! returns the attached state; the attachment is changed with the CPython API
DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    (void)new_state;
    return PyThreadState_GetUnchecked();
}

//! no tracking of the GIL
#define _QORE_SET_GIL_HELD(v) ((void)0)

#define _QORE_PYTHON_REENABLE_GIL_CHECK /* no-op in Python 3.13 - check is always enabled */

// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)


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
    PyEval_AcquireThread(tstate);
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
}

#endif
