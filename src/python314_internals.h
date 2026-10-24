/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python314_internals.h

    Qore Programming Language - Python 3.14+ support (GIL-enabled and free-threaded builds)

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

#ifndef _QORE_PYTHON314_INTERNALS_H
#define _QORE_PYTHON314_INTERNALS_H

#include <fileobject.h>

/*
    Python 3.14 and later, in the default (GIL-enabled) build and in the free-threaded build (PEP 703)

    In free-threading mode (Py_GIL_DISABLED), there is no Global Interpreter Lock.
    Thread synchronization is handled differently:
    - Reference counting is thread-safe using atomic operations
    - Container operations have internal locking
    - Critical sections API is available for explicit locking

    The GIL-related APIs (PyGILState_*, PyEval_SaveThread, etc.) still exist
    but behave differently - they're primarily used for:
    - Allowing garbage collection to run during blocking operations
    - Thread state management for threads created outside Python
*/

// _PyGILState_GetInterpreterStateUnsafe was removed in Python 3.14
// Use PyInterpreterState_Main() as a replacement when there's no thread state
inline PyInterpreterState* _PyGILState_GetInterpreterStateUnsafe() {
    return PyInterpreterState_Main();
}
#define QORE_PY_GILSTATE_UNSAFE_DEFINED 1

/*
    In Python 3.14 free-threading mode, the traditional GIL state tracking
    via _PyRuntime.gilstate is not available. Instead, we use the public APIs:
    - PyGILState_GetThisThreadState() - get thread state for current thread
    - PyThreadState_Swap() - swap thread states
    - PyGILState_Ensure()/PyGILState_Release() - for thread safety around blocking ops

    In GIL-enabled mode, the attached state (PyThreadState_GetUnchecked()) tells whether and with which state the
    thread holds the GIL.
*/

#ifdef Py_GIL_DISABLED
// In free-threading mode, there's no GIL to track
#define _QORE_SET_GIL_HELD(v) ((void)0)

// The following functions are provided for API compatibility with GIL-enabled code paths
// but may not be used in all compilation units in free-threading mode

// Read the actual attachment; auto-TSS can still name a detached state.
[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return PyThreadState_GetUnchecked();
}

// PyThreadState_Swap detaches the previous state before attaching the new one.
[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyThreadState_SafeSwap(PyThreadState* new_state) {
    return PyThreadState_Swap(new_state);
}

// Set this thread's state in thread-local storage
[[maybe_unused]]
DLLLOCAL static void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    _qore_PyThreadState_SafeSwap(state);
}
// Free-threading mode - GIL doesn't exist, these are no-ops or simplified

[[maybe_unused]]
DLLLOCAL static bool _qore_PyCeval_GetGilLockedStatus() {
    // No GIL in free-threading mode
    return false;
}

[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyCeval_GetThreadState() {
    // Return current thread state
    return PyThreadState_GetUnchecked();
}

// Provided for API compatibility but not used in free-threading mode
[[maybe_unused]]
DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    return PyThreadState_Swap(new_state);
}

// No GIL check to re-enable in free-threading mode
#define _QORE_PYTHON_REENABLE_GIL_CHECK

// In free-threading mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

/*
    In free-threading mode, we don't need to acquire/release the GIL.
    PyEval_AcquireThread/PyEval_ReleaseThread still exist but behave differently -
    they primarily manage thread state attachment, not GIL acquisition.

    PyThreadState_Swap() handles both sides of an attachment transition.
*/
DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    return PyThreadState_GetUnchecked() != nullptr;
}

// In free-threading mode, thread state management is different:
// - Each thread has exactly one attached thread state
// - We must swap to attach a thread state before using Python APIs
// - PyThreadState_Swap handles the attach/detach internally
DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    // In free-threading mode, we need to ensure a valid thread state is attached
    // before any Python API calls. Use PyThreadState_Swap to properly attach.
    PyThreadState* current = PyThreadState_GetUnchecked();
    if (current != tstate) {
        PyThreadState_Swap(tstate);
    }
}

// In free-threading mode, we restore the previous thread state
DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    // In free-threading mode, swap to nullptr to detach the thread state
    // This allows other thread states to be attached later
    (void)tstate;  // The tstate to release is for reference only
    PyThreadState_Swap(nullptr);
}

#else
// the default (GIL-enabled) build of Python 3.14 and later

// The thread state that the thread has attached (PyThreadState_GetUnchecked()) tells whether and with which state
// the thread holds the GIL: the module keeps no tracking of its own (see python313_internals.h)

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

#define _QORE_PYTHON_REENABLE_GIL_CHECK

// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)


// In GIL-enabled mode, use the standard GIL functions
DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    return PyGILState_Check();
}

DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    PyEval_AcquireThread(tstate);
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
}

#endif // Py_GIL_DISABLED

#endif // _QORE_PYTHON314_INTERNALS_H
