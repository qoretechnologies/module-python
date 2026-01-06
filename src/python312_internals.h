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

#include <dynamic_annotations.h>
#include <fileobject.h>

inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    return state->py_recursion_limit;
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    state->py_recursion_limit = new_limit;
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
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
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

#ifdef PY_HAVE_PERF_TRAMPOLINE
typedef enum {
    PERF_STATUS_FAILED = -1,  // Perf trampoline is in an invalid state
    PERF_STATUS_NO_INIT = 0,  // Perf trampoline is not initialized
    PERF_STATUS_OK = 1,       // Perf trampoline is ready to be executed
} perf_status_t;

struct code_arena_st;

struct trampoline_api_st {
    void* (*init_state)(void);
    void (*write_state)(void* state, const void *code_addr,
                        unsigned int code_size, PyCodeObject* code);
    int (*free_state)(void* state);
    void *state;
};
#endif

struct _pending_calls {
    int busy;
    PyThread_type_lock lock;
    /* Request for running pending calls. */
    _Py_atomic_int calls_to_do;
    /* Request for looking at the `async_exc` field of the current
       thread state.
       Guarded by the GIL. */
    int async_exc;
#define NPENDINGCALLS 32
    struct _pending_call {
        int (*func)(void *);
        void *arg;
    } calls[NPENDINGCALLS];
    int first;
    int last;
};

struct _ceval_runtime_state {
     struct {
#ifdef PY_HAVE_PERF_TRAMPOLINE
        perf_status_t status;
        Py_ssize_t extra_code_index;
        void *code_arena;
        struct trampoline_api_st trampoline_api;
        FILE *map_file;
#else
        int _not_used;
#endif
    } perf;
    /* Request for checking signals. It is shared by all interpreters (see
       bpo-40513). Any thread of any interpreter can receive a signal, but only
       the main thread of the main interpreter can handle signals: see
       _Py_ThreadCanHandleSignals(). */
    _Py_atomic_int signals_pending;
    /* Pending calls to be made only on the main thread. */
    struct _pending_calls pending_mainthread;
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

typedef struct {
    /* We tag each block with an API ID in order to tag API violations */
    char api_id;
    PyMemAllocatorEx alloc;
} debug_alloc_api_t;

struct _pymem_allocators {
    PyThread_type_lock mutex;
    struct {
        PyMemAllocatorEx raw;
        PyMemAllocatorEx mem;
        PyMemAllocatorEx obj;
    } standard;
    struct {
        debug_alloc_api_t raw;
        debug_alloc_api_t mem;
        debug_alloc_api_t obj;
    } debug;
    PyObjectArenaAllocator obj_arena;
};

struct _obmalloc_global_state {
    int dump_debug_stats;
    Py_ssize_t interpreter_leaks;
};

struct _time_runtime_state {
#ifdef HAVE_TIMES
    int ticks_per_second_initialized;
    long ticks_per_second;
#else
    int _not_used;
#endif
};

struct _pythread_runtime_state {
    int initialized;

#ifdef _USE_PTHREADS
    // This matches when thread_pthread.h is used.
    struct {
        /* NULL when pthread_condattr_setclock(CLOCK_MONOTONIC) is not supported. */
        pthread_condattr_t *ptr;
# ifdef CONDATTR_MONOTONIC
    /* The value to which condattr_monotonic is set. */
        pthread_condattr_t val;
# endif
    } _condattr_monotonic;

#endif  // USE_PTHREADS

#if defined(HAVE_PTHREAD_STUBS)
    struct {
        struct py_stub_tls_entry tls_entries[PTHREAD_KEYS_MAX];
    } stubs;
#endif
};

#ifdef _SIG_MAXSIG
   // gh-91145: On FreeBSD, <signal.h> defines NSIG as 32: it doesn't include
   // realtime signals: [SIGRTMIN,SIGRTMAX]. Use _SIG_MAXSIG instead. For
   // example on x86-64 FreeBSD 13, SIGRTMAX is 126 and _SIG_MAXSIG is 128.
#  define Py_NSIG _SIG_MAXSIG
#elif defined(NSIG)
#  define Py_NSIG NSIG
#elif defined(_NSIG)
#  define Py_NSIG _NSIG            // BSD/SysV
#elif defined(_SIGMAX)
#  define Py_NSIG (_SIGMAX + 1)    // QNX
#elif defined(SIGMAX)
#  define Py_NSIG (SIGMAX + 1)     // djgpp
#else
#  define Py_NSIG 64               // Use a reasonable default value
#endif

struct _signals_runtime_state {
    volatile struct {
        _Py_atomic_int tripped;
        /* func is atomic to ensure that PyErr_SetInterrupt is async-signal-safe
         * (even though it would probably be otherwise, anyway).
         */
        _Py_atomic_address func;
    } handlers[Py_NSIG];

    volatile struct {
#ifdef MS_WINDOWS
        /* This would be "SOCKET fd" if <winsock2.h> were always included.
           It isn't so we must cast to SOCKET where appropriate. */
        volatile int fd;
#elif defined(__VXWORKS__)
        int fd;
#else
        sig_atomic_t fd;
#endif

        int warn_on_full_buffer;
#ifdef MS_WINDOWS
        int use_send;
#endif
    } wakeup;

    /* Speed up sigcheck() when none tripped */
    _Py_atomic_int is_tripped;

    /* These objects necessarily belong to the main interpreter. */
    PyObject *default_handler;
    PyObject *ignore_handler;

#ifdef MS_WINDOWS
    /* This would be "HANDLE sigint_event" if <windows.h> were always included.
       It isn't so we must cast to HANDLE everywhere "sigint_event" is used. */
    void *sigint_event;
#endif

    /* True if the main interpreter thread exited due to an unhandled
     * KeyboardInterrupt exception, suggesting the user pressed ^C. */
    int unhandled_keyboard_interrupt;
};

typedef void (*atexit_callbackfunc)(void);

struct _atexit_runtime_state {
    PyThread_type_lock mutex;
#define NEXITFUNCS 32
    atexit_callbackfunc callbacks[NEXITFUNCS];
    int ncallbacks;
};

struct _import_runtime_state {
    /* The builtin modules (defined in config.c). */
    struct _inittab *inittab;
    /* The most recent value assigned to a PyModuleDef.m_base.m_index.
       This is incremented each time PyModuleDef_Init() is called,
       which is just about every time an extension module is imported.
       See PyInterpreterState.modules_by_index for more info. */
    Py_ssize_t last_module_index;
    struct {
        /* A lock to guard the cache. */
        PyThread_type_lock mutex;
        /* The actual cache of (filename, name, PyModuleDef) for modules.
           Only legacy (single-phase init) extension modules are added
           and only if they support multiple initialization (m_size >- 0)
           or are imported in the main interpreter.
           This is initialized lazily in _PyImport_FixupExtensionObject().
           Modules are added there and looked up in _imp.find_extension(). */
        void *hashtable;  // _Py_hashtable_t* - opaque pointer, not accessed directly
    } extensions;
    /* Package context -- the full module name for package imports */
    const char * pkgcontext;
};

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

// Get the current thread state from our thread-local tracking
DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return _qore_tss_tstate;
}

// Set this thread's state in thread-local storage
// NOTE: Only set _qore_tss_initialized when actually acquiring the GIL (state != nullptr).
// When releasing the GIL (state = nullptr), we keep initialized=true to indicate
// this is a Qore-managed thread that released the GIL (vs a Python-created thread).
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
    // Set our thread-local tracking since we acquired the GIL
    _qore_tss_tstate = tstate;
    _qore_tss_initialized = true;
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
    // Clear our thread-local tracking since we released the GIL
    _qore_tss_tstate = nullptr;
}

#endif
