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

// For Python with internal includes, we need Py_BUILD_CORE defined before
// including Python.h to access internal headers. However, for Python 3.13+,
// we also define Py_BUILD_CORE_MODULE to ensure that _PyThreadState_GET() uses
// _PyThreadState_GetCurrent() instead of the non-exported _Py_tss_tstate variable.
#ifdef HAVE_PYTHON_INTERNAL_INCLUDES
#define Py_BUILD_CORE
// For Python 3.13+, define Py_BUILD_CORE_MODULE to avoid using _Py_tss_tstate
// which is not exported from the Python library
#define Py_BUILD_CORE_MODULE
#endif

#define PY_SSIZE_T_CLEAN
// Python 3.16 deprecates PyStatus::exitcode, a member of a structure that the API passes by value (ex:
// PyStatus_Exception()); the implicit copy constructor of PyStatus then uses the deprecated member, which clang reports
// at the definition of the structure in the Python headers, where a pragma around the call has no effect.  Deprecation
// warnings are disabled for the Python headers only: a deprecated API that the module uses is still reported where the
// module uses it (https://github.com/python/cpython/issues/160436)
#include <patchlevel.h>
#if PY_VERSION_HEX >= 0x03100000
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <Python.h>
#include <structmember.h>
#if PY_VERSION_HEX >= 0x03100000
#pragma GCC diagnostic pop
#endif

#include <qore/Qore.h>
#include <qore/QoreSandboxManager.h>

#include <memory>
#include <mutex>
#include <atomic>
#include <optional>

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

//! set in a child process created with fork() while a thread that does not exist in the child held the GIL
/** The GIL can never be acquired in such a process, so Python is not used in it at all, as after it has been shut
    down (python_shutdown is also set): a call to Python raises PYTHON-FORK-ERROR, and the process exits if its thread
    returns to Python code (see qore_python_fork_lost_exit()).
*/
DLLLOCAL extern bool python_fork_lost;

//! raises PYTHON-FORK-ERROR for a call to Python in a process in which python_fork_lost is set
DLLLOCAL void qore_python_raise_fork_error(ExceptionSink* xsink);

//! ends a process in which python_fork_lost is set when its thread returns to Python code
/** The thread cannot continue: Python code needs the GIL.  Writes a message to stderr and calls _exit(1).
*/
[[noreturn]] DLLLOCAL void qore_python_fork_lost_exit();

//! the thread state of the thread that calls Qore's fork() from Python code of the main interpreter
/** Set by QorePythonProgram::callQoreFunction() while the thread calls Qore's fork() with the GIL released; the fork
    handlers of the module then run Python's fork protocol for that fork(), as os.fork() does (see
    qore_python_register_fork_protocol())
*/
DLLLOCAL extern thread_local PyThreadState* python_fork_protocol_tstate;

//! registers the fork handlers that run Python's fork protocol for Qore's fork() called from Python code
/** Called once in the module's initialization function; the handlers cannot be removed, so the module's code is
    kept loaded until the process ends.

    @return 0 for OK, -1 for an error; the error is described in @p err
*/
DLLLOCAL int qore_python_register_fork_protocol(QoreString& err);

//! registers the functions that Python calls when it makes a fork() with its fork protocol (os.register_at_fork())
/** Called with the GIL held when the qoreloader module is initialized in the main interpreter: they tell the fork
    handlers of the module that Python recreates the GIL in the child.

    @return 0 for OK, -1 if a Python exception was raised
*/
DLLLOCAL int qore_python_register_at_fork_functions();

//! returns the interpreter of the given thread state
DLLLOCAL static inline PyInterpreterState* qore_python_get_interpreter(PyThreadState* tstate) {
    return PyThreadState_GetInterpreter(tstate);
}
DLLLOCAL bool _qore_has_gil(PyThreadState* t_state);

/** NOTE: depends on Python internals to work around limitations with the GIL and multiple thread states with multiple
          interpreters
*/
#ifdef HAVE_PYTHON_INTERNAL_INCLUDES
// Py_BUILD_CORE is already defined at the top of this file before Python.h
#include <internal/pycore_pystate.h>
#if PY_VERSION_HEX >= 0x030D0000
#include <internal/pycore_ceval.h>
#include <internal/pycore_gil.h>
#include <internal/pycore_pylifecycle.h>
#endif

// Define QORE macros when using Python internal includes
// In GIL-enabled mode, use standard PyThreadState_Swap
#define _QORE_PYTHREAD_STATE_SWAP(new_state) PyThreadState_Swap(new_state)

// Thread-local GIL ownership tracking
// CRITICAL: This flag tracks whether the CURRENT THREAD holds the GIL, independent of
// Python's internal tstate_current tracking. This is needed because:
// 1. After PyInterpreterState_Clear/Delete, tstate_current can be NULL but GIL is still held
// 2. With sub-interpreters, PyGILState_Check() can be unreliable
// When using internal includes, _PyRuntime.ceval.gil.locked is reliable but we still need
// this for consistent API with the non-internal path.
inline thread_local bool _qore_gil_held = false;

// gilstate_counter access macros - these are available in GIL-enabled Python
#define _QORE_GILSTATE_COUNTER_INC(tstate) (++(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_DEC(tstate) (--(tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_GET(tstate) ((tstate)->gilstate_counter)
#define _QORE_GILSTATE_COUNTER_ASSERT_ONE(tstate) assert((tstate)->gilstate_counter == 1)

// Python 3.14+: _PyRuntime.gilstate.check_enabled doesn't exist - GIL check is always enabled
#if PY_VERSION_HEX >= 0x030E0000
#define _QORE_PYTHON_REENABLE_GIL_CHECK /* no-op in Python 3.14+ */
#else
#define _QORE_PYTHON_REENABLE_GIL_CHECK { assert(!_PyRuntime.gilstate.check_enabled); _PyRuntime.gilstate.check_enabled = 1; }
#endif

// Thread state functions using internal Python APIs
DLLLOCAL static inline PyThreadState* _qore_PyRuntimeGILState_GetThreadState() {
    return _PyThreadState_GET();
}

DLLLOCAL static inline void _qore_PyGILState_SetThisThreadState(PyThreadState* state) {
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+: the thread-local state (_Py_tss_tstate) is not exported,
    // so we cannot set it directly. The Python runtime handles TSS synchronization
    // automatically through PyThreadState_Swap, PyEval_RestoreThread, etc.
    // This function becomes a no-op for Python 3.13+.
    (void)state;
#else
    _PyGILState_SetThisThreadState(state);
#endif
}

#if PY_VERSION_HEX >= 0x030D0000
// Python 3.13+: GIL is per-interpreter, accessed via tstate->interp->ceval.gil
DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    PyThreadState* tstate = _PyThreadState_GET();
    if (!tstate || !tstate->interp || !tstate->interp->ceval.gil) {
        return false;
    }
    return (bool)(tstate->interp->ceval.gil->locked);
}

DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    PyThreadState* tstate = _PyThreadState_GET();
    if (!tstate || !tstate->interp || !tstate->interp->ceval.gil) {
        return nullptr;
    }
    return tstate->interp->ceval.gil->last_holder;
}

DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* gil_state) {
    PyThreadState* tstate = _PyThreadState_GET();
    if (!tstate || !tstate->interp || !tstate->interp->ceval.gil) {
        return nullptr;
    }
    PyThreadState* old = tstate->interp->ceval.gil->last_holder;
    if (old != gil_state) {
        tstate->interp->ceval.gil->last_holder = gil_state;
    }
    return old;
}
#else
// Python < 3.13: GIL is global, accessed via _PyRuntime.ceval.gil
DLLLOCAL static inline bool _qore_PyCeval_GetGilLockedStatus() {
    return (bool)(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.locked));
}

DLLLOCAL static inline PyThreadState* _qore_PyCeval_GetThreadState() {
    return reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.last_holder));
}

DLLLOCAL static inline PyThreadState* _qore_PyCeval_SwapThreadState(PyThreadState* gil_state) {
    PyThreadState* old = reinterpret_cast<PyThreadState*>(_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil.last_holder));
    if (old != gil_state) {
        _Py_atomic_store_relaxed(&_PyRuntime.ceval.gil.last_holder, (uintptr_t)gil_state);
    }
    return old;
}
#endif // PY_VERSION_HEX >= 0x030D0000

DLLLOCAL static inline bool _qore_has_thread_state_attached() {
    return PyGILState_Check();
}

DLLLOCAL static inline void _qore_acquire_thread_state(PyThreadState* tstate) {
    PyEval_AcquireThread(tstate);
    _qore_gil_held = true;  // Track that we now hold the GIL
}

DLLLOCAL static inline void _qore_release_thread_state(PyThreadState* tstate) {
    PyEval_ReleaseThread(tstate);
    _qore_gil_held = false;  // Track that we no longer hold the GIL
}

// Check if this might be a Python-created thread that already has the GIL.
// When using Python internal includes, the GIL detection through
// _qore_PyCeval_GetGilLockedStatus() works correctly even for Python-created
// threads, so this function just returns nullptr.
DLLLOCAL static inline PyThreadState* _qore_check_python_created_thread_gil() {
    return nullptr;
}

#else
#if PY_MAJOR_VERSION >= 3
#if PY_MINOR_VERSION >= 14
// Python 3.14+ (GIL or free-threading)
#include "python314_internals.h"
#elif PY_MINOR_VERSION >= 13
#include "python313_internals.h"
#elif PY_MINOR_VERSION == 12
#include "python312_internals.h"
#elif PY_MINOR_VERSION == 11
#include "python311_internals.h"
#else
#error unsupported python version: the minimum Python version is 3.11
#endif
#endif
#endif

// Safe thread state access: avoids PyThreadState_Get() when the GIL is not held
DLLLOCAL static inline PyThreadState* _qore_safe_thread_state_get() {
#ifdef Py_GIL_DISABLED
    return PyThreadState_Get();
#else
    PyThreadState* tss = PyGILState_GetThisThreadState();
    if (tss) {
        return tss;
    }
    return _qore_PyCeval_GetThreadState();
#endif
}

// CPython 3.13+ recursion-limit compatibility for internal-header builds.
#if PY_VERSION_HEX >= 0x030D0000

// Recursion limit APIs - removed in Python 3.13+
// Only define when using Python internal includes (not bundled internals)
// Bundled internals files define their own versions
#ifdef HAVE_PYTHON_INTERNAL_INCLUDES
#ifndef Py_GIL_DISABLED
inline int PyThreadState_GetRecursionLimit(PyThreadState* state) {
    (void)state;  // unused in Python 3.13+
    return Py_GetRecursionLimit();
}

inline void PyThreadState_UpdateRecursionLimit(PyThreadState* state, int new_limit) {
    (void)state;  // unused in Python 3.13+
    Py_SetRecursionLimit(new_limit);
}
#endif // !Py_GIL_DISABLED
#endif // HAVE_PYTHON_INTERNAL_INCLUDES

#endif // PY_VERSION_HEX >= 0x030D0000

// whether the state is bound to its thread for the GIL state API (Python 3.12 and later; checked by CMake, see
// QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE)
#ifdef QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE
DLLLOCAL static inline int qore_py_get_bound_gilstate(PyThreadState* tstate) {
    return tstate->_status.bound_gilstate;
}

DLLLOCAL static inline void qore_py_set_bound_gilstate(PyThreadState* tstate, int value) {
    tstate->_status.bound_gilstate = value;
}
#else
DLLLOCAL static inline int qore_py_get_bound_gilstate(PyThreadState* tstate) {
    (void)tstate;
    return 0;
}

DLLLOCAL static inline void qore_py_set_bound_gilstate(PyThreadState* tstate, int value) {
    (void)tstate;
    (void)value;
}
#endif

// Python 3.14+ specific compatibility APIs
#if PY_VERSION_HEX >= 0x030E0000 && !defined(QORE_PY_GILSTATE_UNSAFE_DEFINED)
#ifndef Py_GIL_DISABLED
// _PyGILState_GetInterpreterStateUnsafe was removed in Python 3.14
// Use PyInterpreterState_Main() as a replacement when there's no thread state
inline PyInterpreterState* _PyGILState_GetInterpreterStateUnsafe() {
    return PyInterpreterState_Main();
}
#endif // !Py_GIL_DISABLED
#endif // PY_VERSION_HEX >= 0x030E0000 && !QORE_PY_GILSTATE_UNSAFE_DEFINED

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
#ifndef Py_GIL_DISABLED
//! Releases the GIL; the thread's own idle state in the main interpreter is bound to the thread meanwhile
DLLLOCAL void qore_python_release_context();

//! deletes the idle thread state of the thread (see qore_python_release_context()); called when the thread ends,
//! without the GIL
DLLLOCAL void qore_python_delete_idle_thread_state();
#endif

class QorePythonGilHelper {
public:
    DLLLOCAL QorePythonGilHelper(PyThreadState* new_thread_state = mainThreadState);

    DLLLOCAL ~QorePythonGilHelper();

    DLLLOCAL void set(PyThreadState* other_state);

    //! Returns true if the GIL was successfully acquired
    /** This returns false if Python is shutting down or not initialized when the
        helper was constructed. Callers should check this before making Python API calls.
    */
    DLLLOCAL bool isInitialized() const {
        return initialized;
    }

#ifdef Py_GIL_DISABLED
    //! Detaches before Py_NewInterpreterFromConfig attaches the new interpreter.
    /** After successful creation, call set() with the new interpreter's thread state.
        The helper restores the original attachment when it leaves scope.
    */
    DLLLOCAL void releaseBeforeSubInterpreter();
#endif

protected:
    PyThreadState* new_thread_state;
    PyThreadState* state;
    PyThreadState* t_state;
    bool release_gil = true;
    bool initialized = false;  // True if successfully initialized (Python is running)
};

class QorePythonReleaseGilHelper {
public:
    DLLLOCAL QorePythonReleaseGilHelper() {
        _save = PyEval_SaveThread();
        // Update our tracking to indicate we don't have the GIL anymore
        // This is critical for multi-threaded scenarios where another thread might check
        // _qore_PyCeval_GetGilLockedStatus() to see if it needs to acquire the GIL.
        _qore_PyGILState_SetThisThreadState(nullptr);
        _qore_gil_held = false;  // Track that we no longer hold the GIL
        printd(5, "QorePythonReleaseGilHelper: released GIL, saved tstate: %p\n", _save);
    }

    DLLLOCAL ~QorePythonReleaseGilHelper() {
        // the thread returns to Python code, which cannot run in a child of fork() whose GIL is lost
        if (python_fork_lost) {
            qore_python_fork_lost_exit();
        }
        printd(5, "~QorePythonReleaseGilHelper: acquiring GIL with saved tstate: %p\n", _save);
        // CRITICAL: Check if the GIL was already reacquired (with a different thread state)
        // by nested code during the Qore method execution. If so, we just need to swap to
        // our saved thread state instead of trying to acquire the GIL again, which would
        // deadlock because the same thread is trying to acquire the GIL twice.
        if (_qore_PyCeval_GetGilLockedStatus()) {
            // GIL is already held (by nested code) - just swap to our thread state
            printd(5, "~QorePythonReleaseGilHelper: GIL already held, swapping to saved tstate\n");
            PyThreadState_Swap(_save);
        } else {
            // GIL is not held - acquire it
            PyEval_RestoreThread(_save);
            _qore_gil_held = true;  // Track that we now hold the GIL
        }
        // Restore our tracking now that we have the GIL with our saved thread state
        _qore_PyGILState_SetThisThreadState(_save);
        printd(5, "~QorePythonReleaseGilHelper: GIL acquired\n");
    }

private:
    PyThreadState* _save;
};

struct QorePythonThreadInfo {
    PyThreadState* tss_state;
    PyThreadState* t_state;
    PyThreadState* ceval_state;
    PyThreadState* released_other_interp_tstate;  // Thread state from another interpreter we released
    PyGILState_STATE g_state;
    bool valid;
    //! the levels by which setContext() reduced the C recursion counter of the thread state (Python 3.11 - 3.13)
    int c_recursion_reduction = 0;
};

//! acquires the GIL and manages thread state
class QorePythonHelper {
public:
    //! Creates a helper for acquiring the Python GIL
    /** @param pypgm the Python program to acquire the GIL for
        @param xsink if not null, an exception will be raised if the context acquisition was interrupted
    */
    DLLLOCAL QorePythonHelper(const QorePythonProgram* pypgm, ExceptionSink* xsink = nullptr);

    DLLLOCAL ~QorePythonHelper();

    DLLLOCAL QorePythonProgram* getOldProgram() const {
        return old_pgm ? reinterpret_cast<QorePythonProgram*>(old_pgm) : const_cast<QorePythonProgram*>(new_pypgm);
    }

    //! Returns true if the context was successfully acquired (not interrupted)
    DLLLOCAL bool isValid() const;

    //! Returns true if context acquisition was interrupted
    DLLLOCAL bool wasInterrupted() const;

    //! Returns true if the helper is counted as a use of its program (see QorePythonProgram::getAcquireThreadState())
    DLLLOCAL bool isCounted() const {
        return old_state.valid;
    }

    //! Returns the program whose context the helper holds
    DLLLOCAL const QorePythonProgram* getProgram() const {
        return new_pypgm;
    }

    //! Returns the helper that the current thread created before this one and that still exists, if any
    DLLLOCAL const QorePythonHelper* getPrevious() const {
        return prev;
    }

    //! Returns the last helper that the current thread created and that still exists, if any
    /** The helpers of a thread form a list, so that the uses of each program by the thread can be found in the child
        of a fork() (see QorePythonProgram::forkChildThreadStates())
    */
    DLLLOCAL static const QorePythonHelper* getCurrent() {
        return current;
    }

protected:
    void* old_pgm;
    QorePythonThreadInfo old_state;
    const QorePythonProgram* new_pypgm;
    //! the helper created before this one in this thread
    const QorePythonHelper* prev;

    //! the state of the interrupt of the Python code that the thread runs, shared with the cancel callback
    struct InterruptState {
        //! held by the cancel callback while it runs; the helper waits for it before it is destroyed
        std::mutex lck;
        //! true while the thread can run Python code of the helper (set and cleared with the GIL held)
        std::atomic<bool> active{true};
        //! true if the callback raised the interrupt in the thread
        std::atomic<bool> raised{false};
        //! the Python thread ID of the thread
        unsigned long thread_id;
        //! the program whose interpreter the thread runs in
        const QorePythonProgram* pypgm;
    };
    //! set if the Program has a sandbox manager, so that SandboxManager::requestInterrupt() interrupts the Python
    //! code (see registerInterrupt())
    std::shared_ptr<InterruptState> interrupt;
    //! the sandbox manager that the cancel callback is registered with
    std::unique_ptr<QoreSandboxManagerHelper> interrupt_smh;

    //! registers a cancel callback with the sandbox manager of the current Program, if any, which raises
    //! KeyboardInterrupt in the thread (PyThreadState_SetAsyncExc()); only for the first helper of the program in the
    //! thread
    DLLLOCAL void registerInterrupt();

    //! clears an undelivered interrupt (with the GIL held) before the context is released
    DLLLOCAL void deactivateInterrupt();

    //! waits for a running cancel callback and removes it (without the GIL)
    DLLLOCAL void deregisterInterrupt();

    DLLLOCAL static thread_local const QorePythonHelper* current;
};

//! Registers a thread that Python created with Qore, for the life of its Python thread state
/** Python can call into Qore on any thread, including a thread that Python created (\c threading.Thread, an
    executor pool, a native thread using \c PyGILState_Ensure()), which has no Qore thread data; the Qore API, starting
    with the lookup of the Python program context in QorePythonProgram::getContext(), requires it.  Such a thread is
    registered with Qore as a foreign thread the first time it calls Qore, and stays registered while its Python thread
    state exists, as a Qore thread does while it runs: the Qore objects that Python code creates on it are kept in its
    thread data until it ends, and Qore resources that it acquires (ex: a lock) stay its own between calls.  It is
    deregistered when its Python thread state is cleared (normally when the thread ends), or when the native thread
    exits.

    Must be called with the GIL held and a current Python thread state.

    @return 0 if Qore code can run on the current thread, -1 if the thread could not be registered
*/
DLLLOCAL int qore_python_register_thread();

//! raises a Python TypeError if keyword arguments are given in a call of Qore code, which takes positional arguments
/** @param kwargs the keyword arguments of the call (a dict or nullptr)
    @param name the Qore code called, for the error message (ex: \c "Qore::Thread::Mutex()")

    @return -1 if keyword arguments were given and the Python exception was raised, 0 if not
*/
DLLLOCAL int qore_python_check_no_kwargs(PyObject* kwargs, const char* name);

//! Registers the fork handlers of the program registry locks (qoreloadermodule.cpp); returns -1 if there is no memory
DLLLOCAL int qpy_register_fork_handlers();

//! Removes the fork handlers registered with qpy_register_fork_handlers()
DLLLOCAL void qpy_unregister_fork_handlers();

//! Serializes the cache of the Python types of Qore subclasses of Python classes (a leaf lock; see
//! QorePythonProgram::getQoreSubclassType())
DLLLOCAL extern QoreThreadLock qpy_subclass_type_lock;

//! Serializes the map of the Python objects of Qore objects (a leaf lock; see PythonQoreClass::findProxy())
DLLLOCAL extern QoreThreadLock qpy_proxy_lock;

//! Initializes the type used to deregister threads that Python created; called once in the main interpreter
DLLLOCAL int qore_python_thread_registration_init();

//! Replaces os.putenv() and os.unsetenv() in the current interpreter with functions that change the environment
//! under the lock that serializes the environment with Qore
/** Called with the GIL held when the qoreloader module is initialized in an interpreter

    @return 0 for OK, -1 if a Python exception was raised
*/
DLLLOCAL int qore_python_install_environment_functions();

//! Enters Qore from a function or slot that Python calls; see qore_python_register_thread()
/** Create it before any other Qore API is used in the call, with the GIL held.
*/
class QorePythonQoreEntryHelper {
public:
    DLLLOCAL QorePythonQoreEntryHelper() : valid(!qore_python_register_thread()) {
        if (valid) {
            applySandbox();
        }
    }

    //! Returns true if Qore code can run on the current thread
    DLLLOCAL bool ok() const {
        return valid;
    }

    //! Returns true if Qore code can run on the current thread; otherwise raises a Python exception
    DLLLOCAL bool check() const {
        if (!valid) {
            PyErr_SetString(PyExc_RuntimeError, "the current thread cannot be registered with Qore");
            return false;
        }
        return true;
    }

private:
    bool valid;
    //! the sandbox applied, kept while it is applied
    std::shared_ptr<QoreSandboxCapture> sandbox;
    //! applies @ref sandbox; destroyed before it
    std::optional<QoreSandboxCaptureHelper> sandbox_helper;

    //! On a thread that Python created, applies the sandbox of the interpreter running on it
    /** Such a thread has no Qore caller whose sandbox could govern the Qore code that it calls: a thread that Python
        starts from Qore code does not inherit a sandbox.  The sandbox of the Program that owns the interpreter, as it
        is when the call is made, or else the sandbox of the code that created the interpreter, is captured for the
        call and applies as its outermost caller, as the sandbox of the starting code does on a thread that Qore code
        starts.  On a Qore thread, the sandbox of its own callers governs the call.
    */
    DLLLOCAL void applySandbox();

    QorePythonQoreEntryHelper(const QorePythonQoreEntryHelper&) = delete;
    QorePythonQoreEntryHelper& operator=(const QorePythonQoreEntryHelper&) = delete;
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
        purge();
    }

    DLLLOCAL void purge() {
        if (!obj) {
            return;
        }
        if (!python_shutdown) {
#if PY_VERSION_HEX >= 0x030D0000
            // Python 3.13+: Always use PyGILState_Ensure/Release for cleanup.
            // Our tracking (_qore_tss_tstate) may be out of sync with Python's actual
            // GIL state if external code (like JNI) releases the GIL without going
            // through our API. PyGILState_Ensure is safe to call even if we already
            // have the GIL - it just increments the gilstate_counter.
            if (Py_IsInitialized()) {
                // CRITICAL: Before calling PyGILState_Ensure, check if the TSS thread state
                // (if any) has a valid interpreter. If the interpreter was deleted (sub-interpreter
                // cleanup), PyGILState_Ensure will crash with "_PyMem_IsPtrFreed(tstate->interp)".
                PyThreadState* tss_tstate = PyGILState_GetThisThreadState();
                if (tss_tstate) {
                    // Check if the interpreter is still in the global list
                    PyInterpreterState* interp = tss_tstate->interp;
                    bool interp_valid = false;
                    PyInterpreterState* check_interp = PyInterpreterState_Head();
                    while (check_interp) {
                        if (check_interp == interp) {
                            interp_valid = true;
                            break;
                        }
                        check_interp = PyInterpreterState_Next(check_interp);
                    }
                    if (!interp_valid) {
                        // The TSS points to a thread state with a freed interpreter.
                        // The Python object was part of a deleted sub-interpreter and is
                        // already freed along with it - just clear our reference.
                        obj = nullptr;
                        return;
                    }
                }
                PyGILState_STATE gstate = PyGILState_Ensure();
                py_deref();
                PyGILState_Release(gstate);
            }
#else
            if (Py_IsInitialized() && !_qore_has_gil(_qore_PyRuntimeGILState_GetThreadState())) {
                PyGILState_STATE gstate = PyGILState_Ensure();
                py_deref();
                PyGILState_Release(gstate);
            } else {
                py_deref();
            }
#endif
        }
        obj = nullptr;
    }

    DLLLOCAL QorePythonReferenceHolder& operator=(PyObject* obj) {
        if (this->obj) {
            purge();
        }
        this->obj = obj;
        return *this;
    }
};

//! A Python reference owned by a Qore object that may outlive its interpreter.
class QorePythonPersistentReferenceHolder : public QorePythonManualReferenceHolder {
public:
    DLLLOCAL QorePythonPersistentReferenceHolder(QorePythonProgram* owner, PyObject* obj);
    DLLLOCAL ~QorePythonPersistentReferenceHolder();

    DLLLOCAL QorePythonProgram* getOwner() const {
        return owner;
    }

private:
    QorePythonProgram* owner;
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

//! returns true if releasing a reference to the value can run Qore code (a destructor)
/** This is the case for an object, a closure or a call reference (whose captured values can be objects) whose last
    reference this is, or a list or hash whose last reference this is that contains such a value; strings, numbers,
    dates and binaries, and values that have other references, are released without running Qore code.
*/
DLLLOCAL bool qore_python_release_runs_qore_code(const QoreValue& v);

//! releases the value held while the GIL is held: without the GIL if this can run Qore code, which runs without the
//! GIL so that it cannot deadlock with a thread that holds a Qore lock and waits for the GIL
DLLLOCAL void qore_python_release(ValueHolder& v);

//! releases the list held while the GIL is held: without the GIL if this can run Qore code
DLLLOCAL void qore_python_release(ReferenceHolder<QoreListNode>& l);

// Base type for Qore objects in Python
DLLLOCAL extern PyTypeObject PythonQoreObjectBase_Type;

// Python program control for Qore interfacing
DLLLOCAL extern QorePythonProgram* qore_python_pgm;

//! Python module definition function for the qoreloader module
DLLLOCAL PyMODINIT_FUNC PyInit_qoreloader();

//! the name of the capsule that holds the ExceptionInfo hash of a Qore exception raised in Python
#define QORE_PYTHON_EXCEPTION_CAPSULE_NAME "qoreloader.QoreException._qore_exception"

//! the attribute of a qoreloader.QoreException object raised for a Qore exception that holds its capsule
#define QORE_PYTHON_EXCEPTION_CAPSULE_ATTR "_qore_exception"

//! returns a new reference to the qoreloader.QoreException type of the current interpreter
/** Each interpreter has its own type, created when the qoreloader module is executed in it; the module is imported if
    necessary.  Returns nullptr with a Python exception set on error.
*/
DLLLOCAL PyObject* qore_python_get_qore_exception_type();

//! returns a new reference to the qoreloader.QoreStaticMember type of the current interpreter
/** Returns nullptr with a Python exception set on error.
*/
DLLLOCAL PyObject* qore_python_get_qore_static_member_type();

//! Creates the global QOre Python program object
DLLLOCAL int init_global_qore_python_pgm();

//! Returns true if the current thread is holding the GIL
// NOTE: For Python 3.12, the default uses our tracking instead of Python's TSS
// because PyGILState_GetThisThreadState() can be stale after PyEval_ReleaseThread().
DLLLOCAL bool _qore_has_gil(PyThreadState* t_state = _qore_PyRuntimeGILState_GetThreadState());

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

//! Track ALL QorePythonProgram pointers for validity checking in destructors
DLLLOCAL void qpy_global_register(QorePythonProgram* p);
DLLLOCAL void qpy_global_deregister(QorePythonProgram* p);
DLLLOCAL bool qpy_is_valid(QorePythonProgram* p);
//! Returns the count of destroyed interpreters (for detecting unsafe destructor calls)
DLLLOCAL int qpy_get_destroyed_count();
//! Increments the destroyed interpreter count (call when an interpreter is actually destroyed)
DLLLOCAL void qpy_interpreter_destroyed();

//! if true then this module was initialized by Python
DLLLOCAL extern bool qore_needs_shutdown;

#if 0
DLLLOCAL int q_reset_python(ExceptionSink* xsink);
#endif

#endif
