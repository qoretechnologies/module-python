/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file QorePythonProgram.cpp defines the QorePythonProgram class */
/*
    QorePythonProgram.qpp

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

#include "QC_PythonProgram.h"

#include <qore/QoreForkHandler.h>
#include <qore/QoreIteratorBase.h>

#include <atomic>
#include <pthread.h>

#include "QorePythonPrivateData.h"
#include "QorePythonClass.h"
#include "QoreLoader.h"
#include "PythonQoreClass.h"
#include "QoreMetaPathFinder.h"
#include "QorePythonSignalWakeup.h"

#include "PythonCallableCallReferenceNode.h"
#include "PythonQoreCallable.h"
#include "ModuleNamespace.h"
#include "QorePythonStackLocationHelper.h"

#include <structmember.h>
#include <frameobject.h>
#include <datetime.h>

#include <algorithm>
#include <climits>
#include <vector>
#include <string>

// type used for imported Qore namespaces in Python; a plan type with only a dictionary
struct PythonQoreNs {
    PyObject_HEAD
    PyObject* dict;
};

static strvec_t get_dot_path_list(const std::string str) {
    strvec_t rv;
    size_t start = 0;
    while (true) {
        size_t pos = str.find('.', start);
        rv.push_back(str.substr(start, pos - start));
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return rv;
}

#if defined(DEBUG) && !defined(Py_GIL_DISABLED)
// from Python internal code
static bool _qore_PyThreadState_IsCurrent(PyThreadState* tstate) {
    // Must be the tstate for this thread
    return tstate == _qore_PyRuntimeGILState_GetThreadState();
}
#endif

// static member declarations
QorePythonProgram::py_thr_map_t QorePythonProgram::py_thr_map;
QorePythonProgram::py_global_tid_map_t QorePythonProgram::py_global_tid_map;
QoreThreadLock QorePythonProgram::py_thr_lck;
QoreThreadLock QorePythonProgram::main_ts_lck;

// fork handlers of the process-wide locks of this class (see QoreForkHandler.h)
//
// py_thr_lck protects the maps of thread states and the persistent references; its holders only change containers
// (Python reference operations are made after it is released) and never wait for the GIL while they hold it, so it
// is a leaf (QFR_MODULE_LEAF) acquired before fork(), and the maps are consistent in the child.
//
// main_ts_lck serializes the use of the main thread state while an interpreter is created; its holders wait for the
// GIL, which the thread calling os.fork() holds while the fork handlers run, so fork() must not wait for it
// (QFR_MODULE_RESET): the child initializes it again, and takes it again if the forking thread held it.
static QoreForkHandlerRegistration* py_thr_lck_fork_reg = nullptr;
static QoreForkHandlerRegistration* main_ts_lck_fork_reg = nullptr;
// the thread that holds main_ts_lck, valid while main_ts_lck_held is set; updated while the lock is held
static std::atomic<pthread_t> main_ts_lck_holder;
static std::atomic<bool> main_ts_lck_held{false};

//! holds main_ts_lck and records the holder for its fork handler
class MainThreadStateLockHelper {
public:
    DLLLOCAL MainThreadStateLockHelper(QoreThreadLock& lck) : lck(lck) {
        lck.lock();
        main_ts_lck_holder.store(pthread_self(), std::memory_order_relaxed);
        main_ts_lck_held.store(true, std::memory_order_relaxed);
    }

    DLLLOCAL ~MainThreadStateLockHelper() {
        main_ts_lck_held.store(false, std::memory_order_relaxed);
        lck.unlock();
    }

private:
    QoreThreadLock& lck;

    MainThreadStateLockHelper(const MainThreadStateLockHelper&) = delete;
    MainThreadStateLockHelper& operator=(const MainThreadStateLockHelper&) = delete;
};

unsigned QorePythonProgram::fork_generation = 0;
int QorePythonProgram::fork_tid = 0;
bool QorePythonProgram::fork_purge_pending = false;

void QorePythonProgram::forkChildThreadStates(void* arg) {
    qore_fork_child_thread_lock(static_cast<QoreThreadLock*>(arg));
    ++fork_generation;
    fork_tid = q_gettid();
    fork_purge_pending = true;
    // the threads of the parent that used an interpreter do not exist: only the uses of the forking thread remain
    for (auto& i : py_thr_map) {
        i.first->pgm_thr_cnt = 0;
        i.first->pgm_thr_waiting = 0;
        i.first->pgm_thr_uses.clear();
        i.first->pgm_cancelled_tids.clear();
    }
    for (const QorePythonHelper* helper = QorePythonHelper::getCurrent(); helper; helper = helper->getPrevious()) {
        if (helper->isCounted()) {
            ++helper->getProgram()->pgm_thr_cnt;
            ++helper->getProgram()->pgm_thr_uses[fork_tid];
        }
    }
}

void QorePythonProgram::purgeForkedThreadStatesIntern() {
    assert(fork_purge_pending);
    fork_purge_pending = false;
    for (auto& i : py_thr_map) {
        for (auto ti = i.second.begin(); ti != i.second.end();) {
            if (ti->first == fork_tid) {
                ++ti;
            } else {
                ti = i.second.erase(ti);
            }
        }
    }
    for (auto gi = py_global_tid_map.begin(); gi != py_global_tid_map.end();) {
        if (gi->first == fork_tid) {
            ++gi;
        } else {
            gi = py_global_tid_map.erase(gi);
        }
    }
}

bool QorePythonProgram::hasOtherThreadStates(PyThreadState* finalizing) {
    for (PyThreadState* state = PyInterpreterState_ThreadHead(finalizing->interp); state;
            state = PyThreadState_Next(state)) {
        if (state != finalizing) {
            return true;
        }
    }
    return false;
}

static void main_ts_lck_fork_child(void* arg) {
    QoreThreadLock* lck = static_cast<QoreThreadLock*>(arg);
    bool held = main_ts_lck_held.load(std::memory_order_relaxed)
        && pthread_equal(main_ts_lck_holder.load(std::memory_order_relaxed), pthread_self());
    qore_fork_child_thread_lock(lck);
    if (held) {
        lck->lock();
    } else {
        main_ts_lck_held.store(false, std::memory_order_relaxed);
    }
}

int QorePythonProgram::registerForkHandlers() {
    assert(!py_thr_lck_fork_reg && !main_ts_lck_fork_reg);
    py_thr_lck_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_thread_states",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, forkChildThreadStates, &py_thr_lck);
    main_ts_lck_fork_reg = qore_register_fork_handler(QFR_MODULE_RESET, "python_main_thread_state", nullptr,
        nullptr, main_ts_lck_fork_child, &main_ts_lck);
    if (!py_thr_lck_fork_reg || !main_ts_lck_fork_reg) {
        unregisterForkHandlers();
        return -1;
    }
    return 0;
}

void QorePythonProgram::unregisterForkHandlers() {
    qore_unregister_fork_handler(main_ts_lck_fork_reg);
    main_ts_lck_fork_reg = nullptr;
    qore_unregister_fork_handler(py_thr_lck_fork_reg);
    py_thr_lck_fork_reg = nullptr;
}
unsigned QorePythonProgram::pgm_count = 0;

QorePythonProgram::QorePythonProgram() : save_object_callback(nullptr) {
    printd(5, "QorePythonProgram::QorePythonProgram() this: %p\n", this);
    qpy_global_register(this);
#ifdef Py_GIL_DISABLED
    PyThreadState* python = PyThreadState_GetUnchecked();
    assert(python);
    interpreter = python->interp;
#elif PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+: PyGILState_Check() and PyGILState_GetThisThreadState() check Python's
    // Thread Specific Storage (TSS/autoTSSkey). When external modules like JNI are loaded,
    // they can clear or corrupt Python's TSS without affecting actual GIL ownership.
    // Use our own tracking exclusively - it's maintained independently and remains reliable.
    assert(_qore_PyCeval_GetGilLockedStatus());
    PyThreadState* python = _qore_PyRuntimeGILState_GetThreadState();
    assert(python);
    interpreter = python->interp;
#else
    assert(PyGILState_Check());
    bool have_gil = PyGILState_Check();
    PyThreadState* python;
    if (have_gil) {
        assert(_qore_PyRuntimeGILState_GetThreadState() == PyGILState_GetThisThreadState());
        python = PyGILState_GetThisThreadState();
        interpreter = python->interp;
    } else {
        python = nullptr;
        interpreter = _PyGILState_GetInterpreterStateUnsafe();
    }
#endif
    owns_interpreter = false;

    createQoreProgram();

    // insert thread state into thread map
    AutoLocker al(py_thr_lck);
    checkForkIntern();
    assert(py_thr_map.find(this) == py_thr_map.end());
    int tid = q_gettid();
    py_thr_map[this] = {{tid, {python, false}}};
    py_global_tid_map[tid].insert(python);
    ++pgm_count;
    needs_deregistration = qpy_register(this);
}

QorePythonProgram::QorePythonProgram(QoreProgram* qpgm, QoreNamespace* pyns, ExceptionSink* xsink)
        : qpgm(qpgm), pyns(pyns), save_object_callback(nullptr) {
    // the code of the interpreter on threads that Python creates is governed by the sandbox of the code that creates
    // the interpreter, which runs in the given Program
    sandbox = std::make_shared<QoreSandboxCapture>();
    qpy_global_register(this);

    // Two-phase locking to avoid deadlocks:
    // Phase 1: Use main_ts_lck to serialize mainThreadState access during GIL acquisition
    // Phase 2: Use py_thr_lck for thread map updates (after we have our own thread state)
    // This avoids deadlock because:
    // - Constructor: main_ts_lck -> GIL -> py_thr_lck (acquires in this order)
    // - setContext/releaseContext: py_thr_lck (brief) -> GIL (no conflict with main_ts_lck)
    MainThreadStateLockHelper mts_al(main_ts_lck);

    QorePythonGilHelper qpgh;

    ExceptionSink local_xsink;
    if (!xsink) {
        xsink = &local_xsink;
    }
    if (createInterpreter(qpgh, xsink)) {
        valid = false;
        return;
    }

    // ensure that the __main__ module is created
    // returns a borrowed reference
    module = PyImport_AddModule("__main__");
    module.py_ref();

    // the builtin functions and classes are imported (ex: Python::builtins::len()); its constants are not: they are
    // Python objects (ex: exit and quit, which shadowed Qore's exit() function, and the other helpers of the site
    // module) or values with Qore equivalents (True, False, None)
    import(xsink, "builtins", nullptr, IF_ALL | IF_NO_CONSTANTS);
    if (*xsink) {
        valid = false;
        return;
    }

    // returns a borrowed reference
    setGlobalDictionary(*module);
    assert(!PyErr_Occurred());

    // import qoreloader module
    QorePythonReferenceHolder qoreloader(PyImport_ImportModule("qoreloader"));
    if (!qoreloader) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-COMPILE-ERROR", "cannot load the 'qoreloader' module");
        }
        valid = false;
        return;
    }

    PyDict_SetItemString(module_dict, "qoreloader", *qoreloader);
    needs_deregistration = qpy_register(this);
}

QorePythonProgram::QorePythonProgram(const QoreString& source_code, const QoreString& source_label, int start,
        ExceptionSink* xsink) : save_object_callback(nullptr) {
    // the code of the interpreter on threads that Python creates is governed by the sandbox of the code that creates
    // the PythonProgram object
    sandbox = std::make_shared<QoreSandboxCapture>();
    printd(5, "QorePythonProgram::QorePythonProgram() this: %p\n", this);
    qpy_global_register(this);
    TempEncodingHelper src_code(source_code, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"source_code\" argument)");
        return;
    }
    TempEncodingHelper src_label(source_label, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"source_label\" argument)");
        return;
    }

    //printd(5, "QorePythonProgram::QorePythonProgram() GIL thread state: %p\n", PyGILState_GetThisThreadState());

    // Two-phase locking to avoid deadlocks:
    // Phase 1: Use main_ts_lck to serialize mainThreadState access during GIL acquisition
    // Phase 2: Use py_thr_lck for thread map updates (inside createInterpreter, after we have our own thread state)
    // This avoids deadlock because:
    // - Constructor: main_ts_lck -> GIL -> py_thr_lck (acquires in this order)
    // - setContext/releaseContext: py_thr_lck (brief) -> GIL (no conflict with main_ts_lck)
    MainThreadStateLockHelper mts_al(main_ts_lck);

    QorePythonGilHelper qpgh;

    //printd(5, "QorePythonProgram::QorePythonProgram() GIL thread state: %p\n", PyGILState_GetThisThreadState());
    if (createInterpreter(qpgh, xsink)) {
        return;
    }

    // use the parent Program object as the source for importing
    qpgm = getProgram();
    owns_qore_program_ref = false;
    pyns = qpgm->findNamespace(QORE_PYTHON_NS_NAME);
    assert(pyns);
    // Imports executed by the source belong to this interpreter. Establish its
    // execution context before Python calls back into the Qore module loader.
    QorePythonHelper qph(this, xsink);
    if (*xsink) {
        return;
    }

    // import qoreloader module
    QorePythonReferenceHolder qoreloader(PyImport_ImportModule("qoreloader"));
    if (!qoreloader) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-COMPILE-ERROR", "cannot load the 'qoreloader' module");
        }
        return;
    }
    //printd(5, "QorePythonProgram::QorePythonProgram() loaded qoreloader: %p\n", *qoreloader);

    // parse and compile code
    python_code = (PyObject*)Py_CompileString(src_code->c_str(), src_label->c_str(), start);
    if (!python_code) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-COMPILE-ERROR", "parsing and compilation failed");
        }
        return;
    }

    assert(!module);

    // create module for code
    QorePythonReferenceHolder new_module(PyImport_ExecCodeModule(src_label->c_str(), *python_code));
    if (!new_module) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-COMPILE-ERROR", "compile failed");
        }
        return;
    }

    module = new_module.release();

    // returns a borrowed reference
    setGlobalDictionary(*module);

    PyDict_SetItemString(module_dict, "qoreloader", *qoreloader);

    //printd(5, "QorePythonProgram::QorePythonProgram() this: %p pgm: %p rootns: %p\n", this, qpgm,
    //  qpgm->getRootNS());
    // create Qore program object with the same restrictions as the parent
    //createQoreProgram();
    needs_deregistration = qpy_register(this);
}

int QorePythonProgram::setGlobalDictionary(PyObject* mod) {
    module_dict = PyModule_GetDict(mod);
    assert(module_dict);
    // returns a borrowed reference
    builtin_dict = PyDict_GetItemString(module_dict, "__builtins__");
    assert(builtin_dict);
    return 0;
}

void QorePythonProgram::createQoreProgram() {
    // create Qore program object with the same restrictions as the parent
    QoreProgram* pgm = getProgram();
    QoreParseOptions parse_options = pgm ? pgm->getParseOptions() : QoreParseOptions();
    qpgm = new QoreProgram(parse_options);
    owns_qore_program_ref = true;
    pyns = PNS->copy();
    qpgm->getRootNS()->addNamespace(pyns);
    qpgm->setExternalData(QORE_PYTHON_MODULE_NAME, this);

    // Inherit sandbox manager from parent program
    if (pgm) {
        QoreSandboxManagerHelper smh(pgm);
        if (smh) {
            qpgm->setSandboxManager(smh.get());
        }
    }

    //printd(5, "QorePythonProgram::createQoreProgram() this: %p pgm: %p rootns: %p\n", this, qpgm,
    //  qpgm->getRootNS());
}

QorePythonProgram* QorePythonProgram::getExecutionContext() {
    return getContext();
}

QorePythonProgram* QorePythonProgram::getContext() {
    // A PythonProgram can share its parent QoreProgram while owning a different
    // Python interpreter. Its wrappers must be cached in the executing program.
    QorePythonProgram* pypgm = reinterpret_cast<QorePythonProgram*>(q_get_thread_local_data(python_u_tld_key));
#if PY_VERSION_HEX >= 0x030D0000
    PyThreadState* current = PyThreadState_GetUnchecked();
#else
    PyThreadState* current = PyGILState_Check() ? _PyThreadState_UncheckedGet() : nullptr;
    if (current && current->thread_id != PyThread_get_thread_ident()) {
        current = nullptr;
    }
#endif
    if (pypgm && pypgm->qpgm && (!current || pypgm->interpreter == current->interp)) {
        return pypgm;
    }
    if (current) {
        // Python-created threads have no Qore execution context yet. Locate the
        // owner of their attached interpreter instead of falling back to the main
        // interpreter. CPython owns the attached state for the duration of this call.
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        for (const auto& entry : py_thr_map) {
            if (entry.first->interpreter == current->interp && entry.first->qpgm) {
                return const_cast<QorePythonProgram*>(entry.first);
            }
        }
    }
    // first try to get the actual Program context
    QoreProgram* pgm = getProgram();
    if (pgm) {
        pypgm = static_cast<QorePythonProgram*>(pgm->getExternalData(QORE_PYTHON_MODULE_NAME));
        if (pypgm) {
            //printd(5, "QorePythonProgram::getContext() got local program context pgm: %p pypgm: %p\n", pgm, pypgm);
            return pypgm;
        }
    }
    pgm = qore_get_call_program_context();
    if (pgm) {
        pypgm = static_cast<QorePythonProgram*>(pgm->getExternalData(QORE_PYTHON_MODULE_NAME));
        if (pypgm) {
            //printd(5, "QorePythonProgram::getContext() got local call context pgm: %p pypgm: %p\n", pgm, pypgm);
            return pypgm;
        }
    }
    //printd(5, "QorePythonProgram::getContext() got global program context pypgm: %p\n", pypgm);
    return qore_python_pgm;
}

int QorePythonProgram::staticInit() {
    PyDateTime_IMPORT;
    assert(PyDateTimeAPI);
    return 0;
}

#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030E0000
bool QorePythonProgram::hasThreadsNotJoinedAtEnd(PyThreadState* finalizing) {
    // the identifiers of the threads with a state in the interpreter, collected without running Python code, which
    // could release the GIL to a thread that ends and deletes its state meanwhile
    std::vector<unsigned long> idents;
    try {
        for (PyThreadState* state = PyInterpreterState_ThreadHead(finalizing->interp); state;
                state = PyThreadState_Next(state)) {
            if (state != finalizing) {
                idents.push_back(state->thread_id);
            }
        }
    } catch (const std::bad_alloc&) {
        // the threads cannot be checked: the interpreter is left rather than risking the abort
        return true;
    }
    if (idents.empty()) {
        return false;
    }
    // Py_EndInterpreter() waits for the threads of the threading module that are not daemon threads
    // (threading._shutdown()); threading._active maps their identifiers to their Thread objects
    QorePythonReferenceHolder threading_name(PyUnicode_FromString("threading"));
    QorePythonReferenceHolder threading(threading_name ? PyImport_GetModule(*threading_name) : nullptr);
    QorePythonReferenceHolder active(threading ? PyObject_GetAttrString(*threading, "_active") : nullptr);
    bool rv = false;
    if (!active || !PyDict_Check(*active)) {
        rv = true;
    } else {
        for (unsigned long id : idents) {
            QorePythonReferenceHolder ident(PyLong_FromUnsignedLong(id));
            // returns a borrowed reference
            PyObject* thread = ident ? PyDict_GetItemWithError(*active, *ident) : nullptr;
            QorePythonReferenceHolder daemon(thread ? PyObject_GetAttrString(thread, "daemon") : nullptr);
            if (*daemon != Py_False) {
                rv = true;
                break;
            }
        }
    }
    if (PyErr_Occurred()) {
        PyErr_Clear();
    }
    return rv;
}
#endif

#if PY_VERSION_HEX < 0x030D0000
void QorePythonProgram::forgetEndedThreadingMainThread() {
    QorePythonReferenceHolder threading_name(PyUnicode_FromString("threading"));
    QorePythonReferenceHolder threading(threading_name ? PyImport_GetModule(*threading_name) : nullptr);
    QorePythonReferenceHolder main_thread(threading ? PyObject_GetAttrString(*threading, "_main_thread") : nullptr);
    QorePythonReferenceHolder lock(main_thread ? PyObject_GetAttrString(*main_thread, "_tstate_lock") : nullptr);
    QorePythonReferenceHolder ident(lock && *lock != Py_None ? PyObject_GetAttrString(*main_thread, "ident")
        : nullptr);
    if (ident && PyLong_Check(*ident) && PyLong_AsUnsignedLong(*ident) == PyThread_get_thread_ident()) {
        // the lock is released when the state of the thread is deleted
        QorePythonReferenceHolder locked(PyObject_CallMethod(*lock, "locked", nullptr));
        if (locked && *locked == Py_False) {
            QorePythonReferenceHolder rv(PyObject_CallMethod(*main_thread, "_stop", nullptr));
            if (rv) {
                PyObject_SetAttrString(*main_thread, "_ident", Py_None);
            }
        }
    }
    if (PyErr_Occurred()) {
        PyErr_Clear();
    }
}
#endif

#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030C0000
// os.fork(), os.forkpty() in a subinterpreter: CPython supports fork() only in the main interpreter, and the child of
// a fork() made in a subinterpreter dies
static PyObject* qore_python_subinterpreter_fork(PyObject* self, PyObject* args) {
    PyErr_SetString(PyExc_RuntimeError, "fork not supported for subinterpreters");
    return nullptr;
}

static PyMethodDef qore_python_subinterpreter_fork_def = {
    "fork", qore_python_subinterpreter_fork, METH_NOARGS,
    "Raises RuntimeError: fork() is not supported in a subinterpreter"
};

static PyMethodDef qore_python_subinterpreter_forkpty_def = {
    "forkpty", qore_python_subinterpreter_fork, METH_NOARGS,
    "Raises RuntimeError: fork() is not supported in a subinterpreter"
};

int QorePythonProgram::disallowFork() {
    for (const char* name : {"posix", "os"}) {
        QorePythonReferenceHolder mod(PyImport_ImportModule(name));
        if (!mod) {
            return -1;
        }
        for (PyMethodDef* def : {&qore_python_subinterpreter_fork_def, &qore_python_subinterpreter_forkpty_def}) {
            if (!PyObject_HasAttrString(*mod, def->ml_name)) {
                continue;
            }
            QorePythonReferenceHolder func(PyCFunction_New(def, nullptr));
            if (!func || PyObject_SetAttrString(*mod, def->ml_name, *func) < 0) {
                return -1;
            }
        }
    }
    return 0;
}
#endif

void QorePythonProgram::waitForThreadsIntern() {
    while (pgm_thr_cnt) {
        ++pgm_thr_waiting;
        pgm_thr_cond.wait(py_thr_lck);
        --pgm_thr_waiting;
    }
}

QorePythonPersistentReferenceHolder::QorePythonPersistentReferenceHolder(QorePythonProgram* owner, PyObject* obj)
        : owner(obj ? owner : nullptr) {
    QorePythonReferenceHolder reference(obj);
    if (this->owner) {
        owner->registerPersistentReference(this);
        owner->weakRef();
        this->obj = reference.release();
    }
}

QorePythonPersistentReferenceHolder::~QorePythonPersistentReferenceHolder() {
    reset();
}

void QorePythonPersistentReferenceHolder::reset() {
    if (owner) {
        QorePythonProgram* previous = owner;
        owner = nullptr;
        previous->releasePersistentReference(this);
        previous->weakDeref();
    }
}

void QorePythonProgram::registerPersistentReference(QorePythonManualReferenceHolder* reference) {
    AutoLocker al(py_thr_lck);
    checkForkIntern();
    assert(valid);
    assert(persistent_references.find(reference) == persistent_references.end());
    persistent_references.insert(reference);
}

void QorePythonProgram::releasePersistentReference(QorePythonManualReferenceHolder* reference) {
    QorePythonHelper qph(this);
    PyObject* object;
    {
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        persistent_references.erase(reference);
        object = reference->release();
    }
    // in a child of fork() that cannot use Python, the reference is left: the GIL cannot be acquired
    if (python_fork_lost) {
        return;
    }
    // Finalization clears every registered reference before invalidating this context.
    assert(!object || (valid && qph.isValid()));
    Py_XDECREF(object);
}

void QorePythonProgram::clearPersistentReferences() {
    // A Python finalizer can destroy another Qore holder. Remove each entry before
    // DECREF and fetch the next entry afresh so that reentrant removal is safe.
    while (true) {
        PyObject* object;
        {
            AutoLocker al(py_thr_lck);
            checkForkIntern();
            if (persistent_references.empty()) {
                break;
            }
            auto entry = persistent_references.begin();
            object = (*entry)->release();
            persistent_references.erase(entry);
        }
        Py_XDECREF(object);
    }
}

bool QorePythonProgram::isUsedByCurrentThread() const {
    for (const QorePythonHelper* h = QorePythonHelper::getCurrent(); h; h = h->getPrevious()) {
        if (h->getProgram() == this) {
            return true;
        }
    }
    return false;
}

void QorePythonProgram::checkPendingDelete() {
    if (!delete_pending || isUsedByCurrentThread()) {
        return;
    }
    {
        AutoLocker al(py_thr_lck);
        // only the thread that deferred the deletion finishes it
        if (deleting_tid != q_gettid()) {
            return;
        }
    }
    delete_pending = false;
    {
        // an exception raised by the deletion (ex: by a destructor) has no caller: it is handled by the default handler
        ExceptionSink xsink;
        deleteIntern(&xsink);
    }
    // the reference taken when the deletion was deferred; the last one deletes the object
    weakDeref();
}

void QorePythonProgram::cancelThreads() {
    std::vector<int> tids;
    {
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        // from now on, other threads cannot use the program (see getAcquireThreadState())
        int tid = q_gettid();
        deleting_tid = tid;
        for (auto& i : pgm_thr_uses) {
            if (i.first != tid) {
                tids.push_back(i.first);
            }
        }
    }
    for (int tid : tids) {
        // a thread whose cancellation was requested by other code keeps it
        if (qore_cancel_thread(tid, "the Python program was deleted") == 0) {
            AutoLocker al(py_thr_lck);
            pgm_cancelled_tids.insert(tid);
            pgm_threads_cancelled = true;
        }
    }
}

void QorePythonProgram::deleteIntern(ExceptionSink* xsink) {
    if (destroyed || delete_pending) {
        return;
    }
    // the current thread runs code of the program (ex: Qore code that the program's Python code called deletes the
    // program): the interpreter cannot be finalized while the thread uses it, and waiting for the uses to end would
    // wait for the thread itself; other threads cannot use the program from now on, and the deletion is finished
    // when the thread leaves the program (see checkPendingDelete()); the object is kept until then
    if (isUsedByCurrentThread() && !q_libqore_exiting()) {
        {
            AutoLocker al(py_thr_lck);
            checkForkIntern();
            deleting_tid = q_gettid();
        }
        weakRef();
        delete_pending = true;
        return;
    }
    destroyed = true;
    // Deregister from global validity tracking FIRST, before anything else
    qpy_global_deregister(this);
    if (needs_deregistration) {
        qpy_deregister(this);
    }

    printd(5, "QorePythonProgram::deleteIntern() this: %p i: %p oi: %d\n", this, interpreter, owns_interpreter);
    if (q_libqore_exiting()) {
#ifdef DEBUG
        qpgm = nullptr;
#endif
        return;
    }

    if (qore_python_exiting) {
        // a Python process that hosts Qore exits: Python abandons its daemon threads, which are not woken (with
        // Python 3.12, a daemon thread that runs while the process exits aborts it), so a program that other threads
        // use (ex: a daemon thread blocked in a Qore call) is left as it is
        bool used = false;
        {
            AutoLocker al(py_thr_lck);
            int tid = q_gettid();
            for (auto& i : pgm_thr_uses) {
                if (i.first != tid) {
                    used = true;
                    break;
                }
            }
        }
        if (used) {
            valid = false;
            return;
        }
    } else {
        // the Qore calls that other threads make from the program's Python code are cancelled (ex: a thread blocked
        // in Queue::get()), so that the threads can leave the program; otherwise the deletion would wait for them
        // forever
        cancelThreads();
    }

    if (qpgm && owns_qore_program_ref) {
        // remove the external data before dereferencing
        qpgm->removeExternalData(QORE_PYTHON_MODULE_NAME);
        // the threads that use the Qore program can need the GIL to leave it (ex: a cancelled thread returning to
        // its Python code)
        std::optional<QorePythonReleaseGilHelper> prgh;
        if (qore_python_attached_thread_state()) {
            prgh.emplace();
        }
        qpgm->waitForTerminationAndDeref(xsink);
    }
    qpgm = nullptr;

    // in a child of fork() that cannot use Python, the interpreter is left as it is: the GIL cannot be acquired,
    // and the threads counted as using it do not exist in the child
    if (python_fork_lost) {
        valid = false;
        return;
    }

    if (interpreter) {
        // the Python code that other Qore threads run in the interpreter is interrupted (KeyboardInterrupt, raised as
        // PYTHON-INTERPRETER-DELETED in Qore), so that they leave it; a thread blocked in a call (ex: time.sleep())
        // leaves it when the call returns
        // only the pointers of the states that the module caches are collected: the state of a thread that has ended
        // can have been freed; the states are dereferenced only if they are in the interpreter's thread list
        std::set<PyThreadState*> states;
        {
            AutoLocker al(py_thr_lck);
            checkForkIntern();
            // from now on, other threads cannot use the program (see getAcquireThreadState()): a thread that passed
            // the check of the validity of the program before must not use the interpreter after the wait below
            deleting_tid = q_gettid();
            if (pgm_thr_cnt) {
                py_thr_map_t::iterator i = py_thr_map.find(this);
                if (i != py_thr_map.end()) {
                    for (auto& ti : i->second) {
                        if (ti.second.state) {
                            states.insert(ti.second.state);
                        }
                    }
                }
            }
        }
        if (!states.empty()) {
            QorePythonHelper qph(this);
            if (qph.isValid()) {
                PyThreadState* self = PyThreadState_Get();
                for (PyThreadState* ts = PyInterpreterState_ThreadHead(interpreter); ts; ts = PyThreadState_Next(ts)) {
                    if (ts != self && states.find(ts) != states.end()) {
                        PyThreadState_SetAsyncExc(ts->thread_id, PyExc_KeyboardInterrupt);
                    }
                }
            }
        }
        {
            AutoLocker al(py_thr_lck);
            checkForkIntern();
            waitForThreadsIntern();
        }
        {
            QorePythonHelper qph(this);
            clearPersistentReferences();

            for (auto& i : obj_sink) {
                Py_DECREF(i);
            }
            obj_sink.clear();

            {
                subclass_type_map_t types;
                {
                    AutoLocker al(qpy_subclass_type_lock);
                    types.swap(subclass_type_map);
                }
                for (auto& i : types) {
                    Py_DECREF(i.second);
                }
            }
            {
                hashdecl_type_map_t types;
                {
                    AutoLocker al(qpy_subclass_type_lock);
                    types.swap(hashdecl_type_map);
                }
                for (auto& i : types) {
                    Py_DECREF(i.second.type);
                }
            }
            {
                enum_type_map_t types;
                {
                    AutoLocker al(qpy_subclass_type_lock);
                    types.swap(enum_type_map);
                }
                for (auto& i : types) {
                    Py_DECREF(i.second.type);
                }
            }

            module.purge();
            python_code.purge();

            // Remove bridge methods before releasing the types. Bound methods
            // that survive own their metadata through their Python capsules.
            for (auto& i : py_cls_map) {
                i.second->clearMethods();
            }
            for (auto& i : stale_py_cls_vec) {
                i->clearMethods();
            }

            // Release references in both embedded and Python-hosted processes.
            for (auto& i : py_cls_map) {
                i.second->release();
            }
            for (auto& i : stale_py_cls_vec) {
                i->release();
            }

            valid = false;
        }
        // remove all thread states; the objects will be deleted by Python when the interpreter is destroyed
        {
            AutoLocker al(py_thr_lck);
            checkForkIntern();
            if (interpreter) {
                py_thr_map_t::iterator i = py_thr_map.find(this);
                assert(i != py_thr_map.end());
                //printd(5, "QorePythonProgram::deleteIntern() this: %p removing all thread states for pgm (delta: %d)\n",
                //  this, (int)i->second.size());
                for (auto& ti : i->second) {
                    //printd(5, "QorePythonProgram::deleteIntern() this: %p removing TID %d\n", this, ti.first);
                    py_global_tid_map_t::iterator gi = py_global_tid_map.find(ti.first);
                    assert(gi != py_global_tid_map.end());
                    py_thr_set_t::iterator thr_i = gi->second.find(ti.second.state);
                    if (thr_i != gi->second.end()) {
                        gi->second.erase(thr_i);
                    }
                }
                py_thr_map.erase(i);

                assert(pgm_count > 0);
                --pgm_count;
            }
        }

        if (interpreter && owns_interpreter) {
            // PyInterpreterState_Clear/Delete only discard interpreter storage. They
            // do not run atexit hooks, finalize modules, or release interpreter free lists.
            // Finish with Py_EndInterpreter while this interpreter is current instead.
            {
                QorePythonGilHelper pgh;
                if (!pgh.isInitialized()) {
                    interpreter = nullptr;
                    return;
                }
                // The interpreter is finalized with a state of the finalizing native thread,
                // and the states of all other Qore threads are deleted first:
                // - threading._shutdown() (Python < 3.13) waits for the sentinel lock of the
                //   thread that imported threading (its "main thread") unless it runs on that
                //   thread; the sentinel is released only when that thread's state is
                //   deleted, so finalizing with the state of another thread (ex: the initial
                //   state of a thread that has ended) waited forever
                // - when it runs on that thread, the sentinel must still be held, so the own
                //   state of the finalizing thread is used rather than a new one
                // - newer runtimes require a state belonging to the finalizing native
                //   thread; reusing another thread's state leaves interpreter objects
                //   uncollected
                PyThreadState* sub_tstate = nullptr;
                {
                    unsigned long ident = PyThread_get_thread_ident();
                    AutoLocker al(py_thr_lck);
                    checkForkIntern();
                    for (PyThreadState* state : owned_thread_states) {
                        if (state->thread_id == ident) {
                            sub_tstate = state;
                            break;
                        }
                    }
                }
                if (!sub_tstate) {
                    sub_tstate = PyThreadState_New(interpreter);
                }
                if (!sub_tstate) {
                    PyErr_Clear();
                    xsink->raiseException("PYTHON-MEMORY-ERROR", "cannot allocate interpreter cleanup state");
                    return;
                }
                qore_py_set_bound_gilstate(sub_tstate, 0);
                PyThreadState* old_tstate = PyThreadState_Swap(sub_tstate);
                _qore_PyGILState_SetThisThreadState(sub_tstate);
                py_thr_set_t states;
                {
                    AutoLocker al(py_thr_lck);
                    checkForkIntern();
                    // QorePythonHelper may have created a cleanup state after the
                    // first cache removal. Remove its reverse lookup as well.
                    auto program_states = py_thr_map.find(this);
                    if (program_states != py_thr_map.end()) {
                        for (const auto& entry : program_states->second) {
                            auto global_states = py_global_tid_map.find(entry.first);
                            assert(global_states != py_global_tid_map.end());
                            global_states->second.erase(entry.second.state);
                            if (global_states->second.empty()) {
                                py_global_tid_map.erase(global_states);
                            }
                        }
                        py_thr_map.erase(program_states);
                    }
                    states.swap(owned_thread_states);
                }
                // Python 3.12 requires the finalizing state to be the only remaining
                // state after joining Python threads. These idle states belong to Qore;
                // Python-created threads retain their states and are joined by Python.
                for (PyThreadState* state : states) {
                    if (state == sub_tstate) {
                        continue;
                    }
                    qore_py_set_bound_gilstate(state, 0);
                    PyThreadState_Clear(state);
                    PyThreadState_Delete(state);
                }
#if PY_VERSION_HEX < 0x030D0000
                forgetEndedThreadingMainThread();
#endif
                // in a child of fork(), the threads of the parent that have states in an interpreter created before
                // the fork do not exist, and the threading module, which does not know it (Python is not told of a
                // fork() that it does not make), waits for them forever when the interpreter ends: it is left in
                // memory and never finalized
                bool leave = interp_fork_generation != fork_generation && hasOtherThreadStates(sub_tstate);
#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030E0000
                // Python 3.13 and earlier abort the process ("Py_EndInterpreter: not the last thread") if another
                // thread still has a state in the interpreter after the threads that Py_EndInterpreter() waits for
                // have ended (Python 3.14 makes such threads exit when they next try to take the GIL); such a thread
                // (a daemon thread, or a thread started with the _thread module) cannot be waited for, so the
                // interpreter is left in memory, with the threads running its code, and is never finalized
                if (!leave) {
                    leave = hasThreadsNotJoinedAtEnd(sub_tstate);
                }
#endif
                // a thread created by Python whose Qore call the deletion cancelled (see cancelThreads()) runs the
                // Python code of the interpreter again when the call returns (ex: the exception of the call), so the
                // interpreter cannot be finalized: it is left in memory, as above
                if (!leave) {
                    bool cancelled;
                    {
                        AutoLocker al(py_thr_lck);
                        cancelled = pgm_threads_cancelled;
                    }
                    leave = cancelled && hasOtherThreadStates(sub_tstate);
                }
                if (leave) {
                    printd(5, "QorePythonProgram::deleteIntern() this: %p interpreter %p left: threads that are not "
                        "waited for have states in it\n", this, interpreter);
                } else {
                    Py_EndInterpreter(sub_tstate);
                }
                initial_thread_state = nullptr;
                interpreter = nullptr;
                PyThreadState_Swap(old_tstate);
                _qore_PyGILState_SetThisThreadState(old_tstate);
            }
            // Signal that an interpreter has been destroyed - must be AFTER deletion
            qpy_interpreter_destroyed();

            interpreter = nullptr;
            owns_interpreter = false;
        }
        for (auto& i : py_cls_map) {
            delete i.second;
        }
        py_cls_map.clear();
        for (auto& i : stale_py_cls_vec) {
            delete i;
        }
        stale_py_cls_vec.clear();
    }

    if (save_object_callback) {
        save_object_callback = nullptr;
    }

    //printd(5, "QorePythonProgram::deleteIntern() this: %p\n", this);
}

QoreValue QorePythonProgram::eval(ExceptionSink* xsink, const QoreString& source_code, const QoreString& source_label,
        int input, bool encapsulate) {
    TempEncodingHelper src_code(source_code, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"source_code\" argument)");
        return QoreValue();
    }
    TempEncodingHelper src_label(source_label, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"source_label\" argument)");
        return QoreValue();
    }

    // ensure atomic access to the Python interpreter (GIL) and manage the Python thread state
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }

    QorePythonReferenceHolder return_value;
    QorePythonReferenceHolder python_code;
    {
        //printd(5, "QorePythonProgram::QorePythonProgram() GIL thread state: %p\n", PyGILState_GetThisThreadState());
        // parse and compile code
        python_code = (PyObject*)Py_CompileString(src_code->c_str(), src_label->c_str(), input);
        if (!python_code) {
            if (!checkPythonException(xsink)) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "parsing and compilation failed");
            }
            return QoreValue();
        }
    }

    PyObject* main_dict;
    if (encapsulate) {
        // returns a borrowed reference
        PyObject* main = PyImport_AddModule("__main__");
        // returns a borrowed reference
        main_dict = PyModule_GetDict(main);
    } else {
        assert(module);
        // returns a borrowed reference
        main_dict = PyModule_GetDict(*module);
    }

    return_value = PyEval_EvalCode(*python_code, main_dict, main_dict);

    // check for Python exceptions
    if (checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, return_value);
}

void QorePythonProgram::pythonThreadCleanup(void*) {
    int tid = q_gettid();
    //printd(5, "QorePythonProgram::pythonThreadCleanup()\n");

    // issue #4651: when called when the Qore library and the python module are initialized from Java during static
    // process destruction, exit handlers may have already destroyed the static objects in this module; otherwise the
    // lock is waited for: when another thread held it, the cleanup was skipped, and the thread states of the ended
    // thread stayed in the cache under its Qore thread ID, which Qore gives to the next thread
    if (py_thr_lck.trylock()) {
        if (q_libqore_exiting()) {
            return;
        }
        py_thr_lck.lock();
    }

    // delete all thread states for the tid
    for (auto& i : py_thr_map) {
        py_tid_map_t::iterator ti = i.second.find(tid);
        if (ti != i.second.end()) {
            //printd(5, "QorePythonProgram::pythonThreadCleanup() deleting state %p for TID %d", ti->second, tid);
            i.second.erase(ti);
        }
    }

    // delete reverse lookups
    py_global_tid_map_t::iterator gi = py_global_tid_map.find(tid);
    if (gi != py_global_tid_map.end()) {
        //printd(5, "QorePythonProgram::pythonThreadCleanup() deleting global info for TID %d", tid);
        py_global_tid_map.erase(gi);
    }
    //printd(5, "QorePythonProgram::pythonThreadCleanup() done\n");
    py_thr_lck.unlock();

#ifndef Py_GIL_DISABLED
    // the idle state is deleted with the GIL, which is not taken while py_thr_lck is held
    qore_python_delete_idle_thread_state();
#endif
}

bool QorePythonProgram::haveGil() {
    if (!_qore_PyCeval_GetGilLockedStatus()) {
        return false;
    }

    PyThreadState* tstate = _qore_PyCeval_GetThreadState();
    if (!tstate) {
        return false;
    }

    int tid = q_gettid();
    AutoLocker al(py_thr_lck);
    checkForkIntern();

    py_global_tid_map_t::iterator i = py_global_tid_map.find(tid);
    if (i != py_global_tid_map.end()) {
        return i->second.find(tstate) != i->second.end();
    }
    return false;
}

bool QorePythonProgram::haveGilUnlocked(PyThreadState* check_tstate) {
    if (!_qore_PyCeval_GetGilLockedStatus()) {
        return false;
    }

    PyThreadState* tstate = _qore_PyCeval_GetThreadState();
    return tstate == check_tstate;
}

int QorePythonProgram::createInterpreter(QorePythonGilHelper& qpgh, ExceptionSink* xsink) {
    // the GIL is not held if Python cannot be used (see QorePythonGilHelper::isInitialized())
    if (!qpgh.isInitialized()) {
        if (xsink) {
            if (python_fork_lost) {
                qore_python_raise_fork_error(xsink);
            } else {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "cannot create a Python interpreter: the Python "
                    "library has been shut down");
            }
        }
        return -1;
    }
#ifndef Py_GIL_DISABLED
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+: Use our own tracking since Python's TSS can be cleared by external modules
    // (like JNI) without affecting our actual GIL ownership. PyGILState_Check() is unreliable.
    assert(_qore_PyCeval_GetGilLockedStatus());
#else
    assert(PyGILState_Check());
#endif
#endif
    PyThreadState* python;
    // NOTE: py_thr_lck must be held by caller to serialize interpreter creation
    // and prevent multiple threads from using mainThreadState concurrently.
    {
#ifdef Py_GIL_DISABLED
        // In free-threading mode, use Py_NewInterpreterFromConfig with appropriate settings
        // Share main obmalloc to avoid per-interpreter heap issues
        // Require multi-phase init extensions for sub-interpreter compatibility
        PyInterpreterConfig config = {
            .use_main_obmalloc = 1,  // Share main allocator
            .allow_fork = 0,
            .allow_exec = 0,
            .allow_threads = 1,
            .allow_daemon_threads = 0,
            .check_multi_interp_extensions = 1,  // Require multi-phase init extensions
            .gil = PyInterpreterConfig_SHARED_GIL,  // Shared GIL (no real GIL in free-threading)
        };

        // Creation attaches the new interpreter. Detach first so allocator and
        // thread-local state are initialized for that interpreter.
        qpgh.releaseBeforeSubInterpreter();

        PyStatus status = Py_NewInterpreterFromConfig(&python, &config);
        if (PyStatus_Exception(status)) {
            if (xsink) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "error creating the Python subinterpreter: %s",
                    status.err_msg ? status.err_msg : "unknown error");
            }
            return -1;
        }
#elif PY_VERSION_HEX >= 0x030C0000
        // the configuration of Py_NewInterpreter() (_PyInterpreterConfig_LEGACY_INIT), except that os.fork() is not
        // allowed: CPython supports fork() only in the main interpreter, and a child forked in a subinterpreter
        // dies ("Fatal Python error: _PyInterpreterState_DeleteExceptMain: not main interpreter"), while with
        // allow_fork = 0, os.fork() raises RuntimeError in the parent; processes can still be started (allow_exec)
        PyInterpreterConfig config = {
            .use_main_obmalloc = 1,
            .allow_fork = 0,
            .allow_exec = 1,
            .allow_threads = 1,
            .allow_daemon_threads = 1,
            .check_multi_interp_extensions = 0,
            .gil = PyInterpreterConfig_SHARED_GIL,
        };
        python = nullptr;
        PyStatus status;
        {
            // CPython waits for the GIL while it creates the interpreter (see QorePythonGilWait)
            QorePythonGilWait gil_wait;
            status = Py_NewInterpreterFromConfig(&python, &config);
        }
        if (PyStatus_Exception(status)) {
            if (xsink) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "error creating the Python subinterpreter: %s",
                    status.err_msg ? status.err_msg : "unknown error");
            }
            return -1;
        }
#else
        // with Python 3.11, the thread keeps the GIL while it creates the interpreter: the thread states are swapped
        // without releasing it
        python = Py_NewInterpreter();
#endif

        if (!python) {
            if (xsink) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "error creating the Python subinterpreter");
            }
            return -1;
        }
        //printd(5, "QorePythonProgram::createInterpreter() created thead state: %p\n", python);

        // NOTE: we have to reenable PyGILState_Check() here
        _QORE_PYTHON_REENABLE_GIL_CHECK

        qpgh.set(python);
        interp_fork_generation = fork_generation;
#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030C0000
        // before Python 3.12, a subinterpreter cannot be created with fork() disallowed (see above), so os.fork() is
        // replaced in it
        if (disallowFork()) {
            if (xsink && !checkPythonException(xsink)) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "cannot disallow fork() in the Python subinterpreter");
            }
            PyErr_Clear();
        }
#endif
        // Ensure the sub-interpreter has a usable sys.path (stdlib + extension modules).
        // copies made under the environment lock: another thread can change the environment meanwhile
        QoreEnvironmentValue pyhome_value("PYTHONHOME");
        QoreEnvironmentValue pypath_value("PYTHONPATH");
        const char* pyhome = pyhome_value.c_str();
        const char* pypath = pypath_value.c_str();
        if ((pyhome && *pyhome) || (pypath && *pypath)) {
#ifdef _Q_WINDOWS
            const char path_sep = ';';
#else
            const char path_sep = ':';
#endif
            std::string path;
            if (pypath) {
                path += pypath;
            }
            if (pyhome && *pyhome) {
                if (!path.empty()) {
                    path += path_sep;
                }
                path += pyhome;
                path += "/Lib";
                path += path_sep;
                path += pyhome;
                path += "/Modules";
            }
            PyObject* sys_path = PySys_GetObject("path");  // borrowed
            if (!sys_path || !PyList_Check(sys_path)) {
                sys_path = PyList_New(0);
                if (sys_path) {
                    PySys_SetObject("path", sys_path);
                    Py_DECREF(sys_path);
                }
            }
            if (sys_path && PyList_Check(sys_path)) {
                size_t start = 0;
                // Preserve empty entries to keep CWD semantics (ex: leading/trailing separators).
                while (start <= path.size()) {
                    size_t end = path.find(path_sep, start);
                    if (end == std::string::npos) {
                        end = path.size();
                    }
                    std::string entry = path.substr(start, end - start);
                    PyObject* py_entry = PyUnicode_DecodeFSDefault(entry.c_str());
                    if (py_entry) {
                        PyList_Append(sys_path, py_entry);
                        Py_DECREF(py_entry);
                    }
                    start = end + 1;
                }
            }
        }
        // Ensure PyDateTimeAPI is initialized for this interpreter; the macro only runs if the
        // global pointer is null, so clear it to avoid stale main-interpreter state.
        PyDateTimeAPI = nullptr;
        PyDateTime_IMPORT;
        if (!PyDateTimeAPI) {
            PyErr_Clear();
        }
    }

    interpreter = python->interp;
    initial_thread_state = python;
    owns_interpreter = true;
    printd(5, "QorePythonProgram::createInterpreter() interpreter: %p\n", interpreter);
    if (!PyDateTimeAPI) {
        PyDateTime_IMPORT;
        if (!PyDateTimeAPI) {
            PyErr_Clear();
            PyDateTimeAPI = nullptr;
        }
    }

    // save thread state
    // NOTE: Acquire py_thr_lck here for thread map updates.
    // The caller holds main_ts_lck (not py_thr_lck) to serialize mainThreadState access.
    // This allows setContext/releaseContext to use py_thr_lck without conflicting with main_ts_lck.
    int tid = q_gettid();
    {
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        {
            py_thr_map_t::iterator ti = py_thr_map.lower_bound(this);
            if (ti == py_thr_map.end() || ti->first != this) {
                py_thr_map.insert(ti, {this, {{tid, {python, true}}}});
            } else {
                ti->second[tid] = {python, true};
            }
        }
        {
            py_global_tid_map_t::iterator i = py_global_tid_map.lower_bound(tid);
            if (i == py_global_tid_map.end() || i->first != tid) {
                py_global_tid_map.insert(i, {tid, {python}});
            } else {
                i->second.insert(python);
            }
            //printd(5, "QorePythonProgram::createInterpreter() inserted TID %d -> %p\n", tid, python);
        }

        owned_thread_states.insert(python);
        ++pgm_count;
    }

    //printd(5, "QorePythonProgram::createInterpreter() this: %p\n", this);
    return setRecursionLimit(xsink);
}

int QorePythonProgram::getRecursionLimit() {
#if QORE_VERSION_CODE > 10007
    //printd(5, "QorePythonProgram::getRecursionLimit() stack remaining: %lld total size: %lld\n",
    //    q_thread_stack_remaining(), q_thread_get_this_stack_size());
    // for some reason the recusion depth needs to be calculated smaller with smaller stack sizes
    // testing results in using an estimated 6K python stack frame size for stacks 1M and smaller
    // and 10K for larger stacks
    int64 ss = (int64)q_thread_stack_remaining();
    int64 lim;
    if (ss > (1024 * 1024)) {
        lim = (int64)q_thread_stack_remaining() / PYTHON_LARGE_STACK_FACTOR;
    } else {
        lim = (int64)q_thread_stack_remaining() / PYTHON_SMALL_STACK_FACTOR;
    }
#else    // q_thread_stack_remaining() was broken in Qore < 1.0.8
    int64 lim = (int64)q_thread_get_stack_size() / PYTHON_SMALL_STACK_FACTOR;
#endif
    return lim;
}

int QorePythonProgram::setRecursionLimit(ExceptionSink* xsink) {
    QorePythonReferenceHolder sys(PyImport_ImportModule("sys"));
    if (!sys) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-ERROR", "cannot load the 'sys' module");
        }
        return -1;
    }
    if (!PyObject_HasAttrString(*sys, "setrecursionlimit")) {
        xsink->raiseException("PYTHON-ERROR", "'sys.setrecursionlimit' not found");
        return -1;
    }

    QorePythonReferenceHolder func(PyObject_GetAttrString(*sys, "setrecursionlimit"));
    assert(PyCFunction_Check(*func));
    //printd(5, "QorePythonProgram::setRecursionLimit() setrecursionlimit: %s\n", Py_TYPE(*func)->tp_name);
    // use the q_thread_stack_remaining() API if Qore >= 1.0.8
    QorePythonReferenceHolder py_args(PyTuple_New(1));
    PyTuple_SET_ITEM(*py_args, 0, PyLong_FromLongLong(getRecursionLimit()));
    QorePythonReferenceHolder return_value(PyObject_Call(*func, *py_args, nullptr));
    return checkPythonException(xsink);
}

// setContext() - Acquire the Python execution context for the current thread
//
// This function manages thread state and GIL acquisition to allow safe Python API calls.
// It handles multiple complex scenarios:
// 1. Creating new thread states for threads that don't have one yet
// 2. Reusing cached thread states for threads that have called Python before
// 3. Cross-interpreter thread state switching when a thread moves between PythonPrograms
// 4. Detection and handling of Python-created threads (e.g., threading.Thread callbacks)
// 5. Stale TSS (Thread-Specific Storage) cleanup when interpreters are deleted
//
// Returns QorePythonThreadInfo containing saved state for releaseContext() to restore.
// The caller MUST call releaseContext() when done with Python operations.
//
// Thread safety: Uses py_thr_lck for thread map access, but GIL acquisition happens outside
// the lock to avoid deadlocks. See design/python-module.md for detailed documentation.
QorePythonThreadInfo QorePythonProgram::setContext(bool check_interrupt) const {
    // CRITICAL: Check if Python is shutting down before any Python API calls.
    // After Py_FinalizeEx(), calling PyGILState_Ensure() or PyEval_RestoreThread() will crash.
    if (python_shutdown || !Py_IsInitialized()) {
        return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
    }

    if (!valid) {
        return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
    }

    // Check for interrupt before acquiring any state (only if requested)
    if (check_interrupt) {
        if (qore_check_cancel(nullptr, "Python execution")) {
            return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
        }
    }

    // Safety check: verify our interpreter is still in the global list of interpreters.
    // This can happen when another PythonProgram that owned the shared interpreter
    // has been destroyed, deleting the interpreter out from under us.
    if (!interpreter) {
        return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
    }

    // Check if interpreter is still valid by verifying it's in the global list
    {
        PyInterpreterState* interp = PyInterpreterState_Head();
        bool found = false;
        while (interp) {
            if (interp == interpreter) {
                found = true;
                break;
            }
            interp = PyInterpreterState_Next(interp);
        }
        if (!found) {
            // Interpreter has been deleted - mark ourselves as invalid and return
            const_cast<QorePythonProgram*>(this)->valid = false;
            return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
        }
    }

    assert(interpreter);
    PyThreadState* python = nullptr;
    // another thread deletes the program: the interpreter cannot be used
    if (!getAcquireThreadState(python)) {
        return {nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false};
    }

    // CRITICAL: Verify the cached thread state's interpreter matches ours.
    // The thread state's interp pointer can become stale if:
    // 1. PyInterpreterState_Clear was called (sets tstate->interp = NULL)
    // 2. The thread state was somehow detached from the interpreter
    // If the cached thread state isn't in the interpreter's thread list, it's stale/invalid.
    // The cache is keyed by Qore thread ID, which Qore reuses after a thread is deregistered: the cached state of a
    // thread that Python created and that has ended (or whose state's memory now holds the state of another thread)
    // can be found for another native thread, which must not use it (attaching the state of another thread corrupts
    // the interpreter: "_PyThreadState_Attach: non-NULL old thread state", or critical sections of free-threaded
    // Python that are unlocked on the wrong state); a state belongs to the native thread that created it
    if (python) {
        // the state is only dereferenced once it is found in the interpreter's thread list: the cached state of an
        // ended thread can have been freed
        bool found = false;
        for (PyThreadState* ts = PyInterpreterState_ThreadHead(interpreter); ts; ts = PyThreadState_Next(ts)) {
            if (ts == python) {
                found = python->thread_id == PyThread_get_thread_ident();
                break;
            }
        }
        if (!found) {
            printd(5, "QorePythonProgram::setContext() cached thread state %p not found in interpreter %p "
                "thread list - will create new\n", python, interpreter);
            // Remove this thread's entry from the cache - it's invalid
            {
                int tid = q_gettid();
                AutoLocker al(py_thr_lck);
                checkForkIntern();
                py_thr_map_t::iterator i = py_thr_map.find(this);
                if (i != py_thr_map.end()) {
                    i->second.erase(tid);
                }
                py_global_tid_map_t::iterator gi = py_global_tid_map.find(tid);
                if (gi != py_global_tid_map.end()) {
                    gi->second.erase(python);
                    if (gi->second.empty()) {
                        py_global_tid_map.erase(gi);
                    }
                }
                // Don't decrement pgm_thr_cnt here - getAcquireThreadState already incremented it
            }
            python = nullptr;  // Force creation of new thread state below
        }
    }

    // CRITICAL: Also check if TSS points to a stale thread state (from a deleted interpreter).
    // This can happen when an interpreter is deleted but TSS still points to one of its thread states.
    // When we create a new thread state with PyThreadState_New(), it will try to bind to TSS,
    // but if TSS already has a stale thread state with bound_gilstate=1, tstate_activate will fail.
#if PY_VERSION_HEX >= 0x030D0000 && !defined(Py_GIL_DISABLED)
    // CRITICAL: Check for stale TSS from deleted interpreters BEFORE creating new thread states.
    // This can happen when an interpreter is deleted but TSS still points to one of its thread states.
    {
        PyThreadState* tss_tstate = PyGILState_GetThisThreadState();
        if (tss_tstate && tss_tstate != python) {
            // TSS has a thread state different from our cached/expected one
            // Check if the TSS thread state's interpreter is valid
            bool tss_interp_valid = false;
            PyInterpreterState* interp = PyInterpreterState_Head();
            while (interp) {
                if (interp == tss_tstate->interp) {
                    tss_interp_valid = true;
                    break;
                }
                interp = PyInterpreterState_Next(interp);
            }
            if (!tss_interp_valid || tss_tstate->interp == nullptr) {
                // TSS points to a thread state with a deleted/invalid interpreter, which can be freed: it is not
                // touched (its flag).  A thread that does not hold the GIL has its idle state in the main interpreter
                // bound (see qore_python_release_context()), so this is not expected.
                // CRITICAL: Also clear TSS. PyThreadState_New() only sets TSS if it's currently NULL.
                // If we don't clear TSS, PyThreadState_New() won't update it, and when we activate
                // our new thread state, TSS won't match.
                _qore_PyGILState_SetThisThreadState(nullptr);
                PyThreadState_Swap(nullptr);
            }
        }
    }

#endif
    printd(5, "QorePythonProgram::setContext() ENTRY this: %p owns_interpreter: %d python: %p "
        "_qore_tss_tstate: %p tid: %d\n", this, owns_interpreter, python, _qore_PyCeval_GetThreadState(),
        q_gettid());


    // Flag to track if this is a Python-created thread (threading.Thread calling back into Qore)
    bool is_python_created_thread = false;

    // create new thread state if necessary
    if (!python) {
        // IMPORTANT - Python-Created Thread Detection (Python 3.12+)
        // Before creating a new thread state, we must check if this is a Python-created thread
        // (e.g., a threading.Thread) that already has a thread state and holds the GIL.
        //
        // For Python-created threads:
        // - Python already created a thread state when the thread was spawned
        // - The thread already has the GIL (from Python's thread startup)
        // - We should USE that existing thread state, not create a new one
        //
        // For Qore-created threads (background threads calling into Python):
        // - No thread state exists yet
        // - We need to create one with PyThreadState_New()
        // - Then acquire the GIL with PyEval_RestoreThread()
        //
        // The _qore_check_python_created_thread_gil() function checks Python's TSS to detect
        // Python-created threads. We MUST call it BEFORE PyThreadState_New() because in Python 3.12,
        // PyThreadState_New() modifies TSS (see bind_tstate/bind_gilstate_tstate in pystate.c),
        // which would corrupt our detection.
        // NOTE: This detection is also needed for Python 3.11 - Python's threading.Thread creates
        // threads that already have thread states and the GIL. Without this check, we'd create a
        // duplicate thread state and deadlock trying to acquire an already-held GIL.
#ifdef Py_GIL_DISABLED
        PyThreadState* python_thread_state = PyThreadState_GetUnchecked();
#else
        PyThreadState* python_thread_state = _qore_check_python_created_thread_gil();
#endif
        if (python_thread_state && python_thread_state->interp == interpreter) {
            // This is a Python-created thread - use the existing thread state
            // The thread already has the GIL, so we don't need to acquire it
            python = python_thread_state;
            is_python_created_thread = true;
            printd(5, "QorePythonProgram::setContext() detected Python-created thread, using existing "
                "thread state: %p\n", python);
        }

        if (!python) {
            // This is a Qore-created thread - create a new thread state
            printd(5, "QorePythonProgram::setContext() creating new thread state for interpreter: %p\n",
                interpreter);
            python = PyThreadState_New(interpreter);

            printd(5, "QorePythonProgram::setContext() this: %p created new thread context: %p (py_thr_map: %p "
                "size: %d)\n", this, python, &py_thr_map, (int)py_thr_map.size());
            assert(python);


            // IMPORTANT - Python 3.12 PyThreadState_New() Behavior Change:
            // In Python 3.12, PyThreadState_New() automatically binds the newly created thread state
            // to the current thread via TSS (see Python/pystate.c bind_tstate/bind_gilstate_tstate).
            // This is different from older Python versions where PyThreadState_New() did not modify TSS.
            //
            // After PyThreadState_New() returns:
            // - PyGILState_GetThisThreadState() returns the new thread state
            // - PyGILState_Check() returns 1 (true)
            // Even though we HAVEN'T ACQUIRED THE GIL yet!
            //
            // The fix: immediately mark our tracking as initialized (_qore_tss_initialized = true)
            // but WITHOUT setting _qore_tss_tstate. This tells _qore_check_python_created_thread_gil()
            // that this is a Qore-managed thread (not Python-created) that doesn't have the GIL yet.
            // The GIL will be properly acquired later via PyEval_RestoreThread().
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000
            _qore_tss_initialized = true;
            // Keep _qore_tss_tstate = nullptr to indicate we don't have the GIL
            printd(5, "QorePythonProgram::setContext() Python 3.12: marked _qore_tss_initialized=true "
                "after PyThreadState_New() to prevent false Python-created thread detection\n");
#endif
        }

        // Keep ownership independently of the per-thread lookup: Qore thread exit
        // removes that lookup, but the state must still be finalized with its interpreter.
        int tid = q_gettid();
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        if (!is_python_created_thread) {
            owned_thread_states.insert(python);
        }
        {
            py_thr_map_t::iterator i = py_thr_map.find(this);
            if (i == py_thr_map.end()) {
                py_thr_map[this] = {{tid, {python, !is_python_created_thread}}};
            } else {
                assert(i->second.find(tid) == i->second.end());
                i->second[tid] = {python, !is_python_created_thread};
            }
            assert(py_thr_map.find(this) != py_thr_map.end());
        }
        {
            py_global_tid_map_t::iterator i = py_global_tid_map.lower_bound(tid);
            if (i == py_global_tid_map.end() || i->first != tid) {
                py_global_tid_map.insert(i, {tid, py_thr_set_t {python,}});
            } else {
                i->second.insert(python);
            }
            //printd(5, "QorePythonProgram::setContext() inserted TID %d -> %p\n", tid, python);
        }
        //printd(5, "QorePythonProgram::setContext() this: %p\n", this);
    }

    printd(5, "QorePythonProgram::setContext() got thread context: %p (GIL: %d hG: %d)\n", python,
        PyGILState_Check(), haveGil());
    // the state belongs to this native thread (see the check of the cached state above)
    assert(python->thread_id == PyThread_get_thread_ident());

    PyGILState_STATE g_state;
    // the TSS state needs to be restored in any case
    PyThreadState* tss_state = PyGILState_GetThisThreadState();
    PyThreadState* t_state, * ceval_state;
    PyThreadState* released_other_interp_tstate = nullptr;  // Track if we released another interpreter's thread state

#ifdef Py_GIL_DISABLED
    // PyGILState_GetThisThreadState() is an auto-TSS cache, not the attached
    // state. Save the actual attachment, including nullptr, and restore it
    // when leaving this context.
    ceval_state = nullptr;
    t_state = PyThreadState_GetUnchecked();
    g_state = t_state == python ? PyGILState_LOCKED : PyGILState_UNLOCKED;
    if (t_state != python) {
        PyThreadState_Swap(python);
    }
#else
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ changed thread state management - use PyEval_AcquireThread/ReleaseThread
    // which properly handle TSS and fast TLS synchronization
    t_state = PyGILState_GetThisThreadState();

    // IMPORTANT: If is_python_created_thread is true (set earlier when we detected a Python-created
    // thread like threading.Thread), we already have the GIL and should not try to acquire it.
    bool have_gil = is_python_created_thread || _qore_has_gil(t_state);
    printd(5, "QorePythonProgram::setContext() Python 3.13+ is_python_created_thread: %d have_gil: %d "
        "t_state: %p tid: %d\n", is_python_created_thread, have_gil, t_state, q_gettid());
    if (have_gil) {
        // Already have the GIL - just swap thread states if needed
        ceval_state = t_state;
        if (t_state != python) {
            PyThreadState_Swap(python);
        }
        g_state = PyGILState_LOCKED;
    } else {
        // Don't have the GIL - need to acquire it
        // CRITICAL: Save the python thread state we're acquiring with, so releaseContext can
        // use the SAME thread state for PyEval_ReleaseThread. Using _qore_PyCeval_GetThreadState()
        // in releaseContext can return a different thread state after nested context switches.
        ceval_state = python;
        // Without the GIL, this thread has no state bound to it for the GIL state API but one that only it uses
        // (qore_python_release_context() unbinds the state it releases): CPython binds python to the thread when
        // python is attached, and clears the flag of a state bound before, with the GIL held.  No bound_gilstate flag
        // is changed here: other threads use the main thread state, and states of other interpreters, with the GIL
        // held; previously the flags of the states bound to this thread were cleared here without the GIL, also the
        // flag of the main thread state, which was bound to every thread, a data race.
        //
        // If a state of another interpreter is attached to this thread, it is released here and attached again in
        // releaseContext(): PyEval_RestoreThread() requires that no state is attached.
        PyThreadState* currently_attached = PyThreadState_GetUnchecked();
        if (currently_attached && currently_attached != python && currently_attached->interp != python->interp) {
            released_other_interp_tstate = PyEval_SaveThread();  // Release GIL and detach
        }
        {
            QorePythonGilWait gil_wait(python);
            PyEval_RestoreThread(python);
        }
        g_state = PyGILState_UNLOCKED;
        _QORE_SET_GIL_HELD(true);  // Track that we now hold the GIL
    }

    // Update our tracking
    _qore_PyGILState_SetThisThreadState(python);
#elif PY_VERSION_HEX >= 0x030C0000
    // Python 3.12 with bundled internals
    // NOTE: Don't use _qore_has_gil(tss_state) here because tss_state (from Python's TSS)
    // can be stale in Python 3.12 after PyEval_ReleaseThread(). Instead, check our own
    // tracking to determine if we have the GIL.
    //
    // IMPORTANT: If is_python_created_thread is true (set earlier when we detected a Python-created
    // thread like threading.Thread), we already have the GIL and should not try to acquire it.
    bool have_gil = is_python_created_thread || _qore_PyCeval_GetGilLockedStatus();
    printd(5, "QorePythonProgram::setContext() Python 3.12 is_python_created_thread: %d have_gil: %d "
        "_qore_tss_tstate: %p _qore_tss_initialized: %d tid: %d\n", is_python_created_thread, have_gil,
        _qore_PyCeval_GetThreadState(), _qore_tss_initialized, q_gettid());
    printd(5, "QorePythonProgram::setContext() final have_gil: %d\n", have_gil);
    // the state that the thread has attached (ex: the state of a thread created by Python, with which Python called
    // the module), which releaseContext() attaches again: the state that the tracking or the GIL state API gives can be
    // another one, and the thread would then return to Python with the wrong state attached (an exception set on it
    // is not seen by the Python code: "error return without exception set")
    PyThreadState* attached = have_gil ? qore_python_attached_thread_state() : nullptr;
    if (have_gil) {
        // If bound_gilstate is stale, clear it to avoid tstate_activate assertions.
        if (qore_py_get_bound_gilstate(python) && PyGILState_GetThisThreadState() != python) {
            qore_py_set_bound_gilstate(python, 0);
        }
        // We already have the GIL - swap thread states
        _qore_PyCeval_SwapThreadState(python);
        ceval_state = attached;
        g_state = PyGILState_LOCKED;
        // PyThreadState_Swap updates both the attached state and autoTSSkey
        if (attached != python) {
            PyThreadState_Swap(python);
        }
    } else {
        // We don't have the GIL - acquire it
        // Without the GIL, this thread has no state bound to it for the GIL state API but one that only it uses
        // (qore_python_release_context() unbinds the state it releases): CPython binds python to the thread when
        // python is attached, and clears the flag of a state bound before, with the GIL held.  No bound_gilstate flag
        // is changed here (see the same case for Python 3.13 and later above).
        ceval_state = nullptr;
        {
            QorePythonGilWait gil_wait(python);
            PyEval_RestoreThread(python);
        }
        // Set our tracking now that we have the GIL
        _qore_PyGILState_SetThisThreadState(python);
        _QORE_SET_GIL_HELD(true);  // Track that we now hold the GIL
        g_state = PyGILState_UNLOCKED;
    }

    // releaseContext() attaches t_state again if the GIL was held
    t_state = have_gil ? attached : _qore_PyRuntimeGILState_GetThreadState();
#else
    // Python < 3.12 (3.11 and earlier)
    // In these versions, PyGILState_* functions are mostly reliable, but with sub-interpreters
    // on the same thread, PyGILState_Check() can return false even when our thread holds the GIL
    // with a different thread state. Use _qore_PyCeval_GetGilLockedStatus() which checks the
    // actual thread ID instead of relying on TSS matching.
    t_state = PyGILState_GetThisThreadState();
    // IMPORTANT: If is_python_created_thread is true (set earlier when we detected a Python-created
    // thread like threading.Thread), we already have the GIL and should not try to acquire it.
    // Use _qore_PyCeval_GetGilLockedStatus() which checks if tstate_current->thread_id matches
    // our thread ID, which correctly handles sub-interpreters on the same thread.
    bool have_gil = is_python_created_thread || _qore_PyCeval_GetGilLockedStatus();
    printd(5, "QorePythonProgram::setContext() Python <3.12 is_python_created_thread: %d have_gil: %d "
        "t_state: %p tid: %d\n", is_python_created_thread, have_gil, t_state, q_gettid());

    if (have_gil) {
        // We already have the GIL - swap thread states if needed
        ceval_state = t_state;
        if (t_state != python) {
            PyThreadState_Swap(python);
        }
        g_state = PyGILState_LOCKED;
    } else {
        // We don't have the GIL - acquire it
        ceval_state = nullptr;
        {
            QorePythonGilWait gil_wait(python);
            PyEval_RestoreThread(python);
        }
        g_state = PyGILState_UNLOCKED;
        _QORE_SET_GIL_HELD(true);  // Track that we now hold the GIL
    }
    // Update tracking for consistency
    _qore_PyGILState_SetThisThreadState(python);
#endif
#endif

#ifndef Py_GIL_DISABLED
    // Now we have the GIL - verify our state is consistent
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+: the attached state, not the state bound for the GIL state API (PyGILState_Check(),
    // PyGILState_GetThisThreadState()), tells that the thread holds the GIL with this program's state
    assert(PyThreadState_GetUnchecked() == python);
#else
    assert(PyGILState_Check());
    // TSS state assertions - only reliable in Python < 3.13
    assert(PyGILState_GetThisThreadState() == python);
#endif
    assert(haveGilUnlocked(python));
    assert(_qore_PyCeval_GetThreadState() == python);
    assert(_qore_PyRuntimeGILState_GetThreadState() == python);
#endif

    //printd(5, "QorePythonProgram::setContext() old thread context: %p\n", t_state);


    QorePythonThreadInfo rv = {tss_state, t_state, ceval_state, released_other_interp_tstate, g_state, true};
#ifdef QORE_PYTHON_TSTATE_C_RECURSION_COUNTER
    // Python 3.11 - 3.13 limit the recursion that uses the C stack (ex: repr() of nested containers) with a counter
    // in the thread state that is sized for the stack of the main thread (ex: 10000 levels), while a Qore thread can
    // have a much smaller stack, which the recursion then overflows (SIGBUS); the counter is reduced to the levels
    // that the remaining stack can hold, and restored by releaseContext() (Python 3.14 checks the stack of the thread
    // itself).  Only the counter can be changed: when it reaches zero, CPython compares the depth (the thread
    // state's limit minus the counter) with the interpreter's recursion limit, so a lower limit in the thread state
    // would let the recursion continue
    int c_limit = static_cast<int>(q_thread_stack_remaining() / PYTHON_C_STACK_FACTOR);
    if (c_limit < python->QORE_PYTHON_TSTATE_C_RECURSION_COUNTER) {
        rv.c_recursion_reduction = python->QORE_PYTHON_TSTATE_C_RECURSION_COUNTER - c_limit;
        python->QORE_PYTHON_TSTATE_C_RECURSION_COUNTER = c_limit;
    }
#endif
    return rv;
}

// releaseContext() - Release the Python execution context acquired by setContext()
//
// This function restores the previous thread state and releases the GIL if it was acquired.
// It handles:
// 1. Releasing the GIL when g_state == PyGILState_UNLOCKED (we acquired it in setContext)
// 2. Restoring thread states when we swapped them for cross-interpreter calls
// 3. Restoring the module's tracking of the GIL (Python 3.11 and 3.12; 3.13+ use the attached state)
// 4. Decrementing thread reference counts for thread map management
//
// The oldstate parameter contains the saved state from setContext() that must be restored.
// See design/python-module.md for detailed documentation.
void QorePythonProgram::releaseContext(const QorePythonThreadInfo& oldstate) const {
    // CRITICAL: Check if Python is shutting down before any Python API calls.
    // After Py_FinalizeEx(), calling Python APIs will crash.
    if (python_shutdown || !Py_IsInitialized()) {
        return;
    }
    if (!oldstate.valid) {
        return;
    }

    // CRITICAL: Get thread state from Python's internal tracking WITHOUT acquiring py_thr_lck.
    // This avoids a deadlock: if we acquire py_thr_lck while holding the GIL, and another
    // thread is in a constructor holding py_thr_lck while waiting for the GIL, we deadlock.
    // Lock ordering: py_thr_lck -> GIL (constructor acquires lock first, then GIL)
    // Here we release GIL first, then acquire py_thr_lck to decrement thread count.
    PyThreadState* python = _qore_PyCeval_GetThreadState();
    assert(python);
#ifndef Py_GIL_DISABLED
    assert(_qore_PyThreadState_IsCurrent(python));
#endif

#ifdef QORE_PYTHON_TSTATE_C_RECURSION_COUNTER
    python->QORE_PYTHON_TSTATE_C_RECURSION_COUNTER += oldstate.c_recursion_reduction;
#endif

    // restore the old state
#ifndef Py_GIL_DISABLED
    // In GIL mode, restore the ceval thread state if it was different
    if (oldstate.ceval_state != python) {
        // set GIL context
        _qore_PyCeval_SwapThreadState(oldstate.ceval_state);
    }
#endif


    //printd(5, "QorePythonProgram::releaseContext() t_state: %p g_state: %d\n", oldstate.t_state, oldstate.g_state);

#ifdef Py_GIL_DISABLED
    // In free-threading mode, restore the previous thread state if we swapped
    if (oldstate.g_state == PyGILState_UNLOCKED) {
        // We swapped in setContext, so swap back to the original (or nullptr)
        PyThreadState_Swap(oldstate.t_state);
    }
#elif PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ - restore original state and release GIL if needed
    if (oldstate.g_state == PyGILState_UNLOCKED) {
        // We acquired the GIL with PyEval_RestoreThread(ceval_state) in setContext.
        // Use the SAME thread state we saved for release to avoid TSS mismatch.
        PyThreadState* release_tstate = oldstate.ceval_state;
        assert(release_tstate);

        // CRITICAL: PyEval_ReleaseThread expects Python's TSS and fast TLS to match the thread state.
        // Ensure they're set to release_tstate before releasing.
        PyThreadState* current_tss = PyGILState_GetThisThreadState();
        if (current_tss != release_tstate) {
            PyThreadState_Swap(release_tstate);
        }
        // Use PyEval_ReleaseThread which properly clears both TSS and fast TLS
        qore_python_release_context();

        // If we released another interpreter's thread state in setContext, restore it now
        if (oldstate.released_other_interp_tstate) {
            PyEval_RestoreThread(oldstate.released_other_interp_tstate);
            _qore_PyGILState_SetThisThreadState(oldstate.released_other_interp_tstate);
            _QORE_SET_GIL_HELD(true);
        }
    } else {
        // We already had the GIL - swap back to original thread state if needed
        // For LOCKED case, ceval_state holds the original thread state we swapped FROM
        if (oldstate.ceval_state != python && oldstate.ceval_state != nullptr) {
            PyThreadState_Swap(oldstate.ceval_state);
        }
        // Restore tracking to original state
        _qore_PyGILState_SetThisThreadState(oldstate.t_state);
    }
#else
    if (oldstate.g_state == PyGILState_UNLOCKED) {
        // We acquired the GIL in setContext() with PyEval_RestoreThread(python).
        // During our context, nested setContext calls might have changed the ceval thread state.
        // Ensure we release the correct thread state by swapping back to python first.
        // Use _qore_PyCeval_GetThreadState() instead of PyThreadState_Get() - the latter crashes
        // in Python 3.11 if the GIL is not held properly (e.g., after PyInterpreterState_Delete).
        PyThreadState* current = _qore_PyCeval_GetThreadState();
        if (current != python) {
            PyThreadState_Swap(python);
        }
        qore_python_release_context();
        // NOTE we cannot assert !PyGILState_Check() here, as we have released the GIL, and another thread may have
        // created a new interpreter, which will temporarily disbale the GIL check, which would cause
        // PyGILState_Check() to return 1
        // NOTE: Do NOT restore tss_state here - we've released the GIL and _qore_release_thread_state already
        // cleared our tracking. Restoring stale tss_state would incorrectly indicate we still have the GIL.
    } else {
        if (python != oldstate.t_state) {
            _QORE_PYTHREAD_STATE_SWAP(oldstate.t_state);
        }
        // Restore our tracking to the original ceval_state (which was saved from our tracking).
        // NOTE: Don't use oldstate.tss_state here - in Python 3.12, Python's TSS can be stale
        // after PyEval_ReleaseThread(). Use oldstate.ceval_state which came from our tracking.
        // The tracking was already restored at lines 886-889 above via _qore_PyCeval_SwapThreadState,
        // so we don't need to do it again here.
    }
#endif

    // Decrement thread count.
    // In the nested case (g_state == LOCKED), we still have the GIL but this is safe because:
    // - Constructors use main_ts_lck (not py_thr_lck) for GIL acquisition
    // - getAcquireThreadState() in setContext only briefly holds py_thr_lck, releasing before GIL acquisition
    // So no thread holds py_thr_lck while waiting for the GIL, avoiding deadlocks.
    bool clear_cancel = false;
    {
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        int tid = q_gettid();
        std::map<int, int>::iterator i = pgm_thr_uses.find(tid);
        if (i != pgm_thr_uses.end() && !--i->second) {
            pgm_thr_uses.erase(i);
            // the thread leaves the program, whose deletion cancelled its Qore calls: the cancellation does not
            // apply to the code that the thread runs outside of the program
            clear_cancel = pgm_cancelled_tids.erase(tid);
        }
        if (!--pgm_thr_cnt && pgm_thr_waiting) {
            pgm_thr_cond.signal();
        }
    }
    if (clear_cancel) {
        qore_clear_thread_cancel();
    }
}

PythonQoreClass* QorePythonProgram::findCreatePythonClass(const QoreClass& cls, const char* mod_name) {
    printd(5, "QorePythonProgram::findCreatePythonClass() %s.%s\n", mod_name, cls.getName());

    py_cls_map_t::iterator i = py_cls_map.lower_bound(&cls);
    if (i != py_cls_map.end() && i->first == &cls) {
        if (i->second->isValid()) {
            printd(5, "QorePythonProgram::findCreatePythonClass() returning existing %s.%s\n", mod_name,
                cls.getName());
            return i->second;
        }
        // the class at this address was freed with its Program; the Python class stays usable for its existing
        // objects and types, which raise an exception when they use the class
        stale_py_cls_vec.push_back(i->second);
        i = py_cls_map.erase(i);
    }

    std::unique_ptr<PythonQoreClass> py_cls(new PythonQoreClass(this, mod_name, cls));
    PyTypeObject* t = py_cls->getPythonType();
    if (!t) {
        // the Python type could not be created; the Python exception is set
        assert(PyErr_Occurred());
        return nullptr;
    }
    // creating the class can create it in a nested call (ex: a constant of a base class is an object of the class),
    // whose Python class is in the map then: it is the class's Python class, and the one made here is released with
    // the classes in the map (its type can already be in use, ex: as the base of another type)
    py_cls_map_t::iterator ci = py_cls_map.find(&cls);
    assert(ci != py_cls_map.end());
    if (ci->second != py_cls.get()) {
        stale_py_cls_vec.push_back(py_cls.release());
        return ci->second;
    }
    printd(5, "QorePythonProgram::findCreatePythonClass() returning new %s.%s type: %p (%s)\n", mod_name,
        cls.getName(), t, t->tp_name);
    return py_cls.release();
}

//! makes Qore's fork() called on this thread run Python's fork protocol (see python_fork_protocol_tstate)
class QorePythonForkProtocolHelper {
public:
    //! @param tstate the thread state that the thread released to call Qore's fork(), or nullptr if the protocol is
    //! not run (the function called is not fork(), or the code calling it does not run in the main interpreter)
    DLLLOCAL QorePythonForkProtocolHelper(PyThreadState* tstate) : old_tstate(python_fork_protocol_tstate) {
        python_fork_protocol_tstate = tstate;
    }

    DLLLOCAL ~QorePythonForkProtocolHelper() {
        python_fork_protocol_tstate = old_tstate;
    }

    QorePythonForkProtocolHelper(const QorePythonForkProtocolHelper&) = delete;
    QorePythonForkProtocolHelper& operator=(const QorePythonForkProtocolHelper&) = delete;

private:
    PyThreadState* old_tstate;
};

//! returns true if the function is Qore's fork() (see qore_python_register_fork_protocol())
static bool is_qore_fork_function(const QoreExternalFunction& func) {
    // the only builtin function of the Qore library with this name is Qore::fork()
    return func.isBuiltin() && !func.getModuleName() && !strcmp(func.getName(), "fork");
}

struct func_capsule_t {
    const QoreExternalFunction& func;
    QorePythonProgram* py_pgm;
    const std::string name;
    PyMethodDef definition;
    //! true if the function is Qore's fork()
    const bool is_fork;

    DLLLOCAL func_capsule_t(const QoreExternalFunction& func, QorePythonProgram* py_pgm, PyCFunction callback)
            : func(func), py_pgm(py_pgm), name(func.getName()),
              definition{name.c_str(), callback, METH_VARARGS, nullptr}, is_fork(is_qore_fork_function(func)) {
        py_pgm->weakRef();
    }

    DLLLOCAL ~func_capsule_t() {
        py_pgm->weakDeref();
    }
};

static void func_capsule_destructor(PyObject* func_capsule) {
    func_capsule_t* fc = reinterpret_cast<func_capsule_t*>(PyCapsule_GetPointer(func_capsule, nullptr));
    delete fc;
}

int QorePythonProgram::importQoreFunctionToPython(PyObject* mod, const QoreExternalFunction& func) {
    printd(5, "QorePythonProgram::importQoreFunctionToPython() %s()\n", func.getName());

    std::unique_ptr<func_capsule_t> fc(new func_capsule_t(func, this, callQoreFunction));
    QorePythonReferenceHolder capsule(PyCapsule_New(fc.get(), nullptr, func_capsule_destructor));
    if (!capsule) {
        return -1;
    }
    func_capsule_t* data = fc.release();
    QorePythonReferenceHolder pyfunc(PyCFunction_New(&data->definition, *capsule));
    if (!pyfunc) {
        return -1;
    }
    if (PyObject_SetAttrString(mod, func.getName(), *pyfunc)) {
        assert(PyErr_Occurred());
        printd(5, "QorePythonProgram::importQoreFunctionToPython() error setting function %s in %p (%s)\n",
            func.getName(), mod, Py_TYPE(mod)->tp_name);
        return -1;
    }
    printd(5, "QorePythonProgram::importQoreFunctionToPython() ns: %p (%s) %s = %p\n", mod, Py_TYPE(mod)->tp_name,
        func.getName(), &func);
    return 0;
}

int QorePythonProgram::saveQoreObjectFromPython(const QoreValue& rv, ExceptionSink& xsink) {
    // save object in thread-local data if relevant
    if (rv.getType() != NT_OBJECT) {
        return 0;
    }

    //printd(5, "QorePythonProgram::saveQoreObjectFromPython() this: %p val: %s soc: %p\n", this,
    //  rv.getFullTypeName(), *save_object_callback);

    if (!save_object_callback) {
        return 0;
    }

    // the callback is Qore code, which runs without the GIL, so that it cannot deadlock with a thread that holds a
    // Qore lock and waits for the GIL; the arguments and the result are released without the GIL as well
    {
        QorePythonReleaseGilHelper prgh;
        ReferenceHolder<QoreListNode> args(new QoreListNode(autoTypeInfo), &xsink);
        args->push(rv.refSelf(), &xsink);
        ValueHolder result(save_object_callback->execValue(*args, &xsink), &xsink);
    }
    if (xsink) {
        raisePythonException(xsink);
        return -1;
    }
    return 0;
}

// the internal member of the Qore objects of Python iterators that holds the current item in a list
static constexpr const char* QORE_PYTHON_ITERATOR_KEY = "__$PYITER__";

// the maximum number of chained exceptions converted between Qore and Python, as for the ExceptionInfo hash in Qore
static constexpr int QORE_PYTHON_MAX_CHAINED_EXCEPTIONS = 20;

// returns the name of a Python exception class as the "err" of its Qore exception: "<module>.<qualified name>"
static std::string qore_python_get_exception_name(PyTypeObject* cls) {
    PyObject* cls_obj = reinterpret_cast<PyObject*>(cls);
    std::string name;
    QorePythonReferenceHolder mod(PyObject_GetAttrString(cls_obj, "__module__"));
    const char* str = mod && PyUnicode_Check(*mod) ? PyUnicode_AsUTF8(*mod) : nullptr;
    if (str) {
        name = str;
        name += '.';
    }
    QorePythonReferenceHolder qualname(PyObject_GetAttrString(cls_obj, "__qualname__"));
    str = qualname && PyUnicode_Check(*qualname) ? PyUnicode_AsUTF8(*qualname) : nullptr;
    // the class name is used if the attributes are not available
    if (PyErr_Occurred()) {
        PyErr_Clear();
    }
    name += str ? str : cls->tp_name;
    return name;
}

// returns str() of the Python object as a Qore string
static QoreStringNode* qore_python_get_string(PyObject* obj) {
    QorePythonReferenceHolder str(PyObject_Str(obj));
    Py_ssize_t size;
    // ex: a string with lone surrogates, which cannot be encoded in UTF-8
    const char* cstr = str ? PyUnicode_AsUTF8AndSize(*str, &size) : nullptr;
    if (!cstr) {
        PyErr_Clear();
        return new QoreStringNodeMaker("<%s object>", Py_TYPE(obj)->tp_name);
    }
    return new QoreStringNode(cstr, size, QCS_UTF8);
}

// holds the ExceptionInfo hash of a Qore exception raised in Python in a capsule
static void qore_python_exception_capsule_destructor(PyObject* capsule) {
    QoreHashNode* info = static_cast<QoreHashNode*>(PyCapsule_GetPointer(capsule,
        QORE_PYTHON_EXCEPTION_CAPSULE_NAME));
    assert(info);
    // releasing the exception can run Qore code (the destructors of objects in it), which needs a thread registered
    // with Qore and runs without the GIL; if the thread cannot be registered, the reference is not released
    QorePythonQoreEntryHelper qeh;
    if (!qeh.ok()) {
        return;
    }
    ExceptionSink xsink;
    ValueHolder holder(info, &xsink);
    qore_python_release(holder);
}

PyObject* QorePythonProgram::getOriginalPythonException(ExceptionSink& xsink) {
    QoreValue arg(xsink.getExceptionArg());
    if (arg.getType() != NT_OBJECT) {
        return nullptr;
    }
    QoreObject* obj = const_cast<QoreObject*>(arg.get<const QoreObject>());
    // the private data of an object that has been deleted is not available: the exception is not raised again
    ExceptionSink xsink2;
    TryPrivateDataRefHolder<QorePythonPrivateData> pypd(obj, CID_PYTHONBASEOBJECT, &xsink2);
    if (!pypd || !sharesInterpreter(pypd->getOwner())) {
        xsink2.clear();
        return nullptr;
    }
    PyObject* ex = pypd->get();
    if (!PyExceptionInstance_Check(ex)) {
        return nullptr;
    }
    // the exception is raised again only if the Qore exception has the name of its class: Qore code can raise
    // another exception with a Python exception as its argument
    QoreValue err(xsink.getExceptionErr());
    if (err.getType() != NT_STRING
        || qore_python_get_exception_name(Py_TYPE(ex)) != err.get<const QoreStringNode>()->c_str()) {
        return nullptr;
    }
    Py_INCREF(ex);
    return ex;
}

PyObject* QorePythonProgram::getPythonQoreException(const QoreHashNode* info, int depth) {
    QorePythonReferenceHolder type(qore_python_get_qore_exception_type());
    if (!type) {
        return nullptr;
    }

    // converts a value to Python; returns nullptr with the reason in "error" if it cannot be converted
    auto get_value = [this](QoreValue v, std::string& error) -> PyObject* {
        ExceptionSink xsink;
        PyObject* rv = getPythonValue(v, &xsink);
        if (!rv) {
            PyErr_Clear();
            QoreStringValueHelper err(xsink.getExceptionErr());
            QoreStringValueHelper desc(xsink.getExceptionDesc());
            error = err->c_str();
            error += ": ";
            error += desc->c_str();
            xsink.clear();
        }
        return rv;
    };
    // converts err or desc to Python; a value that cannot be converted is passed as its string
    auto get_string_value = [&get_value](QoreValue v) -> PyObject* {
        std::string error;
        PyObject* rv = get_value(v, error);
        if (!rv) {
            QoreStringValueHelper str(v);
            rv = PyUnicode_FromStringAndSize(str->c_str(), str->size());
        }
        return rv;
    };

    // the argument; if it cannot be converted, it is None, and the reason is added to the description
    std::string arg_error;
    QorePythonReferenceHolder py_arg;
    QoreValue arg = info->getKeyValue("arg");
    if (arg) {
        py_arg = get_value(arg, arg_error);
        if (!py_arg) {
            Py_INCREF(Py_None);
            py_arg = Py_None;
        }
    }

    QorePythonReferenceHolder args(PyTuple_New(arg ? 3 : 2));
    if (!args) {
        return nullptr;
    }
    PyObject* val = get_string_value(info->getKeyValue("err"));
    if (!val) {
        return nullptr;
    }
    PyTuple_SET_ITEM(*args, 0, val);
    QoreValue desc = info->getKeyValue("desc");
    if (!arg_error.empty()) {
        QoreStringValueHelper desc_str(desc);
        std::string str = desc_str->c_str();
        str += " (the exception argument cannot be converted to Python: " + arg_error + ")";
        val = PyUnicode_FromStringAndSize(str.c_str(), str.size());
    } else if (desc) {
        val = get_string_value(desc);
    } else {
        Py_INCREF(Py_None);
        val = Py_None;
    }
    if (!val) {
        return nullptr;
    }
    PyTuple_SET_ITEM(*args, 1, val);
    if (arg) {
        PyTuple_SET_ITEM(*args, 2, py_arg.release());
    }

    QorePythonReferenceHolder ex(PyObject_Call(*type, *args, nullptr));
    if (!ex) {
        return nullptr;
    }

    // the location of the exception and its call stack
    for (const char* key : {"file", "line", "endline", "source", "offset", "lang", "callstack"}) {
        QoreValue v = info->getKeyValue(key);
        if (!v) {
            continue;
        }
        // the values of the location and the call stack are strings, integers, and lists of hashes of them
        std::string error;
        QorePythonReferenceHolder pyval(get_value(v, error));
        if (!pyval) {
            PyErr_Format(PyExc_RuntimeError, "cannot convert the Qore exception's '%s' to Python: %s", key,
                error.c_str());
            return nullptr;
        }
        if (PyObject_SetAttrString(*ex, key, *pyval) < 0) {
            return nullptr;
        }
    }

    // the Qore exception is raised again in Qore when the Python exception is
    QorePythonReferenceHolder capsule(PyCapsule_New(info->refSelf(), QORE_PYTHON_EXCEPTION_CAPSULE_NAME,
        qore_python_exception_capsule_destructor));
    if (!capsule) {
        // the capsule does not own the reference
        ExceptionSink xsink;
        ValueHolder holder(const_cast<QoreHashNode*>(info), &xsink);
        qore_python_release(holder);
        return nullptr;
    }
    if (PyObject_SetAttrString(*ex, QORE_PYTHON_EXCEPTION_CAPSULE_ATTR, *capsule) < 0) {
        return nullptr;
    }

    // the exception chained to the Qore exception is the cause of the Python exception
    QoreValue next = info->getKeyValue("next");
    if (next.getType() == NT_HASH && depth < QORE_PYTHON_MAX_CHAINED_EXCEPTIONS) {
        PyObject* cause = getPythonQoreException(next.get<const QoreHashNode>(), depth + 1);
        if (!cause) {
            return nullptr;
        }
        // steals the reference
        PyException_SetCause(*ex, cause);
    }
    return ex.release();
}

void QorePythonProgram::raisePythonException(ExceptionSink& xsink) {
    assert(xsink);
    // a Qore call of the main thread cancelled by a signal: the exception raised by the signal's Python handler (ex:
    // KeyboardInterrupt) replaces the Qore exception (THREAD-CANCELLED)
    if (QorePythonSignalWakeup::checkSignal()) {
        ExceptionSink xsink2;
        // the Qore exception is consumed; releasing it can run Qore code, which runs without the GIL
        ValueHolder info(xsink.getExceptionInfo(), &xsink2);
        qore_python_release(info);
        return;
    }
    // the exception is raised in the interpreter of the current thread, whose program converts the values
    QorePythonProgram* pypgm = getContext();
    if (!pypgm || !pypgm->interpreter || pypgm->interpreter != qore_python_get_interpreter(PyThreadState_Get())) {
        pypgm = this;
    }

    // the thread_exit statement in Qore code called from Python leaves no exception; Python code cannot exit the Qore
    // thread
    if (!xsink.isException()) {
        assert(xsink.isThreadExit());
        xsink.clear();
        PyErr_SetString(PyExc_RuntimeError, "the thread_exit statement cannot be used in Qore code called from "
            "Python");
        return;
    }

    // a Python exception that was raised in Qore is raised again
    QorePythonReferenceHolder ex(pypgm->getOriginalPythonException(xsink));
    ExceptionSink xsink2;
    // the Qore exception is consumed; releasing it can run Qore code, which runs without the GIL
    ValueHolder info(xsink.getExceptionInfo(), &xsink2);
    if (!ex) {
        assert(info->getType() == NT_HASH);
        ex = pypgm->getPythonQoreException(info->get<const QoreHashNode>(), 0);
    }
    qore_python_release(info);
    // if the exception could not be created, the Python exception that describes the error is raised
    if (ex) {
        PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(*ex)), *ex);
    }
    assert(PyErr_Occurred());
}

// import Qore definitions to Python
void QorePythonProgram::importQoreToPython(PyObject* mod, const QoreNamespace& ns, const char* mod_name) {
    //printd(5, "QorePythonProgram::importQoreToPython() mod: %p (%d) ns: %p '%s' mod name: '%s'\n", mod,
    //  Py_REFCNT(mod), &ns, ns.getName(), mod_name);

    // import all functions
    QoreNamespaceFunctionIterator fi(ns);
    while (fi.next()) {
        const QoreExternalFunction& func = fi.get();
        // do not import deprecated functions
        if (func.getCodeFlags() & QCF_DEPRECATED) {
            continue;
        }
        if (importQoreFunctionToPython(mod, func)) {
            return;
        }
    }

    // import all constants
    QoreNamespaceConstantIterator consti(ns);
    while (consti.next()) {
        if (importQoreConstantToPython(mod, consti.get())) {
            return;
        }
    }

    // import all enums as enum classes
    QoreNamespaceEnumIterator ei(ns);
    while (ei.next()) {
        if (importQoreEnumToPython(mod, ei.get())) {
            return;
        }
    }

    // import all hashdecls as dict subclasses
    QoreNamespaceTypedHashIterator hi(ns);
    while (hi.next()) {
        const TypedHashDecl& hd = hi.get();
        ExceptionSink xsink;
        PyObject* type = getHashDeclType(&xsink, &hd);
        if (!type) {
            raisePythonException(xsink);
            return;
        }
        if (PyObject_SetAttrString(mod, hd.getName(), type) < 0) {
            return;
        }
    }

    // import all classes
    QoreNamespaceClassIterator clsi(ns);
    while (clsi.next()) {
        if (importQoreClassToPython(mod, clsi.get(), mod_name)) {
            return;
        }
    }

    // import all subnamespaces as modules
    QoreNamespaceNamespaceIterator ni(ns);
    while (ni.next()) {
        // the namespace of an enum's members is the enum class
        if (ns.findLocalEnum(ni.get().getName())) {
            continue;
        }
        if (importQoreNamespaceToPython(mod, ni.get())) {
            return;
        }
    }
}

int QorePythonProgram::importQoreConstantToPython(PyObject* mod, const QoreExternalConstant& constant) {
    //printd(5, "QorePythonProgram::importQoreConstantToPython() %s\n", constant.getName());

    ExceptionSink xsink;
    ValueHolder qoreval(constant.getReferencedValue(), &xsink);
    QorePythonReferenceHolder val;
    if (!xsink) {
        val = getPythonValue(*qoreval, &xsink);
    }
    // a constant that cannot be read or converted to Python is skipped, as with class constants, so that the rest of
    // the namespace is imported
    if (xsink) {
        xsink.clear();
        return 0;
    }
    if (!PyObject_SetAttrString(mod, constant.getName(), *val)) {
        return 0;
    }
    return -1;
}

// the attribute of the Python enum classes of Qore enums that holds the enum in a capsule
static constexpr const char* QORE_PYTHON_ENUM_ATTR = "_qore_enum_";

int QorePythonProgram::importQoreEnumToPython(PyObject* mod, const QoreEnumDecl& ed) {
    ExceptionSink xsink;
    PyObject* cls = getEnumType(&xsink, &ed);
    if (!cls) {
        raisePythonException(xsink);
        return -1;
    }
    return PyObject_SetAttrString(mod, ed.getName(), cls) < 0 ? -1 : 0;
}

PyObject* QorePythonProgram::getEnumType(ExceptionSink* xsink, const QoreEnumDecl* ed) {
    // a stale type and its handle are released after the lock, as the handle's destructor takes another lock
    QorePythonReferenceHolder stale;
    std::shared_ptr<QorePythonDeclHandle> stale_handle;
    {
        AutoLocker al(qpy_subclass_type_lock);
        enum_type_map_t::iterator i = enum_type_map.find(ed);
        if (i != enum_type_map.end()) {
            if (i->second.handle->isValid()) {
                return i->second.type;
            }
            // the enum at this address was freed with its Program
            stale = i->second.type;
            stale_handle = std::move(i->second.handle);
            enum_type_map.erase(i);
        }
    }

    // the members as (name, value) pairs; an enum whose values are all integers is an enum.IntEnum, all strings an
    // enum.StrEnum, so that its members compare equal to their values, otherwise an enum.Enum
    QorePythonReferenceHolder members(PyList_New(0));
    if (!members) {
        checkPythonException(xsink);
        return nullptr;
    }
    bool all_int = true, all_str = true;
    QoreEnumMemberIterator i(*ed);
    while (i.next()) {
        const QoreEnumMember* m = i.getMember();
        QorePythonReferenceHolder val(getPythonValue(m->getValue(), xsink));
        if (!val) {
            return nullptr;
        }
        all_int = all_int && PyLong_CheckExact(*val);
        all_str = all_str && PyUnicode_CheckExact(*val);
        QorePythonReferenceHolder pair(Py_BuildValue("(sO)", m->getName(), *val));
        if (!pair || PyList_Append(*members, *pair) < 0) {
            checkPythonException(xsink);
            return nullptr;
        }
    }
    const char* cls_name = PyList_GET_SIZE(*members) && all_int ? "IntEnum"
        : (PyList_GET_SIZE(*members) && all_str ? "StrEnum" : "Enum");
    QorePythonReferenceHolder enum_mod(PyImport_ImportModule("enum"));
    QorePythonReferenceHolder enum_cls(enum_mod ? PyObject_GetAttrString(*enum_mod, cls_name) : nullptr);
    QorePythonReferenceHolder args(Py_BuildValue("(sO)", ed->getName(), *members));
    QorePythonReferenceHolder kwargs(Py_BuildValue("{s:s}", "module", "qore.__root__"));
    QorePythonReferenceHolder cls(enum_cls && args && kwargs ? PyObject_Call(*enum_cls, *args, *kwargs) : nullptr);
    std::shared_ptr<QorePythonDeclHandle> handle = QorePythonDeclHandle::create(ed, ed->getProgram(),
        QorePythonDeclHandle::ENUM, ed->getName());
    QorePythonReferenceHolder capsule(cls ? QorePythonDeclHandle::newCapsule(handle) : nullptr);
    if (!capsule || PyObject_SetAttrString(*cls, QORE_PYTHON_ENUM_ATTR, *capsule) < 0) {
        checkPythonException(xsink);
        return nullptr;
    }

    AutoLocker al(qpy_subclass_type_lock);
    // another thread can have created the class meanwhile; the map keeps the first one
    std::pair<enum_type_map_t::iterator, bool> rv = enum_type_map.insert(enum_type_map_t::value_type(ed,
        decl_type_t{*cls, handle}));
    if (rv.second) {
        cls.release();
    }
    return rv.first->second.type;
}

static PyMethodDef qore_hashdecl_init_def = {"__init__", reinterpret_cast<PyCFunction>(
    reinterpret_cast<void(*)(void)>(QorePythonProgram::hashDeclInit)), METH_VARARGS | METH_KEYWORDS,
    "sets the default values of the hashdecl's keys, then the arguments as dict.update()"};

// the attribute of the Python types of hashdecls that holds the hashdecl in a capsule
static constexpr const char* QORE_PYTHON_HASHDECL_ATTR = "_qore_hashdecl_";

PyObject* QorePythonProgram::getHashDeclType(ExceptionSink* xsink, const TypedHashDecl* hd) {
    // a stale type and its handle are released after the lock, as the handle's destructor takes another lock
    QorePythonReferenceHolder stale;
    std::shared_ptr<QorePythonDeclHandle> stale_handle;
    {
        AutoLocker al(qpy_subclass_type_lock);
        hashdecl_type_map_t::iterator i = hashdecl_type_map.find(hd);
        if (i != hashdecl_type_map.end()) {
            if (i->second.handle->isValid()) {
                return i->second.type;
            }
            // the hashdecl at this address was freed with its Program
            stale = i->second.type;
            stale_handle = std::move(i->second.handle);
            hashdecl_type_map.erase(i);
        }
    }

    std::shared_ptr<QorePythonDeclHandle> handle = QorePythonDeclHandle::create(hd, hd->getProgram(),
        QorePythonDeclHandle::HASHDECL, hd->getName());
    QorePythonReferenceHolder capsule(QorePythonDeclHandle::newCapsule(handle));
    QorePythonReferenceHolder func(capsule ? PyCFunction_NewEx(&qore_hashdecl_init_def, *capsule, nullptr)
        : nullptr);
    // an instance method: the object is passed as the first argument
    QorePythonReferenceHolder init(func ? PyInstanceMethod_New(*func) : nullptr);
    QorePythonReferenceHolder doc(PyUnicode_FromFormat("A hash of Qore hashdecl %s: a dict whose values are "
        "converted to the hashdecl when passed to Qore", hd->getName()));
    QorePythonReferenceHolder slots(PyTuple_New(0));
    QorePythonReferenceHolder dict(PyDict_New());
    QorePythonReferenceHolder module_name(PyUnicode_FromString("qore.__root__"));
    if (!init || !doc || !slots || !dict || !module_name
        || PyDict_SetItemString(*dict, "__init__", *init) < 0
        || PyDict_SetItemString(*dict, "__doc__", *doc) < 0
        || PyDict_SetItemString(*dict, "__slots__", *slots) < 0
        || PyDict_SetItemString(*dict, "__module__", *module_name) < 0
        || PyDict_SetItemString(*dict, QORE_PYTHON_HASHDECL_ATTR, *capsule) < 0) {
        checkPythonException(xsink);
        return nullptr;
    }
    QorePythonReferenceHolder type(PyObject_CallFunction(reinterpret_cast<PyObject*>(&PyType_Type), "s(O)O",
        hd->getName(), &PyDict_Type, *dict));
    if (!type) {
        checkPythonException(xsink);
        return nullptr;
    }

    AutoLocker al(qpy_subclass_type_lock);
    // another thread can have created the type meanwhile; the map keeps the first one
    std::pair<hashdecl_type_map_t::iterator, bool> rv = hashdecl_type_map.insert(
        hashdecl_type_map_t::value_type(hd, decl_type_t{*type, handle}));
    if (rv.second) {
        type.release();
    }
    return rv.first->second.type;
}

PyObject* QorePythonProgram::hashDeclInit(PyObject* capsule, PyObject* args, PyObject* kwargs) {
    QorePythonDeclHandle* handle = QorePythonDeclHandle::getCapsuleHandle(capsule);
    assert(handle);
    assert(PyTuple_Check(args));
    if (!PyTuple_GET_SIZE(args) || !PyDict_Check(PyTuple_GET_ITEM(args, 0))) {
        PyErr_Format(PyExc_TypeError, "%s.__init__() must be called with a dict", handle->getName().c_str());
        return nullptr;
    }
    PyObject* self = PyTuple_GET_ITEM(args, 0);

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    QorePythonProgram* pypgm = QorePythonProgram::getExecutionContext();
    ExceptionSink xsink;
    QoreExternalProgramContextHelper pch(&xsink, pypgm->getQoreProgram());
    if (!xsink) {
        // the default values of the keys; their initializers are Qore code, which runs without the GIL
        ValueHolder defaults(&xsink);
        {
            // the hashdecl is not freed while its default values are made
            QorePythonDeclUse use(handle);
            if (!use) {
                handle->raiseDeleted();
                return nullptr;
            }
            const TypedHashDecl* hd = static_cast<const TypedHashDecl*>(handle->getDecl());
            QorePythonReleaseGilHelper prgh;
            defaults = new QoreHashNode(hd, &xsink);
        }
        QorePythonReferenceHolder py_defaults;
        if (!xsink) {
            py_defaults = pypgm->getPythonDict(&xsink, defaults->get<const QoreHashNode>());
        }
        qore_python_release(defaults);
        if (!xsink) {
            if (PyDict_Update(self, *py_defaults) < 0) {
                return nullptr;
            }
            // the arguments, as dict.update()
            QorePythonReferenceHolder update(PyObject_GetAttrString(reinterpret_cast<PyObject*>(&PyDict_Type),
                "update"));
            QorePythonReferenceHolder rv(update ? PyObject_Call(*update, args, kwargs) : nullptr);
            if (!rv) {
                return nullptr;
            }
            Py_RETURN_NONE;
        }
    }
    pypgm->raisePythonException(xsink);
    return nullptr;
}

int QorePythonProgram::importQoreClassToPython(PyObject* mod, const QoreClass& cls, const char* mod_name) {
    PythonQoreClass* py_cls = findCreatePythonClass(cls, mod_name);
    if (!py_cls) {
        return -1;
    }
    //printd(5, "QorePythonProgram::importQoreClassToPython() py_cls: %p ('%s') cn: '%s' mod_name: '%s'\n", py_cls,
    //  py_cls->getPythonType()->tp_name, cls.getName(), mod_name);

    if (PyObject_SetAttrString(mod, cls.getName(), reinterpret_cast<PyObject*>(py_cls->getPythonType()))) {
        return -1;
    }
    return 0;
}

int QorePythonProgram::importQoreNamespaceToPython(PyObject* mod, const QoreNamespace& ns) {
    //printd(5, "QorePythonProgram::importQoreNamespaceToPython() %s\n", ns.getName());
    assert(PyModule_Check(mod));

    QoreStringMaker nsname("%s.%s", PyModule_GetName(mod), ns.getName());

    // create a submodule
    QorePythonReferenceHolder new_mod(newModule(nsname.c_str(), &ns));
    printd(5, "QorePythonProgram::importQoreNamespaceToPython() (mod) created new module '%s'\n", nsname.c_str());
    importQoreToPython(*new_mod, ns, nsname.c_str());
    if (PyObject_SetAttrString(mod, ns.getName(), *new_mod)) {
        return -1;
    }
    return 0;
}

PyObject* QorePythonProgram::newModule(const char* name, const QoreNamespace* ns_pkg) {
    //QorePythonReferenceHolder new_mod(PyModule_New(name));
    QorePythonReferenceHolder new_mod(ModuleNamespace_New(name, ns_pkg));
    if (ns_pkg) {
        // the module is a package with an empty __path__: its submodules are found by QoreMetaPathFinder, never in
        // the file system (the path finder searches the directories of __path__)
        QorePythonReferenceHolder path(PyList_New(0));
        if (!path || PyObject_SetAttrString(*new_mod, "__path__", *path) < 0) {
            PyErr_Clear();
        }
    }
    saveModule(name, *new_mod);
    return new_mod.release();
}

PyObject* QorePythonProgram::newPackageModule(const char* name) {
    QorePythonReferenceHolder new_mod(PyModule_New(name));
    // an empty __path__, as for the modules of Qore namespaces
    QorePythonReferenceHolder py_path(PyList_New(0));
    if (!py_path || PyObject_SetAttrString(*new_mod, "__path__", *py_path) < 0) {
        PyErr_Clear();
    }
    saveModule(name, *new_mod);
    return new_mod.release();
}

void QorePythonProgram::importQoreNamespaceToPython(const QoreNamespace& ns, const QoreString& py_mod_path,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::getCreateModule() '%s'\n", path);
    assert(!py_mod_path.empty());

    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return;
    }

    assert(module);
    // start with the root module
    module.py_ref();
    QorePythonReferenceHolder mod(*module);

    strvec_t strpath = get_dot_path_list(py_mod_path.c_str());
    assert(!strpath.empty());
    QoreString nspath;
    for (const std::string& str : strpath) {
        bool store = nspath.empty();
        if (!store) {
            nspath.concat('.');
        }
        nspath.concat(str);

        //printd(5, "QorePythonProgram::importQoreNamespaceToPython() '%s' str: '%s' mod: %p ('%s')\n",
        //  py_mod_path.c_str(), str.c_str(), *mod, Py_TYPE(*mod)->tp_name);
        if (PyObject_HasAttrString(*mod, str.c_str())) {
            QorePythonReferenceHolder new_mod(PyObject_GetAttrString(*mod, str.c_str()));
            if (PyModule_Check(*new_mod) || PyDict_Check(*new_mod)) {
                mod = new_mod.release();
                continue;
            }
            // WARNING: any existing attribute will be replaced with the new module
        }

        QorePythonReferenceHolder new_mod(newModule(nspath.c_str(), &ns));
        //printd(5, "QorePythonProgram::importQoreNamespaceToPython() created new module '%s' (store: %d)\n",
        //  nspath.c_str(), store);
        assert(new_mod);
        if (PyObject_SetAttrString(*mod, str.c_str(), *new_mod)) {
            if (!checkPythonException(xsink)) {
                xsink->raiseException("IMPORT-NS-ERROR", "could not set element '%s' when creating path '%s'",
                    str.c_str(), py_mod_path.c_str());
            }
            return;
        }
        mod = new_mod.release();
    }
    printd(5, "QorePythonProgram::importQoreNamespaceToPython() %s => %p ('%s': %s) mr: %d\n", py_mod_path.c_str(),
        *mod, Py_TYPE(*mod)->tp_name, ns.getName(), Py_REFCNT(*mod));
    assert(mod);
    importQoreToPython(*mod, ns, strpath.back().c_str());
    checkPythonException(xsink);
}

void QorePythonProgram::aliasDefinition(const QoreString& source_path, const QoreString& target_path) {
    ExceptionSink xsink;
    QorePythonHelper qph(this, &xsink);
    if (xsink || checkValid(&xsink)) {
        throw QoreXSinkException(xsink);
    }

    if (!module) {
        throw QoreStandardException("PYTHON-ALIAS-ERROR", "source path '%s' cannot be found as there is no code "
            "context", source_path.c_str());
    }

    // start with the root Dict
    module.py_ref();
    QorePythonReferenceHolder source_obj(*module);

    strvec_t strpath = get_dot_path_list(source_path.c_str());
    assert(!strpath.empty());
    for (size_t i = 0, e = strpath.size(); i < e; ++i) {
        const std::string& str = strpath[i];
        if (!PyObject_HasAttrString(*source_obj, str.c_str())) {
            throw QoreStandardException("PYTHON-ALIAS-ERROR", "source path '%s': element '%s' not found",
                source_path.c_str(), str.c_str());
        }
        source_obj = PyObject_GetAttrString(*source_obj, str.c_str());
        assert(source_obj);
    }

    // get / create target
    module.py_ref();
    QorePythonReferenceHolder obj(*module);
    strpath = get_dot_path_list(target_path.c_str());
    assert(!strpath.empty());
    QoreString nspath;
    for (size_t i = 0, e = strpath.size(); i < e; ++i) {
        const std::string& str = strpath[i];
        if (!nspath.empty()) {
            nspath.concat('.');
        }
        nspath.concat(str);

        //printd(5, "QorePythonProgram::aliasDefinition() '%s' str: '%s' obj: %p ('%s')\n", target_path.c_str(),
        //  str.c_str(), *obj, Py_TYPE(*obj)->tp_name);
        QorePythonReferenceHolder new_elem;
        if (i < (e - 1)) {
            if (PyObject_HasAttrString(*obj, str.c_str())) {
                obj = PyObject_GetAttrString(*obj, str.c_str());
                continue;
            }
            // create a new module, if the parent is a module, or a dictionary
            new_elem = newPackageModule(nspath.c_str());
            //printd(5, "QorePythonProgram::aliasDefinition() created module '%s'\n", nspath.c_str());
        } else {
            new_elem = source_obj.release();
            //printd(5, "QorePythonProgram::aliasDefinition() got elem %p %s (path %s)\n", *new_elem,
            //  Py_TYPE(*new_elem)->tp_name, nspath.c_str());
            // if aliasing a module, insert the new alias in sys.modules
            if (PyModule_Check(*new_elem)) {
                saveModule(nspath.c_str(), *new_elem);
            }
        }

        if (PyObject_SetAttrString(*obj, str.c_str(), *new_elem) < 0) {
            ExceptionSink xsink;
            if (!checkPythonException(&xsink)) {
                new QoreStandardException("PYTHON-ALIAS-ERROR", "could not set element '%s' when creating " \
                    "target path '%s' while aliasing source path '%s'", str.c_str(), target_path.c_str(),
                    source_path.c_str());
            }
            throw QoreXSinkException(xsink);
        }
        if (i == (e - 1)) {
            break;
        }
        obj = new_elem.release();
    }
}

void QorePythonProgram::exportClass(ExceptionSink* xsink, QoreString& arg) {
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return;
    }

    // start with the root module
    module.py_ref();
    QorePythonReferenceHolder obj(*module);

    strvec_t strpath = get_dot_path_list(arg.c_str());
    assert(!strpath.empty());
    for (const std::string& str : strpath) {
        //printd(5, "QorePythonProgram::exportClass() '%s' str: '%s' mod: %p ('%s')\n", arg.c_str(), str.c_str(), mod,
        //  Py_TYPE(mod)->tp_name);
        if (!PyObject_HasAttrString(*obj, str.c_str())) {
            xsink->raiseException("EXPORT-CLASS-ERROR", "could find component '%s' in path '%s'", str.c_str(),
                arg.c_str());
            return;
        }
        obj = PyObject_GetAttrString(*obj, str.c_str());
    }

    if (!PyType_Check(*obj)) {
        xsink->raiseException("EXPORT-CLASS-ERROR", "path '%s' is not a class; got type '%s' instead", arg.c_str(),
            Py_TYPE(*obj)->tp_name);
        return;
    }

    PyTypeObject* type = reinterpret_cast<PyTypeObject*>(*obj);
    clmap_t::iterator i = clmap.lower_bound(type);
    if (i != clmap.end() && i->first == type) {
        xsink->raiseException("EXPORT-CLASS-ERROR", "Qore class for Python path '%s' already exists", arg.c_str());
        return;
    }

    qore_offset_t ci = arg.rfind('.');

    //QoreExternalProgramContextHelper pch(xsink, qpgm);
    // grab current Program's parse lock before manipulating namespaces
    CurrentProgramRuntimeExternalParseContextHelper pch;

    QoreNamespace* ns = qpgm->getRootNS();

    if (ci > 0) {
        QoreString ns_str(arg);
        ns_str.terminate(ci);
        ns_str.replaceAll(".", "::");
        ns = ns->findCreateNamespacePathAll(ns_str.c_str());
    }

    strset_t nsset;
    addClassToNamespaceIntern(xsink, ns, (PyTypeObject*)*obj, strpath.back().c_str(), i, nsset);
}

void QorePythonProgram::addModulePath(ExceptionSink* xsink, QoreString& arg) {
    q_env_subst(arg);
    printd(5, "QorePythonProgram::addModulePath() this: %p arg: '%s'\n", this, arg.c_str());

    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return;
    }

    QorePythonReferenceHolder sys(PyImport_ImportModule("sys"));
    if (!sys) {
        if (!checkPythonException(xsink)) {
            throw QoreStandardException("PYTHON-ERROR", "cannot load 'sys' module");
        }
        return;
    }

    QorePythonReferenceHolder path;

    if (!PyObject_HasAttrString(*sys, "path")) {
        path = PyList_New(0);
    } else {
        path = PyObject_GetAttrString(*sys, "path");
        if (!PyList_Check(*path)) {
            throw QoreStandardException("PYTHON-ERROR", "'sys.path' is not a list; got type '%s' instead",
                Py_TYPE(*path)->tp_name);
        }
    }
    QorePythonReferenceHolder item(PyUnicode_FromStringAndSize(arg.c_str(), arg.size()));
    PyList_Append(*path, *item);
}

void QorePythonProgram::exportFunction(ExceptionSink* xsink, QoreString& arg) {
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return;
    }

    // start with the root module
    module.py_ref();
    QorePythonReferenceHolder obj(*module);

    strvec_t strpath = get_dot_path_list(arg.c_str());
    assert(!strpath.empty());
    for (const std::string& str : strpath) {
        //printd(5, "QorePythonProgram::exportClass() '%s' str: '%s' mod: %p ('%s')\n", arg.c_str(), str.c_str(), mod,
        //  Py_TYPE(mod)->tp_name);
        if (!PyObject_HasAttrString(*obj, str.c_str())) {
            xsink->raiseException("EXPORT-FUNCTION-ERROR", "could find component '%s' in path '%s'", str.c_str(),
                arg.c_str());
            return;
        }
        obj = PyObject_GetAttrString(*obj, str.c_str());
    }

    q_external_func_t qore_func = nullptr;
    if (PyFunction_Check(*obj)) {
        qore_func = (q_external_func_t)QorePythonProgram::execPythonFunction;
    } else if (PyCFunction_Check(*obj)) {
        qore_func = (q_external_func_t)QorePythonProgram::execPythonCFunction;
    } else {
        xsink->raiseException("EXPORT-FUNCTION-ERROR", "path '%s' is not a function; got type '%s' instead",
            arg.c_str(), Py_TYPE(*obj)->tp_name);
        return;
    }

    flmap_t::iterator i = flmap.lower_bound(*obj);
    if (i != flmap.end() && i->first == *obj) {
        xsink->raiseException("EXPORT-FUNCTION-ERROR", "Qore function for Python path '%s' already exists",
            arg.c_str());
        return;
    }

    qore_offset_t ci = arg.rfind('.');

    //QoreExternalProgramContextHelper pch(xsink, qpgm);
    // grab current Program's parse lock before manipulating namespaces
    CurrentProgramRuntimeExternalParseContextHelper pch;

    QoreNamespace* ns = qpgm->getRootNS();

    if (ci > 0) {
        QoreString ns_str(arg);
        ns_str.terminate(ci);
        ns_str.replaceAll(".", "::");
        ns = ns->findCreateNamespacePathAll(ns_str.c_str());
    }

    const char* func_name = strpath.back().c_str();
    if (findCreateQoreFunction(*obj, func_name, qore_func)) {
        xsink->raiseException("EXPORT-FUNCTION-ERROR", "Qore function for Python path '%s' already exists",
            arg.c_str());
    }
}

QoreListNode* QorePythonProgram::getQoreListFromList(ExceptionSink* xsink, PyObject* val) {
    pyobj_set_t rset;
    return getQoreListFromList(xsink, val, rset);
}

namespace {
//! keeps a Python container on the path of a conversion to Qore while its elements are converted
/** A container that is already on the path is part of a cycle, which a Qore value cannot represent; a container that
    is referenced more than once without a cycle is converted each time.
*/
class QorePythonConversionPathHelper {
public:
    //! adds the container to the conversion path; a container nested too deeply for the stack raises RecursionError
    /** Python's recursion guard limits the depth by the stack that the thread has left (see setContext()), so that
        the recursive conversion does not overflow the stack
    */
    DLLLOCAL QorePythonConversionPathHelper(std::set<PyObject*>& path, PyObject* val)
            : path(path), val(val), cycle(!path.insert(val).second),
            too_deep(!cycle && Py_EnterRecursiveCall(" while converting a Python value to Qore")) {
    }

    DLLLOCAL ~QorePythonConversionPathHelper() {
        if (!cycle) {
            if (!too_deep) {
                Py_LeaveRecursiveCall();
            }
            path.erase(val);
        }
    }

    //! returns true and raises a Qore exception if the container is part of a cycle or nested too deeply
    /** the RecursionError is not converted: no Python code can run at this depth (ex: str() of the exception)
    */
    DLLLOCAL bool checkCycle(QorePythonProgram* pypgm, ExceptionSink* xsink) const {
        if (too_deep) {
            PyErr_Clear();
            if (xsink) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Python value of type '%s' that is nested "
                    "too deeply for the stack of the thread to a Qore value", Py_TYPE(val)->tp_name);
            }
            return true;
        }
        if (cycle) {
            if (xsink) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Python value of type '%s' that contains "
                    "itself to a Qore value", Py_TYPE(val)->tp_name);
            }
            return true;
        }
        // Qore's limit of the thread's stack, which also accounts for stack that Python does not know about (ex: the
        // guard zones of a JVM loaded in the process)
        ExceptionSink stack_xsink;
        if (q_check_stack(xsink ? xsink : &stack_xsink)) {
            stack_xsink.clear();
            return true;
        }
        return false;
    }

private:
    std::set<PyObject*>& path;
    PyObject* val;
    bool cycle;
    bool too_deep;
};
}

namespace {
//! limits the depth of the recursive conversion of Qore containers to Python by the stack that the thread has left
/** with Python's recursion guard, as QorePythonConversionPathHelper does for the conversion to Qore; a container
    nested too deeply raises a PYTHON-VALUE-ERROR exception (the RecursionError is not converted: no Python code can
    run at this depth)
*/
class QorePythonRecursionGuard {
public:
    DLLLOCAL QorePythonRecursionGuard(const QoreValue& val, ExceptionSink* xsink)
            : too_deep(Py_EnterRecursiveCall(" while converting a Qore value to Python")) {
        if (too_deep) {
            PyErr_Clear();
            if (xsink) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Qore value of type '%s' that is nested "
                    "too deeply for the stack of the thread to a Python value", val.getTypeName());
            }
            return;
        }
        // Qore's limit of the thread's stack, which also accounts for stack that Python does not know about (ex: the
        // guard zones of a JVM loaded in the process)
        ExceptionSink stack_xsink;
        stack_exceeded = q_check_stack(xsink ? xsink : &stack_xsink);
        stack_xsink.clear();
    }

    DLLLOCAL ~QorePythonRecursionGuard() {
        if (!too_deep) {
            Py_LeaveRecursiveCall();
        }
    }

    //! returns true if the value is nested too deeply, with a Qore exception raised
    DLLLOCAL bool tooDeep() const {
        return too_deep || stack_exceeded;
    }

private:
    bool too_deep;
    bool stack_exceeded = false;
};
}

//! converts the pending Python exception to a Qore exception, or clears it if there is no ExceptionSink
static void qore_python_conversion_error(QorePythonProgram* pypgm, ExceptionSink* xsink) {
    if (xsink) {
        pypgm->checkPythonException(xsink);
    } else {
        PyErr_Clear();
    }
}

//! returns a new reference to the item of a list at the given index
/** In free-threaded builds, other threads can change a list while it is converted and a borrowed item can be freed,
    so a strong reference is taken atomically.  With the GIL, no other thread runs while the size and the item are
    read.

    @return a new reference, or nullptr if the index is past the end of the list (with no Python exception set)
*/
static PyObject* qore_python_list_get_item(PyObject* list, Py_ssize_t i) {
#ifdef Py_GIL_DISABLED
    PyObject* rv = PyList_GetItemRef(list, i);
    if (!rv) {
        // IndexError: another thread made the list shorter
        PyErr_Clear();
    }
    return rv;
#else
    return i < PyList_GET_SIZE(list) ? Py_NewRef(PyList_GET_ITEM(list, i)) : nullptr;
#endif
}

//! returns a new reference to the dict to iterate with PyDict_Next()
/** In free-threaded builds, PyDict_Next() is not safe while other threads change the dict; a copy is iterated there,
    which is made atomically.  With the GIL, the dict itself is iterated.

    @return a new reference, or nullptr with a Python exception set
*/
static PyObject* qore_python_dict_for_iteration(PyObject* dict) {
#ifdef Py_GIL_DISABLED
    return PyDict_Copy(dict);
#else
    return Py_NewRef(dict);
#endif
}

QoreListNode* QorePythonProgram::getQoreListFromList(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset) {
    assert(PyList_Check(val));
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(this, xsink)) {
        return nullptr;
    }
    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
    // the size is checked for each element and each element is referenced while it is converted: converting a value
    // can run Python code (ex: utcoffset() of a tzinfo) that changes the list
    for (Py_ssize_t i = 0; ; ++i) {
        QorePythonReferenceHolder item(qore_python_list_get_item(val, i));
        if (!item) {
            break;
        }
        ValueHolder qval(getQoreValue(xsink, *item, rset), xsink);
        if (*xsink) {
            return nullptr;
        }
        rv->push(qval.release(), xsink);
        assert(!*xsink);
    }
    return rv.release();
}

QoreListNode* QorePythonProgram::getQoreListFromTuple(ExceptionSink* xsink, PyObject* val, size_t offset,
    bool for_args) {
    pyobj_set_t rset;
    return getQoreListFromTupleElements(xsink, val, rset, offset, for_args);
}

QoreListNode* QorePythonProgram::getQoreListFromTuple(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset,
    size_t offset, bool for_args) {
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(this, xsink)) {
        return nullptr;
    }
    return getQoreListFromTupleElements(xsink, val, rset, offset, for_args);
}

QoreListNode* QorePythonProgram::getQoreListFromTupleElements(ExceptionSink* xsink, PyObject* val,
    pyobj_set_t& rset, size_t offset, bool for_args) {
    assert(PyTuple_Check(val));
    Py_ssize_t len = PyTuple_Size(val);
    if (for_args && !len) {
        return nullptr;
    }
    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
    for (Py_ssize_t i = offset; i < len; ++i) {
        ValueHolder qval(getQoreValue(xsink, PyTuple_GetItem(val, i), rset), xsink);
        if (*xsink || checkPythonException(xsink)) {
            return nullptr;
        }
        rv->push(qval.release(), xsink);
        assert(!*xsink);
    }
    return rv.release();
}

QoreHashNode* QorePythonProgram::getQoreHashFromNamedTuple(ExceptionSink* xsink, PyObject* val, PyObject* fields,
        pyobj_set_t& rset) {
    assert(PyTuple_Check(val) && PyTuple_Check(fields) && PyTuple_Size(val) == PyTuple_Size(fields));
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(this, xsink)) {
        return nullptr;
    }
    ReferenceHolder<QoreHashNode> rv(new QoreHashNode(autoTypeInfo), xsink);
    for (Py_ssize_t i = 0, e = PyTuple_Size(val); i < e; ++i) {
        const char* field = PyUnicode_AsUTF8(PyTuple_GET_ITEM(fields, i));
        if (!field) {
            qore_python_conversion_error(this, xsink);
            return nullptr;
        }
        ValueHolder qval(getQoreValue(xsink, PyTuple_GET_ITEM(val, i), rset), xsink);
        if (*xsink) {
            return nullptr;
        }
        rv->setKeyValue(field, qval.release(), xsink);
    }
    return rv.release();
}

QoreListNode* QorePythonProgram::getQoreListFromIterable(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset) {
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(this, xsink)) {
        return nullptr;
    }
    QorePythonReferenceHolder iter(PyObject_GetIter(val));
    if (!iter) {
        qore_python_conversion_error(this, xsink);
        return nullptr;
    }
    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
    while (true) {
        QorePythonReferenceHolder item(PyIter_Next(*iter));
        if (!item) {
            if (PyErr_Occurred()) {
                qore_python_conversion_error(this, xsink);
                return nullptr;
            }
            break;
        }
        ValueHolder qval(getQoreValue(xsink, *item, rset), xsink);
        if (*xsink) {
            return nullptr;
        }
        rv->push(qval.release(), xsink);
    }
    return rv.release();
}

QoreHashNode* QorePythonProgram::getQoreHashFromDict(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset) {
    assert(PyDict_Check(val));
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(this, xsink)) {
        return nullptr;
    }

    QorePythonReferenceHolder dict(qore_python_dict_for_iteration(val));
    if (!dict) {
        qore_python_conversion_error(this, xsink);
        return nullptr;
    }
    ReferenceHolder<QoreHashNode> rv(new QoreHashNode(autoTypeInfo), xsink);

    PyObject* borrowed_key, * borrowed_value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(*dict, &pos, &borrowed_key, &borrowed_value)) {
        // the key and the value are referenced while they are converted: converting a value can run Python code that
        // changes the dict
        QorePythonReferenceHolder key(borrowed_key);
        key.py_ref();
        QorePythonReferenceHolder value(borrowed_value);
        value.py_ref();
        const char* keystr;
        QorePythonReferenceHolder tkey;
        // str subclasses (ex: StrEnum members) are strings
        if (PyUnicode_Check(*key)) {
            keystr = PyUnicode_AsUTF8(*key);
        } else {
            tkey = PyObject_Repr(*key);
            keystr = tkey ? PyUnicode_AsUTF8(*tkey) : nullptr;
        }
        // ex: a key with lone surrogates, which cannot be encoded in UTF-8
        if (!keystr) {
            qore_python_conversion_error(this, xsink);
            return nullptr;
        }

        // a key that is not a string is converted with repr(), which can give the same string as another key (ex: 1
        // and '1'); one value would replace the other (string keys are unique, so an existing key is a collision)
        if (rv->existsKey(keystr)) {
            if (xsink) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Python dict to a Qore hash: the key "
                    "'%s' (of type '%s') gives the same Qore hash key as another key of the dict", keystr,
                    Py_TYPE(*key)->tp_name);
            }
            return nullptr;
        }

        // keystr belongs to key or tkey, which are referenced until the value is set
        ValueHolder qval(getQoreValue(xsink, *value, rset), xsink);
        if (*xsink) {
            return nullptr;
        }
        rv->setKeyValue(keystr, qval.release(), xsink);
    }
    return rv.release();
}

BinaryNode* QorePythonProgram::getQoreBinaryFromBytes(PyObject* val) {
    assert(PyBytes_Check(val));
    SimpleRefHolder<BinaryNode> rv(new BinaryNode);
    rv->append(PyBytes_AS_STRING(val), PyBytes_GET_SIZE(val));
    return rv.release();
}

BinaryNode* QorePythonProgram::getQoreBinaryFromByteArray(PyObject* val) {
    assert(PyByteArray_Check(val));
    SimpleRefHolder<BinaryNode> rv(new BinaryNode);
    // in free-threaded builds, another thread can resize the bytearray while it is copied; the copy runs no Python
    // code
#ifdef Py_GIL_DISABLED
    Py_BEGIN_CRITICAL_SECTION(val);
#endif
    rv->append(PyByteArray_AsString(val), PyByteArray_Size(val));
#ifdef Py_GIL_DISABLED
    Py_END_CRITICAL_SECTION();
#endif
    return rv.release();
}

DateTimeNode* QorePythonProgram::getQoreDateTimeFromDelta(PyObject* val) {
    assert(PyDelta_Check(val));
    // datetime.timedelta keeps its seconds and microseconds positive (-1 second is days=-1, seconds=86399); the Qore
    // value is made of days, hours, minutes, seconds and microseconds that all have the sign of the value, which
    // compares equal to the same value made from other units (a relative date with components of different signs
    // does not); each component fits an int for every timedelta (at most 999999999 days)
    int64 secs = static_cast<int64>(PyDateTime_DELTA_GET_DAYS(val)) * 86400 + PyDateTime_DELTA_GET_SECONDS(val);
    int64 us = PyDateTime_DELTA_GET_MICROSECONDS(val);
    bool neg = secs < 0;
    if (neg) {
        secs = -secs;
        if (us) {
            --secs;
            us = 1000000 - us;
        }
    }
    int sign = neg ? -1 : 1;
    return DateTimeNode::makeRelative(0, 0, sign * static_cast<int>(secs / 86400),
        sign * static_cast<int>((secs % 86400) / 3600), sign * static_cast<int>((secs % 3600) / 60),
        sign * static_cast<int>(secs % 60), sign * static_cast<int>(us));
}

DateTimeNode* QorePythonProgram::getQoreDateTimeFromDateTime(ExceptionSink* xsink, PyObject* val) {
    assert(PyDateTime_Check(val));

    // a naive datetime is in the current time zone; nullptr is UTC
    const AbstractQoreZoneInfo* zone = currentTZ();

    // Use the PyDateTime_DATE_GET_TZINFO() macro to directly access the tzinfo field.
    //
    // CRITICAL: In Python 3.13+ with sub-interpreters, PyObject_HasAttrString() can SIGSEGV because:
    // 1. PyDateTimeAPI is a per-interpreter global pointer initialized by PyDateTime_IMPORT
    // 2. When JNI or other external modules are loaded, they may initialize their own Python
    //    sub-interpreter context, leaving our PyDateTimeAPI pointing to stale type objects
    // 3. PyObject_HasAttrString() internally does type lookups using the stale PyDateTimeAPI
    // 4. This causes segfaults when accessing freed memory
    //
    // The PyDateTime_DATE_GET_TZINFO() macro directly accesses the datetime struct's tzinfo
    // field without using PyDateTimeAPI, making it safe across interpreter boundaries.
    PyObject* tzinfo = PyDateTime_DATE_GET_TZINFO(val);
    if (tzinfo && tzinfo != Py_None && PyTZInfo_Check(tzinfo)) {
        // datetime.utcoffset() checks the result of tzinfo.utcoffset(): a timedelta strictly between -24 and 24 hours
        // or None (TypeError or ValueError otherwise), as for datetime.time
        QorePythonReferenceHolder delta(PyObject_CallMethod(val, "utcoffset", nullptr));
        if (!delta) {
            qore_python_conversion_error(this, xsink);
            return nullptr;
        }
        if (*delta != Py_None && getQoreOffsetZone(xsink, *delta, zone)) {
            return nullptr;
        }
    }

    return DateTimeNode::makeAbsolute(zone, PyDateTime_GET_YEAR(val), PyDateTime_GET_MONTH(val),
        PyDateTime_GET_DAY(val), PyDateTime_DATE_GET_HOUR(val), PyDateTime_DATE_GET_MINUTE(val),
        PyDateTime_DATE_GET_SECOND(val), PyDateTime_DATE_GET_MICROSECOND(val));
}

DateTimeNode* QorePythonProgram::getQoreDateTimeFromDate(PyObject* val) {
    assert(PyDate_Check(val));
    return DateTimeNode::makeAbsolute(currentTZ(), PyDateTime_GET_YEAR(val), PyDateTime_GET_MONTH(val),
        PyDateTime_GET_DAY(val), 0, 0, 0, 0);
}

int QorePythonProgram::getQoreOffsetZone(ExceptionSink* xsink, PyObject* offset, const AbstractQoreZoneInfo*& zone) {
    assert(PyDelta_Check(offset));
    // the offset is a timedelta strictly between -24 and 24 hours; its days are -1 for negative offsets
    int secs = PyDateTime_DELTA_GET_DAYS(offset) * 86400 + PyDateTime_DELTA_GET_SECONDS(offset);
    if (PyDateTime_DELTA_GET_MICROSECONDS(offset)) {
        if (xsink) {
            xsink->raiseException("PYTHON-DATETIME-ERROR", "cannot convert a UTC offset with microseconds (%d seconds "
                "and %d microseconds) to a Qore time zone, which has an offset in seconds", secs,
                PyDateTime_DELTA_GET_MICROSECONDS(offset));
        }
        return -1;
    }
    // nullptr for UTC
    zone = findCreateOffsetZone(secs);
    return 0;
}

DateTimeNode* QorePythonProgram::getQoreDateTimeFromTime(ExceptionSink* xsink, PyObject* val) {
    // also subclasses of datetime.time
    assert(PyTime_Check(val));
    // a time of day is an absolute date on 1970-01-01, as with the time values of Qore's database drivers, in the
    // zone given by the time's UTC offset, or in the current time zone
    const AbstractQoreZoneInfo* zone = currentTZ();
    QorePythonReferenceHolder offset(PyObject_CallMethod(val, "utcoffset", nullptr));
    if (!offset) {
        qore_python_conversion_error(this, xsink);
        return nullptr;
    }
    if (*offset != Py_None && getQoreOffsetZone(xsink, *offset, zone)) {
        return nullptr;
    }
    return DateTimeNode::makeAbsolute(zone, 1970, 1, 1, PyDateTime_TIME_GET_HOUR(val),
        PyDateTime_TIME_GET_MINUTE(val), PyDateTime_TIME_GET_SECOND(val), PyDateTime_TIME_GET_MICROSECOND(val));
}

ResolvedCallReferenceNode* QorePythonProgram::getQoreCallRefFromFunc(ExceptionSink* xsink, PyObject* val) {
    assert(PyFunction_Check(val));
    Py_INCREF(val);
    weakRef();
    return new PythonCallableCallReferenceNode(this, val);
}

ResolvedCallReferenceNode* QorePythonProgram::getQoreCallRefFromMethod(ExceptionSink* xsink, PyObject* val) {
    assert(PyMethod_Check(val));
    PyMethodObject* m = reinterpret_cast<PyMethodObject*>(val);
    Py_INCREF(m->im_func);
    if (!PyType_Check(m->im_self)) {
        Py_INCREF(m->im_self);
    }
    weakRef();
    // NOTE: classmethods will have their "self" arg = the class / type
    return new PythonCallableCallReferenceNode(this, m->im_func, PyType_Check(m->im_self) ? nullptr : m->im_self);
}

//! returns the number of significant bits of a non-negative Python int (its bit length without the trailing zero bits:
//! n.bit_length() - (n & -n).bit_length() + 1; 0 for 0), or -1 with a Python exception
static long long qore_python_significant_bits(PyObject* n) {
    QorePythonReferenceHolder neg(PyNumber_Negative(n));
    QorePythonReferenceHolder lowest(neg ? PyNumber_And(n, *neg) : nullptr);
    QorePythonReferenceHolder bits(lowest ? PyObject_CallMethod(n, "bit_length", nullptr) : nullptr);
    QorePythonReferenceHolder lowest_bits(bits ? PyObject_CallMethod(*lowest, "bit_length", nullptr) : nullptr);
    if (!lowest_bits) {
        return -1;
    }
    long long b = PyLong_AsLongLong(*bits);
    long long lb = PyLong_AsLongLong(*lowest_bits);
    if (PyErr_Occurred()) {
        return -1;
    }
    return b ? b - lb + 1 : 0;
}

// the number of decimal digits converted at once for a Python int outside the int64 range: Python refuses to convert an
// int with more digits than sys.get_int_max_str_digits() (4300 by default, at least 640) to a decimal string
static constexpr int QORE_PYTHON_INT_PART_DIGITS = 600;

QoreValue QorePythonProgram::getQoreIntegerFromLong(ExceptionSink* xsink, PyObject* val) {
    assert(PyLong_Check(val));
    int overflow;
    long long ival = PyLong_AsLongLongAndOverflow(val, &overflow);
    if (!overflow) {
        if (ival == -1 && PyErr_Occurred()) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        return static_cast<int64>(ival);
    }

    // outside the int64 range: a Qore number, if it can hold the value exactly: the number of significant bits (the
    // bit length without the trailing zero bits: (n & -n).bit_length() - 1) must not exceed the precision of a Qore
    // number, which has a maximum
    QorePythonReferenceHolder rest(PyNumber_Absolute(val));
    long long sig_bits = rest ? qore_python_significant_bits(*rest) : -1;
    if (sig_bits < 0) {
        qore_python_conversion_error(this, xsink);
        return QoreValue();
    }
    {
        SimpleRefHolder<QoreNumberNode> probe(new QoreNumberNode("0",
            sig_bits > UINT_MAX ? UINT_MAX : static_cast<unsigned>(sig_bits)));
        if (probe->getPrec() < sig_bits) {
            if (xsink) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Python int with %lld significant bits "
                    "to a Qore number exactly; the maximum precision of a Qore number is %u bits", sig_bits,
                    probe->getPrec());
            }
            return QoreValue();
        }
    }

    // the decimal digits of the absolute value are taken in parts of QORE_PYTHON_INT_PART_DIGITS digits, from the
    // least significant one
    QorePythonReferenceHolder ten(PyLong_FromLong(10));
    QorePythonReferenceHolder exp(PyLong_FromLong(QORE_PYTHON_INT_PART_DIGITS));
    QorePythonReferenceHolder divisor(ten && exp ? PyNumber_Power(*ten, *exp, Py_None) : nullptr);
    if (!divisor) {
        qore_python_conversion_error(this, xsink);
        return QoreValue();
    }
    std::vector<std::string> parts;
    while (true) {
        int more = PyObject_RichCompareBool(*rest, *divisor, Py_GE);
        if (more < 0) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        if (!more) {
            break;
        }
        QorePythonReferenceHolder qr(PyNumber_Divmod(*rest, *divisor));
        QorePythonReferenceHolder part(qr ? PyObject_Str(PyTuple_GET_ITEM(*qr, 1)) : nullptr);
        const char* part_str = part ? PyUnicode_AsUTF8(*part) : nullptr;
        if (!part_str) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        // the part with its leading zeros
        parts.push_back(std::string(QORE_PYTHON_INT_PART_DIGITS - strlen(part_str), '0') + part_str);
        PyObject* quotient = PyTuple_GET_ITEM(*qr, 0);
        Py_INCREF(quotient);
        rest = quotient;
    }
    QorePythonReferenceHolder first(PyObject_Str(*rest));
    const char* first_str = first ? PyUnicode_AsUTF8(*first) : nullptr;
    if (!first_str) {
        qore_python_conversion_error(this, xsink);
        return QoreValue();
    }

    QoreString digits;
    if (overflow < 0) {
        digits.concat('-');
    }
    digits.concat(first_str);
    for (std::vector<std::string>::const_reverse_iterator i = parts.rbegin(), e = parts.rend(); i != e; ++i) {
        digits.concat(i->c_str(), i->size());
    }
    return new QoreNumberNode(digits.c_str());
}

QoreValue QorePythonProgram::getQoreValue(ExceptionSink* xsink, QorePythonReferenceHolder& val) {
    return getQoreValue(xsink, *val);
}

QoreValue QorePythonProgram::getQoreValue(ExceptionSink* xsink, QorePythonReferenceHolder& val, pyobj_set_t& rset) {
    return getQoreValue(xsink, *val, rset);
}

QoreValue QorePythonProgram::getQoreValue(ExceptionSink* xsink, PyObject* val) {
    pyobj_set_t rset;
    return getQoreValue(xsink, val, rset);
}

QoreValue QorePythonProgram::getQoreValue(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset) {
    if (!val || val == Py_None) {
        return QoreValue();
    }
    PyTypeObject* type = Py_TYPE(val);
    if (!type) {
        if (xsink) {
            xsink->raiseException("PYTHON-IMPORT-ERROR", "Python object has null type");
        }
        return QoreValue();
    }
    printd(5, "QorePythonBase::getQoreValue() this: %p PyDateTimeAPI: %p val: %p '%s'\n", this, PyDateTimeAPI, val,
        type->tp_name);
    if (!PyDateTimeAPI) {
        PyDateTime_IMPORT;
        if (!PyDateTimeAPI) {
            PyErr_Clear();
            PyDateTimeAPI = nullptr;
        }
    }
    bool have_datetime_api = (PyDateTimeAPI != nullptr);

    // if this is already a Qore object, then return it
    if (PyQoreObject_Check(val)) {
        PyQoreObject* pyobj = reinterpret_cast<PyQoreObject*>(val);
        if (!pyobj->qobj) {
            // an object without a Qore object: the constructor of the Qore class was not called, ex: by an
            // __init__() of a Python subclass that does not call it (see python_qore_object_lifecycle_deleted)
            xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert an object of Python type '%s' to Qore: it "
                "was not initialized; its __init__() did not call the constructor of its Qore class",
                Py_TYPE(val)->tp_name);
            return QoreValue();
        }
        // a weak Python object (of a Qore object that Qore created for a Qore class inheriting its Python class)
        // holds only the existence count of the Qore object: a reference is only taken while the object's destructor
        // has not started, as for Qore's weak references
        if (!pyobj->strong && !pyobj->qobj->isCurrent()) {
            xsink->raiseException("OBJECT-ALREADY-DELETED", "cannot convert an object of Python type '%s' to Qore: its "
                "Qore object of class '%s' has already been deleted", Py_TYPE(val)->tp_name,
                pyobj->qobj->getClassName());
            return QoreValue();
        }
        return pyobj->qobj->refSelf();
    }

    if (type == &PyBool_Type) {
        return QoreValue(val == Py_True);
    }

    if (type == &PyLong_Type) {
        return getQoreIntegerFromLong(xsink, val);
    }

    if (type == &PyFloat_Type) {
        return QoreValue(PyFloat_AS_DOUBLE(val));
    }

    if (type == &PyUnicode_Type) {
        Py_ssize_t size;
        const char* str = PyUnicode_AsUTF8AndSize(val, &size);
        // ex: a string with lone surrogates, which cannot be encoded in UTF-8
        if (!str) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        return new QoreStringNode(str, size, QCS_UTF8);
    }

    if (type == &PyList_Type) {
        return getQoreListFromList(xsink, val, rset);
    }

    if (type == &PyTuple_Type) {
        return getQoreListFromTuple(xsink, val, rset);
    }

    if (type == &PyBytes_Type) {
        return getQoreBinaryFromBytes(val);
    }

    if (type == &PyByteArray_Type) {
        return getQoreBinaryFromByteArray(val);
    }

    if (have_datetime_api && type == PyDateTimeAPI->DateType) {
        return getQoreDateTimeFromDate(val);
    }

    if (have_datetime_api && type == PyDateTimeAPI->TimeType) {
        return getQoreDateTimeFromTime(xsink, val);
    }

    if (have_datetime_api && type == PyDateTimeAPI->DateTimeType) {
        return getQoreDateTimeFromDateTime(xsink, val);
    }

    if (have_datetime_api && type == PyDateTimeAPI->DeltaType) {
        return getQoreDateTimeFromDelta(val);
    }

    if (type == &PyDict_Type) {
        return getQoreHashFromDict(xsink, val, rset);
    }

    // a member of the Python enum class of a Qore enum is the value of the Qore enum (the class has the attribute
    // in its dict)
    {
        PyObject* type_dict = type->tp_dict;
        // returns a borrowed reference
        PyObject* capsule = type_dict ? PyDict_GetItemString(type_dict, QORE_PYTHON_ENUM_ATTR) : nullptr;
        QorePythonDeclHandle* handle = QorePythonDeclHandle::getCapsuleHandle(capsule);
        if (handle) {
            QorePythonReferenceHolder name(PyObject_GetAttrString(val, "name"));
            const char* namestr = name && PyUnicode_Check(*name) ? PyUnicode_AsUTF8(*name) : nullptr;
            if (namestr) {
                // the enum is not freed while its member is found
                QorePythonDeclUse use(handle);
                if (!use) {
                    handle->raiseDeleted();
                    qore_python_conversion_error(this, xsink);
                    return QoreValue();
                }
                const QoreEnumMember* member = static_cast<const QoreEnumDecl*>(handle->getDecl())
                    ->findMember(namestr);
                if (member) {
                    return QoreValue::makeEnum(member);
                }
            }
            PyErr_Clear();
        }
    }

    // subclasses of the types above (ex: IntEnum, StrEnum, OrderedDict, defaultdict) are converted as their base
    // types; a named tuple is a hash of its fields, so that its fields are accessed in Qore as with the Python object
    if (PyLong_Check(val)) {
        return getQoreIntegerFromLong(xsink, val);
    }
    if (PyFloat_Check(val)) {
        double d = PyFloat_AsDouble(val);
        if (d == -1.0 && PyErr_Occurred()) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        return QoreValue(d);
    }
    if (PyUnicode_Check(val)) {
        Py_ssize_t size;
        const char* str = PyUnicode_AsUTF8AndSize(val, &size);
        if (!str) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        return new QoreStringNode(str, size, QCS_UTF8);
    }
    if (PyDict_Check(val)) {
        // an object of the dict subclass of a hashdecl is a hash of the hashdecl
        QorePythonReferenceHolder capsule(PyObject_GetAttrString(reinterpret_cast<PyObject*>(type),
            QORE_PYTHON_HASHDECL_ATTR));
        QorePythonDeclHandle* handle = QorePythonDeclHandle::getCapsuleHandle(*capsule);
        if (!handle) {
            PyErr_Clear();
            return getQoreHashFromDict(xsink, val, rset);
        }
        ValueHolder h(getQoreHashFromDict(xsink, val, rset), xsink);
        if (!h) {
            return QoreValue();
        }
        // the hashdecl is not freed while the hash is converted to it
        QorePythonDeclUse use(handle);
        if (!use) {
            handle->raiseDeleted();
            qore_python_conversion_error(this, xsink);
            qore_python_release(h);
            return QoreValue();
        }
        QoreValue rv = static_cast<const TypedHashDecl*>(handle->getDecl())->doRuntimeCast(
            h->get<const QoreHashNode>(), xsink);
        qore_python_release(h);
        return rv;
    }
    if (PyList_Check(val)) {
        return getQoreListFromList(xsink, val, rset);
    }
    if (PyTuple_Check(val)) {
        QorePythonReferenceHolder fields(PyObject_GetAttrString(val, "_fields"));
        if (fields && PyTuple_Check(*fields) && PyTuple_Size(*fields) == PyTuple_Size(val)) {
            return getQoreHashFromNamedTuple(xsink, val, *fields, rset);
        }
        PyErr_Clear();
        return getQoreListFromTuple(xsink, val, rset);
    }
    if (PyBytes_Check(val)) {
        return getQoreBinaryFromBytes(val);
    }
    if (PyByteArray_Check(val)) {
        return getQoreBinaryFromByteArray(val);
    }
    if (have_datetime_api) {
        // datetime.datetime is a subclass of datetime.date
        if (PyDateTime_Check(val)) {
            return getQoreDateTimeFromDateTime(xsink, val);
        }
        if (PyDate_Check(val)) {
            return getQoreDateTimeFromDate(val);
        }
        if (PyTime_Check(val)) {
            return getQoreDateTimeFromTime(xsink, val);
        }
        if (PyDelta_Check(val)) {
            return getQoreDateTimeFromDelta(val);
        }
    }
    // set and frozenset: a list in iteration order
    if (PyAnySet_Check(val)) {
        return getQoreListFromIterable(xsink, val, rset);
    }
    // a memoryview is a binary of its bytes (other objects supporting the buffer protocol, ex: arrays, stay objects)
    if (PyMemoryView_Check(val)) {
        QorePythonReferenceHolder bytes(PyBytes_FromObject(val));
        if (!bytes) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        return getQoreBinaryFromBytes(*bytes);
    }

    if (PyFunction_Check(val)) {
        return getQoreCallRefFromFunc(xsink, val);
    }

    if (PyMethod_Check(val)) {
        return getQoreCallRefFromMethod(xsink, val);
    }

    // decimal.Decimal (and subclasses) -> number; the module is not imported here: when it is not loaded, the value
    // cannot be a Decimal
    {
        QorePythonReferenceHolder decimal_name(PyUnicode_FromString("decimal"));
        QorePythonReferenceHolder decimal_mod(decimal_name ? PyImport_GetModule(*decimal_name) : nullptr);
        QorePythonReferenceHolder decimal_type(decimal_mod ? PyObject_GetAttrString(*decimal_mod, "Decimal")
            : nullptr);
        int is_decimal = decimal_type ? PyObject_IsInstance(val, *decimal_type) : 0;
        if (is_decimal < 0 || (!decimal_type && PyErr_Occurred())) {
            qore_python_conversion_error(this, xsink);
            return QoreValue();
        }
        if (is_decimal) {
            return getQoreNumberFromDecimal(xsink, val);
        }
    }

    // the Python object of a Qore object whose class inherits a Python class is the Qore object
    {
        // the Python object holds the existence count of the Qore object (a weak reference): the object stays
        // allocated, and a reference is only taken while its destructor has not started (as with Qore's weak
        // references)
        QoreObject* qobj = getQoreSubclassObject(val);
        if (qobj) {
            if (qobj->isCurrent()) {
                return qobj->refSelf();
            }
            // the Qore object has been deleted: the Python object is not wrapped as a new Qore object, which would
            // be an object of another class that bypasses the Qore overrides
            xsink->raiseException("OBJECT-ALREADY-DELETED", "cannot convert an object of Python type '%s' to Qore: "
                "its Qore object of class '%s' has already been deleted", Py_TYPE(val)->tp_name, qobj->getClassName());
            return QoreValue();
        }
    }

    QoreClass* cls = getCreateQorePythonClass(xsink, type);
    if (!cls) {
        assert(*xsink);
        return QoreValue();
    }

    Py_INCREF(val);
    QoreObject* obj = new QoreObject(cls, qpgm, new QorePythonPrivateData(this, val));
    //printd(5, "QorePythonProgram::getQoreValue() obj: %p cls: %p '%s' id: %d\n", obj, cls, cls->getName(),
    //  cls->getID());
    return obj;
}

QoreValue QorePythonProgram::getQoreAttr(PyObject* obj, const char* attr, ExceptionSink* xsink) {
    QorePythonReferenceHolder return_value(PyObject_GetAttrString(obj, attr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, *return_value);
}

PyObject* QorePythonProgram::getPythonList(ExceptionSink* xsink, const QoreListNode* l) {
    QorePythonReferenceHolder list(PyList_New(l->size()));
    ConstListIterator i(l);
    while (i.next()) {
        QorePythonReferenceHolder val(getPythonValue(i.getValue(), xsink));
        if (*xsink) {
            // the Qore exception must be left in place; all callers detect the error with the ExceptionSink,
            // and converting it to a Python exception here would clear the sink and leave them with a nullptr
            // that they treat as success
            return nullptr;
        }
        assert(val);
        PyList_SetItem(*list, i.index(), val.release());
    }

    return list.release();
}

PyObject* QorePythonProgram::getPythonTupleValue(ExceptionSink* xsink, const QoreListNode* l, size_t arg_offset,
    PyObject* first) {
    //printd(5, "QorePythonProgram::getPythonTupleValue() l: %d first: %p\n", l ? (int)l->size() : 0, first);
    bool has_list = (l && l->size() >= arg_offset);

    Py_ssize_t size = has_list ? (l->size() - arg_offset) : 0;
    if (first) {
        ++size;
    }
    QorePythonReferenceHolder tuple(PyTuple_New(size));
    if (!tuple) {
        checkPythonException(xsink);
        return nullptr;
    }
    size_t offset = 0;
    if (first) {
        Py_INCREF(first);
        PyTuple_SET_ITEM(*tuple, 0, first);
        offset = 1;
    }
    if (has_list) {
        ConstListIterator i(l, arg_offset - 1);
        while (i.next()) {
            QorePythonReferenceHolder val(getPythonValue(i.getValue(), xsink));
            if (*xsink) {
                // the Qore exception must be left in place; see the comment in getPythonList()
                return nullptr;
            }
            assert(val);
            PyTuple_SET_ITEM(*tuple, i.index() - arg_offset + offset, val.release());
        }
    }

    return tuple.release();
}

PyObject* QorePythonProgram::getPythonDict(ExceptionSink* xsink, const QoreHashNode* h) {
    QorePythonReferenceHolder dict(PyDict_New());
    ConstHashIterator i(h);
    while (i.next()) {
        // Use getKey() which returns const char* (borrowed reference) instead of
        // getKeyString() which returns a newly allocated QoreString* that must be freed
        QorePythonReferenceHolder key(PyUnicode_FromString(i.getKey()));
        if (!key) {
            checkPythonException(xsink);
            return nullptr;
        }
        QorePythonReferenceHolder val(getPythonValue(i.get(), xsink));
        if (*xsink) {
            // the Qore exception must be left in place; see the comment in getPythonList()
            return nullptr;
        }
        assert(val);
        PyDict_SetItem(*dict, *key, *val);
    }

    return dict.release();
}

PyObject* QorePythonProgram::getPythonString(ExceptionSink* xsink, const QoreString* str) {
    TempEncodingHelper py_str(str, QCS_UTF8, xsink);
    if (*xsink) {
        qore_python_pgm->raisePythonException(*xsink);
        return nullptr;
    }
    return PyUnicode_FromStringAndSize(py_str->c_str(), py_str->size());
}

PyObject* QorePythonProgram::getPythonByteArray(ExceptionSink* xsink, const BinaryNode* b) {
    return PyByteArray_FromStringAndSize(reinterpret_cast<const char*>(b->getPtr()), b->size());
}

PyObject* QorePythonProgram::getPythonDelta(ExceptionSink* xsink, const DateTime* dt) {
    assert(dt->isRelative());

    // WARNING: years are converted to 365 days; months are converted to 30 days
    // getInfo() is used rather than getYear(), which returns a short and therefore silently truncates years
    // outside the range of a 16-bit integer
    qore_tm info;
    dt->getInfo(info);
    // the components are normalized as datetime.timedelta does it (seconds in [0, 86400), microseconds in
    // [0, 1000000)), in 64-bit arithmetic, as the components of a relative date can exceed these ranges
    // (ex: PT1000000H)
    auto floor_div = [](int64 n, int64 d) -> int64 {
        return n >= 0 ? n / d : -((d - 1 - n) / d);
    };
    int64 us = info.us;
    int64 secs = static_cast<int64>(info.hour) * 3600 + static_cast<int64>(info.minute) * 60 + info.second
        + floor_div(us, 1000000);
    us -= floor_div(us, 1000000) * 1000000;
    int64 days = static_cast<int64>(info.year) * 365 + static_cast<int64>(info.month) * 30 + info.day
        + floor_div(secs, 86400);
    secs -= floor_div(secs, 86400) * 86400;
    if (days > INT_MAX || days < INT_MIN) {
        PyErr_Format(PyExc_OverflowError, "the relative date/time value has %lld days, which is outside the range of "
            "datetime.timedelta", static_cast<long long>(days));
        return nullptr;
    }

    if (PyDateTimeAPI) {
        return PyDelta_FromDSU(static_cast<int>(days), static_cast<int>(secs), static_cast<int>(us));
    }

    // Fallback: create datetime.timedelta via Python when PyDateTimeAPI is unavailable.
    QorePythonReferenceHolder datetime_mod(PyImport_ImportModule("datetime"));
    QorePythonReferenceHolder timedelta_cls(datetime_mod ? PyObject_GetAttrString(*datetime_mod, "timedelta")
        : nullptr);
    if (!timedelta_cls) {
        return nullptr;
    }
    return PyObject_CallFunction(*timedelta_cls, "iii", static_cast<int>(days), static_cast<int>(secs),
        static_cast<int>(us));
}

PyObject* QorePythonProgram::getPythonDateTime(ExceptionSink* xsink, const DateTime* dt) {
    assert(dt->isAbsolute());

    // DateTime::getYear() returns a short, which silently truncates years outside the range of a 16-bit
    // integer; a truncated year can land inside the range accepted by datetime.datetime and therefore produce
    // a silently incorrect value, so the broken-down date is retrieved with getInfo() instead, which reports
    // the year as an int (this is also faster, as the date is broken down only once)
    qore_tm info;
    dt->getInfo(info);

    // a date in the current time zone is a naive datetime (Python's local time), which is converted back in the
    // current time zone; a date in another zone is an aware datetime with the date's UTC offset, so that the instant
    // is preserved
    bool naive = info.zone == currentTZ();

    if (PyDateTimeAPI) {
        if (naive) {
            return PyDateTime_FromDateAndTime(info.year, info.month, info.day, info.hour, info.minute, info.second,
                info.us);
        }
        QorePythonReferenceHolder offset(PyDelta_FromDSU(0, info.utc_secs_east, 0));
        QorePythonReferenceHolder tz(offset ? PyTimeZone_FromOffset(*offset) : nullptr);
        if (!tz) {
            return nullptr;
        }
        return PyDateTimeAPI->DateTime_FromDateAndTime(info.year, info.month, info.day, info.hour, info.minute,
            info.second, info.us, *tz, PyDateTimeAPI->DateTimeType);
    }

    // Fallback: create datetime.datetime via Python when PyDateTimeAPI is unavailable.
    QorePythonReferenceHolder datetime_mod(PyImport_ImportModule("datetime"));
    QorePythonReferenceHolder datetime_cls(datetime_mod ? PyObject_GetAttrString(*datetime_mod, "datetime")
        : nullptr);
    if (!datetime_cls) {
        return nullptr;
    }
    QorePythonReferenceHolder tz;
    if (naive) {
        Py_INCREF(Py_None);
        tz = Py_None;
    } else {
        QorePythonReferenceHolder timedelta_cls(PyObject_GetAttrString(*datetime_mod, "timedelta"));
        QorePythonReferenceHolder timezone_cls(timedelta_cls ? PyObject_GetAttrString(*datetime_mod, "timezone")
            : nullptr);
        QorePythonReferenceHolder offset(timezone_cls
            ? PyObject_CallFunction(*timedelta_cls, "ii", 0, info.utc_secs_east) : nullptr);
        tz = offset ? PyObject_CallFunctionObjArgs(*timezone_cls, *offset, nullptr) : nullptr;
        if (!tz) {
            return nullptr;
        }
    }
    return PyObject_CallFunction(*datetime_cls, "iiiiiiiO", info.year, info.month, info.day, info.hour, info.minute,
        info.second, info.us, *tz);
}

PyObject* QorePythonProgram::getPythonCallable(ExceptionSink* xsink, const ResolvedCallReferenceNode* call) {
    QorePythonImplicitQoreArgHelper qpiqoh((void*)call);
    return PyObject_CallObject((PyObject*)&PythonQoreCallable_Type, nullptr);
}

PyObject* QorePythonProgram::getPythonValue(QoreValue val, ExceptionSink* xsink) {
    // A member assigned with the weak reference operator ":=" or the opaque reference operator
    // "@=" is stored in its container as the reference itself, and getPythonList(),
    // getPythonTuple() and getPythonDict() pass what the container holds straight back here.
    // Resolve it, or an ordinary convertible hash or list is reported as a Qore value that
    // cannot be converted to Python.
    val = val.resolveIndirect();
    PyObject* rv = getPythonValueIntern(val, xsink);
    // ensure that a nullptr return always corresponds to a Qore exception in xsink; Python C API calls
    // (ex: PyDateTime_FromDateAndTime() with a year outside the range supported by datetime.datetime) fail
    // by setting a Python exception, which leaves the ExceptionSink untouched; callers use the ExceptionSink
    // alone to detect errors, so without this conversion they would dereference a nullptr
    if (!rv && xsink && !*xsink) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Qore value of type '%s' to a Python "
                "value", val.getFullTypeName());
        }
    }
    assert(rv || !xsink || *xsink);
    return rv;
}

PyObject* QorePythonProgram::getPythonValueIntern(QoreValue val, ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::getPythonValue() type '%s'\n", val.getFullTypeName());
    // a value of a Qore enum is the member of the enum's Python enum class
    if (val.isEnum()) {
        const QoreEnumMember* member = val.getEnumMember();
        const QoreEnumDecl* ed = member->getEnumDecl();
        // an enum without a Program outside the system namespace has outlived the Program that declared it, so it
        // has no Python enum class, which would outlive it: the value is its member's value
        if (!ed->getProgram() && !ed->isSystem()) {
            return getPythonValueIntern(member->getValue(), xsink);
        }
        PyObject* cls = getEnumType(xsink, ed);
        if (!cls) {
            return nullptr;
        }
        PyObject* rv = PyObject_GetAttrString(cls, member->getName());
        if (!rv) {
            checkPythonException(xsink);
        }
        return rv;
    }
    switch (val.getType()) {
        case NT_NOTHING:
        case NT_NULL:
            Py_INCREF(Py_None);
            return Py_None;

        case NT_BOOLEAN: {
            PyObject* rv = val.getAsBool() ? Py_True : Py_False;
            Py_INCREF(rv);
            return rv;
        }

        case NT_INT:
            return PyLong_FromLongLong(val.getAsBigInt());

        case NT_FLOAT:
            return PyFloat_FromDouble(val.getAsFloat());

        case NT_NUMBER:
            return getPythonNumber(xsink, val.get<const QoreNumberNode>());

        case NT_STRING: {
            QoreStringNodeValueHelper str(val);
            return getPythonString(xsink, *str);
        }

        case NT_LIST: {
            QorePythonRecursionGuard guard(val, xsink);
            if (guard.tooDeep()) {
                return nullptr;
            }
            return getPythonList(xsink, val.get<const QoreListNode>());
        }

        case NT_HASH: {
            const QoreHashNode* h = val.get<const QoreHashNode>();
            QorePythonReferenceHolder dict;
            {
                QorePythonRecursionGuard guard(val, xsink);
                if (guard.tooDeep()) {
                    return nullptr;
                }
                dict = getPythonDict(xsink, h);
            }
            const TypedHashDecl* hd = h->getHashDecl();
            // a hashdecl without a Program outside the system namespace has outlived the Program that declared it, so
            // it has no Python type, which would outlive it: the hash is a dict
            if (!dict || !hd || (!hd->getProgram() && !hd->isSystem())) {
                return dict.release();
            }
            // a hash of a hashdecl is an object of the hashdecl's dict subclass
            PyObject* type = getHashDeclType(xsink, hd);
            if (!type) {
                return nullptr;
            }
            QorePythonReferenceHolder empty(PyTuple_New(0));
            QorePythonReferenceHolder rv(empty ? reinterpret_cast<PyTypeObject*>(type)->tp_new(
                reinterpret_cast<PyTypeObject*>(type), *empty, nullptr) : nullptr);
            if (!rv || PyDict_Update(*rv, *dict) < 0) {
                checkPythonException(xsink);
                return nullptr;
            }
            return rv.release();
        }

        case NT_BINARY:
            return getPythonByteArray(xsink, val.get<const BinaryNode>());

        case NT_DATE: {
            const DateTimeNode* dt = val.get<const DateTimeNode>();
            return dt->isRelative()
                ? getPythonDelta(xsink, dt)
                : getPythonDateTime(xsink, dt);
        }

        case NT_RUNTIME_CLOSURE:
        case NT_FUNCREF: {
            return getPythonCallable(xsink, val.get<const ResolvedCallReferenceNode>());
        }

        case NT_OBJECT: {
            QoreObject* o = const_cast<QoreObject*>(val.get<const QoreObject>());
            if (!o->isValid()) {
                Py_INCREF(Py_None);
                return Py_None;
            }

            TryPrivateDataRefHolder<QorePythonPrivateData> pypd(o, CID_PYTHONBASEOBJECT, xsink);
            if (pypd && sharesInterpreter(pypd->getOwner())) {
                PyObject* rv = pypd->get();
                Py_XINCREF(rv);
                return rv;
            }

            // A foreign interpreter's Python object must remain behind its Qore
            // proxy. Passing the raw object lets the receiving interpreter's GC
            // traverse and corrupt the owner's object graph during finalization.
            const QoreClass* cls = o->getClass();
            // a class without a Program outside the system namespace has outlived the Program that declared it, so
            // it has no Python class, which would outlive it
            if (!cls->getProgram() && !cls->isSystem()) {
                xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert an object of class '%s' to Python: the "
                    "Qore Program that declared the class has been deleted", cls->getName());
                return nullptr;
            }
            PythonQoreClass* py_cls = findCreatePythonClass(*cls, "qore");
            if (!py_cls) {
                checkPythonException(xsink);
                return nullptr;
            }
            return py_cls->wrap(o);
        }
    }

    // a type that cannot be converted to a Python value; getPythonValue() raises the exception
    return nullptr;
}

PyObject* QorePythonProgram::getPythonNumber(ExceptionSink* xsink, const QoreNumberNode* n) {
    // the shortest decimal representation that gives the same value at the number's precision; special values are
    // spelled NaN, INF and -INF, which decimal.Decimal accepts
    QoreString str;
    if (n->toStringRoundTrip(str, false, xsink)) {
        return nullptr;
    }
    // an integral value outside the int64 range is a Python int if the number holds it exactly, as it holds the Python
    // ints outside the int64 range that it was made from (Python ints in the range are Qore integers, and other numbers
    // are Decimals, so that both round-trip with their types): the integer given by the digits then needs no more
    // significant bits than the number's precision, and as the digits give the number's value at its precision, they
    // give it exactly; otherwise (ex: a number made from a float) the digits are only the shortest decimal
    // representation of a binary value, and the value is a Decimal
    if (!strpbrk(str.c_str(), ".eENI")) {
        QorePythonReferenceHolder ival(PyLong_FromString(str.c_str(), nullptr, 10));
        if (!ival) {
            return nullptr;
        }
        int overflow;
        PyLong_AsLongLongAndOverflow(*ival, &overflow);
        if (overflow) {
            QorePythonReferenceHolder abs_val(PyNumber_Absolute(*ival));
            long long sig_bits = abs_val ? qore_python_significant_bits(*abs_val) : -1;
            if (sig_bits < 0) {
                return nullptr;
            }
            if (sig_bits <= n->getPrec()) {
                return ival.release();
            }
        }
    }
    // the decimal.Decimal is made from the same representation, in scientific notation when the plain one is long
    // (numbers with big or small exponents)
    if (str.size() > 50) {
        str.clear();
        if (n->toStringRoundTrip(str, true, xsink)) {
            return nullptr;
        }
    }
    QorePythonReferenceHolder decimal_mod(PyImport_ImportModule("decimal"));
    QorePythonReferenceHolder decimal_type(decimal_mod ? PyObject_GetAttrString(*decimal_mod, "Decimal") : nullptr);
    if (!decimal_type) {
        return nullptr;
    }
    return PyObject_CallFunction(*decimal_type, "s", str.c_str());
}

QoreNumberNode* QorePythonProgram::getQoreNumberFromDecimal(ExceptionSink* xsink, PyObject* val) {
    QorePythonReferenceHolder is_nan(PyObject_CallMethod(val, "is_nan", nullptr));
    if (!is_nan) {
        qore_python_conversion_error(this, xsink);
        return nullptr;
    }
    // a signaling NaN ("sNaN") is a NaN in Qore
    if (*is_nan == Py_True) {
        return new QoreNumberNode("nan");
    }
    QorePythonReferenceHolder str(PyObject_Str(val));
    const char* cstr = str ? PyUnicode_AsUTF8(*str) : nullptr;
    if (!cstr) {
        qore_python_conversion_error(this, xsink);
        return nullptr;
    }
    return new QoreNumberNode(cstr);
}

QoreValue QorePythonProgram::callFunction(ExceptionSink* xsink, const QoreString& func_name, const QoreListNode* args,
        size_t arg_offset, const QoreHashNode* kwargs) {
    assert(!haveGil());
    //printd(5, "QorePythonProgram::callFunction() this: %p func: '%s' module: %p module_dict: %p valid: %d\n",
    //    this, func_name.c_str(), *module, module_dict, (int)valid);
    TempEncodingHelper fname(func_name, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"func_name\" argument)");
        return QoreValue();
    }

    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }

    ValueHolder rv(xsink);
    {
        QorePythonHelper qph(this, xsink);
        //printd(5, "QorePythonProgram::callFunction() after qph: module: %p module_dict: %p\n", *module, module_dict);
        if (*xsink || checkValid(xsink)) {
            return QoreValue();
        }

        //printd(5, "QorePythonProgram::callFunction() calling PyDict_GetItemString(module_dict: %p, fname: '%s')\n",
        //    module_dict, fname->c_str());
        // any callable in the module's globals or in the builtins (ex: a function, a class, a functools.partial
        // object, len()); returns a borrowed reference
        PyObject* py_func = PyDict_GetItemString(module_dict, fname->c_str());
        if (!py_func && builtin_dict) {
            py_func = PyDict_GetItemString(builtin_dict, fname->c_str());
        }
        if (!py_func || !PyCallable_Check(py_func)) {
            if (py_func) {
                xsink->raiseException("NO-FUNCTION", "'%s' is not callable (type '%s')", fname->c_str(),
                    Py_TYPE(py_func)->tp_name);
            } else {
                xsink->raiseException("NO-FUNCTION", "cannot find function '%s'", fname->c_str());
            }
            return QoreValue();
        }

        //printd(5, "QorePythonProgram::callFunction() this: %p %s()\n", this, func_name.c_str());
        rv = callWithContext(xsink, py_func, args, arg_offset, nullptr, kwargs);
    }
    assert(!haveGil());
    return rv.release();
}

QoreValue QorePythonProgram::getAttribute(ExceptionSink* xsink, const QoreString& name) {
    TempEncodingHelper str(name, QCS_UTF8, xsink);
    if (*xsink) {
        return QoreValue();
    }
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }
    // returns a borrowed reference
    PyObject* val = PyDict_GetItemString(module_dict, str->c_str());
    if (!val) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("NO-ATTRIBUTE", "the Python program has no global variable '%s'", str->c_str());
        }
        return QoreValue();
    }
    return getQoreValue(xsink, val);
}

void QorePythonProgram::setAttribute(ExceptionSink* xsink, const QoreString& name, QoreValue value) {
    TempEncodingHelper str(name, QCS_UTF8, xsink);
    if (*xsink) {
        return;
    }
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return;
    }
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return;
    }
    QorePythonReferenceHolder val(getPythonValue(value, xsink));
    if (!val) {
        return;
    }
    if (PyDict_SetItemString(module_dict, str->c_str(), *val) < 0) {
        checkPythonException(xsink);
    }
}

QoreValue QorePythonProgram::callMethod(ExceptionSink* xsink, const QoreString& class_name,
    const QoreString& method_name, const QoreListNode* args, size_t arg_offset, const QoreHashNode* kwargs) {
    TempEncodingHelper cname(class_name, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"class_name\" argument)");
        return QoreValue();
    }

    TempEncodingHelper mname(method_name, QCS_UTF8, xsink);
    if (*xsink) {
        xsink->appendLastDescription(" (while processing the \"method_name\" argument)");
        return QoreValue();
    }

    return callMethod(xsink, cname->c_str(), mname->c_str(), args, arg_offset, nullptr, kwargs);
}

QoreValue QorePythonProgram::callMethod(ExceptionSink* xsink, const char* cname, const char* mname,
    const QoreListNode* args, size_t arg_offset, PyObject* first, const QoreHashNode* kwargs) {
    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }

    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }

    assert(module_dict);
    assert(builtin_dict);

    // returns a borrowed reference
    PyObject* py_class = PyDict_GetItemString(module_dict, cname);
    if (!py_class || !PyType_Check(py_class)) {
        // returns a borrowed reference
        py_class = PyDict_GetItemString(builtin_dict, cname);
        if (!py_class || !PyType_Check(py_class)) {
            xsink->raiseException("NO-CLASS", "cannot find class '%s'", cname);
            return QoreValue();
        }
    }

    // any callable attribute of the class (ex: a function, a static method, a class method bound to the class, a
    // method descriptor)
    QorePythonReferenceHolder py_method(PyObject_GetAttrString(py_class, mname));
    if (!py_method) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            checkPythonException(xsink);
            return QoreValue();
        }
        PyErr_Clear();
    }
    if (!py_method || !PyCallable_Check(*py_method)) {
        xsink->raiseException("NO-METHOD", "cannot find method '%s.%s()'", cname, mname);
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callMethod() %s::%s() args: %d\n", cname, mname, args ? (int)args->size() : 0);

    return callWithContext(xsink, *py_method, args, arg_offset, first, kwargs);
}

QoreValue QorePythonProgram::callInternal(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
    size_t arg_offset, PyObject* first, const QoreHashNode* kwargs) {
    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }

    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }
    return callWithContext(xsink, callable, args, arg_offset, first, kwargs);
}

QoreValue QorePythonProgram::callWithContext(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
    size_t arg_offset, PyObject* first, const QoreHashNode* kwargs) {
    //printd(5, "QorePythonProgram::callWithContext() f: %p args: %d (%d) self: %p\n", callable,
    //    args ? (int)args->size() : 0, (int)arg_offset, first);
    // the keys of the hash are the names of the keyword arguments
    QorePythonReferenceHolder py_kwargs;
    if (kwargs && !kwargs->empty()) {
        py_kwargs = getPythonDict(xsink, kwargs);
        if (*xsink) {
            xsink->appendLastDescription(" (while converting the keyword arguments)");
            return QoreValue();
        }
    }
    QorePythonReferenceHolder rv(callPythonInternal(xsink, callable, args, arg_offset, first, *py_kwargs));
    return *xsink ? QoreValue() : getQoreValue(xsink, rv);
}

PyObject* QorePythonProgram::callPythonInternal(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
    size_t arg_offset, PyObject* first, PyObject* kwargs) {
    QorePythonReferenceHolder py_args;

    py_args = getPythonTupleValue(xsink, args, arg_offset, first);
    if (*xsink) {
        return nullptr;
    }

    //printd(5, "QorePythonProgram::callPythonInternal(): this: %p valid: %d argcount: %d (first: %p)\n", this, valid,
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0, first);
    QorePythonReferenceHolder return_value(PyObject_Call(callable, *py_args, kwargs));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return nullptr;
    }

    return return_value.release();
}

QoreValue QorePythonProgram::callFunctionObject(ExceptionSink* xsink, PyObject* func, const QoreListNode* args,
    size_t arg_offset, PyObject* first) {
    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }

    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }
    QorePythonReferenceHolder py_args(getPythonTupleValue(xsink, args, arg_offset, first));
    if (*xsink) {
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callFunctionObject(): this: %p valid: %d argcount: %d\n", this, valid,
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0);
    QorePythonReferenceHolder return_value(PyFunction_Type.tp_call(func, *py_args, nullptr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }
    return getQoreValue(xsink, return_value);
}

void QorePythonProgram::clearPythonException() {
    QorePythonReferenceHolder ex_type, ex_value, traceback;
    PyErr_Fetch(ex_type.getRef(), ex_value.getRef(), traceback.getRef());
}

int QorePythonProgram::checkPythonException(ExceptionSink* xsink) {
    // returns a borrowed reference
    PyObject* ex = PyErr_Occurred();
    if (!ex) {
        //printd(5, "QorePythonProgram::checkPythonException() no error\n");
        return 0;
    }

    // the KeyboardInterrupt that an interrupt of the Program raised in the Python code is the Qore interrupt
    // (PROGRAM-INTERRUPTED); the one that the deletion of the program raised is PYTHON-INTERPRETER-DELETED
    if (PyErr_GivenExceptionMatches(ex, PyExc_KeyboardInterrupt)) {
        // the deletion of the program also cancels the thread (see cancelThreads()), which is not a cancellation of
        // its Qore code
        bool deleted;
        {
            AutoLocker al(py_thr_lck);
            deleted = pgm_cancelled_tids.find(q_gettid()) != pgm_cancelled_tids.end();
        }
        if (deleted) {
            PyErr_Clear();
            xsink->raiseException("PYTHON-INTERPRETER-DELETED", "the Python program was deleted while the thread "
                "ran its Python code");
            return -1;
        }
        if (qore_check_cancel(xsink, "running Python code")) {
            PyErr_Clear();
            return -1;
        }
        if (destroyed) {
            PyErr_Clear();
            xsink->raiseException("PYTHON-INTERPRETER-DELETED", "the Python program was deleted while the thread "
                "ran its Python code");
            return -1;
        }
    }

    QorePythonReferenceHolder ex_type, ex_value, traceback;
    PyErr_Fetch(ex_type.getRef(), ex_value.getRef(), traceback.getRef());
    assert(ex_type);

    printd(5, "QorePythonProgram::checkPythonException() type: %s val: %s (%p) traceback: %s\n",
        Py_TYPE(*ex_type)->tp_name, ex_value ? Py_TYPE(*ex_value)->tp_name : "(null)", *ex_value,
        traceback ? Py_TYPE(*traceback)->tp_name : "(null)");

    if (!ex_value) {
        ex_value = Py_None;
        Py_INCREF(Py_None);
    }
    if (!traceback) {
        traceback = Py_None;
        Py_INCREF(Py_None);
    }

    PyErr_NormalizeException(ex_type.getRef(), ex_value.getRef(), traceback.getRef());
    printd(5, "QorePythonProgram::checkPythonException() type: %s val: %s (%p) traceback: %s\n",
        Py_TYPE(*ex_type)->tp_name, Py_TYPE(*ex_value)->tp_name, *ex_value, Py_TYPE(*traceback)->tp_name);

    pyobj_set_t seen;
    raiseQoreException(*xsink, *ex_type, *ex_value, *traceback, seen);
    assert(*xsink);
    return -1;
}

bool QorePythonProgram::raiseOriginalQoreException(ExceptionSink& xsink, PyObject* ex_value, int max_exceptions) {
    QorePythonReferenceHolder capsule(PyObject_GetAttrString(ex_value, QORE_PYTHON_EXCEPTION_CAPSULE_ATTR));
    if (!capsule || !PyCapsule_IsValid(*capsule, QORE_PYTHON_EXCEPTION_CAPSULE_NAME)) {
        // a QoreException created in Python
        PyErr_Clear();
        return false;
    }
    const QoreHashNode* info = static_cast<const QoreHashNode*>(PyCapsule_GetPointer(*capsule,
        QORE_PYTHON_EXCEPTION_CAPSULE_NAME));
    assert(info);

    // the exceptions get the current call stack as the outer part of their call stacks, which is removed from the
    // call stacks of the exceptions if they end with it (when the exception passes through Python code on the thread
    // that raised it)
    ReferenceHolder<QoreListNode> current_stack(&xsink);
    {
        ExceptionSink xsink2;
        xsink2.raiseException("CALL-STACK", "the current call stack");
        ReferenceHolder<QoreHashNode> h(xsink2.getExceptionInfo(), &xsink2);
        assert(h);
        QoreValue cs = h->getKeyValue("callstack");
        if (cs.getType() == NT_LIST) {
            current_stack = cs.get<QoreListNode>()->listRefSelf();
        }
    }
    size_t current_size = current_stack ? current_stack->size() : 0;

    for (int depth = 0; info && depth < max_exceptions; ++depth) {
        QoreCallStack callstack;
        QoreValue cs_val = info->getKeyValue("callstack");
        if (cs_val.getType() == NT_LIST) {
            const QoreListNode* cs = cs_val.get<const QoreListNode>();
            size_t size = cs->size();
            if (current_size && current_size <= size) {
                ExceptionSink xsink2;
                bool suffix = true;
                for (size_t i = 0; i < current_size; ++i) {
                    QoreValue v0 = cs->retrieveEntry(size - current_size + i);
                    QoreValue v1 = current_stack->retrieveEntry(i);
                    if (v0.getType() != NT_HASH || v1.getType() != NT_HASH
                        || !v0.get<const QoreHashNode>()->is_equal_hard(v1.get<const QoreHashNode>(), &xsink2)) {
                        suffix = false;
                        break;
                    }
                }
                // comparing the hashes of call stack entries raises no exceptions
                assert(!xsink2);
                if (suffix) {
                    size -= current_size;
                }
            }
            for (size_t i = 0; i < size; ++i) {
                QoreValue v = cs->retrieveEntry(i);
                if (v.getType() != NT_HASH) {
                    continue;
                }
                const QoreHashNode* e = v.get<const QoreHashNode>();
                QoreStringValueHelper file(e->getKeyValue("file"));
                QoreStringValueHelper source(e->getKeyValue("source"));
                QoreStringValueHelper function(e->getKeyValue("function"));
                QoreStringValueHelper lang(e->getKeyValue("lang"));
                callstack.add(static_cast<qore_call_t>(e->getKeyValue("typecode").getAsBigInt()), file->c_str(),
                    static_cast<int>(e->getKeyValue("line").getAsBigInt()),
                    static_cast<int>(e->getKeyValue("endline").getAsBigInt()), source->c_str(),
                    static_cast<unsigned>(e->getKeyValue("offset").getAsBigInt()), function->c_str(),
                    lang->c_str());
            }
        }

        QoreStringValueHelper file(info->getKeyValue("file"));
        QoreStringValueHelper source(info->getKeyValue("source"));
        QoreStringValueHelper lang(info->getKeyValue("lang"));
        QoreExternalProgramLocationWrapper loc(file->c_str(), static_cast<int>(info->getKeyValue("line").getAsBigInt()),
            static_cast<int>(info->getKeyValue("endline").getAsBigInt()), source->c_str(),
            static_cast<int>(info->getKeyValue("offset").getAsBigInt()), lang->c_str());
        QoreStringValueHelper err(info->getKeyValue("err"));
        QoreStringNodeValueHelper desc(info->getKeyValue("desc"));

        // each exception is raised in its own sink, as the call stack given is added to all exceptions in a sink
        ExceptionSink xsink2;
        xsink2.raiseExceptionArg(loc.get(), err->c_str(), info->getKeyValue("arg").refSelf(),
            desc.getReferencedValue(), callstack);
        xsink.assimilate(xsink2);

        QoreValue next = info->getKeyValue("next");
        info = next.getType() == NT_HASH ? next.get<const QoreHashNode>() : nullptr;
    }
    return true;
}

// gets the location of a Python exception and its call stack from its traceback; returns -1 if there is none
static int qore_python_get_exception_location(PyObject* traceback, QoreExternalProgramLocationWrapper& loc,
        QoreCallStack& callstack) {
    if (!PyTraceBack_Check(traceback)) {
        return -1;
    }
    // the frames from the innermost frame, where the exception was raised, with the line executed in each frame: the
    // frames of the traceback with their lines in the traceback, then the frames that called the outermost frame of
    // the traceback with their current lines
    std::vector<std::pair<PyFrameObject*, int>> frames;
    // the traceback holds references to its frames
    for (PyObject* tb = traceback; tb && PyTraceBack_Check(tb);
        tb = reinterpret_cast<PyObject*>(reinterpret_cast<PyTracebackObject*>(tb)->tb_next)) {
        QorePythonReferenceHolder lineno(PyObject_GetAttrString(tb, "tb_lineno"));
        int line = lineno && PyLong_Check(*lineno) ? static_cast<int>(PyLong_AsLong(*lineno)) : -1;
        if (!lineno || PyErr_Occurred()) {
            PyErr_Clear();
        }
        frames.emplace_back(reinterpret_cast<PyTracebackObject*>(tb)->tb_frame, line);
    }
    std::reverse(frames.begin(), frames.end());
    // the frames that called the outermost frame of the traceback; PyFrame_GetBack() returns a new reference
    std::vector<QorePythonReferenceHolder> callers;
    for (PyFrameObject* frame = PyFrame_GetBack(frames.back().first); frame; frame = PyFrame_GetBack(frame)) {
        callers.emplace_back(reinterpret_cast<PyObject*>(frame));
        frames.emplace_back(frame, PyFrame_GetLineNumber(frame));
    }

    // each call stack entry gives the function called and the location of the call in the calling frame
    std::string funcname;
    for (size_t i = 0; i < frames.size(); ++i) {
        PyFrameObject* frame = frames[i].first;
        int line = frames[i].second;
        QorePythonReferenceHolder code(reinterpret_cast<PyObject*>(PyFrame_GetCode(frame)));
        QorePythonReferenceHolder filename_obj(PyObject_GetAttrString(*code, "co_filename"));
        if (!filename_obj) {
            PyErr_Clear();
        }
        QorePythonReferenceHolder np_obj;
        if (filename_obj) {
            // normalizePath() takes the reference
            filename_obj.py_ref();
            np_obj = QorePythonStackLocationHelper::normalizePath(*filename_obj);
        }
        const char* filename = nullptr;
        if (np_obj) {
            filename = QorePythonProgram::getCString(*np_obj);
        } else if (filename_obj) {
            filename = PyUnicode_AsUTF8(*filename_obj);
        }
        if (!filename) {
            PyErr_Clear();
            filename = "<unknown>";
        }

        if (!i) {
            loc.set(filename, line, line, nullptr, 0, QORE_PYTHON_LANG_NAME);
        } else {
            callstack.add(CT_USER, filename, line, line, funcname.c_str(), QORE_PYTHON_LANG_NAME);
        }
        QorePythonReferenceHolder funcname_obj(PyObject_GetAttrString(*code, "co_qualname"));
        const char* str = funcname_obj && PyUnicode_Check(*funcname_obj) ? PyUnicode_AsUTF8(*funcname_obj) : nullptr;
        if (!str) {
            PyErr_Clear();
            str = "<unknown>";
        }
        funcname = str;
    }
    // the outermost Python function was called by native code
    callstack.add(CT_BUILTIN, "<builtin>", -1, -1, funcname.c_str(), QORE_PYTHON_LANG_NAME);
    return 0;
}

void QorePythonProgram::raiseQoreException(ExceptionSink& xsink, PyObject* ex_type, PyObject* ex_value,
        PyObject* traceback, pyobj_set_t& seen, const QoreExternalProgramLocationWrapper* chained_loc) {
    seen.insert(ex_value);

    // the qoreloader.QoreException type of the current interpreter
    QorePythonReferenceHolder qore_ex_type(qore_python_get_qore_exception_type());
    if (!qore_ex_type) {
        // without the type, no exception is a Qore exception
        PyErr_Clear();
    }
    bool is_qore_exception = qore_ex_type && PyExceptionInstance_Check(ex_value)
        && PyObject_TypeCheck(ex_value, reinterpret_cast<PyTypeObject*>(*qore_ex_type));

    // a Qore exception raised in Python is raised again as it was raised in Qore
    // the exceptions of its chain are part of the chain of the Python exception, which is limited
    if (is_qore_exception && raiseOriginalQoreException(xsink, ex_value,
            QORE_PYTHON_MAX_CHAINED_EXCEPTIONS - static_cast<int>(seen.size()) + 1)) {
        return;
    }

    QoreExternalProgramLocationWrapper loc;
    QoreCallStack callstack;
    // a chained exception that was never raised has no traceback: it gets the Python location of the exception that
    // it is chained to, not the location of the Qore code that called Python
    const QoreExternalProgramLocationWrapper* ex_loc = !qore_python_get_exception_location(traceback, loc, callstack)
        ? &loc : chained_loc;

    // a value that cannot be converted to Qore: the exception is raised without it, and the reason is added to its
    // description
    auto get_arg = [this](PyObject* val, QoreStringNode& desc) -> QoreValue {
        ExceptionSink xsink2;
        ValueHolder rv(getQoreValue(&xsink2, val), &xsink2);
        if (xsink2) {
            PyErr_Clear();
            QoreValue err(xsink2.getExceptionErr());
            QoreValue edesc(xsink2.getExceptionDesc());
            QoreStringValueHelper err_str(err);
            QoreStringValueHelper edesc_str(edesc);
            desc.sprintf(" (the exception argument cannot be converted to Qore: %s: %s)", err_str->c_str(),
                edesc_str->c_str());
            xsink2.clear();
            return QoreValue();
        }
        return rv.release();
    };

    std::string err;
    QoreStringNodeHolder desc;
    ValueHolder arg(&xsink);
    if (is_qore_exception) {
        // a QoreException created in Python: its err, desc and arg are the Qore exception's; the arguments are used
        // if the attributes are not set, ex: by a subclass whose __init__() does not call QoreException.__init__()
        QorePythonReferenceHolder args(PyObject_GetAttrString(ex_value, "args"));
        if (!args || !PyTuple_Check(*args)) {
            PyErr_Clear();
            args = PyTuple_New(0);
        }
        auto get_attr = [&](const char* name, Py_ssize_t i) -> PyObject* {
            PyObject* rv = PyObject_GetAttrString(ex_value, name);
            if (!rv) {
                PyErr_Clear();
                if (*args && i < PyTuple_Size(*args)) {
                    rv = PyTuple_GetItem(*args, i);
                    Py_INCREF(rv);
                }
            }
            return rv;
        };
        QorePythonReferenceHolder py_err(get_attr("err", 0));
        if (py_err) {
            SimpleRefHolder<QoreStringNode> err_str(qore_python_get_string(*py_err));
            err = err_str->c_str();
        } else {
            err = qore_python_get_exception_name(Py_TYPE(ex_value));
        }
        QorePythonReferenceHolder py_desc(get_attr("desc", 1));
        desc = py_desc && *py_desc != Py_None ? qore_python_get_string(*py_desc) : new QoreStringNode;
        QorePythonReferenceHolder py_arg(get_attr("arg", 2));
        if (py_arg) {
            arg = get_arg(*py_arg, **desc);
        }
    } else {
        // get full exception class name
        // NOTE: Use ex_type directly instead of Py_TYPE(ex_value) because after PyErr_NormalizeException,
        // ex_value might still be the exception class itself (not an instance) in some cases like SyntaxError.
        // In such cases, Py_TYPE(ex_value) returns 'type' (the metaclass), not the actual exception class.
        bool is_instance = PyExceptionInstance_Check(ex_value);
        PyTypeObject* py_cls = is_instance ? Py_TYPE(ex_value) : reinterpret_cast<PyTypeObject*>(ex_type);
        err = qore_python_get_exception_name(py_cls);
        desc = qore_python_get_string(ex_value);
        // the argument is the Python exception object
        if (is_instance) {
            arg = get_arg(ex_value, **desc);
        }
    }

    // each exception is raised in its own sink, as the call stack given is added to all exceptions in a sink
    ExceptionSink xsink2;
    if (ex_loc) {
        xsink2.raiseExceptionArg(ex_loc->get(), err.c_str(), arg.release(), desc.release(), callstack);
    } else {
        xsink2.raiseExceptionArg(err.c_str(), arg.release(), desc.release(), callstack);
    }
    xsink.assimilate(xsink2);

    // the exception that caused this exception, or that was being handled when it was raised, is chained to it
    if (!PyExceptionInstance_Check(ex_value) || seen.size() >= QORE_PYTHON_MAX_CHAINED_EXCEPTIONS) {
        return;
    }
    QorePythonReferenceHolder cause(PyException_GetCause(ex_value));
    if (!cause) {
        QorePythonReferenceHolder suppress(PyObject_GetAttrString(ex_value, "__suppress_context__"));
        int suppress_context = suppress ? PyObject_IsTrue(*suppress) : 0;
        if (suppress_context < 0 || !suppress) {
            PyErr_Clear();
        }
        if (suppress_context <= 0) {
            cause = PyException_GetContext(ex_value);
        }
    }
    if (!cause || seen.find(*cause) != seen.end()) {
        return;
    }
    QorePythonReferenceHolder cause_tb(PyException_GetTraceback(*cause));
    raiseQoreException(xsink, reinterpret_cast<PyObject*>(Py_TYPE(*cause)), *cause, cause_tb ? *cause_tb : Py_None,
        seen, ex_loc);
}

QoreClass* QorePythonProgram::getCreateQorePythonClass(ExceptionSink* xsink, PyTypeObject* type, int flags) {
    // grab current Program's parse lock before manipulating namespaces
    CurrentProgramRuntimeExternalParseContextHelper pch;
    if (!pch) {
        xsink->raiseException("PROGRAM-ERROR", "cannot process Python type '%s' in deleted Program object",
            type->tp_name);
        return nullptr;
    }

    strset_t nsset;
    // the Qore classes of Python types made for Qore classes are not freed while the class is created
    QorePythonDeclUseList uses;
    return getCreateQorePythonClassIntern(xsink, type, nsset, uses, nullptr, flags);
}

QoreNamespace* QorePythonProgram::getNamespaceForObject(PyObject* obj) {
    //printd(5, "QorePythonProgram::getNamespaceForObject() obj: %p (%s)\n", obj, Py_TYPE(obj)->tp_name);
    QoreString ns_path;

    // use the __name__ attribute to derive the namespace path if possible
    if (PyObject_HasAttrString(obj, "__name__")) {
        QorePythonReferenceHolder name(PyObject_GetAttrString(obj, "__name__"));
        const char* name_str = PyUnicode_AsUTF8(*name);
        //printd(5, "QorePythonProgram::getNamespaceForObject() obj %p __name__ '%s'\n", obj, name_str);
        const char* p = strrchr(name_str, '.');
        if (p) {
            ns_path = name_str;
            ns_path.terminate(p - name_str);
            ns_path.replaceAll(".", "::");
        }
    }

    // otherwise use "module_context", if not available, use the __module__ attribute, if available
    if (!module_context && ns_path.empty() && PyObject_HasAttrString(obj, "__module__")) {
        QorePythonReferenceHolder mod(PyObject_GetAttrString(obj, "__module__"));
        if (PyUnicode_Check(*mod)) {
            const char* mod_str = PyUnicode_AsUTF8(*mod);
            //printd(5, "QorePythonProgram::getNamespaceForObject() obj %p __module__ '%s'\n", obj, mod_str);
            ns_path = mod_str;
            ns_path.replaceAll(".", "::");
        }
    }

    if (ns_path.empty()) {
        //printd(5, "QorePythonProgram::getNamespaceForObject() obj %p (%s) -> ns: Python\n", obj,
        //  Py_TYPE(obj)->tp_name);
        if (!module_context) {
            return pyns;
        }
        ns_path = module_context;
        ns_path.replaceAll(".", "::");
    }

    //printd(5, "QorePythonProgram::getNamespaceForObject() obj %p (%s) -> ns: Python::%s\n", obj,
    //  Py_TYPE(obj)->tp_name, ns_path.c_str());
    QoreNamespace* ns = pyns->findCreateNamespacePathAll(ns_path.c_str());
#if QORE_VERSION_CODE >= 10013
    if (module_context) {
        if (ns->setKeyValueIfNotSet("python_module", module_context)) {
            printd(5, "set python_module: '%s' in ns: '%s'\n", module_context, ns->getPath(true).c_str());
        }
    }
#endif
    return ns;
}

QoreNamespace* QorePythonProgram::getNamespaceForClass(ExceptionSink* xsink, PyTypeObject* type,
        const QoreString& relative_path) {
    QoreString module_name;
    bool defining_module = false;
    // Unlike functions re-exported by a module, a class has one wrapper shared by all import routes and bases.
    // Resolve its defining module before using the importing module's context. Attribute access can run Python.
    for (const char* attr : {"__module__", "__name__"}) {
        QorePythonReferenceHolder value(PyObject_GetAttrString(reinterpret_cast<PyObject*>(type), attr));
        if (!value) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
                checkPythonException(xsink);
                return nullptr;
            }
            PyErr_Clear();
            continue;
        }
        if (!PyUnicode_Check(*value)) {
            continue;
        }
        Py_ssize_t size;
        const char* str = PyUnicode_AsUTF8AndSize(*value, &size);
        if (!str) {
            checkPythonException(xsink);
            return nullptr;
        }
        if (memchr(str, '\0', size)) {
            xsink->raiseException("PYTHON-IMPORT-ERROR", "Python class attribute '%s' contains a null byte", attr);
            return nullptr;
        }
        if (!strcmp(attr, "__module__")) {
            module_name.set(str, size);
            defining_module = !module_name.empty();
        } else {
            const char* dot = strrchr(str, '.');
            if (dot) {
                module_name.set(str, dot - str);
            }
        }
        if (!module_name.empty()) {
            break;
        }
    }
    if (module_name.empty()) {
        if (!module_context) {
            return pyns;
        }
        module_name = module_context;
    }
    // Some import paths already use Qore separators. Keep the namespace metadata in Python's dotted form.
    module_name.replaceAll("::", ".");
    QoreString ns_path(module_name);
    ns_path.replaceAll(".", "::");
    QoreNamespace* ns = pyns->findCreateNamespacePathAll(ns_path.c_str());
    // Preserve the legacy relative type path fallback, but never append it to a known defining module.
    if (!defining_module && !relative_path.empty() && relative_path.startsWith(ns->getName())) {
        size_t len = strlen(ns->getName());
        if (relative_path.size() > len && relative_path[len] == '.') {
            QoreString suffix(relative_path);
            suffix.replace(0, len + 1, nullptr);
            module_name.concat('.');
            module_name.concat(suffix.c_str());
            suffix.replaceAll(".", "::");
            ns = ns->findCreateNamespacePathAll(suffix.c_str());
        }
    }
#if QORE_VERSION_CODE >= 10013
    ns->setKeyValueIfNotSet("python_module", module_name.c_str());
#endif
    return ns;
}

QoreClass* QorePythonProgram::getCreateQorePythonClassIntern(ExceptionSink* xsink, PyTypeObject* type,
        strset_t& nsset, QorePythonDeclUseList& uses, const char* cname, int flags) {
    //printd(5, "QorePythonProgram::getCreateQorePythonClassIntern() class: '%s'\n", type->tp_name);
    // see if the Python type already represents a Qore class
    if (PyQoreObjectType_Check(type)) {
        //printd(5, "QorePythonProgram::getCreateQorePythonClassIntern() class: '%s' is Qore\n", type->tp_name);
        QorePythonDeclHandle* handle = PythonQoreClass::getQoreClass(type);
        if (handle) {
            std::unique_ptr<QorePythonDeclUse> use(new QorePythonDeclUse(handle));
            if (*use) {
                uses.push_back(std::move(use));
                return const_cast<QoreClass*>(static_cast<const QoreClass*>(handle->getDecl()));
            }
            handle->raiseDeleted();
        }
        checkPythonException(xsink);
        return nullptr;
    }
    //printd(5, "QorePythonProgram::getCreateQorePythonClassIntern() creating Qore class for Python class: '%s' \n",
    //  type->tp_name);

    clmap_t::iterator i = clmap.lower_bound(type);
    if (i != clmap.end() && i->first == type) {
        return i->second;
    }

    // get relative path to class and class name
    QoreString rpath_str;
    if (!cname) {
        const char* p = strrchr(type->tp_name, '.');
        if (p) {
            rpath_str.set(type->tp_name, p - type->tp_name);
            cname = p + 1;
        } else {
            cname = type->tp_name;
        }
    }

    // Attribute access can change tp_name or re-enter the importer, so own the name and recheck the cache.
    QoreString class_name(cname);
    QoreNamespace* ns = getNamespaceForClass(xsink, type, rpath_str);
    if (!ns) {
        return nullptr;
    }
    i = clmap.lower_bound(type);
    if (i != clmap.end() && i->first == type) {
        return i->second;
    }
    return addClassToNamespaceIntern(xsink, ns, type, class_name.c_str(), i, nsset, flags);
}

QorePythonClass* QorePythonProgram::addClassToNamespaceIntern(ExceptionSink* xsink, QoreNamespace* ns,
        PyTypeObject* type, const char* cname, clmap_t::iterator i, strset_t& nsset, int flags) {
    // get a unique name for the class
    QoreString cname_str = cname;
    // namespace path
    std::string ns_path = ns->getPath(true);
    // full path of class with ns
    std::string full_path;
    strset_t::iterator nsi;
    {
        int base = 0;
        while (true) {
            full_path = ns_path + "::" + cname_str.c_str();
            nsi = nsset.find(full_path);
            if (nsi == nsset.end() && !ns->findLocalClass(cname_str.c_str())) {
                break;
            }
            cname_str.clear();
            cname_str.sprintf("%s_base_%d", cname, base++);
        }
    }
    cname = cname_str.c_str();

    // create new class
    std::unique_ptr<QorePythonClass> cls(new QorePythonClass(this, cname, full_path.c_str()));

    // add to nsset
    nsset.insert(nsi, full_path);

    // insert into map
    clmap.insert(i, clmap_t::value_type(type, cls.get()));

    //printd(5, "QorePythonProgram::addClassToNamespaceIntern() ns: '%s' cls: '%s' (%s id: %d)\n", ns->getName(),
    //  cls->getName(), type->tp_name, cls->getID());

    QorePythonClass* rv = setupQorePythonClass(xsink, ns, type, cls, nsset, flags);
    if (!rv) {
        // A base's metadata can raise an exception. Remove both mappings before the incomplete class is freed,
        // otherwise retrying the conversion finds a dangling Qore class (or reuses its reverse mapping).
        clmap.erase(type);
        nsset.erase(full_path);
        py_cls_map_t::iterator pi = py_cls_map.find(cls.get());
        if (pi != py_cls_map.end()) {
            std::unique_ptr<PythonQoreClass> py_cls(pi->second);
            py_cls_map.erase(pi);
        }
    }
    return rv;
}

static constexpr int static_meth_flags = QCF_USES_EXTRA_ARGS;
static constexpr int normal_meth_flags = static_meth_flags | QCF_ABSTRACT_OVERRIDE_ALL;

//! returns true for the names of the methods that have a special meaning in Qore classes
static bool qore_python_is_special_method_name(const char* name) {
    return !strcmp(name, "constructor") || !strcmp(name, "destructor") || !strcmp(name, "copy")
        || !strcmp(name, "methodGate") || !strcmp(name, "memberGate") || !strcmp(name, "memberNotification");
}

QorePythonClass* QorePythonProgram::setupQorePythonClass(ExceptionSink* xsink, QoreNamespace* ns, PyTypeObject* type,
        std::unique_ptr<QorePythonClass>& cls, strset_t& nsset, int flags) {
    //printd(5, "QorePythonProgram::setupQorePythonClass() ns: '%s' cls: '%s' (%s) flags: %d\n", ns->getName(),
    //  cls->getName(), type->tp_name, flags);
    cls->addConstructor((void*)type, (q_external_constructor_t)execPythonConstructor, Public,
            QCF_USES_EXTRA_ARGS, QDOM_UNCONTROLLED_API);
    cls->setDestructor((void*)type, (q_external_destructor_t)execPythonDestructor);

    // create Python mapping for QoreClass if necessary
    if (!PyQoreObjectType_Check(type)) {
        new PythonQoreClass(this, type, *cls.get());
    }

    // the Python types between the type and its nearest Qore base class (the Python type of a Qore class), if any
    std::vector<PyTypeObject*> python_bases;
    PyTypeObject* qore_base = nullptr;
    for (PyTypeObject* t = type->tp_base; t; t = t->tp_base) {
        if (PyQoreObjectType_Check(t)) {
            qore_base = t;
            break;
        }
        python_bases.push_back(t);
    }

    // the Qore classes of the Python types of Qore classes are not freed while they are added as base classes
    QorePythonDeclUseList uses;

    // a Python base class shares the Python object and is a virtual base class; a Qore base class (the base is the
    // Python type of a Qore class) is not: Qore runs the constructor, destructor and copy method of a base class only
    // if it is not virtual, and the destructor of a Qore class must run for the objects of its Python subclasses (its
    // constructor is run by PythonQoreClass::py_init()).  As Qore makes the ancestors of a virtual base class virtual,
    // a Qore class reached through Python base classes is added first as a non-virtual base class, and the methods of
    // the Python base classes are added to this class, so that they take precedence over the Qore class's methods
    if (qore_base && !python_bases.empty()) {
        QoreClass* qore_bclass = getCreateQorePythonClassIntern(xsink, qore_base, nsset, uses);
        if (!qore_bclass) {
            assert(*xsink);
            return nullptr;
        }
        cls->addBaseClass(qore_bclass, false);
    }

    // add the base classes in the order of their declaration, which is the order of Python's method resolution for
    // the methods that the class inherits
    PyObject* bases = type->tp_bases;
    Py_ssize_t base_count = bases && PyTuple_Check(bases) ? PyTuple_GET_SIZE(bases) : 0;
    for (Py_ssize_t i = 0; i < base_count; ++i) {
        PyTypeObject* base = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(bases, i));
        QoreClass* bclass = getCreateQorePythonClassIntern(xsink, base, nsset, uses);
        if (!bclass) {
            assert(*xsink);
            return nullptr;
        }

        printd(5, "QorePythonProgram::setupQorePythonClass() %s (id: %d) parent: %s (bclass: %p id: %d)\n",
            type->tp_name, cls->getID(), base->tp_name, bclass, bclass->getID());
        cls->addBaseClass(bclass, !PyQoreObjectType_Check(base));
    }

    cls->addBuiltinVirtualBaseClass(QC_PYTHONBASEOBJECT);

    // a Python iterator (ex: a generator) is a Qore iterator, so that foreach and other iterator APIs iterate it; the
    // current item is an internal member, so Qore releases it; not for types that have attributes with the names of
    // the methods
    if (PyObject_HasAttrString(reinterpret_cast<PyObject*>(type), "__next__")
        && !PyObject_HasAttrString(reinterpret_cast<PyObject*>(type), "next")
        && !PyObject_HasAttrString(reinterpret_cast<PyObject*>(type), "getValue")
        && !PyObject_HasAttrString(reinterpret_cast<PyObject*>(type), "valid")) {
        cls->addBuiltinVirtualBaseClass(QC_ABSTRACTITERATOR);
        cls->addMember(QORE_PYTHON_ITERATOR_KEY, Internal, autoTypeInfo);
        cls->addMethod(nullptr, "next", (q_external_method_t)QorePythonProgram::execPythonIteratorNext, Public,
            QCF_NO_FLAGS, QDOM_UNCONTROLLED_API, boolTypeInfo);
        cls->addMethod(nullptr, "getValue", (q_external_method_t)QorePythonProgram::execPythonIteratorGetValue,
            Public, QCF_NO_FLAGS, QDOM_UNCONTROLLED_API, autoTypeInfo);
        cls->addMethod(nullptr, "valid", (q_external_method_t)QorePythonProgram::execPythonIteratorValid, Public,
            QCF_NO_FLAGS, QDOM_UNCONTROLLED_API, boolTypeInfo);
    }

    ns->addSystemClass(cls.get());

    printd(5, "QorePythonProgram::setupQorePythonClass() %s methods: %p\n", type->tp_name,
        type->tp_methods);

    // process the dict of the type, and of the Python base classes before a Qore base class, in method resolution order
    strset_t names;
    addPythonTypeAttributes(type, cls.get(), names);
    if (qore_base) {
        for (PyTypeObject* t : python_bases) {
            addPythonTypeAttributes(t, cls.get(), names);
        }
    }

    addPythonClassMethods(type, cls.get());

    return cls.release();
}

void QorePythonProgram::addPythonClassMethods(PyTypeObject* type, QorePythonClass* cls) {
    // the class methods of the type and the ones it inherits, in method resolution order, are static methods of the
    // class bound to the type, so that they get the class they are called for, as in Python (a class method that the
    // class inherits gets the class, not the base class that defines it)
    PyObject* mro = type->tp_mro;
    Py_ssize_t mro_size = mro && PyTuple_Check(mro) ? PyTuple_GET_SIZE(mro) : 0;
    strset_t names;
    for (Py_ssize_t i = 0; i < mro_size; ++i) {
        PyTypeObject* t = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
        if (!t->tp_dict) {
            continue;
        }
        QorePythonReferenceHolder type_dict(qore_python_dict_for_iteration(t->tp_dict));
        if (!type_dict) {
            PyErr_Clear();
            continue;
        }
        PyObject* key, * value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(*type_dict, &pos, &key, &value)) {
            const char* keystr = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : nullptr;
            if (!keystr) {
                PyErr_Clear();
                continue;
            }
            // a name defined by a class earlier in the method resolution order hides the others
            if (!names.insert(keystr).second || Py_TYPE(value) != &PyClassMethod_Type) {
                continue;
            }
            QoreString special_name;
            if (qore_python_is_special_method_name(keystr)) {
                special_name.sprintf("_%s", keystr);
                keystr = special_name.c_str();
            }
            // returns a new reference: the method bound to the type
            PyObject* py_method = PyClassMethod_Type.tp_descr_get(value, nullptr, reinterpret_cast<PyObject*>(type));
            if (!py_method) {
                PyErr_Clear();
                continue;
            }
            cls->addObj(py_method);
            cls->addStaticMethod((void*)py_method, keystr,
                (q_external_static_method_t)QorePythonProgram::execPythonStaticMethod, Public, static_meth_flags,
                QDOM_UNCONTROLLED_API, autoTypeInfo);
            printd(5, "QorePythonProgram::addPythonClassMethods() added class method %s.%s() from %s\n",
                type->tp_name, keystr, t->tp_name);
        }
    }
}

// the slot of the Python objects of Qore objects whose classes inherit Python classes that holds the Qore object
static constexpr const char* QORE_PYTHON_QORE_OBJECT_SLOT = "_qore_object_";
// the name of the capsule in the slot
static constexpr const char* QORE_PYTHON_QORE_OBJECT_CAPSULE_NAME = "qoreloader.qore_object";

// the capsule holds a weak reference to the Qore object (the Qore object holds the Python object)
static void qore_python_qore_object_capsule_destructor(PyObject* capsule) {
    QoreObject* obj = static_cast<QoreObject*>(PyCapsule_GetPointer(capsule, QORE_PYTHON_QORE_OBJECT_CAPSULE_NAME));
    assert(obj);
    // after the Qore library has been shut down, the object is left to the exit of the process
    if (q_libqore_shutdown()) {
        return;
    }
    // releases only the memory of a deleted object: no Qore code runs
    obj->tDeref();
}

static PyMethodDef qore_override_def = {"qore_override", QorePythonProgram::callQoreOverride, METH_VARARGS,
    "calls the Qore method that overrides the Python method"};

QoreObject* QorePythonProgram::getQoreSubclassObject(PyObject* pyobj) {
    // only the objects of the generated types have the slot (a member descriptor in the dict of the type)
    PyTypeObject* type = Py_TYPE(pyobj);
    PyObject* descr = type->tp_dict ? PyDict_GetItemString(type->tp_dict, QORE_PYTHON_QORE_OBJECT_SLOT) : nullptr;
    if (!descr || Py_TYPE(descr) != &PyMemberDescr_Type) {
        return nullptr;
    }
    QorePythonReferenceHolder capsule(PyObject_GetAttrString(pyobj, QORE_PYTHON_QORE_OBJECT_SLOT));
    if (!capsule || !PyCapsule_IsValid(*capsule, QORE_PYTHON_QORE_OBJECT_CAPSULE_NAME)) {
        PyErr_Clear();
        return nullptr;
    }
    return static_cast<QoreObject*>(PyCapsule_GetPointer(*capsule, QORE_PYTHON_QORE_OBJECT_CAPSULE_NAME));
}

PyObject* QorePythonProgram::getQoreSubclassType(ExceptionSink* xsink, const QoreClass& qcls, PyTypeObject* pycls) {
    subclass_type_map_t::key_type key(qcls.getID(), pycls);
    {
        AutoLocker al(qpy_subclass_type_lock);
        subclass_type_map_t::iterator i = subclass_type_map.find(key);
        if (i != subclass_type_map.end()) {
            return i->second;
        }
    }

    // the methods of the Python class that the Qore class overrides call the Qore methods: the public normal methods
    // with the names of the methods of the Python class (in method resolution order) that are not implemented by
    // the Qore classes of Python classes
    QorePythonReferenceHolder dict(PyDict_New());
    if (!dict) {
        checkPythonException(xsink);
        return nullptr;
    }
    bool overrides = false;
    PyObject* mro = pycls->tp_mro;
    Py_ssize_t mro_size = mro && PyTuple_Check(mro) ? PyTuple_GET_SIZE(mro) : 0;
    strset_t names;
    for (Py_ssize_t i = 0; i < mro_size; ++i) {
        PyTypeObject* t = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
        if (!t->tp_dict) {
            continue;
        }
        QorePythonReferenceHolder type_dict(qore_python_dict_for_iteration(t->tp_dict));
        if (!type_dict) {
            checkPythonException(xsink);
            return nullptr;
        }
        PyObject* name, * value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(*type_dict, &pos, &name, &value)) {
            const char* namestr = PyUnicode_Check(name) ? PyUnicode_AsUTF8(name) : nullptr;
            if (!namestr) {
                PyErr_Clear();
                continue;
            }
            if (!names.insert(namestr).second) {
                continue;
            }
            // only methods: static methods and class methods are static methods in Qore
            PyTypeObject* vtype = Py_TYPE(value);
            if (vtype == &PyStaticMethod_Type || vtype == &PyClassMethod_Type || !PyCallable_Check(value)) {
                continue;
            }
            QoreString qname;
            if (qore_python_is_special_method_name(namestr)) {
                qname.sprintf("_%s", namestr);
            } else {
                qname.set(namestr);
            }
            ClassAccess access;
            const QoreMethod* m = qcls.findMethod(qname.c_str(), access);
            if (!m || m->isStatic() || access != Public
                || dynamic_cast<const QorePythonClass*>(m->getClass())
                || m->getClass()->getID() == QC_PYTHONBASEOBJECT->getID()) {
                continue;
            }
            QorePythonReferenceHolder qname_obj(PyUnicode_FromStringAndSize(qname.c_str(), qname.size()));
            QorePythonReferenceHolder func(qname_obj ? PyCFunction_NewEx(&qore_override_def, *qname_obj, nullptr)
                : nullptr);
            // an instance method: the object is passed as the first argument
            QorePythonReferenceHolder meth(func ? PyInstanceMethod_New(*func) : nullptr);
            if (!meth || PyDict_SetItem(*dict, name, *meth) < 0) {
                checkPythonException(xsink);
                return nullptr;
            }
            overrides = true;
        }
    }

    QorePythonReferenceHolder type;
    if (overrides) {
        QorePythonReferenceHolder slots(Py_BuildValue("(s)", QORE_PYTHON_QORE_OBJECT_SLOT));
        QorePythonReferenceHolder module_name(PyObject_GetAttrString(reinterpret_cast<PyObject*>(pycls),
            "__module__"));
        if (!module_name) {
            PyErr_Clear();
        }
        QorePythonReferenceHolder qualname(PyUnicode_FromString(qcls.getName()));
        if (!slots || !qualname || PyDict_SetItemString(*dict, "__slots__", *slots) < 0
            || PyDict_SetItemString(*dict, "__qualname__", *qualname) < 0
            || (module_name && PyDict_SetItemString(*dict, "__module__", *module_name) < 0)) {
            checkPythonException(xsink);
            return nullptr;
        }
        type = PyObject_CallFunction(reinterpret_cast<PyObject*>(&PyType_Type), "s(O)O", qcls.getName(), pycls,
            *dict);
        if (!type) {
            if (!checkPythonException(xsink)) {
                xsink->raiseException("PYTHON-ERROR", "cannot create the Python type of Qore class '%s'",
                    qcls.getName());
            }
            xsink->appendLastDescription(" (while creating the Python type of Qore class '%s', which overrides "
                "methods of Python class '%s')", qcls.getName(), pycls->tp_name);
            return nullptr;
        }
    } else {
        Py_INCREF(pycls);
        type = reinterpret_cast<PyObject*>(pycls);
    }

    AutoLocker al(qpy_subclass_type_lock);
    // another thread can have created the type meanwhile; the map keeps the first one
    std::pair<subclass_type_map_t::iterator, bool> rv = subclass_type_map.insert(
        subclass_type_map_t::value_type(key, *type));
    if (rv.second) {
        type.release();
    }
    return rv.first->second;
}

int QorePythonProgram::newQoreSubclassObject(ExceptionSink* xsink, PyTypeObject* type, QoreObject* self,
        qore_classid_t cid, const QoreListNode* args) {
    QorePythonReferenceHolder py_args(getPythonTupleValue(xsink, args));
    if (*xsink) {
        return -1;
    }
    // the object holds its Qore object, and is the private data of the Qore object, before it is initialized, so that
    // the Qore overrides, and the methods of the Python class that they call, can be called from its __init__()
    // method as well
    QorePythonReferenceHolder pyobj(type->tp_new(type, *py_args, nullptr));
    if (!pyobj) {
        checkPythonException(xsink);
        return -1;
    }
    self->tRef();
    QorePythonReferenceHolder capsule(PyCapsule_New(self, QORE_PYTHON_QORE_OBJECT_CAPSULE_NAME,
        qore_python_qore_object_capsule_destructor));
    if (!capsule) {
        self->tDeref();
        checkPythonException(xsink);
        return -1;
    }
    if (PyObject_SetAttrString(*pyobj, QORE_PYTHON_QORE_OBJECT_SLOT, *capsule) < 0) {
        checkPythonException(xsink);
        return -1;
    }
    PyObject* obj = *pyobj;
    // the private data owns the reference; if the initialization fails, it is released with the object
    self->setPrivate(cid, new QorePythonPrivateData(this, pyobj.release()));
    if (type->tp_init && type->tp_init(obj, *py_args, nullptr) < 0) {
        checkPythonException(xsink);
        return -1;
    }
    return 0;
}

PyObject* QorePythonProgram::callQoreOverride(PyObject* name, PyObject* args) {
    assert(PyUnicode_Check(name));
    assert(PyTuple_Check(args));
    const char* mname = PyUnicode_AsUTF8(name);
    if (!mname) {
        return nullptr;
    }
    QoreObject* obj = PyTuple_GET_SIZE(args) ? getQoreSubclassObject(PyTuple_GET_ITEM(args, 0)) : nullptr;
    if (!obj) {
        PyErr_Format(PyExc_TypeError, "the Qore method %s() must be called on the Python object of a Qore object",
            mname);
        return nullptr;
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    // the thread state that Python called us with; it must be current again before returning
    PyThreadState* entry_tstate = qore_python_attached_thread_state();

    QorePythonProgram* pypgm = QorePythonProgram::getExecutionContext();
    // the object is kept while its method runs, also if its slot is changed meanwhile; a deleted object raises
    // OBJECT-ALREADY-DELETED, as with Qore's weak references
    struct WeakObjectHolder {
        QoreObject* obj;
        DLLLOCAL WeakObjectHolder(QoreObject* obj) : obj(obj) {
            obj->tRef();
        }
        DLLLOCAL ~WeakObjectHolder() {
            obj->tDeref();
        }
    } weak_holder(obj);

    ExceptionSink xsink;
    {
        QorePythonHelper qph(pypgm, &xsink);
        if (!xsink) {
            ReferenceHolder<QoreListNode> qargs(pypgm->getQoreListFromTuple(&xsink, args, 1), &xsink);
            if (!xsink) {
                ValueHolder rv(&xsink);
                {
                    // the method is Qore code, which runs without the GIL, as other Qore code called from Python
                    QorePythonReleaseGilHelper prgh;
                    // the Python frames of the call are part of the Qore call stack
                    QorePythonStackLocationHelper slh(qph.getOldProgram());
                    rv = obj->evalMethod(mname, *qargs, &xsink);
                }
                QorePythonReferenceHolder py_rv;
                if (!xsink) {
                    py_rv = pypgm->getPythonValue(*rv, &xsink);
                }
                // the arguments and the result of the call are released without the GIL if this can run Qore code
                qore_python_release(rv);
                qore_python_release(qargs);
                if (!xsink) {
                    assert(py_rv);
                    return py_rv.release();
                }
            }
        }
    }

    // the exception must be raised in the thread state that Python called us with
    PyThreadState* exit_tstate = qore_python_attached_thread_state();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }
    assert(xsink);
    pypgm->raisePythonException(xsink);
    return nullptr;
}

void QorePythonProgram::addPythonTypeAttributes(PyTypeObject* type, QorePythonClass* cls, strset_t& names) {
    QorePythonReferenceHolder type_dict(type->tp_dict ? qore_python_dict_for_iteration(type->tp_dict) : nullptr);
    if (type->tp_dict && !type_dict) {
        PyErr_Clear();
    }
    if (type_dict) {
        PyObject* key, * value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(*type_dict, &pos, &key, &value)) {
            assert(Py_TYPE(key) == &PyUnicode_Type);
            const char* keystr = PyUnicode_AsUTF8(key);
            if (!keystr) {
                PyErr_Clear();
                continue;
            }
            // a name defined by a subclass, which comes first in the method resolution order, is not added again
            if (!names.insert(keystr).second) {
                continue;
            }
            // the names of special Qore methods (ex: constructor, copy, methodGate) are imported with a '_' prefix
            // (ex: Python's copy() is _copy() in Qore), as Qore gives these names special semantics
            QoreString special_name;
            if (qore_python_is_special_method_name(keystr)) {
                special_name.sprintf("_%s", keystr);
                keystr = special_name.c_str();
            }

            PyTypeObject* var_type = Py_TYPE(value);
            // check for static method
            if (var_type == &PyStaticMethod_Type) {
                // get callable from static method
                // returns a new reference
                PyObject* py_method = PyStaticMethod_Type.tp_descr_get(value, nullptr, nullptr);
                assert(py_method);
                cls->addObj(py_method);
                cls->addStaticMethod((void*)py_method, keystr,
                    (q_external_static_method_t)QorePythonProgram::execPythonStaticMethod, Public, static_meth_flags,
                    QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added static method " \
                    "%s.%s() (%s)\n", type->tp_name, keystr, Py_TYPE(value)->tp_name);
                continue;
            }
            // check for wrapper descriptors -> normal method
            if (var_type == &PyWrapperDescr_Type) {
                Py_INCREF(value);
                cls->addObj(value);
                cls->addMethod((void*)value, keystr,
                    (q_external_method_t)QorePythonProgram::execPythonNormalWrapperDescriptorMethod, Public,
                    normal_meth_flags, QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added normal wrapper " \
                    "descriptor method %s.%s() (%s) %p: %zd\n", type->tp_name, keystr, Py_TYPE(value)->tp_name, value,
                    (Py_ssize_t)Py_REFCNT(value));
                continue;
            }
            // check for method descriptors -> normal method
            if (var_type == &PyMethodDescr_Type) {
                Py_INCREF(value);
                cls->addObj(value);
                cls->addMethod((void*)value, keystr,
                    (q_external_method_t)QorePythonProgram::execPythonNormalMethodDescriptorMethod, Public,
                    normal_meth_flags, QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added normal method " \
                    "descriptor method %s.%s() (%s) %p: %zd\n", type->tp_name, keystr, Py_TYPE(value)->tp_name, value,
                    (Py_ssize_t)Py_REFCNT(value));
                continue;
            }
            // check for classmethod descriptors -> normal method
            if (var_type == &PyClassMethodDescr_Type) {
                // do not need to save reference here
                cls->addMethod((void*)value, keystr,
                    (q_external_method_t)QorePythonProgram::execPythonNormalClassMethodDescriptorMethod, Public,
                    normal_meth_flags, QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added normal "
                    "classmethod descriptor method %s.%s() (%s)\n", type->tp_name, keystr, Py_TYPE(value)->tp_name);
                continue;
            }
            // check for normal user methods
            if (PyFunction_Check(value)) {
                Py_INCREF(value);
                cls->addObj(value);
                cls->addMethod((void*)value, keystr,
                    (q_external_method_t)QorePythonProgram::execPythonNormalMethod, Public, normal_meth_flags,
                    QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added normal method " \
                    "%s.%s() (%s)\n", type->tp_name, keystr, Py_TYPE(value)->tp_name);
                continue;
            }
            // check for builtin functions -> static method
            if (PyCFunction_Check(value)) {
                // do not need to save reference here
                cls->addStaticMethod((void*)value, keystr,
                    (q_external_static_method_t)QorePythonProgram::execPythonStaticCFunctionMethod, Public,
                    static_meth_flags, QDOM_UNCONTROLLED_API, autoTypeInfo);
                printd(5, "QorePythonProgram::setupQorePythonClass() added static C function method " \
                    "%s.%s() (%s)\n", type->tp_name, keystr, Py_TYPE(value)->tp_name);
                continue;
            }
            // check for member descriptors
            if (var_type == &PyMemberDescr_Type) {
                cls->addPythonMember(keystr, reinterpret_cast<PyMemberDescrObject*>(value)->d_member);
                continue;
            }

            printd(5, "QorePythonProgram::setupQorePythonClass() %s: member '%s': %s\n", type->tp_name, keystr,
                Py_TYPE(value)->tp_name);
        }
    }

}

QoreValue QorePythonProgram::execPythonStaticCFunctionMethod(const QoreMethod& meth, PyObject* func,
        const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonStaticCFunctionMethod() m: %s::%s() argcount: %d\n", meth.getClassName(),
    //    meth.getName(), args ? (int)args->size() : 0);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callCFunctionMethod(xsink, func, args);
}

QoreValue QorePythonProgram::execPythonCFunction(PyObject* func, const QoreListNode* args, q_rt_flags_t rtflags,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonCFunction() argcount: %d\n", args ? (int)args->size() : 0);
    QorePythonProgram* pypgm = QorePythonProgram::getContext();
    return pypgm->callCFunctionMethod(xsink, func, args);
}

QoreValue QorePythonProgram::execPythonFunction(PyObject* func, const QoreListNode* args, q_rt_flags_t rtflags,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonFunction() argcount: %d\n", args ? (int)args->size() : 0);
    QorePythonProgram* pypgm = QorePythonProgram::getContext();
    return pypgm->callFunctionObject(xsink, func, args);
}

QoreValue QorePythonProgram::callCFunctionMethod(ExceptionSink* xsink, PyObject* func, const QoreListNode* args,
        size_t arg_offset) {
    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, qpgm);
    if (*xsink) {
        return QoreValue();
    }

    QorePythonHelper qph(this, xsink);
    QorePythonReferenceHolder py_args;
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }

    py_args = getPythonTupleValue(xsink, args, arg_offset);
    if (*xsink) {
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callCMethod(): argcount: %d\n",
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0);
    QorePythonReferenceHolder return_value(PyObject_Call(func, *py_args, nullptr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, *return_value);
}

void QorePythonProgram::execPythonConstructor(const QoreMethod& meth, PyObject* pycls, QoreObject* self,
    const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    // set Qore program context for Qore APIs
    QoreExternalProgramContextHelper pch(xsink, pypgm->qpgm);
    if (*xsink) {
        return;
    }

    QorePythonHelper qph(pypgm, xsink);
    if (*xsink || pypgm->checkValid(xsink)) {
        return;
    }

    assert(PyType_Check(pycls));

    //printd(5, "QorePythonProgram::execPythonConstructor() m: %s args: %d self: %s\n", meth.getName(),
    //    args ? (int)args->size() : 0, self->getClassName());

    // save Qore object for any Python class that needs it
    QorePythonImplicitQoreArgHelper qpiqoh(self);
    // the object of a Qore class that inherits the Python class is an object of a generated subclass whose methods
    // that the Qore class overrides call the Qore methods
    PyObject* type = pycls;
    const QoreClass* qcls = self->getClass();
    if (qcls != meth.getClass() && !dynamic_cast<const QorePythonClass*>(qcls)) {
        type = pypgm->getQoreSubclassType(xsink, *qcls, reinterpret_cast<PyTypeObject*>(pycls));
        if (!type) {
            assert(*xsink);
            return;
        }
    }
    if (type != pycls) {
        pypgm->newQoreSubclassObject(xsink, reinterpret_cast<PyTypeObject*>(type), self, meth.getClass()->getID(),
            args);
        return;
    }
    QorePythonReferenceHolder pyobj(pypgm->callPythonInternal(xsink, pycls, args));
    if (*xsink) {
        return;
    }

    // check base class initialization
    self->setPrivate(meth.getClass()->getID(), new QorePythonPrivateData(pypgm, pyobj.release()));
}

void QorePythonProgram::execPythonDestructor(const QorePythonClass& thisclass, PyObject* pycls, QoreObject* self,
        QorePythonPrivateData* pd, ExceptionSink* xsink) {
    if (!q_libqore_exiting()) {
        // The holder releases in the owning interpreter, or has already been
        // invalidated during interpreter finalization. Always release the C++ data.
        pd->deref(xsink);
    }
}

QoreValue QorePythonProgram::execPythonIteratorNext(const QoreMethod& meth, const void* m, QoreObject* self,
        QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    ValueHolder item(xsink);
    bool valid = false;
    {
        QoreExternalProgramContextHelper pch(xsink, pypgm->qpgm);
        if (*xsink) {
            return QoreValue();
        }
        QorePythonHelper qph(pypgm, xsink);
        if (*xsink || pypgm->checkValid(xsink)) {
            return QoreValue();
        }
        QorePythonReferenceHolder py_item(PyIter_Next(pd->get()));
        if (py_item) {
            item = pypgm->getQoreValue(xsink, *py_item);
            if (*xsink) {
                return QoreValue();
            }
            valid = true;
        } else if (pypgm->checkPythonException(xsink)) {
            return QoreValue();
        }
    }
    // the current item: a list with the item, or no value at the end of the iteration
    ValueHolder current(xsink);
    if (valid) {
        ReferenceHolder<QoreListNode> l(new QoreListNode(autoTypeInfo), xsink);
        l->push(item.release(), xsink);
        current = l.release();
    }
    self->setMemberValue(QORE_PYTHON_ITERATOR_KEY, meth.getClass(), current.release(), xsink);
    return *xsink ? QoreValue() : QoreValue(valid);
}

QoreValue QorePythonProgram::execPythonIteratorGetValue(const QoreMethod& meth, const void* m, QoreObject* self,
        QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    ValueHolder current(self->getReferencedMemberNoMethod(QORE_PYTHON_ITERATOR_KEY, meth.getClass(), xsink), xsink);
    if (*xsink) {
        return QoreValue();
    }
    if (current->getType() != NT_LIST) {
        xsink->raiseException("ITERATOR-ERROR", "the %s object is not on a valid iterator position",
            meth.getClassName());
        return QoreValue();
    }
    return current->get<const QoreListNode>()->getReferencedEntry(0);
}

QoreValue QorePythonProgram::execPythonIteratorValid(const QoreMethod& meth, const void* m, QoreObject* self,
        QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    ValueHolder current(self->getReferencedMemberNoMethod(QORE_PYTHON_ITERATOR_KEY, meth.getClass(), xsink), xsink);
    return *xsink ? QoreValue() : QoreValue(current->getType() == NT_LIST);
}

QoreValue QorePythonProgram::execPythonStaticMethod(const QoreMethod& meth, PyObject* m,
        const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callInternal(xsink, m, args);
}

QoreValue QorePythonProgram::execPythonNormalMethod(const QoreMethod& meth, PyObject* m, QoreObject* self,
        QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonNormalMethod() %s::%s() pyobj: %p: %d\n", meth.getClassName(),
    //    meth.getName(), m, m->ob_refcnt);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callInternal(xsink, m, args, 0, pd->get());
}

QoreValue QorePythonProgram::execPythonNormalWrapperDescriptorMethod(const QoreMethod& meth, PyObject* m,
        QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonNormalWrapperDescriptorMethod() %s::%s() pyobj: %p: %zd\n",
    //    meth.getClassName(), meth.getName(), m, (Py_ssize_t)Py_REFCNT(m));
    assert(Py_REFCNT(m) > 0);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callWrapperDescriptorMethod(xsink, pd->get(), m, args);
}

QoreValue QorePythonProgram::execPythonNormalMethodDescriptorMethod(const QoreMethod& meth, PyObject* m,
        QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonNormalMethodDescriptorMethod() %s::%s() pyobj: %p: %zd\n",
    //    meth.getClassName(), meth.getName(), m, (Py_ssize_t)Py_REFCNT(m));
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callMethodDescriptorMethod(xsink, pd->get(), m, args);
}

QoreValue QorePythonProgram::execPythonNormalClassMethodDescriptorMethod(const QoreMethod& meth, PyObject* m,
        QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
        ExceptionSink* xsink) {
    //printd(5, "QorePythonProgram::execPythonNormalClassMethodDescriptorMethod() %s::%s() pyobj: %p: %d\n",
    //    meth.getClassName(), meth.getName(), m, m->ob_refcnt);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    return pypgm->callClassMethodDescriptorMethod(xsink, pd->get(), m, args);
}

QoreValue QorePythonProgram::callWrapperDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
        const QoreListNode* args, size_t arg_offset) {
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }
    QorePythonReferenceHolder py_args;
    py_args = getPythonTupleValue(xsink, args, arg_offset, self);
    if (*xsink) {
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callWrapperDescriptorMethod() argcount: %d\n",
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0);
    QorePythonReferenceHolder return_value(PyWrapperDescr_Type.tp_call(obj, *py_args, nullptr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, *return_value);
}

QoreValue QorePythonProgram::callMethodDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
        const QoreListNode* args, size_t arg_offset) {
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }
    QorePythonReferenceHolder py_args;

    py_args = getPythonTupleValue(xsink, args, arg_offset, self);
    if (*xsink) {
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callMethodDescriptorMethod() argcount: %d\n",
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0);
    QorePythonReferenceHolder return_value(PyMethodDescr_Type.tp_call(obj, *py_args, nullptr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, *return_value);
}

QoreValue QorePythonProgram::callClassMethodDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
        const QoreListNode* args, size_t arg_offset) {
    QorePythonHelper qph(this, xsink);
    if (*xsink || checkValid(xsink)) {
        return QoreValue();
    }

    // get class from self
    QorePythonReferenceHolder cls(PyObject_GetAttrString(self, "__class__"));

    QorePythonReferenceHolder py_args;
    py_args = getPythonTupleValue(xsink, args, arg_offset, *cls);
    if (*xsink) {
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callClassMethodDescriptorMethod(): argcount: %d\n",
    //    (args && args->size() > arg_offset) ? args->size() - arg_offset : 0);
    QorePythonReferenceHolder return_value(PyClassMethodDescr_Type.tp_call(obj, *py_args, nullptr));
    // check for Python exceptions
    if (!return_value && checkPythonException(xsink)) {
        return QoreValue();
    }

    return getQoreValue(xsink, *return_value);
}

int QorePythonProgram::saveModule(const char* name, PyObject* mod) {
    QorePythonReferenceHolder sys(PyImport_ImportModule("sys"));
    if (!sys) {
        return -1;
    }
    if (!PyObject_HasAttrString(*sys, "modules")) {
        return -1;
    }
    QorePythonReferenceHolder modules(PyObject_GetAttrString(*sys, "modules"));
    printd(5, "QorePythonProgram::saveModule() modules: %p '%s' %s\n", *modules, name, Py_TYPE(*modules)->tp_name);
    if (!PyDict_Check(*modules)) {
        return -1;
    }
    return PyDict_SetItemString(*modules, name, mod);
}

int QorePythonProgram::import(ExceptionSink* xsink, const char* module, const char* symbol, int filter) {
    //printd(5, "QorePythonProgram::import() module: '%s' symbol: '%s'\n", module, symbol ? symbol : "n/a");

    QoreString mod_name(module);
    QorePythonReferenceHolder mod;
    if (symbol) {
        QoreString sym(symbol);
        qore_offset_t i = sym.rfind('.');
        if (i > 0 && (size_t)i != (sym.size() - 1)) {
            mod_name.concat('.');
            mod_name.concat(sym, 0, i, xsink);
            //printd(5, "trying to import '%s'\n", mod_name.c_str());
            mod = PyImport_ImportModule(mod_name.c_str());

            if (!mod) {
                if (!checkPythonException(xsink)) {
                    throw QoreStandardException("PYTHON-IMPORT-ERROR", "Python could not load module '%s'", module);
                }
                return -1;
            }

            // if the module has already been imported, then ignore
            if (mod_set.find(*mod) != mod_set.end()) {
                return 0;
            }

            QoreString ns_path(mod_name);
            ns_path.replaceAll(".", "::");

            symbol = sym.c_str() + i + 1;
            //printd(5, "imported module: '%s' importing symbol: '%s' (ns_path: '%s')\n", mod_name.c_str(), symbol,
            //    ns_path.c_str());

            PythonModuleContextHelper mch(this, ns_path.c_str());
            // https://docs.python.org/3/reference/import.html:
            // any module that contains a __path__ attribute is considered a package
            return checkImportSymbol(xsink, mod_name.c_str(), *mod, PyObject_HasAttrString(*mod, "__path__"), symbol,
                filter, false);
        }
    }

    mod = PyImport_ImportModule(module);

    if (!mod) {
        if (!checkPythonException(xsink)) {
            throw QoreStandardException("PYTHON-IMPORT-ERROR", "Python could not load module '%s'", module);
        }
        return -1;
    }

    // if the module has already been imported, then ignore
    if (mod_set.find(*mod) != mod_set.end()) {
        return 0;
    }

    return importModule(xsink, *mod, module, filter);
}

int QorePythonProgram::importModule(ExceptionSink* xsink, PyObject* mod, const char* module,
    int filter) {
    PythonModuleContextHelper mch(this, module);

    // if the module has already been imported, then ignore
    if (mod_set.find(mod) != mod_set.end()) {
        return 0;
    }
    mod_set.insert(mod);

    PyObject* main = PyImport_AddModule("__main__");
    assert(main);
    Py_INCREF(mod);
    if (PyModule_AddObject(main, module, mod) < 0) {
        Py_DECREF(mod);
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-IMPORT-ERROR", "module '%s' could not be added to the main module", module);
        }
        return -1;
    }

    // returns a borrowed reference
    PyObject* mod_dict = PyModule_GetDict(mod);
    if (!mod_dict) {
        // no dictionary; cannot import module
        throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' has no dictionary", module);
    }
    // https://docs.python.org/3/reference/import.html:
    // any module that contains a __path__ attribute is considered a package
    bool is_package = (bool)PyDict_GetItemString(mod_dict, "__path__");

    //printd(5, "QorePythonProgram::importModule() '%s' mod: %p (%d) pkg: %d (def: %p)\n", module, mod, filter,
    //  is_package, PyModule_GetDef(mod));

    // check the dictionary for __all__, giving a list of strings as public symbols
    // returns a borrowed reference
    {
        PyObject* all = PyDict_GetItemString(mod_dict, "__all__");
        // any sequence (usually a list or a tuple); a tuple is made of a list, as importing a symbol runs Python
        // code that can change the list (and other threads can in free-threaded builds)
        QorePythonReferenceHolder all_seq(all ? PySequence_Tuple(all) : nullptr);
        if (all && !all_seq) {
            PyErr_Clear();
            throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' __all__ is not a sequence (type '%s')",
                module, Py_TYPE(all)->tp_name);
        }
        if (all_seq) {
            Py_ssize_t len = PyTuple_GET_SIZE(*all_seq);
            for (Py_ssize_t i = 0; i < len; ++i) {
                PyObject* sv = PyTuple_GET_ITEM(*all_seq, i);
                if (!PyUnicode_Check(sv)) {
                    throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' __all__ has an invalid " \
                        "element with type '%s'; expecting 'str'", module, Py_TYPE(sv)->tp_name);
                }
                // ex: a name with a lone surrogate, which cannot be encoded in UTF-8 (UnicodeEncodeError)
                const char* symbol = PyUnicode_AsUTF8(sv);
                if (!symbol) {
                    checkPythonException(xsink);
                    return -1;
                }
                if (checkImportSymbol(xsink, module, mod, is_package, symbol, filter, true)) {
                    return -1;
                }
            }
            printd(5, "QorePythonProgram::importModule() '%s' mod: %p (%d) pkg: %d imported __all__: %d\n", module,
                mod, filter, is_package, len);
            return 0;
        }
    }

    QorePythonReferenceHolder dir(PyObject_Dir(mod));
    if (dir && PyList_Check(*dir)) {
        Py_ssize_t len = PyList_Size(*dir);
        for (Py_ssize_t i = 0; i < len; ++i) {
            // the list is not referenced elsewhere; an item is referenced while the symbol is imported
            QorePythonReferenceHolder sv(PyList_GET_ITEM(*dir, i));
            sv.py_ref();
            if (!PyUnicode_Check(*sv)) {
                throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' dir() has an invalid " \
                    "element with type '%s'; expecting 'str'", module, Py_TYPE(*sv)->tp_name);
            }
            // a name that cannot be encoded in UTF-8 (ex: with a lone surrogate) cannot be a Qore symbol: skipped
            const char* symbol = PyUnicode_AsUTF8(*sv);
            if (!symbol) {
                PyErr_Clear();
                continue;
            }
            if (checkImportSymbol(xsink, module, mod, is_package, symbol, filter, true)) {
                return -1;
            }
        }
        printd(5, "QorePythonProgram::importModule() '%s' mod: %p (%d) pkg: %d imported dir: %d\n", module, mod,
            filter, is_package, len);
        return 0;
    }

    throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' has no symbol directory", module);
}

int QorePythonProgram::checkImportSymbol(ExceptionSink* xsink, const char* module, PyObject* mod, bool is_package,
    const char* symbol, int filter, bool ignore_missing) {
    if (!PyObject_HasAttrString(mod, symbol)) {
        if (ignore_missing) {
            return 0;
        }
        throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' references unknown symbol '%s'", module,
            symbol);
    }
    QorePythonReferenceHolder value(PyObject_GetAttrString(mod, symbol));
    if (!value) {
        if (!checkPythonException(xsink)) {
            throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' failed to get symbol '%s'", module,
                symbol);
        }
        return -1;
    }

    bool is_class = PyType_Check(*value);
    if (is_class) {
        if (!(filter & IF_CLASS)) {
            return 0;
        }
    } else {
        bool is_module = PyModule_Check(*value);
        if (is_module) {
            if (!is_package) {
                return 0;
            }
        } else if (!(filter & IF_OTHER)) {
            return 0;
        }
    }

    return importSymbol(xsink, *value, module, symbol, filter);
}

int QorePythonProgram::findCreateQoreFunction(PyObject* value, const char* symbol, q_external_func_t func) {
    QoreNamespace* ns = getNamespaceForObject(value);
    if (!ns->findLocalFunction(symbol)) {
        // the function is referenced as long as the program is: it is not necessarily referenced elsewhere (ex: a
        // function that the __getattr__() of a module returned)
        Py_INCREF(value);
        addObj(value);
        ns->addBuiltinVariant((void*)value, symbol, func, QCF_USES_EXTRA_ARGS, QDOM_UNCONTROLLED_API, autoTypeInfo);
        printd(5, "QorePythonProgram::findCreateQoreFunction() added function %s::%s() (%s)\n", ns->getName(),
            symbol, Py_TYPE(value)->tp_name);
        return 0;
    }
    return -1;
}

int QorePythonProgram::importSymbol(ExceptionSink* xsink, PyObject* value, const char* module,
        const char* symbol, int filter) {
    if (!value) {
        if (!checkPythonException(xsink)) {
            xsink->raiseException("PYTHON-IMPORT-ERROR", "module '%s' returned null for symbol '%s'", module, symbol);
        }
        return -1;
    }
    PyTypeObject* value_type = Py_TYPE(value);
    if (!value_type) {
        xsink->raiseException("PYTHON-IMPORT-ERROR", "module '%s' symbol '%s' has null type", module, symbol);
        return -1;
    }
    printd(5, "QorePythonProgram::importSymbol() %s.%s (type %s)\n", module, symbol, value_type->tp_name);
    // check for builtin functions -> static method
    if (PyCFunction_Check(value)) {
        // ignore errors
        findCreateQoreFunction(value, symbol, (q_external_func_t)QorePythonProgram::execPythonCFunction);
        return 0;
    }

    if (PyFunction_Check(value)) {
        // ignore errors
        findCreateQoreFunction(value, symbol, (q_external_func_t)QorePythonProgram::execPythonFunction);
        return 0;
    }

    if (PyType_Check(value)) {
        //printd(5, "QorePythonProgram::importSymbol() class sym: '%s' -> '%s' (%p)\n", symbol,
        //  reinterpret_cast<PyTypeObject*>(value)->tp_name, value);
        strset_t nsset;
        QorePythonDeclUseList uses;
        getCreateQorePythonClassIntern(xsink, reinterpret_cast<PyTypeObject*>(value), nsset, uses);
        if (*xsink) {
            return -1;
        }

        //printd(5, "QorePythonProgram::importSymbol() added class %s.%s (%s)\n", module, symbol, cls->getName());
        return 0;
    }

    if (PyModule_Check(value)) {
        QoreStringMaker sub_module("%s::%s", module, symbol);
        return importModule(xsink, value, sub_module.c_str(), filter);
    }

    if (filter & IF_NO_CONSTANTS) {
        return 0;
    }

    //printd(5, "QorePythonProgram::importSymbol() adding const %s.%s = '%s'\n", module, symbol,
    //  Py_TYPE(value)->tp_name);
    ValueHolder v(getQoreValue(xsink, value), xsink);
    if (*xsink) {
        return -1;
    }
    // skip empty values
    if (!v) {
        return 0;
    }
    const QoreTypeInfo* typeInfo = v->getFullTypeInfo();

    QoreString ns_path = module_context;
    ns_path.replaceAll(".", "::");
    QoreNamespace* ns = pyns->findCreateNamespacePathAll(ns_path.c_str());
    if (ns->findLocalConstant(symbol)) {
        return 0;
    }

    //std::string path = ns->getPath();
    //printd(5, "QorePythonProgram::importSymbol() adding const %s.%s = '%s' (ns: %s)\n", module_context, symbol,
    //  qore_type_get_name(typeInfo), path.c_str());
    ns->addConstant(symbol, v.release(), typeInfo);
    return 0;
}

// Python integration
PyObject* QorePythonProgram::callQoreFunction(PyObject* self, PyObject* args) {
    /*
    QorePythonReferenceHolder selfstr(PyObject_Repr(self));
    assert(PyUnicode_Check(*selfstr));
    QorePythonReferenceHolder argstr(PyObject_Repr(args));
    assert(PyUnicode_Check(*argstr));
    printd(5, "QorePythonProgram::callQoreFunction() self: %p (%s) args: %s\n", self, PyUnicode_AsUTF8(*selfstr),
        PyUnicode_AsUTF8(*argstr));
    */

    assert(PyCapsule_CheckExact(self));
    func_capsule_t* fc = reinterpret_cast<func_capsule_t*>(PyCapsule_GetPointer(self, nullptr));
    assert(&fc->func);
    assert(fc->py_pgm);
    if (!fc->py_pgm->isValid() || fc->py_pgm->isDestroyed()) {
        PyErr_SetString(PyExc_RuntimeError, "the owning Qore program has been deleted");
        return nullptr;
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }

    // Qore's fork() called from Python code of the main interpreter runs Python's fork protocol, as os.fork() does
    // (see qore_python_register_fork_protocol())
    PyThreadState* fork_tstate = nullptr;
    if (fc->is_fork) {
        PyThreadState* tstate = PyThreadState_Get();
        if (qore_python_get_interpreter(tstate) == PyInterpreterState_Main()) {
            fork_tstate = tstate;
        }
    }

    // get Qore arguments
    ExceptionSink xsink;
    assert(PyTuple_Check(args));
    ReferenceHolder<QoreListNode> qargs(fc->py_pgm->getQoreListFromTuple(&xsink, args), &xsink);
    if (!xsink) {
        ValueHolder rv(&xsink);
        {
            QorePythonReleaseGilHelper prgh;
            QorePythonForkProtocolHelper fph(fork_tstate);
            // the Python frames of the call are part of the Qore call stack
            QorePythonStackLocationHelper slh(fc->py_pgm);
            rv = fc->func.evalFunction(nullptr, *qargs, fc->py_pgm->getQoreProgram(), &xsink);
        }
        QorePythonReferenceHolder py_rv;
        if (!xsink) {
            py_rv = fc->py_pgm->getPythonValue(*rv, &xsink);
        }
        // the arguments and the result of the call are released without the GIL if this can run Qore code
        qore_python_release(rv);
        qore_python_release(qargs);
        if (!xsink) {
            assert(py_rv);
            return py_rv.release();
        }
    }

    fc->py_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return nullptr;
}
