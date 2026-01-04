/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python36_internals.h

    Qore Programming Language

    Copyright 2020 - 2021 Qore Technologies, s.r.o.

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

#include <pystate.h>
#include <dynamic_annotations.h>

#error unimplemented - cannot support Python < 3.7

#define _Py_atomic_load_relaxed(ATOMIC_VAL) \
    _Py_atomic_load_explicit(ATOMIC_VAL, _Py_memory_order_relaxed)

typedef enum _Py_memory_order {
    _Py_memory_order_relaxed,
    _Py_memory_order_acquire,
    _Py_memory_order_release,
    _Py_memory_order_acq_rel,
    _Py_memory_order_seq_cst
} _Py_memory_order;

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

extern _Py_atomic_address _PyThreadState_Current;

#if defined(__GNUC__) && (defined(__i386__) || defined(__amd64))
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

#define atomic_load(PTR)  atomic_load_explicit (PTR, __ATOMIC_SEQ_CST)

#define _Py_atomic_load_explicit(ATOMIC_VAL, ORDER)   \
    atomic_load_explicit(&(ATOMIC_VAL)->_value, ORDER)
#endif

DLLLOCAL static inline int
PyThread_set_key_value(int key, void *value)
{
    int fail;
    fail = pthread_setspecific(key, value);
    return fail ? -1 : 0;
}

DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyThreadState_Current));
}

#define NEED_PYTHON_36_TLS_KEY
DLLLOCAL extern int autoTLSkey;

DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
    PyThread_set_key_value(autoTLSkey, (void*)state);
}

/*
DLLLOCAL static bool _qore_PyCeval_GetGilLockedStatus() {
}

DLLLOCAL static PyThreadState* _qore_PyCeval_GetThreadState() {
}

DLLLOCAL static PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* gil_state) {
}
*/

#define _QORE_PYTHON_REENABLE_GIL_CHECK { assert(!_PyGILState_check_enabled); _PyGILState_check_enabled = 1; }

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
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
}

#endif
