/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python314_internals.h

    Qore Programming Language - Python 3.14+ free-threading support

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
    Python 3.14 free-threading (PEP 703) support

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

// Recursion limit APIs - Python 3.14 uses global limits, not per-thread
inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    (void)state;  // unused in Python 3.14+
    return Py_GetRecursionLimit();
}

// _PyGILState_GetInterpreterStateUnsafe was removed in Python 3.14
// Use PyInterpreterState_Main() as a replacement when there's no thread state
inline PyInterpreterState* _PyGILState_GetInterpreterStateUnsafe() {
    return PyInterpreterState_Main();
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    (void)state;  // unused in Python 3.14+
    Py_SetRecursionLimit(new_limit);
}

/*
    In Python 3.14 free-threading mode, the traditional GIL state tracking
    via _PyRuntime.gilstate is not available. Instead, we use the public APIs:
    - PyGILState_GetThisThreadState() - get thread state for current thread
    - PyThreadState_Swap() - swap thread states
    - PyGILState_Ensure()/PyGILState_Release() - for thread safety around blocking ops

    In GIL-enabled mode, we use a thread-local variable to track thread state,
    similar to Python 3.12 internals.
*/

#ifdef Py_GIL_DISABLED
// The following functions are provided for API compatibility with GIL-enabled code paths
// but may not be used in all compilation units in free-threading mode

// Get the current thread state - uses public API
// In free-threading mode, use PyGILState_GetThisThreadState which is safer
[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    // PyGILState_GetThisThreadState returns NULL if no thread state is attached
    // This is safe to call even during shutdown
    return PyGILState_GetThisThreadState();
}

// Safe wrapper for PyThreadState_Swap in free-threading mode
// In free-threading, we cannot just swap thread states - we need to be careful
// about attachment state to avoid "_PyThreadState_Attach: non-NULL old thread state"
[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyThreadState_SafeSwap(PyThreadState* new_state) {
    // In free-threading mode, thread state management is fundamentally different.
    // Each thread has exactly one attached thread state, and we cannot easily swap.
    // The safest approach is to do nothing and return the current state.
    PyThreadState* current = PyGILState_GetThisThreadState();

    if (new_state == nullptr || current == new_state) {
        // Swapping to nullptr or to same state - just return current
        return current;
    }

    // In free-threading mode, if we have a thread state already, we keep it
    // The thread state will be used for all Python operations on this thread
    return current;
}

// Set this thread's state in thread-local storage
[[maybe_unused]]
DLLLOCAL static void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    // In free-threading mode, thread state management is different
    // We use the safe swap function that avoids attachment errors
    if (state) {
        _qore_PyThreadState_SafeSwap(state);
    }
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
    return PyThreadState_Get();
}

// Provided for API compatibility but not used in free-threading mode
[[maybe_unused]]
DLLLOCAL static PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    return PyThreadState_Swap(new_state);
}

// No GIL check to re-enable in free-threading mode
#define _QORE_PYTHON_REENABLE_GIL_CHECK

// In free-threading mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

/*
    In free-threading mode, gilstate_counter doesn't exist or isn't meaningful.
    These macros provide no-op wrappers for code that manipulates it.
*/
#define _QORE_GILSTATE_COUNTER_INC(tstate) ((void)0)
#define _QORE_GILSTATE_COUNTER_DEC(tstate) ((void)0)
#define _QORE_GILSTATE_COUNTER_GET(tstate) 1
#define _QORE_GILSTATE_COUNTER_ASSERT_ONE(tstate) ((void)0)

/*
    In free-threading mode, we don't need to acquire/release the GIL.
    PyEval_AcquireThread/PyEval_ReleaseThread still exist but behave differently -
    they primarily manage thread state attachment, not GIL acquisition.

    IMPORTANT: In free-threading mode, PyThreadState_Swap() internally calls
    _PyThreadState_Attach() which fails with "non-NULL old thread state" error
    if a thread state is already attached. We must check first and only swap
    if no thread state is currently attached.
*/
DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    // Use PyGILState_GetThisThreadState to check without causing errors
    return PyGILState_GetThisThreadState() != nullptr;
}

// In free-threading mode, thread state management is different:
// - Each thread has exactly one attached thread state
// - We must swap to attach a thread state before using Python APIs
// - PyThreadState_Swap handles the attach/detach internally
DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    // In free-threading mode, we need to ensure a valid thread state is attached
    // before any Python API calls. Use PyThreadState_Swap to properly attach.
    PyThreadState* current = PyGILState_GetThisThreadState();
    if (current == nullptr || current != tstate) {
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
// GIL-enabled build of Python 3.14+ - should not normally happen but handle it

#include <dynamic_annotations.h>

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

// Simplified memory order definitions
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

#define _Py_atomic_load_relaxed(ATOMIC_VAL) \
    __atomic_load_n(&(ATOMIC_VAL)->_value, __ATOMIC_RELAXED)

#define _Py_atomic_store_relaxed(ATOMIC_VAL, NEW_VAL) \
    __atomic_store_n(&(ATOMIC_VAL)->_value, (NEW_VAL), __ATOMIC_RELAXED)

// For GIL-enabled Python 3.14, use thread-local state to track current thread state
// This mirrors the approach used in Python 3.12 internals
// Using inline thread_local ensures a single instance shared across all compilation units (C++17)
inline thread_local PyThreadState* _qore_tss_tstate = nullptr;

// Get the current thread state from our thread-local tracking
DLLLOCAL static PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return _qore_tss_tstate;
}

// Set this thread's state in thread-local storage
DLLLOCAL static void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    _qore_tss_tstate = state;
}

DLLLOCAL static bool _qore_PyCeval_GetGilLockedStatus() {
    // Assume GIL is held if we have a valid thread state
    return PyGILState_Check();
}

DLLLOCAL static PyThreadState* _qore_PyCeval_GetThreadState() {
    return PyThreadState_Get();
}

DLLLOCAL static PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    PyThreadState* old = _qore_tss_tstate;
    _qore_tss_tstate = new_state;
    return old;
}

#define _QORE_PYTHON_REENABLE_GIL_CHECK

// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

// In GIL-enabled mode, these work with gilstate_counter (if accessible)
#define _QORE_GILSTATE_COUNTER_INC(tstate) (++(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_DEC(tstate) (--(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_GET(tstate) ((tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_ASSERT_ONE(tstate) assert((tstate)->gilstate_counter == 1)

// In GIL-enabled mode, use the standard GIL functions
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

#endif // Py_GIL_DISABLED

#endif // _QORE_PYTHON314_INTERNALS_H
