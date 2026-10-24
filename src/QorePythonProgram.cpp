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

#include <atomic>
#include <pthread.h>

#include "QorePythonPrivateData.h"
#include "QorePythonClass.h"
#include "QoreLoader.h"
#include "PythonQoreClass.h"
#include "QoreMetaPathFinder.h"

#include "PythonCallableCallReferenceNode.h"
#include "PythonQoreCallable.h"
#include "ModuleNamespace.h"
#include "QorePythonStackLocationHelper.h"

#include <structmember.h>
#include <frameobject.h>
#include <datetime.h>

#include <climits>
#include <vector>
#include <string>

static const char* this_file = q_basenameptr(__FILE__);

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
    }
    for (const QorePythonHelper* helper = QorePythonHelper::getCurrent(); helper; helper = helper->getPrevious()) {
        if (helper->isCounted()) {
            ++helper->getProgram()->pgm_thr_cnt;
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

QorePythonProgram::QorePythonProgram(QoreProgram* qpgm, QoreNamespace* pyns)
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

    ExceptionSink xsink;
    if (createInterpreter(qpgh, &xsink)) {
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
    import(&xsink, "builtins", nullptr, IF_ALL | IF_NO_CONSTANTS);
    if (xsink) {
        valid = false;
        return;
    }
    //assert(!xsink);

    // returns a borrowed reference
    setGlobalDictionary(*module);
    assert(!PyErr_Occurred());

    // import qoreloader module
    QorePythonReferenceHolder qoreloader(PyImport_ImportModule("qoreloader"));
    if (!qoreloader) {
        if (!checkPythonException(&xsink)) {
            xsink.raiseException("PYTHON-COMPILE-ERROR", "cannot load the 'qoreloader' module");
        }
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
    if (owner) {
        owner->releasePersistentReference(this);
        owner->weakDeref();
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

void QorePythonProgram::deleteIntern(ExceptionSink* xsink) {
    if (destroyed) {
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

    if (qpgm && owns_qore_program_ref) {
        // remove the external data before dereferencing
        qpgm->removeExternalData(QORE_PYTHON_MODULE_NAME);
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

            module.purge();
            python_code.purge();

            // Remove bridge methods before releasing the types. Bound methods
            // that survive own their metadata through their Python capsules.
            for (auto& i : py_cls_map) {
                i.second->clearMethods();
            }

            // Release references in both embedded and Python-hosted processes.
            for (auto& i : py_cls_map) {
                i.second->release();
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
        PyStatus status = Py_NewInterpreterFromConfig(&python, &config);
        if (PyStatus_Exception(status)) {
            if (xsink) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "error creating the Python subinterpreter: %s",
                    status.err_msg ? status.err_msg : "unknown error");
            }
            return -1;
        }
#else
        python = Py_NewInterpreter();
#endif

        if (!python) {
            if (xsink) {
                xsink->raiseException("PYTHON-COMPILE-ERROR", "error creating the Python subinterpreter");
            }
            return -1;
        }
        _QORE_GILSTATE_COUNTER_ASSERT_ONE(python);
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
    PyThreadState* python = getAcquireThreadState();

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
        bool found = false;
        if (python->thread_id == PyThread_get_thread_ident()) {
            PyThreadState* ts = PyInterpreterState_ThreadHead(interpreter);
            while (ts) {
                if (ts == python) {
                    found = true;
                    break;
                }
                ts = PyThreadState_Next(ts);
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
#elif !defined(QORE_PYTHON_INCLUDE_INTERNALS)
        PyThreadState* python_thread_state = _qore_check_python_created_thread_gil();
#endif
#if defined(Py_GIL_DISABLED) || !defined(QORE_PYTHON_INCLUDE_INTERNALS)
        if (python_thread_state && python_thread_state->interp == interpreter) {
            // This is a Python-created thread - use the existing thread state
            // The thread already has the GIL, so we don't need to acquire it
            python = python_thread_state;
            is_python_created_thread = true;
            printd(5, "QorePythonProgram::setContext() detected Python-created thread, using existing "
                "thread state: %p\n", python);
        }
#endif

        if (!python) {
            // This is a Qore-created thread - create a new thread state
            printd(5, "QorePythonProgram::setContext() creating new thread state for interpreter: %p\n",
                interpreter);
            python = PyThreadState_New(interpreter);

            printd(5, "QorePythonProgram::setContext() this: %p created new thread context: %p (py_thr_map: %p "
                "size: %d)\n", this, python, &py_thr_map, (int)py_thr_map.size());
            assert(python);
            _QORE_GILSTATE_COUNTER_ASSERT_ONE(python);


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
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && !defined(QORE_PYTHON_INCLUDE_INTERNALS)
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

    printd(5, "QorePythonProgram::setContext() got thread context: %p (GIL: %d hG: %d) refs: %d\n", python,
        PyGILState_Check(), haveGil(), _QORE_GILSTATE_COUNTER_GET(python));
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
        PyEval_RestoreThread(python);
        g_state = PyGILState_UNLOCKED;
        _qore_gil_held = true;  // Track that we now hold the GIL
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
    if (have_gil) {
        // If bound_gilstate is stale, clear it to avoid tstate_activate assertions.
        if (qore_py_get_bound_gilstate(python) && PyGILState_GetThisThreadState() != python) {
            qore_py_set_bound_gilstate(python, 0);
        }
        // We already have the GIL - swap thread states
        ceval_state = _qore_PyCeval_SwapThreadState(python);
        g_state = PyGILState_LOCKED;
        // Also update Python's TSS via PyThreadState_Swap
        // NOTE: Our tracking is updated above, but Python's autoTSSkey is not.
        // PyThreadState_Swap updates both the ceval thread state and autoTSSkey.
        if (PyGILState_GetThisThreadState() != python) {
            PyThreadState_Swap(python);
        }
    } else {
        // We don't have the GIL - acquire it
        // Without the GIL, this thread has no state bound to it for the GIL state API but one that only it uses
        // (qore_python_release_context() unbinds the state it releases): CPython binds python to the thread when
        // python is attached, and clears the flag of a state bound before, with the GIL held.  No bound_gilstate flag
        // is changed here (see the same case for Python 3.13 and later above).
        ceval_state = nullptr;
        PyEval_RestoreThread(python);
        // Set our tracking now that we have the GIL
        _qore_PyGILState_SetThisThreadState(python);
        _qore_gil_held = true;  // Track that we now hold the GIL
        g_state = PyGILState_UNLOCKED;
    }

    t_state = _qore_PyRuntimeGILState_GetThreadState();

    if (t_state != python) {
        _QORE_PYTHREAD_STATE_SWAP(python);
    }
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
        PyEval_RestoreThread(python);
        g_state = PyGILState_UNLOCKED;
        _qore_gil_held = true;  // Track that we now hold the GIL
    }
    // Update tracking for consistency
    _qore_PyGILState_SetThisThreadState(python);
#endif
#endif

#ifndef Py_GIL_DISABLED
    // Now we have the GIL - verify our state is consistent
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+: PyGILState_Check() and PyGILState_GetThisThreadState() rely on Python's
    // internal TSS (autoTSSkey) which can become stale or cleared when external modules like
    // JNI interact with Python. Our thread-local tracking (_qore_tss_tstate) is authoritative.
    assert(_qore_PyCeval_GetGilLockedStatus());
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

    _QORE_GILSTATE_COUNTER_INC(python);

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
// 3. Restoring our tracking variables (_qore_tss_tstate, _qore_gil_held, etc.)
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

    _QORE_GILSTATE_COUNTER_DEC(python);

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
            _qore_gil_held = true;
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
    {
        AutoLocker al(py_thr_lck);
        checkForkIntern();
        if (!--pgm_thr_cnt && pgm_thr_waiting) {
            pgm_thr_cond.signal();
        }
    }
}

PythonQoreClass* QorePythonProgram::findCreatePythonClass(const QoreClass& cls, const char* mod_name) {
    printd(5, "QorePythonProgram::findCreatePythonClass() %s.%s\n", mod_name, cls.getName());

    py_cls_map_t::iterator i = py_cls_map.lower_bound(&cls);
    if (i != py_cls_map.end() && i->first == &cls) {
        printd(5, "QorePythonProgram::findCreatePythonClass() returning existing %s.%s\n", mod_name, cls.getName());
        return i->second;
    }

    std::unique_ptr<PythonQoreClass> py_cls(new PythonQoreClass(this, mod_name, cls, i));
    [[maybe_unused]] PyTypeObject* t = py_cls->getPythonType();
    printd(5, "QorePythonProgram::findCreatePythonClass() returning new %s.%s type: %p (%s)\n", mod_name,
        cls.getName(), t, t->tp_name);
    //py_cls_map.insert(i, py_cls_map_t::value_type(&cls, py_cls.get()));
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

    DLLLOCAL func_capsule_t(const QoreExternalFunction& func, QorePythonProgram* py_pgm, PyCFunction callback)
            : func(func), py_pgm(py_pgm), name(func.getName()),
              definition{name.c_str(), callback, METH_VARARGS, nullptr} {
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

void QorePythonProgram::raisePythonException(ExceptionSink& xsink) {
    assert(xsink);
    QoreValue err(xsink.getExceptionErr());
    QoreValue desc(xsink.getExceptionDesc());
    QoreValue arg(xsink.getExceptionArg());

    if (err.getType() == NT_STRING) {
        QoreStringValueHelper err_str(err);
        printd(5, "QorePythonProgram::raisePythonException() this: %p owns_interpreter: %d err: %s\n",
            this, owns_interpreter, err_str->c_str());
    } else {
        printd(5, "QorePythonProgram::raisePythonException() this: %p owns_interpreter: %d err: N/A\n",
            this, owns_interpreter);
    }

    // In sub-interpreters (owns_interpreter == true), we cannot use PythonQoreException_Type
    // because it's a custom type initialized in the main interpreter. Using it in a sub-interpreter
    // causes crashes in Python 3.12+ when the exception handling machinery tries to access
    // type-specific data structures that aren't properly set up for the sub-interpreter.
    //
    // IMPORTANT: In Python 3.12+, each interpreter has its own builtin types. Using the global
    // PyExc_RuntimeError (which is the main interpreter's type) in a sub-interpreter can cause
    // crashes when Python's internal machinery tries to process the exception.
    // We must get the RuntimeError type from the current interpreter's builtins.
    if (owns_interpreter) {
        // Convert values to Python objects for the exception args
        // This matches the PythonQoreException behavior where args[0] is err, args[1] is desc, etc.
        //
        // NOTE: "err", "desc", and "arg" are owned by the exception in "xsink", so the exception must not be
        // cleared until all of them have been converted; the error string is used in the fallback path below
        // after the exception has been cleared, so it is copied here
        ExceptionSink xsink2;
        QorePythonReferenceHolder py_err(getPythonValue(err, &xsink2));
        QorePythonReferenceHolder py_desc(getPythonValue(desc, &xsink2));

        std::string err_msg;
        {
            QoreStringValueHelper err_str(err);
            err_msg = err_str->c_str();
        }

        // Create args tuple: (err, desc) or (err, desc, arg)
        QorePythonReferenceHolder tuple(PyTuple_New(arg ? 3 : 2));
        if (py_err) {
            PyTuple_SET_ITEM(*tuple, 0, py_err.release());
        } else {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(*tuple, 0, Py_None);
        }
        if (py_desc) {
            PyTuple_SET_ITEM(*tuple, 1, py_desc.release());
        } else {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(*tuple, 1, Py_None);
        }
        if (arg) {
            QorePythonReferenceHolder py_arg(getPythonValue(arg, &xsink2));
            if (py_arg) {
                PyTuple_SET_ITEM(*tuple, 2, py_arg.release());
            } else {
                Py_INCREF(Py_None);
                PyTuple_SET_ITEM(*tuple, 2, Py_None);
            }
        }

        // all values owned by the exception have been converted; "err", "desc", and "arg" are invalid after this
        xsink.clear();

        // Get RuntimeError from the current interpreter's builtins
        PyObject* builtins = PyEval_GetBuiltins();
        if (builtins) {
            PyObject* runtime_error = PyDict_GetItemString(builtins, "RuntimeError");
            if (runtime_error && PyType_Check(runtime_error)) {
                // Create the exception with args tuple
                QorePythonReferenceHolder exc(PyObject_Call(runtime_error, *tuple, nullptr));
                if (exc) {
                    PyErr_SetObject(runtime_error, *exc);
                    return;
                }
            }
        }

        // Fallback - use error string as message
        PyErr_SetString(PyExc_RuntimeError, err_msg.c_str());
        return;
    }

    ExceptionSink xsink2;
    QorePythonReferenceHolder tuple(PyTuple_New(arg ? 3 : 2));
    QorePythonReferenceHolder ex_arg(getPythonValue(err, &xsink2));
    if (ex_arg) {
        PyTuple_SET_ITEM(*tuple, 0, ex_arg.release());
    } else {
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(*tuple, 0, Py_None);
    }
    ex_arg = getPythonValue(desc, &xsink2);
    if (ex_arg) {
        PyTuple_SET_ITEM(*tuple, 1, ex_arg.release());
    } else {
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(*tuple, 1, Py_None);
    }
    if (arg) {
        ex_arg = getPythonValue(arg, &xsink2);
        if (ex_arg) {
            PyTuple_SET_ITEM(*tuple, 2, ex_arg.release());
        } else {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(*tuple, 2, Py_None);
        }
    }
    xsink.clear();

    ex_arg = PyObject_CallObject((PyObject*)&PythonQoreException_Type, *tuple);
    //printd(5,  "QorePythonProgram::raisePythonException() py_ex: %p %s\n", *ex_arg, Py_TYPE(*ex_arg)->tp_name);
    PyErr_SetObject((PyObject*)&PythonQoreException_Type, *ex_arg);
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

int QorePythonProgram::importQoreClassToPython(PyObject* mod, const QoreClass& cls, const char* mod_name) {
    PyTypeObject* py_cls = findCreatePythonClass(cls, mod_name)->getPythonType();
    //printd(5, "QorePythonProgram::importQoreClassToPython() py_cls: %p ('%s') cn: '%s' mod_name: '%s'\n", py_cls,
    //  py_cls->tp_name, cls.getName(), mod_name);

    PyObject* clsobj = (PyObject*)py_cls;
    if (PyObject_SetAttrString(mod, cls.getName(), clsobj)) {
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
        std::string nspath = ns_pkg->getPath();
        QorePythonReferenceHolder path(PyUnicode_FromStringAndSize(nspath.c_str(), nspath.size()));
        PyObject_SetAttrString(*new_mod, "__path__", *path);
    }
    saveModule(name, *new_mod);
    return new_mod.release();
}

PyObject* QorePythonProgram::newModule(const char* name, const char* path) {
    QorePythonReferenceHolder new_mod(PyModule_New(name));
    QorePythonReferenceHolder py_path(PyUnicode_FromStringAndSize(path, strlen(path)));
    PyObject_SetAttrString(*new_mod, "__path__", *py_path);
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
            QoreStringMaker path("alias:%s", source_path.c_str());
            new_elem = newModule(nspath.c_str(), path.c_str());
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
    DLLLOCAL QorePythonConversionPathHelper(std::set<PyObject*>& path, PyObject* val)
            : path(path), val(val), cycle(!path.insert(val).second) {
    }

    DLLLOCAL ~QorePythonConversionPathHelper() {
        if (!cycle) {
            path.erase(val);
        }
    }

    //! returns true and raises a Qore exception if the container is part of a cycle
    DLLLOCAL bool checkCycle(ExceptionSink* xsink) const {
        if (cycle && xsink) {
            xsink->raiseException("PYTHON-VALUE-ERROR", "cannot convert a Python value of type '%s' that contains "
                "itself to a Qore value", Py_TYPE(val)->tp_name);
        }
        return cycle;
    }

private:
    std::set<PyObject*>& path;
    PyObject* val;
    bool cycle;
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

QoreListNode* QorePythonProgram::getQoreListFromList(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset) {
    assert(PyList_Check(val));
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(xsink)) {
        return nullptr;
    }
    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
    Py_ssize_t len = PyList_Size(val);
    for (Py_ssize_t i = 0; i < len; ++i) {
        ValueHolder qval(getQoreValue(xsink, PyList_GetItem(val, i), rset), xsink);
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
    return getQoreListFromTuple(xsink, val, rset, offset, for_args);
}

QoreListNode* QorePythonProgram::getQoreListFromTuple(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset,
    size_t offset, bool for_args) {
    assert(PyTuple_Check(val));
    Py_ssize_t len = PyTuple_Size(val);
    if (for_args && !len) {
        return nullptr;
    }
    QorePythonConversionPathHelper path(rset, val);
    if (path.checkCycle(xsink)) {
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
    if (path.checkCycle(xsink)) {
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
    if (path.checkCycle(xsink)) {
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
    if (path.checkCycle(xsink)) {
        return nullptr;
    }

    ReferenceHolder<QoreHashNode> rv(new QoreHashNode(autoTypeInfo), xsink);

    PyObject* key, * value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(val, &pos, &key, &value)) {
        const char* keystr;
        QorePythonReferenceHolder tkey;
        if (Py_TYPE(key) == &PyUnicode_Type) {
            keystr = PyUnicode_AsUTF8(key);
        } else {
            tkey = PyObject_Repr(key);
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
                    Py_TYPE(key)->tp_name);
            }
            return nullptr;
        }

        ValueHolder qval(getQoreValue(xsink, value, rset), xsink);
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
    rv->append(PyByteArray_AsString(val), PyByteArray_Size(val));
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
        // Get UTC offset using PyObject_CallMethod which is safer than attribute access
        QorePythonReferenceHolder delta(PyObject_CallMethod(tzinfo, "utcoffset", "O", val));
        if (!delta) {
            // utcoffset() raised an exception - propagate to caller if xsink provided
            if (xsink) {
                // Fetch and format the Python exception
                QorePythonReferenceHolder ex_type, ex_value, traceback;
                PyErr_Fetch(ex_type.getRef(), ex_value.getRef(), traceback.getRef());
                QoreStringNode* desc = new QoreStringNode("tzinfo.utcoffset() failed");
                if (ex_value) {
                    QorePythonReferenceHolder str(PyObject_Str(*ex_value));
                    if (str) {
                        desc->concat(": ");
                        desc->concat(PyUnicode_AsUTF8(*str));
                    }
                }
                xsink->raiseException("PYTHON-DATETIME-ERROR", desc);
                return nullptr;
            }
            // No xsink provided - clear exception and proceed without timezone
            PyErr_Clear();
        } else if (*delta != Py_None && PyDelta_Check(*delta) && getQoreOffsetZone(xsink, *delta, zone)) {
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
    assert(Py_TYPE(val) == PyDateTimeAPI->TimeType);
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
        return pyobj->qobj ? pyobj->qobj->refSelf() : QoreValue();
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
        return getQoreHashFromDict(xsink, val, rset);
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

        case NT_LIST:
            return getPythonList(xsink, val.get<const QoreListNode>());

        case NT_HASH:
            return getPythonDict(xsink, val.get<const QoreHashNode>());

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
            PythonQoreClass* py_cls = findCreatePythonClass(*o->getClass(), "qore");
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
        size_t arg_offset) {
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
        // returns a borrowed reference
        PyObject* py_func = PyDict_GetItemString(module_dict, fname->c_str());
        if (!py_func || !PyFunction_Check(py_func)) {
            xsink->raiseException("NO-FUNCTION", "cannot find function '%s'", fname->c_str());
            return QoreValue();
        }

        //printd(5, "QorePythonProgram::callFunction() this: %p %s()\n", this, func_name.c_str());
        rv = callInternal(xsink, py_func, args, arg_offset);
    }
    assert(!haveGil());
    return rv.release();
}

QoreValue QorePythonProgram::callMethod(ExceptionSink* xsink, const QoreString& class_name,
    const QoreString& method_name, const QoreListNode* args, size_t arg_offset) {
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

    return callMethod(xsink, cname->c_str(), mname->c_str(), args, arg_offset);
}

QoreValue QorePythonProgram::callMethod(ExceptionSink* xsink, const char* cname, const char* mname,
    const QoreListNode* args, size_t arg_offset, PyObject* first) {
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

    // returns a borrowed reference
    QorePythonReferenceHolder py_method;
    if (PyObject_HasAttrString(py_class, mname)) {
        py_method = PyObject_GetAttrString(py_class, mname);
    }
    if (!py_method || (!PyFunction_Check(*py_method) && (Py_TYPE(*py_method) != &PyMethodDescr_Type))) {
        xsink->raiseException("NO-METHOD", "cannot find method '%s.%s()'", cname, mname);
        return QoreValue();
    }

    //printd(5, "QorePythonProgram::callMethod() %s::%s() args: %d\n", cname, mname, args ? (int)args->size() : 0);

    return callInternal(xsink, *py_method, args, arg_offset);
}

QoreValue QorePythonProgram::callInternal(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
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
    //printd(5, "QorePythonProgram::callInternal() f: %p args: %d (%d) self: %p\n", callable,
    //    args ? (int)args->size() : 0, (int)arg_offset, first);
    QorePythonReferenceHolder rv(callPythonInternal(xsink, callable, args, arg_offset, first));
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

    QorePythonReferenceHolder ex_type, ex_value, traceback;
    PyErr_Fetch(ex_type.getRef(), ex_value.getRef(), traceback.getRef());
    assert(ex_type);

    // get location
    QoreExternalProgramLocationWrapper loc;
    QoreCallStack callstack;

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

    bool use_loc;
    if (PyTraceBack_Check(*traceback)) {
        PyTracebackObject* tb = reinterpret_cast<PyTracebackObject*>(*traceback);

        int depth = 0;
        PyTracebackObject* tb1 = tb;
        while (tb1) {
            ++depth;
            tb1 = tb1->tb_next;
        }
        while (tb->tb_next && depth > 0) {
            --depth;
            tb = tb->tb_next;
        }

        PyFrameObject* frame = tb->tb_frame;
        const char* funcname = nullptr;
        QorePythonReferenceHolder owned_frame;
        QoreString function_name;
        while (frame) {
            // get line number
            QorePythonReferenceHolder code((PyObject*)PyFrame_GetCode(frame));
            int line = PyCode_Addr2Line((PyCodeObject*)*code, PyFrame_GetLasti(frame));
            QorePythonReferenceHolder filename_obj(PyObject_GetAttrString(*code, "co_filename"));
            const char* filename;
            Py_INCREF(*filename_obj);
            QorePythonReferenceHolder np_obj(QorePythonStackLocationHelper::normalizePath(*filename_obj));
            if (np_obj) {
                filename = getCString(*np_obj);
            } else {
                filename = PyUnicode_AsUTF8(*filename_obj);
            }

            if (frame == tb->tb_frame) {
                loc.set(filename, line, line, nullptr, 0, QORE_PYTHON_LANG_NAME);
            } else {
                callstack.add(CT_USER, filename, line, line, funcname, QORE_PYTHON_LANG_NAME);
            }
            QorePythonReferenceHolder funcname_obj(PyObject_GetAttrString(*code, "co_qualname"));
            function_name = PyUnicode_AsUTF8(*funcname_obj);
            funcname = function_name.c_str();

            // PyFrame_GetBack returns a new reference; keep it owned while walking
            // the stack so the frame and its entire module graph can be collected.
            PyFrameObject* next = PyFrame_GetBack(frame);
            owned_frame = reinterpret_cast<PyObject*>(next);
            frame = next;
        }
        if (funcname) {
            callstack.add(CT_BUILTIN, this_file, __LINE__, __LINE__, funcname, "c++");
        }
        use_loc = true;
    } else {
        use_loc = false;
    }

    // check if it's a QoreException (either PythonQoreException or RuntimeError with our tuple format)
    bool is_qore_exception = false;
    if (*ex_type == (PyObject*)&PythonQoreException_Type) {
        is_qore_exception = true;
    } else if (PyExceptionInstance_Check(*ex_value) && PyErr_GivenExceptionMatches(*ex_type, PyExc_RuntimeError)) {
        // Check if it's a RuntimeError with our tuple format (err, desc[, arg])
        // This is used for exceptions from sub-interpreters.
        // NOTE: Only check RuntimeError specifically, not all exceptions, because other exceptions
        // like SyntaxError also have 2 args but in a different format.
        //
        // Qore exception format: (err, desc[, arg]) where:
        //   - err: can be int or string (error code like 1 or "ERROR-NAME")
        //   - desc: can be None or string (description)
        //   - arg: optional, any type
        //
        // SyntaxError format: ('invalid syntax', ('<string>', 1, 14, ...))
        //   - Second arg is a tuple (location info), not None or string
        //
        // So we validate that the second element is NOT a tuple to distinguish from SyntaxError.
        QorePythonReferenceHolder args(PyObject_GetAttrString(*ex_value, "args"));
        if (args && PyTuple_Check(*args)) {
            Py_ssize_t size = PyTuple_Size(*args);
            if (size >= 2 && size <= 3) {
                // Validate: second element should be None or string, not a tuple
                PyObject* py_desc = PyTuple_GetItem(*args, 1);  // borrowed ref
                if (!PyTuple_Check(py_desc)) {
                    // Looks like our tuple format - treat as a Qore exception
                    is_qore_exception = true;
                }
            }
        }
    }

    if (is_qore_exception) {
        QorePythonReferenceHolder pyval;
        ValueHolder err(xsink);
        ValueHolder desc(xsink);
        ValueHolder arg(xsink);

        if (*ex_type == (PyObject*)&PythonQoreException_Type) {
            // PythonQoreException has err, desc, arg attributes
            assert(PyObject_HasAttrString(*ex_value, "err"));
            pyval = PyObject_GetAttrString(*ex_value, "err");
            err = getQoreValue(xsink, *pyval);
            if (!*xsink && PyObject_HasAttrString(*ex_value, "desc")) {
                pyval = PyObject_GetAttrString(*ex_value, "desc");
                desc = getQoreValue(xsink, *pyval);
            }
            if (!*xsink && PyObject_HasAttrString(*ex_value, "arg")) {
                pyval = PyObject_GetAttrString(*ex_value, "arg");
                arg = getQoreValue(xsink, *pyval);
            }
        } else {
            // RuntimeError with tuple args: (err, desc[, arg])
            QorePythonReferenceHolder args(PyObject_GetAttrString(*ex_value, "args"));
            if (args && PyTuple_Check(*args)) {
                Py_ssize_t size = PyTuple_Size(*args);
                if (size >= 1) {
                    PyObject* py_err = PyTuple_GetItem(*args, 0);  // borrowed ref
                    err = getQoreValue(xsink, py_err);
                }
                if (!*xsink && size >= 2) {
                    PyObject* py_desc = PyTuple_GetItem(*args, 1);  // borrowed ref
                    desc = getQoreValue(xsink, py_desc);
                }
                if (!*xsink && size >= 3) {
                    PyObject* py_arg = PyTuple_GetItem(*args, 2);  // borrowed ref
                    arg = getQoreValue(xsink, py_arg);
                }
            }
        }

        if (!*xsink) {
            QoreStringValueHelper errstr(*err);
            QoreStringNodeValueHelper descstr(*desc);
            if (use_loc) {
                xsink->raiseExceptionArg(loc.get(), errstr->c_str(), arg->refSelf(),
                    descstr.getReferencedValue(), callstack);
            } else {
                xsink->raiseExceptionArg(errstr->c_str(), arg->refSelf(), descstr.getReferencedValue(),
                    callstack);
            }
            return -1;
        }
    }

    if (!*xsink) {
        // get full exception class name
        // NOTE: Use ex_type directly instead of Py_TYPE(*ex_value) because after PyErr_NormalizeException,
        // ex_value might still be the exception class itself (not an instance) in some cases like SyntaxError.
        // In such cases, Py_TYPE(*ex_value) returns 'type' (the metaclass), not the actual exception class.
        PyTypeObject* py_cls = PyExceptionInstance_Check(*ex_value)
            ? Py_TYPE(*ex_value)
            : reinterpret_cast<PyTypeObject*>(*ex_type);
        QoreString ex_name(py_cls->tp_name);
        if (PyObject_HasAttrString(reinterpret_cast<PyObject*>(py_cls), "__module__")) {
            QorePythonReferenceHolder ex_mod(PyObject_GetAttrString(reinterpret_cast<PyObject*>(py_cls),
                "__module__"));
            if (PyUnicode_Check(*ex_mod)) {
                ex_name.prepend(".");
                ex_name.prepend(PyUnicode_AsUTF8(*ex_mod));
            }
        }

        // get description
        QorePythonReferenceHolder desc(PyObject_Str(*ex_value));
        ValueHolder qore_desc(getQoreValue(xsink, *desc), xsink);
        if (!*xsink) {
            QoreStringNodeValueHelper descstr(*qore_desc);
            if (use_loc) {
                // check if we have a Qore exception
                xsink->raiseExceptionArg(loc.get(), ex_name.c_str(), QoreValue(),
                    descstr.getReferencedValue(), callstack);
            } else {
                // check if we have a Qore exception
                xsink->raiseExceptionArg(ex_name.c_str(), QoreValue(), descstr.getReferencedValue(), callstack);
            }
            return -1;
        }
    }

    xsink->appendLastDescription(" (while trying to convert Python exception arguments to Qore)");

    return -1;
}

QoreValue QorePythonProgram::callPythonMethod(ExceptionSink* xsink, PyObject* attr, PyObject* obj,
    const QoreListNode* args, size_t arg_offset) {
    PyTypeObject* mtype = Py_TYPE(attr);
    // check for static method
    if (mtype == &PyStaticMethod_Type) {
        // get callable from static method
        QorePythonReferenceHolder py_method(PyStaticMethod_Type.tp_descr_get(attr, nullptr, nullptr));
        assert(py_method);
        return callInternal(xsink, *py_method, args, arg_offset);
    }
    // check for wrapper descriptors -> normal method
    if (mtype == &PyWrapperDescr_Type) {
        return callWrapperDescriptorMethod(xsink, obj, attr, args, arg_offset);
    }
    if (mtype == &PyMethodDescr_Type) {
        return callMethodDescriptorMethod(xsink, obj, attr, args, arg_offset);
    }
    if (mtype == &PyClassMethodDescr_Type) {
        return callClassMethodDescriptorMethod(xsink, obj, attr, args, arg_offset);
    }
    if (PyFunction_Check(attr)) {
        return callFunctionObject(xsink, attr, args, arg_offset, obj);
    }
    if (PyCFunction_Check(attr)) {
        return callCFunctionMethod(xsink, attr, args, arg_offset);
    }

    xsink->raiseException("PYTHON-ERROR", "cannot make a call with Python type '%s'", mtype->tp_name);
    return QoreValue();
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
    return getCreateQorePythonClassIntern(xsink, type, nsset, nullptr, flags);
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

QoreClass* QorePythonProgram::getCreateQorePythonClassIntern(ExceptionSink* xsink, PyTypeObject* type,
        strset_t& nsset, const char* cname, int flags) {
    //printd(5, "QorePythonProgram::getCreateQorePythonClassIntern() class: '%s'\n", type->tp_name);
    // see if the Python type already represents a Qore class
    if (PyQoreObjectType_Check(type)) {
        //printd(5, "QorePythonProgram::getCreateQorePythonClassIntern() class: '%s' is Qore\n", type->tp_name);
        return const_cast<QoreClass*>(PythonQoreClass::getQoreClass(type));
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

    // create new QorePythonClass
    QoreNamespace* ns = getNamespaceForObject(reinterpret_cast<PyObject*>(type));
    if (!rpath_str.empty() && rpath_str.startsWith(ns->getName())) {
        size_t len = strlen(ns->getName());
        if (rpath_str[len] == '.' && rpath_str.size() > len) {
            rpath_str.replace(0, len + 1, nullptr);
            rpath_str.replaceAll(".", "::");
            ns = ns->findCreateNamespacePathAll(rpath_str.c_str());
        }
    }
    return addClassToNamespaceIntern(xsink, ns, type, cname, i, nsset, flags);
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

    return setupQorePythonClass(xsink, ns, type, cls, nsset, flags);
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

    // a Python base class shares the Python object and is a virtual base class; a Qore base class (the base is the
    // Python type of a Qore class) is not: Qore runs the constructor, destructor and copy method of a base class only
    // if it is not virtual, and the destructor of a Qore class must run for the objects of its Python subclasses (its
    // constructor is run by PythonQoreClass::py_init()).  As Qore makes the ancestors of a virtual base class virtual,
    // a Qore class reached through Python base classes is added first as a non-virtual base class, and the methods of
    // the Python base classes are added to this class, so that they take precedence over the Qore class's methods
    if (qore_base && !python_bases.empty()) {
        QoreClass* qore_bclass = getCreateQorePythonClassIntern(xsink, qore_base, nsset);
        if (!qore_bclass) {
            assert(*xsink);
            return nullptr;
        }
        cls->addBaseClass(qore_bclass, false);
    }

    // add single base class
    if (type->tp_base) {
        QoreClass* bclass = getCreateQorePythonClassIntern(xsink, type->tp_base, nsset);
        if (!bclass) {
            assert(*xsink);
            return nullptr;
        }

        printd(5, "QorePythonProgram::setupQorePythonClass() %s (id: %d) parent: %s (bclass: %p id: %d)\n",
            type->tp_name, cls->getID(), type->tp_base->tp_name, bclass, bclass->getID());
        cls->addBaseClass(bclass, !PyQoreObjectType_Check(type->tp_base));
    }

    cls->addBuiltinVirtualBaseClass(QC_PYTHONBASEOBJECT);

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

    return cls.release();
}

void QorePythonProgram::addPythonTypeAttributes(PyTypeObject* type, QorePythonClass* cls, strset_t& names) {
    if (type->tp_dict) {
        PyObject* key, * value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(type->tp_dict, &pos, &key, &value)) {
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
        if (all && PyTuple_Check(all)) {
            Py_ssize_t len = PyTuple_Size(all);
            for (Py_ssize_t i = 0; i < len; ++i) {
                // returns a borrowed reference
                PyObject* sv = PyTuple_GetItem(all, i);
                if (!sv || !PyUnicode_Check(sv)) {
                    throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' __all__ has an invalid " \
                        "element with type '%s'; expecting 'str'", module, sv ? Py_TYPE(sv)->tp_name : "null");
                }
                if (checkImportSymbol(xsink, module, mod, is_package, PyUnicode_AsUTF8(sv), filter, true)) {
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
            // returns a borrowed reference
            PyObject* sv = PyList_GetItem(*dir, i);
            if (!sv || !PyUnicode_Check(sv)) {
                throw QoreStandardException("PYTHON-IMPORT-ERROR", "module '%s' __all__ has an invalid " \
                    "element with type '%s'; expecting 'str'", module, sv ? Py_TYPE(sv)->tp_name : "null");
            }

            if (checkImportSymbol(xsink, module, mod, is_package, PyUnicode_AsUTF8(sv), filter, true)) {
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
        // do not need to save reference here
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
        getCreateQorePythonClassIntern(xsink, reinterpret_cast<PyTypeObject*>(value), nsset);
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
    if (is_qore_fork_function(fc->func)) {
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
