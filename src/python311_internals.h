/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python38_internals.h

    Qore Programming Language

    Copyright 2020 - 2022 Qore Technologies, s.r.o.

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

#include <dynamic_annotations.h>
#include <fileobject.h>

inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    return state->recursion_limit;
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    state->recursion_limit = new_limit;
}

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

struct _gilstate_runtime_state {
    /* bpo-26558: Flag to disable PyGILState_Check().
       If set to non-zero, PyGILState_Check() always return 1. */
    int check_enabled;
    /* Assuming the current thread holds the GIL, this is the
       PyThreadState for the current thread. */
    _Py_atomic_address tstate_current;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

#define FORCE_SWITCHING

#define PyMUTEX_T pthread_mutex_t
#define PyCOND_T pthread_cond_t

struct _gil_runtime_state {
    /* microseconds (the Python API uses seconds, though) */
    unsigned long interval;
    /* Last PyThreadState holding / having held the GIL. This helps us
       know whether anyone else was scheduled after we dropped the GIL. */
    _Py_atomic_address last_holder;
    /* Whether the GIL is already taken (-1 if uninitialized). This is
       atomic because it can be read without any lock taken in ceval.c. */
    _Py_atomic_int locked;
    /* Number of GIL switches since the beginning. */
    unsigned long switch_number;
    /* This condition variable allows one or several threads to wait
       until the GIL is released. In addition, the mutex also protects
       the above variables. */
    PyCOND_T cond;
    PyMUTEX_T mutex;
#ifdef FORCE_SWITCHING
    /* This condition variable helps the GIL-releasing thread wait for
       a GIL-awaiting thread to be scheduled and take the GIL. */
    PyCOND_T switch_cond;
    PyMUTEX_T switch_mutex;
#endif
};

struct _ceval_runtime_state {
    /* Request for checking signals. It is shared by all interpreters (see
       bpo-40513). Any thread of any interpreter can receive a signal, but only
       the main thread of the main interpreter can handle signals: see
       _Py_ThreadCanHandleSignals(). */
    _Py_atomic_int signals_pending;
    struct _gil_runtime_state gil;
};

/* GC information is stored BEFORE the object structure. */
typedef struct {
    // Pointer to next object in the list.
    // 0 means the object is not tracked
    uintptr_t _gc_next;

    // Pointer to previous object in the list.
    // Lowest two bits are used for flags documented later.
    uintptr_t _gc_prev;
} PyGC_Head;

struct gc_generation {
    PyGC_Head head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

#define NUM_GENERATIONS 3

/* Running stats per generation */
struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head *generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;
};

struct _Py_AuditHookEntry;

struct _Py_unicode_runtime_ids {
    PyThread_type_lock lock;
    // next_index value must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times: see _PyUnicode_FromId() implementation.
    Py_ssize_t next_index;
};

typedef struct pyruntimestate {
    /* Has been initialized to a safe state.

       In order to be effective, this must be set to 0 during or right
       after allocation. */
    int _initialized;

    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again.

       Use _PyRuntimeState_GetFinalizing() and _PyRuntimeState_SetFinalizing()
       to access it, don't access it directly. */
    _Py_atomic_address _finalizing;

    struct pyinterpreters {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        /* _next_interp_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Init(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _ceval_runtime_state ceval;
    struct _gilstate_runtime_state gilstate;

    PyPreConfig preconfig;

    Py_OpenCodeHookFunction open_code_hook;
    void *open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head;

    struct _Py_unicode_runtime_ids unicode_ids;

    /* All the objects that are shared by the runtime's interpreters. */
    //struct _Py_global_objects global_objects;

    /* The following fields are here to avoid allocation during init.
       The data is exposed through _PyRuntimeState pointer fields.
       These fields should not be accessed directly outside of init.

       All other _PyRuntimeState pointer fields are populated when
       needed and default to NULL.

       For now there are some exceptions to that rule, which require
       allocation during init.  These will be addressed on a case-by-case
       basis.  Most notably, we don't pre-allocated the several mutex
       (PyThread_type_lock) fields, because on Windows we only ever get
       a pointer type.
       */

    /* PyInterpreterState.interpreters.main */
    //PyInterpreterState _main_interpreter;
} _PyRuntimeState;

DLLLOCAL extern _PyRuntimeState _PyRuntime;

#if defined(__GNUC__) && (defined(__i386__) || defined(__amd64))
typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

static __inline__ void
_Py_atomic_signal_fence(_Py_memory_order order)
{
    if (order != _Py_memory_order_relaxed)
        __asm__ volatile("":::"memory");
}

static __inline__ void
_Py_atomic_thread_fence(_Py_memory_order order)
{
    if (order != _Py_memory_order_relaxed)
        __asm__ volatile("mfence":::"memory");
}

/* Tell the race checker about this operation's effects. */
static __inline__ void
_Py_ANNOTATE_MEMORY_ORDER(const volatile void *address, _Py_memory_order order)
{
    (void)address;              /* shut up -Wunused-parameter */
    switch(order) {
    case _Py_memory_order_release:
    case _Py_memory_order_acq_rel:
    case _Py_memory_order_seq_cst:
        _Py_ANNOTATE_HAPPENS_BEFORE(address);
        break;
    case _Py_memory_order_relaxed:
    case _Py_memory_order_acquire:
        break;
    }
    switch(order) {
    case _Py_memory_order_acquire:
    case _Py_memory_order_acq_rel:
    case _Py_memory_order_seq_cst:
        _Py_ANNOTATE_HAPPENS_AFTER(address);
        break;
    case _Py_memory_order_relaxed:
    case _Py_memory_order_release:
        break;
    }
}

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER) \
    __extension__ ({  \
        __typeof__(ATOMIC_VAL) atomic_val = ATOMIC_VAL; \
        __typeof__(atomic_val->_value) result; \
        volatile __typeof__(result) *volatile_data = &atomic_val->_value; \
        _Py_memory_order order = ORDER; \
        _Py_ANNOTATE_MEMORY_ORDER(atomic_val, order); \
        \
        /* Perform the operation. */ \
        _Py_ANNOTATE_IGNORE_READS_BEGIN(); \
        switch(order) { \
        case _Py_memory_order_release: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            /* Loads on x86 are not releases by default, so need a */ \
            /* thread fence. */ \
            _Py_atomic_thread_fence(_Py_memory_order_release); \
            break; \
        default: \
            /* No fence */ \
            break; \
        } \
        result = *volatile_data; \
        switch(order) { \
        case _Py_memory_order_acquire: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            /* Loads on x86 are automatically acquire operations so */ \
            /* can get by with just a compiler fence. */ \
            _Py_atomic_signal_fence(_Py_memory_order_acquire); \
            break; \
        default: \
            /* No fence */ \
            break; \
        } \
        _Py_ANNOTATE_IGNORE_READS_END(); \
        result; \
    })

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER) \
    __extension__ ({ \
        __typeof__(ATOMIC_VAL) atomic_val = ATOMIC_VAL; \
        __typeof__(atomic_val->_value) new_val = NEW_VAL;\
        volatile __typeof__(new_val) *volatile_data = &atomic_val->_value; \
        _Py_memory_order order = ORDER; \
        _Py_ANNOTATE_MEMORY_ORDER(atomic_val, order); \
        \
        /* Perform the operation. */ \
        _Py_ANNOTATE_IGNORE_WRITES_BEGIN(); \
        switch(order) { \
        case _Py_memory_order_release: \
            _Py_atomic_signal_fence(_Py_memory_order_release); \
            /* fallthrough */ \
        case _Py_memory_order_relaxed: \
            *volatile_data = new_val; \
            break; \
        \
        case _Py_memory_order_acquire: \
        case _Py_memory_order_acq_rel: \
        case _Py_memory_order_seq_cst: \
            __asm__ volatile("xchg %0, %1" \
                         : "+r"(new_val) \
                         : "m"(atomic_val->_value) \
                         : "memory"); \
            break; \
        } \
        _Py_ANNOTATE_IGNORE_WRITES_END(); \
    })
#elif defined(__GNUC__) && (defined(__arm__))
# include <atomic>

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed = std::memory_order_relaxed,
    _Py_memory_order_acquire = std::memory_order_acquire,
    _Py_memory_order_release = std::memory_order_release,
    _Py_memory_order_acq_rel = std::memory_order_acq_rel,
    _Py_memory_order_seq_cst = std::memory_order_seq_cst
} _Py_memory_order;

#define atomic_load_explicit(PTR, MO)                                   \
  __extension__                                                         \
  ({                                                                    \
    __auto_type __atomic_load_ptr = (PTR);                              \
    __typeof__ (*__atomic_load_ptr) __atomic_load_tmp;                  \
    __atomic_load (__atomic_load_ptr, &__atomic_load_tmp, (MO));        \
    __atomic_load_tmp;                                                  \
  })

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER)   \
    atomic_load_explicit(&(ATOMIC_VAL)->_value, ORDER)

#define atomic_store_explicit(PTR, VAL, MO)                             \
  __extension__                                                         \
  ({                                                                    \
    __auto_type __atomic_store_ptr = (PTR);                             \
    __typeof__ (*__atomic_store_ptr) __atomic_store_tmp = (VAL);        \
    __atomic_store (__atomic_store_ptr, &__atomic_store_tmp, (MO));     \
  })

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER)   \
    atomic_store_explicit(&(ATOMIC_VAL)->_value, NEW_VAL, ORDER)
#elif defined(__GNUC__)
// assume all other architectures use standard atomic functions
# include <atomic>

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed = std::memory_order_relaxed,
    _Py_memory_order_acquire = std::memory_order_acquire,
    _Py_memory_order_release = std::memory_order_release,
    _Py_memory_order_acq_rel = std::memory_order_acq_rel,
    _Py_memory_order_seq_cst = std::memory_order_seq_cst
} _Py_memory_order;

#define atomic_load_explicit(PTR, MO)                                   \
  __extension__                                                         \
  ({                                                                    \
    auto __atomic_load_ptr = (PTR);                              \
    __typeof__ (*__atomic_load_ptr) __atomic_load_tmp;                  \
    __atomic_load (__atomic_load_ptr, &__atomic_load_tmp, (MO));        \
    __atomic_load_tmp;                                                  \
  })

#define atomic_store_explicit(PTR, VAL, MO)                             \
  __extension__                                                         \
  ({                                                                    \
    auto __atomic_store_ptr = (PTR);                             \
    __typeof__ (*__atomic_store_ptr) __atomic_store_tmp = (VAL);        \
    __atomic_store (__atomic_store_ptr, &__atomic_store_tmp, (MO));     \
  })

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER)   \
    atomic_load_explicit(&(ATOMIC_VAL)->_value, ORDER)

#define _Py_atomic_store_explicit(ATOMIC_VAL, NEW_VAL, ORDER)   \
    atomic_store_explicit(&(ATOMIC_VAL)->_value, NEW_VAL, ORDER)
#endif

#define _Py_atomic_load_relaxed(ATOMIC_VAL) \
    _Py_atomic_load_explicit((ATOMIC_VAL), _Py_memory_order_relaxed)

#define _Py_atomic_store_relaxed(ATOMIC_VAL, NEW_VAL) \
    _Py_atomic_store_explicit((ATOMIC_VAL), (NEW_VAL), _Py_memory_order_relaxed)

// Python 3.11 GIL Tracking
// ========================
// In Python 3.11, we need our own tracking because:
// 1. After PyInterpreterState_Clear/Delete, tstate_current can be NULL even when the GIL
//    is still held by the current thread. This corrupts the normal tracking.
// 2. With sub-interpreters, PyGILState_Check() can return false even when we hold the GIL
//    with a different thread state on the same OS thread.
//
// Solution: Maintain our own thread-local tracking of GIL ownership that's updated at
// every acquire/release point and survives the corrupted-tracking scenario.

// Thread-local state tracking
inline thread_local PyThreadState* _qore_tss_tstate = nullptr;
inline thread_local bool _qore_tss_initialized = false;
// CRITICAL: This flag tracks whether the CURRENT THREAD holds the GIL, independent of
// tstate_current. This handles the case where tstate_current becomes NULL after
// PyInterpreterState_Clear/Delete but the GIL mutex is still locked by us.
inline thread_local bool _qore_gil_held = false;

// Get the current thread state - use Python's native function which is reliable in 3.11
DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    // In Python 3.11, PyGILState_GetThisThreadState() is reliable
    // But during very early init it might return NULL, so fallback to runtime structure
    PyThreadState* tss = PyGILState_GetThisThreadState();
    if (tss) {
        return tss;
    }
    // Fallback during early initialization
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.gilstate.tstate_current));
}

// Set this thread's state - update both our tracking and Python's TSS
DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    _qore_tss_tstate = state;
    _qore_tss_initialized = true;
    // Update Python's TSS for compatibility
    PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void*)state);
}

// GIL status check - check if the GIL is held by the CURRENT thread
// NOTE: In Python 3.11, PyGILState_Check() is NOT reliable when multiple sub-interpreters
// have different thread states on the same thread. PyGILState_Check() returns false when
// tstate_current != PyGILState_GetThisThreadState(), but the GIL is actually held by
// a different thread state on the same thread!
//
// Additionally, after PyInterpreterState_Clear/Delete, tstate_current can be NULL even
// though the GIL mutex is still locked by the current thread. We must handle this case
// to avoid deadlocking by trying to acquire an already-held GIL.
//
// Solution: Check our thread-local _qore_gil_held flag FIRST (handles corrupted-tracking),
// then fallback to checking tstate_current->thread_id (handles sub-interpreter case).
DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    // First check our own tracking - this handles the corrupted-tracking scenario
    // where tstate_current is NULL but we still hold the GIL mutex
    if (_qore_gil_held) {
        return true;
    }
    // Check if tstate_current is non-NULL and belongs to our thread
    PyThreadState* current = reinterpret_cast<PyThreadState*>(
        _Py_atomic_load_relaxed(&_PyRuntime.gilstate.tstate_current));
    if (!current) {
        return false;
    }
    // Check if the GIL holder is on the same thread as us
    // In Python 3.11, PyThreadState has thread_id field
    unsigned long our_thread_id = PyThread_get_thread_ident();
    return current->thread_id == our_thread_id;
}

// Check if this might be a Python-created thread that already has the GIL.
// In Python 3.11, PyGILState_* functions are reliable so this is straightforward.
DLLLOCAL static inline PyThreadState* _qore_check_python_created_thread_gil() {
    // In Python 3.11, we can trust PyGILState_Check() and PyGILState_GetThisThreadState()
    if (PyGILState_Check()) {
        PyThreadState* tss_state = PyGILState_GetThisThreadState();
        if (tss_state) {
            _qore_tss_tstate = tss_state;
            _qore_tss_initialized = true;
            _qore_gil_held = true;  // Track that we hold the GIL
            return tss_state;
        }
    }
    return nullptr;
}

// Get the thread state that holds the GIL
DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    // In Python 3.11, PyGILState_GetThisThreadState() is reliable
    PyThreadState* tss = PyGILState_GetThisThreadState();
    if (tss) {
        return tss;
    }
    // Fallback during early initialization
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.last_holder));
}

// Swap thread state for ceval purposes
// NOTE: This updates our tracking, Python's TSS, and last_holder to maintain consistency
// across all the different ways thread state is queried.
DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* new_state) {
    PyThreadState* old = _qore_tss_tstate;
    _qore_tss_tstate = new_state;
    // Update Python's TSS so PyGILState_GetThisThreadState() returns consistent value
    PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void*)new_state);
    // Also update last_holder for consistency
    _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder, (uintptr_t)new_state);
    return old;
}

#define _QORE_PYTHON_REENABLE_GIL_CHECK { assert(!_PyRuntime.gilstate.check_enabled); _PyRuntime.gilstate.check_enabled = 1; }

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
    // Set our thread-local tracking since we acquired the GIL
    _qore_tss_tstate = tstate;
    _qore_tss_initialized = true;
    _qore_gil_held = true;  // Track that we now hold the GIL
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    // In Python 3.11, after PyInterpreterState_Clear/Delete, the GIL tracking
    // (tstate_current) may be corrupted (set to NULL) even though the GIL mutex
    // is still locked. We need to restore the tracking before releasing.
    //
    // Check if tstate_current is NULL - if so, restore it to our tstate
    // so that PyEval_SaveThread can work correctly.
    PyThreadState* current = reinterpret_cast<PyThreadState*>(
        _Py_atomic_load_relaxed(&_PyRuntime.gilstate.tstate_current));
    if (current == nullptr && tstate != nullptr) {
        // Restore tstate_current so release functions work
        _Py_atomic_store_relaxed(&_PyRuntime.gilstate.tstate_current, (uintptr_t)tstate);
        // Also update the TSS
        PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void*)tstate);
    }
    // Use PyEval_SaveThread instead of PyEval_ReleaseThread for Python 3.11.
    // PyEval_ReleaseThread checks that tstate matches the current thread state,
    // but after certain Python operations, the current thread state may be
    // different, causing "wrong thread state" errors.
    // PyEval_SaveThread simply releases the GIL without this check.
    PyEval_SaveThread();
    // Clear our thread-local tracking since we released the GIL
    _qore_tss_tstate = nullptr;
    _qore_gil_held = false;  // Track that we no longer hold the GIL
    // CRITICAL: Also clear Python's autoTSSkey. After releasing the GIL, if we don't clear this,
    // PyGILState_GetThisThreadState() will return a stale thread state while tstate_current is NULL.
    // This causes PyGILState_Check() to fail (stale != NULL) even when we later reacquire the GIL
    // with a different thread state.
    PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, nullptr);
}

#endif
