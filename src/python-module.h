/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    python-module.h

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

#ifndef _QORE_PYTHON_MODULE_H
#define _QORE_PYTHON_MODULE_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

#include <qore/Qore.h>

//! the name of the module
#define QORE_PYTHON_MODULE_NAME "python"
//! the name of the main Python namespace in Qore
#define QORE_PYTHON_NS_NAME "Python"
//! the name of the language in stack traces
#define QORE_PYTHON_LANG_NAME "Python"

// module registration function
DLLEXPORT extern "C" void python_qore_module_desc(QoreModuleInfo& mod_info);

// export function for other language modules
DLLEXPORT extern "C" int python_module_import(ExceptionSink* xsink, QoreProgram* pgm, const char* module,
    const char* symbol);

DLLLOCAL extern PyThreadState* mainThreadState;

// forward reference
class QorePythonProgram;
class QorePythonClass;

DLLLOCAL extern QorePythonClass* QC_PYTHONBASEOBJECT;
DLLLOCAL extern qore_classid_t CID_PYTHONBASEOBJECT;

DLLLOCAL extern bool python_shutdown;

/** NOTE: depends on Python internals to work around limitations with the GIL and multiple thread states with multiple
          interpreters
*/
#ifdef HAVE_PYTHON_INTERNAL_INCLUDES
#define Py_BUILD_CORE
#include <internal/pycore_pystate.h>
#else
#if PY_MAJOR_VERSION >= 3
#if PY_MINOR_VERSION >= 14
#ifdef Py_GIL_DISABLED
// Free-threading Python 3.14+
#include "python314_internals.h"
#else
// GIL-enabled Python 3.14+ uses 3.12 internals
#include "python312_internals.h"
#endif
#elif PY_MINOR_VERSION >= 12
#include "python312_internals.h"
#elif PY_MINOR_VERSION == 11
#include "python311_internals.h"
#elif PY_MINOR_VERSION == 10
#include "python310_internals.h"
#elif PY_MINOR_VERSION == 9
#include "python39_internals.h"
#elif PY_MINOR_VERSION == 8
#include "python38_internals.h"
#elif PY_MINOR_VERSION == 7
#include "python37_internals.h"
#else
#error unsupported python version
#endif
#endif
#endif

/*
    Python API compatibility layer for Python 3.14+

    These APIs were removed/deprecated in Python 3.14:
    - PyEval_CallObject -> PyObject_Call
    - PyEval_CallObjectWithKeywords -> PyObject_Call
    - PyCFunction_Call -> PyObject_Call
    - PyThreadState_GetRecursionLimit removed (use Py_GetRecursionLimit)
    - _PyGILState_GetInterpreterStateUnsafe removed (use PyInterpreterState_Main)
*/
#if PY_VERSION_HEX >= 0x030E0000

// Recursion limit APIs - Python 3.14 uses global limits, not per-thread
#ifndef Py_GIL_DISABLED
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
#endif // !Py_GIL_DISABLED

// Python 3.14+ compatibility wrappers for removed APIs

// PyEval_CallObject was removed - provide a compatibility wrapper using PyObject_Call
static inline PyObject* qore_PyEval_CallObject(PyObject* callable, PyObject* args) {
    PyObject* empty_args = nullptr;
    if (!args) {
        empty_args = PyTuple_New(0);
        if (!empty_args) {
            return nullptr;
        }
        args = empty_args;
    }
    PyObject* result = PyObject_Call(callable, args, nullptr);
    Py_XDECREF(empty_args);
    return result;
}
#define PyEval_CallObject(callable, args) qore_PyEval_CallObject((callable), (args))

// PyEval_CallObjectWithKeywords was removed - provide a compatibility wrapper using PyObject_Call
static inline PyObject* qore_PyEval_CallObjectWithKeywords(PyObject* callable, PyObject* args, PyObject* kwargs) {
    PyObject* empty_args = nullptr;
    if (!args) {
        empty_args = PyTuple_New(0);
        if (!empty_args) {
            return nullptr;
        }
        args = empty_args;
    }
    PyObject* result = PyObject_Call(callable, args, kwargs);
    Py_XDECREF(empty_args);
    return result;
}
#define PyEval_CallObjectWithKeywords(callable, args, kwargs) \
    qore_PyEval_CallObjectWithKeywords((callable), (args), (kwargs))

// PyCFunction_Call was removed - use PyObject_Call instead
#define PyCFunction_Call(func, args, kwargs) \
    PyObject_Call((func), (args), (kwargs))

#endif // PY_VERSION_HEX >= 0x030E0000

/** Thread State Locations:
    - _PyRuntime.gilstate.tstate_current - must only be modified while holding the GIL
        read: _qore_PyRuntimeGILState_GetThreadState (_PyThreadState_GET)
        write: PyThreadState_Swap

    - _PyRuntime.ceval.gil.last_holder - must only be modified while holding the GIL
        read: _qore_PyCeval_GetThreadState()
        write: _qore_PyCeval_SwapThreadState()

    - TSS thread state - thread local
        read: PyGILState_GetThisThreadState()
        write: _qore_PyGILState_SetThisThreadState()
*/

//! acquires the GIL and sets the main interpreter thread context
/** This class is used when a new interpreter context is created.

    The new interpreter context has its gilstate_counter decremented in the destructor, and the main interpreter
    thread context is restored before releasing the GIL.

    This way we don't need to use the deprecated GIL acquire and release APIs
*/
class QorePythonGilHelper {
public:
    DLLLOCAL QorePythonGilHelper(PyThreadState* new_thread_state = mainThreadState);

    DLLLOCAL ~QorePythonGilHelper();

    DLLLOCAL void set(PyThreadState* other_state);

#ifdef Py_GIL_DISABLED
    //! Releases the GIL state before creating a sub-interpreter
    /** In free-threading mode, PyGILState_Ensure() initializes thread-local mimalloc heap data
        for the main interpreter. When creating a sub-interpreter, we need to release this state
        so that Py_NewInterpreterFromConfig can properly initialize mimalloc for the new interpreter.

        This method:
        1. Releases the PyGILState to reset thread-local mimalloc state
        2. Detaches the current thread state so Py_NewInterpreterFromConfig can attach a new one

        After calling this, you MUST call set() with the new interpreter's thread state.
    */
    DLLLOCAL void releaseBeforeSubInterpreter();
#endif

protected:
    PyThreadState* new_thread_state;
    PyThreadState* state;
    PyThreadState* t_state;
    bool release_gil = true;
#ifdef Py_GIL_DISABLED
    PyGILState_STATE gstate;
    bool gstate_released = false;  // True if gstate was released for sub-interpreter creation
#endif
};

class QorePythonReleaseGilHelper {
public:
    DLLLOCAL QorePythonReleaseGilHelper() : _save(PyEval_SaveThread()) {
    }

    DLLLOCAL ~QorePythonReleaseGilHelper() {
        PyEval_RestoreThread(_save);
    }

private:
    PyThreadState* _save;
};

struct QorePythonThreadInfo {
    PyThreadState* tss_state;
    PyThreadState* t_state;
    PyThreadState* ceval_state;
    PyGILState_STATE g_state;
    int recursion_depth;
    bool valid;
};

//! acquires the GIL and manages thread state
class QorePythonHelper {
public:
    DLLLOCAL QorePythonHelper(const QorePythonProgram* pypgm);

    DLLLOCAL ~QorePythonHelper();

    DLLLOCAL QorePythonProgram* getOldProgram() const {
        return old_pgm ? reinterpret_cast<QorePythonProgram*>(old_pgm) : const_cast<QorePythonProgram*>(new_pypgm);
    }

protected:
    void* old_pgm;
    QorePythonThreadInfo old_state;
    const QorePythonProgram* new_pypgm;
};

class QorePythonManualReferenceHolder {
public:
    DLLLOCAL QorePythonManualReferenceHolder() : obj(nullptr) {
    }

    DLLLOCAL QorePythonManualReferenceHolder(PyObject* obj) : obj(obj) {
    }

    DLLLOCAL QorePythonManualReferenceHolder(QorePythonManualReferenceHolder&& old) : obj(old.obj) {
        old.obj = nullptr;
    }

    DLLLOCAL void purge() {
        if (obj) {
            py_deref();
            obj = nullptr;
        }
    }

    DLLLOCAL QorePythonManualReferenceHolder& operator=(PyObject* obj) {
        if (this->obj) {
            py_deref();
        }
        this->obj = obj;
        return *this;
    }

    DLLLOCAL PyObject* release() {
        PyObject* rv = obj;
        obj = nullptr;
        return rv;
    }

    DLLLOCAL PyObject** getRef() {
        return &obj;
    }

    DLLLOCAL PyObject* operator*() const {
        return obj;
    }

    DLLLOCAL operator bool() const {
        return (bool)obj;
    }

    DLLLOCAL void py_ref() {
        assert(obj);
        Py_INCREF(obj);
    }

    DLLLOCAL void py_deref() {
        assert(obj);
        Py_DECREF(obj);
    }

protected:
    PyObject* obj;

private:
    QorePythonManualReferenceHolder(const QorePythonManualReferenceHolder&) = delete;
    QorePythonManualReferenceHolder& operator=(QorePythonManualReferenceHolder&) = delete;
};

//! Python ref holder
class QorePythonReferenceHolder : public QorePythonManualReferenceHolder {
public:
    DLLLOCAL QorePythonReferenceHolder() : QorePythonManualReferenceHolder(nullptr) {
    }

    DLLLOCAL QorePythonReferenceHolder(PyObject* obj) : QorePythonManualReferenceHolder(obj) {
    }

    DLLLOCAL QorePythonReferenceHolder(QorePythonReferenceHolder&& old) : QorePythonManualReferenceHolder(std::move(old)) {
    }

    DLLLOCAL ~QorePythonReferenceHolder() {
        if (!python_shutdown) {
            purge();
        }
    }

    DLLLOCAL QorePythonReferenceHolder& operator=(PyObject* obj) {
        if (this->obj) {
            py_deref();
        }
        this->obj = obj;
        return *this;
    }
};

class QorePythonGilStateHelper {
public:
    DLLLOCAL QorePythonGilStateHelper() {
        old_state = PyGILState_Ensure();
    }

    DLLLOCAL ~QorePythonGilStateHelper() {
        PyGILState_Release(old_state);
    }

protected:
    PyGILState_STATE old_state;
};

// Base type for Qore objects in Python
DLLLOCAL extern PyTypeObject PythonQoreObjectBase_Type;

// Python program control for Qore interfacing
DLLLOCAL extern QorePythonProgram* qore_python_pgm;

//! Python module definition function for the qoreloader module
DLLLOCAL PyMODINIT_FUNC PyInit_qoreloader();

//! Creates the global QOre Python program object
DLLLOCAL int init_global_qore_python_pgm();

//! Returns true if the current thread is holding the GIL
DLLLOCAL bool _qore_has_gil(PyThreadState* t_state = PyGILState_GetThisThreadState());

class QorePythonProgram;
DLLLOCAL extern QoreNamespace* PNS;

DLLLOCAL extern int python_u_tld_key;
DLLLOCAL extern int python_qobj_key;

class QorePythonImplicitQoreArgHelper {
public:
    DLLLOCAL QorePythonImplicitQoreArgHelper(void* obj)
        : old_ptr(q_swap_thread_local_data(python_qobj_key, (void*)obj)) {
    }

    DLLLOCAL ~QorePythonImplicitQoreArgHelper() {
        q_swap_thread_local_data(python_qobj_key, old_ptr);
    }

    DLLLOCAL static QoreObject* getQoreObject() {
        return (QoreObject*)q_get_thread_local_data(python_qobj_key);
    }

    DLLLOCAL static ResolvedCallReferenceNode* getQoreCallable() {
        return (ResolvedCallReferenceNode*)q_get_thread_local_data(python_qobj_key);
    }

private:
    void* old_ptr;
};

class PythonQoreClass;
typedef std::map<const QoreClass*, PythonQoreClass*> py_cls_map_t;

//! called from Python only; if it returns -1, a Python exception is raised
DLLLOCAL int load_jni_module(QorePythonProgram* qore_python_pgm);

//! called from Python only; if it returns -1, a Python exception is raised
DLLLOCAL int do_jni_module_import(QorePythonProgram* qore_python_pgm, const char* name_str);

class QorePythonProgram;
//! for tracking QorePythonProgram objects when initialized from Python
DLLLOCAL bool qpy_register(QorePythonProgram* p);
DLLLOCAL void qpy_deregister(QorePythonProgram* p);

//! if true then this module was initialized by Python
DLLLOCAL extern bool qore_needs_shutdown;

#if 0
DLLLOCAL int q_reset_python(ExceptionSink* xsink);
#endif

#endif
