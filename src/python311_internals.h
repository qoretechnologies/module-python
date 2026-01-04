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

// equivalent to: PyThreadState_GET() == _PyThreadState_GET() == _PyRuntimeState_GetThreadState(&_PyRuntime.gilstate.tstate_current)
DLLLOCAL static PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.gilstate.tstate_current));
}

DLLLOCAL static void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    PyThread_tss_set(&_PyRuntime.gilstate.autoTSSkey, (void*)state);
}

DLLLOCAL static bool _qore_PyCeval_GetGilLockedStatus() {
    return (bool)(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked));
}

DLLLOCAL static PyThreadState* _qore_PyCeval_GetThreadState() {
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.last_holder));
}

DLLLOCAL static PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* gil_state) {
    PyThreadState* old = reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.last_holder));
    if (old != gil_state) {
        _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder, (uintptr_t)gil_state);
    }
    return old;
}

#define _QORE_PYTHON_REENABLE_GIL_CHECK { assert(!_PyRuntime.gilstate.check_enabled); _PyRuntime.gilstate.check_enabled = 1; }

#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

#endif
