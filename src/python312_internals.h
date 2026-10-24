/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python312_internals.h

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

#ifndef _QORE_PYTHON_INTERNALS_H
#define _QORE_PYTHON_INTERNALS_H

#include <fileobject.h>

inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    return state->py_recursion_limit;
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    state->py_recursion_limit = new_limit;
}

// Python 3.12 GIL Tracking
// ========================
// In Python 3.12, PyEval_ReleaseThread() does NOT clear the Thread-Specific Storage (TSS)
// that stores the current thread state. This means:
// - PyGILState_Check() returns 1 even after releasing the GIL
// - PyGILState_GetThisThreadState() returns a non-null value after releasing the GIL
// - These functions are unreliable for determining if the current thread holds the GIL
//
// Solution: Use our own thread-local tracking variable (_qore_tss_tstate) that we
// explicitly clear when releasing the GIL and set when acquiring it. All GIL status
// checks must use our tracking, not Python's TSS-based functions.
//
// Python-Created Threads:
// Python-created threads (e.g., threading.Thread) that call back into Qore will not
// have our tracking initialized. To handle this, we track whether our GIL tracking
// has ever been initialized for this thread (_qore_tss_initialized). If a thread
// has never been initialized but Python's TSS shows it has a thread state and the
// GIL, we know it's a Python-created thread that already has the GIL.
//
// Additional Complexity - PyThreadState_New() Behavior:
// In Python 3.12, PyThreadState_New() automatically binds the new thread state to
// the current thread via TSS (see Python/pystate.c bind_tstate/bind_gilstate_tstate).
// This means that after calling PyThreadState_New() from a new Qore thread:
// - PyGILState_GetThisThreadState() will return the new thread state
// - PyGILState_Check() will return 1 (true)
// Even though we haven't acquired the GIL yet! This could cause
// _qore_check_python_created_thread_gil() to incorrectly detect a new Qore thread
// as a Python-created thread. To fix this, setContext() marks _qore_tss_initialized=true
// immediately after PyThreadState_New() to prevent this false detection.

// Thread-local state to track current thread state for GIL ownership
// Using inline thread_local ensures a single instance shared across all compilation units (C++17)
inline thread_local PyThreadState* _qore_tss_tstate = nullptr;

// Track whether we've ever initialized our GIL tracking for this thread.
// This distinguishes between:
// - Qore threads that released the GIL: _qore_tss_initialized=true, _qore_tss_tstate=nullptr
// - Python-created threads with GIL: _qore_tss_initialized=false, _qore_tss_tstate=nullptr
inline thread_local bool _qore_tss_initialized = false;

// Track whether the current thread holds the GIL (independent of tstate tracking)
// This is needed because QorePythonReleaseGilHelper uses this to track GIL state
inline thread_local bool _qore_gil_held = false;

// Get the current thread state from our thread-local tracking
DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return _qore_tss_tstate;
}

// Set this thread's state in thread-local storage
// NOTE: Only set _qore_tss_initialized when actually acquiring the GIL (state != nullptr).
// When releasing the GIL (state = nullptr), we keep initialized=true to indicate
// this is a Qore-managed thread that released the GIL (vs a Python-created thread).
// NOTE: This function does NOT update _qore_gil_held - that's only done by
// _qore_acquire_thread_state and _qore_release_thread_state which actually change GIL ownership.
DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    _qore_tss_tstate = state;
    if (state != nullptr) {
        _qore_tss_initialized = true;
    }
}

// GIL status check - use our own tracking since PyGILState_Check() in Python 3.12
// returns true even after PyEval_ReleaseThread() because the TSS isn't cleared.
DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    // We have the GIL if our thread-local tracking has a thread state
    return _qore_tss_tstate != nullptr;
}

// Check if this might be a Python-created thread that already has the GIL.
// This handles the case where a Python threading.Thread calls back into Qore.
// Returns the thread state if this is a Python-created thread with the GIL, nullptr otherwise.
//
// IMPORTANT - Python 3.12 TSS Reliability Issues:
// In Python 3.12, PyGILState_Check() and PyGILState_GetThisThreadState() are unreliable:
// 1. After PyEval_ReleaseThread(), they still return values suggesting GIL ownership
// 2. After PyThreadState_New() in a new thread, they return the new thread state
//    even though the GIL hasn't been acquired yet
//
// To correctly detect Python-created threads, we:
// 1. Check if _qore_tss_initialized is true - if so, this is a Qore-managed thread
//    (either currently holding GIL or previously held it), not a Python-created thread
// 2. If not initialized, check if the TSS thread state is in any interpreter's thread list
//    - For Python-created threads: it will be a valid thread state in the list
//    - For new Qore threads after PyThreadState_New(): the newly created state is also
//      in the list, BUT setContext() sets _qore_tss_initialized=true BEFORE we get here,
//      so we'll return nullptr (handled by case 1)
// 3. If the TSS state is in the list and we're not initialized, it's a Python-created thread
DLLLOCAL static inline PyThreadState* _qore_check_python_created_thread_gil() {
    // If our tracking is already initialized, this is a Qore-managed thread
    if (_qore_tss_initialized) {
        printd(5, "_qore_check_python_created_thread_gil() already initialized, returning nullptr\n");
        return nullptr;
    }

    // Check if Python thinks this thread has the GIL
    PyThreadState* tss_state = PyGILState_GetThisThreadState();
    int gil_check = PyGILState_Check();
    printd(5, "_qore_check_python_created_thread_gil() tss_state: %p gil_check: %d initialized: %d\n",
        tss_state, gil_check, (int)_qore_tss_initialized);

    if (tss_state != nullptr && gil_check) {
        // Both PyGILState_GetThisThreadState() and PyGILState_Check() indicate we have the GIL.
        // This could be:
        // A) A Python-created thread (e.g., threading.Thread) that truly has the GIL
        // B) A brand new Qore thread with stale/garbage TSS values
        //
        // To distinguish: verify that tss_state is in the interpreter's thread state list.
        // This is safe because PyInterpreterState_Head() and PyInterpreterState_ThreadHead()
        // are public APIs that don't require accessing opaque internal structures.
        bool found = false;
        PyInterpreterState* interp = PyInterpreterState_Head();
        while (interp != nullptr && !found) {
            PyThreadState* ts = PyInterpreterState_ThreadHead(interp);
            while (ts != nullptr) {
                if (ts == tss_state) {
                    found = true;
                    break;
                }
                ts = PyThreadState_Next(ts);
            }
            interp = PyInterpreterState_Next(interp);
        }

        if (!found) {
            // The tss_state is not in any interpreter's thread list - it's stale/garbage
            printd(5, "_qore_check_python_created_thread_gil() tss_state %p not found in thread lists, "
                "returning nullptr\n", tss_state);
            return nullptr;
        }

        // tss_state is a valid thread state - this is a Python-created thread with the GIL
        _qore_tss_tstate = tss_state;
        _qore_tss_initialized = true;
        // NOTE: We set _qore_gil_held here because this is a Python-created thread that
        // already holds the GIL - we're recognizing existing GIL ownership, not acquiring it
        _qore_gil_held = true;
        printd(5, "_qore_check_python_created_thread_gil() detected Python-created thread, tss_state: %p\n",
            tss_state);
        return tss_state;
    }

    printd(5, "_qore_check_python_created_thread_gil() not a Python-created thread with GIL\n");
    return nullptr;
}

// Get the thread state that holds the GIL - use our tracking for consistency with _qore_PyCeval_GetGilLockedStatus()
// NOTE: In Python 3.12, PyGILState_GetThisThreadState() may return non-null even after PyEval_ReleaseThread(),
// so we must use our own tracking to stay consistent.
DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    return _qore_tss_tstate;
}

// Swap thread state for ceval purposes - use our thread-local tracking
DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    PyThreadState* old = _qore_tss_tstate;
    _qore_tss_tstate = new_state;
    return old;
}

#define _QORE_PYTHON_REENABLE_GIL_CHECK /* no-op in bundled internals - check is always enabled */

// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

// gilstate_counter access macros - these are available in GIL-enabled Python
#define _QORE_GILSTATE_COUNTER_INC(tstate) (++(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_DEC(tstate) (--(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_GET(tstate) ((tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_ASSERT_ONE(tstate) assert((tstate)->gilstate_counter == 1)

// Thread state management functions for GIL-enabled Python
DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    return PyGILState_Check();
}

DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    PyEval_AcquireThread(tstate);
    // Set thread-local tracking AFTER acquiring the GIL to ensure consistency.
    // PyEval_AcquireThread() either succeeds or aborts (fatal error), so if we
    // reach this point, we definitely hold the GIL.
    _qore_tss_tstate = tstate;
    _qore_tss_initialized = true;
    _qore_gil_held = true;
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    // Get Python's actual current thread state to avoid "wrong thread state" error
    // Our tracking (_qore_tss_tstate) may be out of sync with Python's internal state
    // in multi-threaded scenarios
    PyThreadState* current = PyThreadState_Get();
    // PyThreadState_Get() should never return nullptr if we hold the GIL (which we must
    // if we're releasing it). If it does return nullptr, fall back to the provided tstate
    // to avoid undefined behavior in PyEval_ReleaseThread().
    if (!current) {
        assert(tstate && "Both current and tstate are nullptr in _qore_release_thread_state");
        printd(5, "_qore_release_thread_state() WARNING: PyThreadState_Get() returned nullptr, using tstate=%p\n",
            tstate);
        current = tstate;
    } else if (current != tstate) {
        printd(5, "_qore_release_thread_state() WARNING: tstate mismatch, tstate=%p current=%p, using current\n",
            tstate, current);
    }
    PyEval_ReleaseThread(current);
    // Clear our thread-local tracking since we released the GIL
    _qore_tss_tstate = nullptr;
    _qore_gil_held = false;
}

#endif
