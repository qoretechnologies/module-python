/* indent-tabs-mode: nil -*- */
/*
    python Qore module

    Copyright (C) 2020 - 2026 Qore Technologies, s.r.o.

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

#include "python-module.h"

#include <pthread.h>
#include <unistd.h>
#ifndef _Q_WINDOWS
#include <dlfcn.h>
#endif
#include <cstring>
#include "QC_PythonProgram.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"
#include "python_fork_gil.h"

#include <qore/QoreForkHandler.h>

#include <string>
#include <vector>

static void python_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink);
static void python_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink);
static void python_module_delete();
static void python_module_parse_cmd(const QoreString& cmd, ExceptionSink* xsink);

static QoreStringNode* python_module_init_intern(bool repeat);

// module declaration for Qore 0.9.5+
void python_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = QORE_PYTHON_MODULE_NAME;
    mod_info.version = PACKAGE_VERSION;
    mod_info.desc = "python module";
    mod_info.author = "David Nichols";
    mod_info.url = "http://qore.org";
    mod_info.api_major = QORE_MODULE_API_MAJOR;
    mod_info.api_minor = QORE_MODULE_API_MINOR;
    mod_info.init = python_module_init;
    mod_info.ns_init = python_module_ns_init;
    mod_info.del = python_module_delete;
    mod_info.parse_cmd = python_module_parse_cmd;
    mod_info.license = QL_LGPL;
    mod_info.license_str = "LGPL-2.1-or-later";

    mod_info.info = new QoreHashNode(autoTypeInfo);
    mod_info.info->setKeyValue("python_version", new QoreStringNodeMaker(PY_VERSION), nullptr);
    mod_info.info->setKeyValue("python_major", PY_MAJOR_VERSION, nullptr);
    mod_info.info->setKeyValue("python_minor", PY_MINOR_VERSION, nullptr);
    mod_info.info->setKeyValue("python_micro", PY_MICRO_VERSION, nullptr);
}

QoreNamespace* PNS = nullptr;
PyThreadState* mainThreadState = nullptr;

QorePythonClass* QC_PYTHONBASEOBJECT;
qore_classid_t CID_PYTHONBASEOBJECT;

// module cmd type
using qore_python_module_cmd_t = void (*) (ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_import(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_import_ns(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_alias(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_parse(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_export_class(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_export_func(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
static void py_mc_add_module_path(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);
//static void py_mc_reset_python(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm);

struct qore_python_cmd_info_t {
    qore_python_module_cmd_t cmd;
    bool requires_arg = true;

    DLLLOCAL qore_python_cmd_info_t(qore_python_module_cmd_t cmd, bool requires_arg)
        : cmd(cmd), requires_arg(requires_arg) {
    }
};

// module cmds
typedef std::map<std::string, qore_python_cmd_info_t> mcmap_t;
static mcmap_t mcmap = {
    {"import", qore_python_cmd_info_t(py_mc_import, true)},
    {"import-ns", qore_python_cmd_info_t(py_mc_import_ns, true)},
    {"alias", qore_python_cmd_info_t(py_mc_alias, true)},
    {"parse", qore_python_cmd_info_t(py_mc_parse, true)},
    {"export-class", qore_python_cmd_info_t(py_mc_export_class, true)},
    {"export-func", qore_python_cmd_info_t(py_mc_export_func, true)},
    {"add-module-path", qore_python_cmd_info_t(py_mc_add_module_path, true)},
#if 0
    {"reset-python", qore_python_cmd_info_t(py_mc_reset_python, false)},
#endif
};

static bool python_needs_shutdown = false;
static bool python_initialized = false;
bool python_shutdown = false;
bool python_fork_lost = false;

thread_local PyThreadState* python_fork_protocol_tstate = nullptr;

namespace {
// set on the thread calling fork() while Python's fork protocol runs for it: from PyOS_BeforeFork(), called by
// os.fork() or by the prepare handler of the module for Qore's fork() called from Python code, until the end of
// PyOS_AfterFork_Parent() or PyOS_AfterFork_Child(), which recreates the GIL in the child
thread_local bool python_fork_protocol_running = false;
}

#ifdef QORE_PYTHON_HAVE_FORK_GIL_CHECK
// the fork handler that finds whether Python can be used in the child (see QoreForkHandler.h)
static QoreForkHandlerRegistration* python_gil_fork_reg = nullptr;

// runs in the child of fork() before anything else runs in it: if a thread that does not exist in the child held the
// GIL, it can never be acquired, and the first use of Python would wait for it forever; Python is then not used in
// the child at all, as after it has been shut down, and a call raises PYTHON-FORK-ERROR
static void python_gil_fork_child(void*) {
    // the thread calling fork() holds the GIL in Python's fork protocol, and Python recreates the GIL in the child:
    // another thread holding one of its mutexes, as a thread waiting for the GIL does from time to time, does not
    // stop the child from using Python
    if (python_fork_protocol_running) {
        return;
    }
    if (!python_shutdown && Py_IsInitialized() && qore_python_fork_child_gil_lost()) {
        python_fork_lost = true;
        python_shutdown = true;
    }
}
#endif

void qore_python_raise_fork_error(ExceptionSink* xsink) {
    xsink->raiseException("PYTHON-FORK-ERROR", "Python cannot be used in this process: it was created with fork() "
        "while another thread held the Python GIL, which no thread can release in this process");
}

void qore_python_fork_lost_exit() {
    static const char msg[] = "python module: a child process created with fork() cannot return to Python code: "
        "another thread held the Python GIL when the process was created, and no thread can release it in this "
        "process\n";
    // only async-signal-safe calls: the process can hold any lock
    ssize_t rc = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)rc;
    _exit(1);
}

#ifndef _Q_WINDOWS
namespace {
// takes the GIL again with the thread state that the thread released to call Qore's fork() (see
// QorePythonReleaseGilHelper), as the helper does when the call returns
void python_fork_protocol_acquire(PyThreadState* tstate) {
    PyEval_RestoreThread(tstate);
    _QORE_SET_GIL_HELD(true);
    _qore_PyGILState_SetThisThreadState(tstate);
}

// releases the GIL again, as it was released for the call to Qore's fork()
void python_fork_protocol_release() {
    PyEval_SaveThread();
    _qore_PyGILState_SetThisThreadState(nullptr);
    _QORE_SET_GIL_HELD(false);
}

// returns true if the main interpreter is the only interpreter; the GIL must be held
bool python_only_main_interpreter() {
    PyInterpreterState* interp = PyInterpreterState_Head();
    return interp && interp == PyInterpreterState_Main() && !PyInterpreterState_Next(interp);
}

// Python's fork protocol for Qore's fork() called from Python code of the main interpreter, as os.fork() runs it:
// Python runs its at-fork functions (os.register_at_fork()), and makes its state consistent in the child, where only
// the thread calling fork() exists (the threading module forgets the other threads)
//
// It is only run when the main interpreter is the only interpreter: in the child, Python deletes the other
// interpreters, which crashes the child (CPython issue gh-139759, also with os.fork()); with other interpreters,
// Qore's fork() is made without the protocol, and they can be used in the child.  The interpreters of the module
// share the GIL of the main interpreter, so no interpreter is created while it is held here (without a GIL, in a
// free-threading build, an interpreter created by another thread at the same moment is not seen, as by os.fork()).
//
// pthread_atfork() runs the prepare handlers in the reverse order of their registration, and the parent and child
// handlers in the order of their registration: these handlers, registered after those of the Qore library, take the
// GIL before the library's handlers take its process-wide locks, as os.fork() does, and Python runs its code in the
// parent and in the child after the library's locks have been released or initialized again
void python_fork_protocol_prepare() {
    PyThreadState* tstate = python_fork_protocol_tstate;
    if (!tstate || python_fork_protocol_running || python_shutdown || !Py_IsInitialized()) {
        return;
    }
    python_fork_protocol_acquire(tstate);
    if (!python_only_main_interpreter()) {
        python_fork_protocol_release();
        return;
    }
    python_fork_protocol_running = true;
    PyOS_BeforeFork();
}

void python_fork_protocol_parent() {
    if (!python_fork_protocol_tstate || !python_fork_protocol_running) {
        return;
    }
    PyOS_AfterFork_Parent();
    python_fork_protocol_running = false;
    python_fork_protocol_release();
}

void python_fork_protocol_child() {
    if (!python_fork_protocol_tstate || !python_fork_protocol_running) {
        return;
    }
    PyOS_AfterFork_Child();
    python_fork_protocol_running = false;
    python_fork_protocol_release();
}
}
#endif

int qore_python_register_fork_protocol(QoreString& err) {
#ifndef _Q_WINDOWS
    // the handlers cannot be removed, so the module must not be unloaded while the process runs: it is marked so that
    // the dynamic loader keeps it
    Dl_info info;
    if (!dladdr(reinterpret_cast<void*>(&python_fork_protocol_prepare), &info) || !info.dli_fname) {
        err.sprintf("cannot find the file of the module: %s", dlerror());
        return -1;
    }
    void* handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
    if (!handle) {
        err.sprintf("cannot keep the module '%s' loaded: %s", info.dli_fname, dlerror());
        return -1;
    }
    int rc = pthread_atfork(python_fork_protocol_prepare, python_fork_protocol_parent, python_fork_protocol_child);
    if (rc) {
        err.sprintf("cannot register the fork handlers of the module: %s", strerror(rc));
        return -1;
    }
#endif
    return 0;
}

// the at-fork functions of the main interpreter (os.register_at_fork()): Python calls them when it makes a fork()
// with its fork protocol, in os.fork() and for Qore's fork() called from Python code; they mark the fork for the
// child handler of the GIL (python_gil_fork_child()), since Python recreates the GIL in the child
static PyObject* qore_python_at_fork_before(PyObject*, PyObject*) {
    python_fork_protocol_running = true;
    Py_RETURN_NONE;
}

static PyObject* qore_python_at_fork_after_in_parent(PyObject*, PyObject*) {
    python_fork_protocol_running = false;
    Py_RETURN_NONE;
}

static PyObject* qore_python_at_fork_after_in_child(PyObject*, PyObject*) {
    python_fork_protocol_running = false;
    Py_RETURN_NONE;
}

static PyMethodDef qore_python_at_fork_md[] = {
    {"_qore_at_fork_before", qore_python_at_fork_before, METH_NOARGS, nullptr},
    {"_qore_at_fork_after_in_parent", qore_python_at_fork_after_in_parent, METH_NOARGS, nullptr},
    {"_qore_at_fork_after_in_child", qore_python_at_fork_after_in_child, METH_NOARGS, nullptr},
};

int qore_python_register_at_fork_functions() {
    // registered once in the main interpreter, where os.fork() is allowed
    static bool registered = false;
    if (registered) {
        return 0;
    }
    QorePythonReferenceHolder os(PyImport_ImportModule("os"));
    if (!os) {
        return -1;
    }
    QorePythonReferenceHolder register_at_fork(PyObject_GetAttrString(*os, "register_at_fork"));
    if (!register_at_fork) {
        return -1;
    }
    QorePythonReferenceHolder kwargs(PyDict_New());
    if (!kwargs) {
        return -1;
    }
    static const char* keys[] = {"before", "after_in_parent", "after_in_child"};
    for (unsigned i = 0; i < 3; ++i) {
        QorePythonReferenceHolder func(PyCFunction_New(&qore_python_at_fork_md[i], nullptr));
        if (!func || PyDict_SetItemString(*kwargs, keys[i], *func)) {
            return -1;
        }
    }
    QorePythonReferenceHolder args(PyTuple_New(0));
    if (!args) {
        return -1;
    }
    QorePythonReferenceHolder rv(PyObject_Call(*register_at_fork, *args, *kwargs));
    if (!rv) {
        return -1;
    }
    registered = true;
    return 0;
}

int python_u_tld_key = -1;
int python_qobj_key = -1;

static sig_vec_t sig_vec = {
#ifndef _Q_WINDOWS
    SIGSEGV, SIGBUS
#endif
};

static void check_python_version() {
    QorePythonReferenceHolder mod(PyImport_ImportModule("sys"));
    if (!mod) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "Python could not load module 'sys'");
    }

    // returns a borrowed reference
    PyObject* mod_dict = PyModule_GetDict(*mod);
    if (!mod_dict) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "Python module 'sys' has no dictionary");
    }

    // returns a borrowed reference
    PyObject* value = PyDict_GetItemString(mod_dict, "version_info");
    if (!value) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "symbol 'sys.version_info' not found; cannot verify the " \
            "runtime version of the Python library");
    }

    if (!PyObject_HasAttrString(value, "major")) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "symbol 'sys.version.major' was not found; cannot " \
            "verify the runtime version of the Python library");
    }

    QorePythonReferenceHolder py_major(PyObject_GetAttrString(value, "major"));
    if (!PyLong_Check(*py_major)) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "symbol 'sys.version.major' has type '%s'; expecting " \
            "'int'; cannot verify the runtime version of the Python library", Py_TYPE(*py_major)->tp_name);
    }

    long major = PyLong_AsLong(*py_major);
    if (major != PY_MAJOR_VERSION) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "Python runtime major version is %ld, but the module was " \
            "compiled with major version %d (%s)", major, PY_MAJOR_VERSION, PY_VERSION);
    }

    QorePythonReferenceHolder py_minor(PyObject_GetAttrString(value, "minor"));
    if (!PyLong_Check(*py_minor)) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "symbol 'sys.version.minor' has type '%s'; expecting " \
            "'int'; cannot verify the runtime version of the Python library", Py_TYPE(*py_minor)->tp_name);
    }

    long minor = PyLong_AsLong(*py_minor);
    if (minor != PY_MINOR_VERSION) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "Python runtime version is %ld.%ld, but the module was " \
            "compiled with version %d.%d (%s)", major, minor, PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_VERSION);
    }

    //printd(5, "python runtime version OK: %ld.%ld.x =~ '%s'\n", major, minor, PY_VERSION);
}

static void python_module_shutdown() {
    // in a child process that cannot use Python (see python_gil_fork_child()), it is not finalized: acquiring the
    // GIL would wait forever
    if (python_fork_lost) {
        python_needs_shutdown = false;
        return;
    }
    if (python_initialized) {
        _QORE_PYTHREAD_STATE_SWAP(nullptr);
        _qore_acquire_thread_state(mainThreadState);
        _qore_PyGILState_SetThisThreadState(mainThreadState);
    }
    python_shutdown = true;
    if (python_needs_shutdown) {
        int rc = Py_FinalizeEx();
        if (rc) {
            printd(0, "Unkown error shutting down Python: rc: %d\n", rc);
        }
        python_needs_shutdown = false;
    }
}

#if 0
// does not work with modules like tensorflow that do not unload cleanly
int q_reset_python(ExceptionSink* xsink) {
    if (!python_needs_shutdown) {
        xsink->raiseException("PYTHON-RESET-ERROR", "The module was loaded into an existing Python process and " \
            "therefore cannot be reset externally");
        return -1;
    }

    unsigned cnt = QorePythonProgram::getProgramCount();
    if (cnt) {
        if (cnt <= 2) {
            QoreProgram* pgm = getProgram();
            if (pgm) {
                QorePythonProgramData* pypgm = static_cast<QorePythonProgramData*>(pgm->removeExternalData(QORE_PYTHON_MODULE_NAME));
                if (pypgm) {
                    pypgm->destructor(xsink);
                    pypgm->weakDeref();
                    if (*xsink) {
                        return -1;
                    }
                    --cnt;
                }
            }
        }

        if (cnt == 1 && qore_python_pgm) {
            qore_python_pgm->destructor(xsink);
            qore_python_pgm->weakDeref();
            qore_python_pgm = nullptr;
            --cnt;
        }

        if (cnt) {
            xsink->raiseException("PYTHON-RESET-ERROR", "Cannot reset the Python library with %d Python program%s " \
                "still valid", cnt, cnt == 1 ? "" : "s");
            return -1;
        }
    }

    python_module_shutdown();

    SimpleRefHolder<QoreStringNode> err(python_module_init_intern(true));
    if (err) {
        xsink->raiseException("PYTHON-RESET-ERROR", err.release());
        return -1;
    }

    return 0;
}
#endif

//! True if this module registered the current thread with Qore; the thread was created by Python
static thread_local bool python_thread_registered = false;

//! Deregisters a thread that Python created when its Python thread state is cleared
/** An object of this type is stored in the thread state's dictionary when the thread is registered; the state is
    cleared on the thread itself when the thread ends (or, for a native thread using PyGILState_Ensure(), when its
    last PyGILState_Release() deletes the state), with the thread's Qore data still usable.  A state cleared on another
    thread (ex: when an interpreter ends) cannot deregister the thread it belongs to; that thread is then deregistered
    when it exits (see QorePythonThreadExit).
*/
struct QorePythonThreadSentinel {
    PyObject_HEAD
    //! the native thread that was registered
    pthread_t thread;
};

static void qore_python_thread_sentinel_dealloc(PyObject* self) {
    QorePythonThreadSentinel* sentinel = reinterpret_cast<QorePythonThreadSentinel*>(self);
    if (python_thread_registered && pthread_equal(sentinel->thread, pthread_self())) {
        python_thread_registered = false;
        q_deregister_foreign_thread();
    }
    Py_TYPE(self)->tp_free(self);
}

static PyTypeObject QorePythonThreadSentinel_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
};

//! Deregisters a thread that Python created when the native thread exits, if it is still registered
/** Normally the thread is deregistered when its Python thread state is cleared; this covers a thread whose state
    was cleared on another thread
*/
struct QorePythonThreadExit {
    //! set when the thread is registered, so that the object is constructed and its destructor runs at the exit
    bool armed = false;

    DLLLOCAL ~QorePythonThreadExit() {
        if (python_thread_registered) {
            python_thread_registered = false;
            q_deregister_foreign_thread();
        }
    }
};

static thread_local QorePythonThreadExit python_thread_exit;

//! The key of the sentinel in the dictionary of a Python thread state
static constexpr const char* QORE_PYTHON_THREAD_SENTINEL_KEY = "__qore_thread_registration__";

int qore_python_thread_registration_init() {
    QorePythonThreadSentinel_Type.tp_name = "qoreloader.QoreThreadRegistration";
    QorePythonThreadSentinel_Type.tp_basicsize = sizeof(QorePythonThreadSentinel);
    QorePythonThreadSentinel_Type.tp_dealloc = qore_python_thread_sentinel_dealloc;
    QorePythonThreadSentinel_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    QorePythonThreadSentinel_Type.tp_doc = "registration of a Python thread with Qore";
    return PyType_Ready(&QorePythonThreadSentinel_Type);
}

int qore_python_check_no_kwargs(PyObject* kwargs, const char* name) {
    if (!kwargs || !PyDict_Size(kwargs)) {
        return 0;
    }
    PyErr_Format(PyExc_TypeError, "%s takes no keyword arguments", name);
    return -1;
}

int qore_python_register_thread() {
    int rc = q_register_foreign_thread();
    if (rc == QFT_REGISTERED) {
        // a Qore thread, or a thread registered before
        return 0;
    }
    if (rc != QFT_OK) {
        return -1;
    }
    // the destructor of the thread-local object runs when the native thread exits
    python_thread_exit.armed = true;
    python_thread_registered = true;

    // the thread is deregistered when its Python thread state is cleared
    PyObject* dict = (QorePythonThreadSentinel_Type.tp_flags & Py_TPFLAGS_READY) ? PyThreadState_GetDict() : nullptr;
    if (dict) {
        QorePythonThreadSentinel* sentinel = PyObject_New(QorePythonThreadSentinel, &QorePythonThreadSentinel_Type);
        if (sentinel) {
            sentinel->thread = pthread_self();
            int set_rc = PyDict_SetItemString(dict, QORE_PYTHON_THREAD_SENTINEL_KEY,
                reinterpret_cast<PyObject*>(sentinel));
            Py_DECREF(sentinel);
            if (set_rc < 0) {
                PyErr_Clear();
            }
        } else {
            PyErr_Clear();
        }
    }
    return 0;
}

#ifndef _Q_WINDOWS
//! Replaces os.putenv(): changes the environment under the lock that serializes the environment with Qore
/** Python changes the C environment with setenv() when a variable of os.environ is set; without the lock, a Qore
    thread reading the environment at the same time could read freed memory.  The arguments are converted, checked
    and audited as Python's own os.putenv() does (Python 3.9 and later).  The GIL is released while the lock is
    waited for, so that no thread holds the environment lock while it waits for the GIL.
*/
static PyObject* qore_python_putenv(PyObject* self, PyObject* args) {
    PyObject* name = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "O&O&:putenv", PyUnicode_FSConverter, &name, PyUnicode_FSConverter, &value)) {
        return nullptr;
    }
    QorePythonReferenceHolder name_holder(name);
    QorePythonReferenceHolder value_holder(value);
    const char* name_str = PyBytes_AS_STRING(name);
    // an empty name is refused by setenv() with EINVAL, as with Python's own function
    if (strchr(name_str, '=')) {
        PyErr_SetString(PyExc_ValueError, "illegal environment variable name");
        return nullptr;
    }
    if (PySys_Audit("os.putenv", "OO", name, value) < 0) {
        return nullptr;
    }
    int rc, err;
    {
        QorePythonReleaseGilHelper prgh;
        rc = SystemEnvironment::set(name_str, PyBytes_AS_STRING(value));
        err = errno;
    }
    if (rc) {
        errno = err;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}

//! Replaces os.unsetenv(): removes a variable under the lock that serializes the environment with Qore
/** See qore_python_putenv()
*/
static PyObject* qore_python_unsetenv(PyObject* self, PyObject* args) {
    PyObject* name = nullptr;
    if (!PyArg_ParseTuple(args, "O&:unsetenv", PyUnicode_FSConverter, &name)) {
        return nullptr;
    }
    QorePythonReferenceHolder name_holder(name);
    // an empty name or one with '=' is refused by unsetenv() with EINVAL, as with Python's own function
    const char* name_str = PyBytes_AS_STRING(name);
    if (PySys_Audit("os.unsetenv", "(O)", name) < 0) {
        return nullptr;
    }
    int rc, err;
    {
        QorePythonReleaseGilHelper prgh;
        rc = SystemEnvironment::unset(name_str);
        err = errno;
    }
    if (rc) {
        errno = err;
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}

static PyMethodDef qore_python_putenv_def = {
    "putenv", qore_python_putenv, METH_VARARGS,
    "Change or add an environment variable; serialized with the environment changes of Qore."
};

static PyMethodDef qore_python_unsetenv_def = {
    "unsetenv", qore_python_unsetenv, METH_VARARGS,
    "Delete an environment variable; serialized with the environment changes of Qore."
};
#endif

int qore_python_install_environment_functions() {
#ifndef _Q_WINDOWS
    // os.environ calls the putenv() and unsetenv() of the os module, which are the functions of posix
    for (const char* name : {"posix", "os"}) {
        QorePythonReferenceHolder mod(PyImport_ImportModule(name));
        if (!mod) {
            return -1;
        }
        QorePythonReferenceHolder putenv_func(PyCFunction_New(&qore_python_putenv_def, nullptr));
        QorePythonReferenceHolder unsetenv_func(PyCFunction_New(&qore_python_unsetenv_def, nullptr));
        if (!putenv_func || !unsetenv_func
                || PyObject_SetAttrString(*mod, "putenv", *putenv_func) < 0
                || PyObject_SetAttrString(*mod, "unsetenv", *unsetenv_func) < 0) {
            return -1;
        }
    }
#endif
    return 0;
}

void QorePythonQoreEntryHelper::applySandbox() {
    if (!python_thread_registered) {
        return;
    }
    QorePythonProgram* pypgm = QorePythonProgram::getContext();
    if (!pypgm) {
        return;
    }
    // the sandbox of the Program that owns the interpreter, as it is when the call is made (it can be attached after
    // the interpreter was created), or else the sandbox of the code that created the interpreter
    {
        std::shared_ptr<QoreSandboxCapture> creation = pypgm->getSandboxCapture();
        std::optional<QoreSandboxCaptureHelper> creation_helper;
        if (creation) {
            creation_helper.emplace(*creation);
        }
        QoreProgramContextHelper pch(pypgm->getQoreProgram());
        sandbox = std::make_shared<QoreSandboxCapture>();
    }
    if (sandbox->isSandboxed()) {
        sandbox_helper.emplace(*sandbox);
    }
}

/* The environment copies that Python makes when an interpreter is created or imports a module are made under Qore's
   environment lock: the exec function of the posix module copies the C environment into posix.environ (which becomes
   os.environ) in every interpreter, including each subinterpreter while Py_NewInterpreter() runs, and the exec
   function of the time module calls tzset(), which reads TZ.  Both modules use multi-phase initialization: their exec
   functions are slots of their module definitions, which every interpreter uses, and are replaced with functions that
   hold the lock while they call the original ones.

   The lock is held only while those functions run: they run C code only, and garbage collection, which could run
   Python code and release the GIL, is disabled meanwhile, so the lock is never held while a thread waits for the GIL
   (see design/python-module.md, "The Process Environment").  Holding it for all of Py_NewInterpreter() instead would
   hold it while the new interpreter runs Python code (importlib, site), which releases the GIL; a thread that holds
   the GIL while it forks (os.fork(), or _posixsubprocess on platforms without vfork()) waits for the lock in Qore's
   fork handlers, and the creating thread would wait for the GIL while holding the lock.
*/
struct QorePythonEnvironmentExecSlot {
    const char* module_name;
    PyModuleDef* def;
    PyModuleDef_Slot* orig_slots;
    int (*orig_exec)(PyObject*);
    std::vector<PyModuleDef_Slot> slots;
};

static QorePythonEnvironmentExecSlot python_env_exec_slots[] = {
    {"posix", nullptr, nullptr, nullptr, {}},
    {"time", nullptr, nullptr, nullptr, {}},
};

template <size_t N>
static int python_exec_under_environment_lock(PyObject* module) {
    int (*orig_exec)(PyObject*) = python_env_exec_slots[N].orig_exec;
    assert(orig_exec);
    // no Python code runs while the lock is held
    int gc_enabled = PyGC_Disable();
    int rc;
    {
        QoreEnvironmentReadLock env_lock;
        rc = orig_exec(module);
    }
    if (gc_enabled) {
        PyGC_Enable();
    }
    return rc;
}

static int (*const python_env_exec_wrappers[])(PyObject*) = {
    python_exec_under_environment_lock<0>,
    python_exec_under_environment_lock<1>,
};

//! replaces the exec slots of the modules that read the environment; must be called with the GIL held
static int python_wrap_environment_exec_slots() {
    static_assert(sizeof(python_env_exec_slots) / sizeof(python_env_exec_slots[0])
        == sizeof(python_env_exec_wrappers) / sizeof(python_env_exec_wrappers[0]), "one wrapper for each module");
    for (size_t i = 0; i < sizeof(python_env_exec_slots) / sizeof(python_env_exec_slots[0]); ++i) {
        QorePythonEnvironmentExecSlot& info = python_env_exec_slots[i];
        if (info.def) {
            continue;
        }
        QorePythonReferenceHolder mod(PyImport_ImportModule(info.module_name));
        if (!mod) {
            return -1;
        }
        PyModuleDef* def = PyModule_GetDef(*mod);
        // a module with single-phase initialization has no exec slot
        if (!def || !def->m_slots) {
            continue;
        }
        size_t count = 0;
        size_t exec_index = SIZE_MAX;
        for (PyModuleDef_Slot* slot = def->m_slots; slot->slot; ++slot, ++count) {
            if (slot->slot == Py_mod_exec) {
                if (exec_index != SIZE_MAX) {
                    // more than one exec function: not known
                    exec_index = SIZE_MAX;
                    break;
                }
                exec_index = count;
            }
        }
        if (exec_index == SIZE_MAX) {
            continue;
        }
        try {
            info.slots.assign(def->m_slots, def->m_slots + count + 1);
        } catch (const std::bad_alloc&) {
            PyErr_NoMemory();
            return -1;
        }
        info.orig_exec = reinterpret_cast<int (*)(PyObject*)>(info.slots[exec_index].value);
        info.slots[exec_index].value = reinterpret_cast<void*>(python_env_exec_wrappers[i]);
        info.orig_slots = def->m_slots;
        info.def = def;
        def->m_slots = info.slots.data();
    }
    return 0;
}

//! restores the exec slots replaced by python_wrap_environment_exec_slots()
static void python_unwrap_environment_exec_slots() {
    for (QorePythonEnvironmentExecSlot& info : python_env_exec_slots) {
        if (info.def) {
            info.def->m_slots = info.orig_slots;
            info.def = nullptr;
            info.orig_slots = nullptr;
            info.orig_exec = nullptr;
        }
    }
}

static void python_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink) {
    // registered in the initialization function and not while static objects are constructed (see QoreForkHandler.h)
    if (QorePythonProgram::registerForkHandlers()) {
        xsink.outOfMemory();
        return;
    }
    if (qpy_register_fork_handlers()) {
        QorePythonProgram::unregisterForkHandlers();
        xsink.outOfMemory();
        return;
    }
#ifdef QORE_PYTHON_HAVE_FORK_GIL_CHECK
    // only reads the state of the GIL in the child
    python_gil_fork_reg = qore_register_fork_handler(QFR_MODULE_RESET, "python_gil", nullptr, nullptr,
        python_gil_fork_child, nullptr);
    if (!python_gil_fork_reg) {
        qpy_unregister_fork_handlers();
        QorePythonProgram::unregisterForkHandlers();
        xsink.outOfMemory();
        return;
    }
#endif
    {
        // Python's fork protocol for Qore's fork() called from Python code; registered once per process
        static bool fork_protocol_registered = false;
        if (!fork_protocol_registered) {
            QoreString err;
            if (qore_python_register_fork_protocol(err)) {
#ifdef QORE_PYTHON_HAVE_FORK_GIL_CHECK
                qore_unregister_fork_handler(python_gil_fork_reg);
                python_gil_fork_reg = nullptr;
#endif
                qpy_unregister_fork_handlers();
                QorePythonProgram::unregisterForkHandlers();
                xsink.raiseException("MODULE-INIT-ERROR", "%s", err.c_str());
                return;
            }
            fork_protocol_registered = true;
        }
    }
    QoreStringNode* err = python_module_init_intern(false);
    if (err) {
        xsink.raiseException("MODULE-INIT-ERROR", err);
    }
}

// defined in the generated ql_python.cpp (from ql_python.qpp)
DLLLOCAL void init_python_functions(QoreNamespace& ns);

static QoreStringNode* python_module_init_intern(bool repeat) {
    if (!PNS) {
        PNS = new QoreNamespace("Python");
        PNS->addSystemClass(initPythonProgramClass(*PNS));
        // register Python namespace functions (e.g. Python::set_save_object_callback())
        init_python_functions(*PNS);
        QC_PYTHONBASEOBJECT = new QorePythonClass("__qore_base__", "::Python::__qore_base__");
        CID_PYTHONBASEOBJECT = QC_PYTHONBASEOBJECT->getID();

        PNS->addSystemClass(QC_PYTHONBASEOBJECT->copy());

        // Add constant to indicate if this is a free-threading Python build
#ifdef Py_GIL_DISABLED
        PNS->addConstant("FreeThreading", true);
#else
        PNS->addConstant("FreeThreading", false);
#endif
    }

    if (!repeat) {
        python_u_tld_key = q_get_unique_thread_local_data_key();
        python_qobj_key = q_get_unique_thread_local_data_key();
    }

    // initialize python library; do not register signal handlers
    if (!Py_IsInitialized()) {
        if (PyImport_AppendInittab("qoreloader", PyInit_qoreloader) == -1) {
            throw QoreStandardException("PYTHON-MODULE-ERROR", "cannot append the qoreloader module to Python");
        }

        {
            // Python reads the environment while it initializes (its configuration variables and the copy that
            // becomes os.environ); no other Python thread exists yet, so the lock is not held while a thread waits
            // for the GIL, which a thread forking with os.fork() could hold while it waits for the lock
            QoreEnvironmentReadLock env_lock;
            Py_InitializeEx(0);
            // the time module reads the environment (TZ) when it is created; it is created here, while the lock is
            // held and no other thread can run Python code, so that its definition can be found and its exec
            // function replaced (see python_wrap_environment_exec_slots())
            QorePythonReferenceHolder time_module(PyImport_ImportModule("time"));
            if (!time_module) {
                PyErr_Clear();
            }
        }
#ifdef QORE_ALLOW_PYTHON_SHUTDOWN
        // issue# 4290: if we actively shut down Python on exit, then exit handlers in modules
        // (such as the h5py module in version 3.3.0) will cause a crash when the process exits,
        // as it requires the Python library to be still in place and initialized
        python_needs_shutdown = true;
#endif
        python_initialized = true;
        //printd(5, "python_module_init() Python initialized\n");
    }

    if (!repeat) {

#ifndef _Q_WINDOWS
        sig_vec_t new_sig_vec;
        for (int sig : sig_vec) {
            QoreStringNode *err = qore_reassign_signal(sig, QORE_PYTHON_MODULE_NAME);
            if (err) {
                // the signal is managed by another module (ex: jni), which sets its mask
                err->deref();
                continue;
            }
            new_sig_vec.push_back(sig);
        }
        if (!new_sig_vec.empty()) {
            sigset_t mask;
            // setup signal mask
            sigemptyset(&mask);
            for (auto& sig : new_sig_vec) {
                //printd(LogLevel, "python_module_init() unblocking signal %d\n", sig);
                sigaddset(&mask, sig);
            }
            // unblock threads
            pthread_sigmask(SIG_UNBLOCK, &mask, 0);
        }
#endif
    }

    // ensure that runtime version matches compiled version
    check_python_version();

    // Ensure sys.path is initialized when embedding (esp. for debug builds).
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

    // Initialize thread-local state tracking to match Python's state
    // This must be done before creating any QorePythonProgram instances
#ifdef Py_GIL_DISABLED
    // In free-threading mode, ensure main thread state is attached before any Python API calls
    mainThreadState = _qore_safe_thread_state_get();
    //printd(5, "python_module_init_intern() mainThreadState: %p current: %p\n",
    //    mainThreadState, PyGILState_GetThisThreadState());
    if (!PyGILState_GetThisThreadState()) {
        PyThreadState_Swap(mainThreadState);
    }
#else
    // In GIL mode, PyGILState_GetThisThreadState() might return NULL during early init
    // even though we have the GIL. Use _qore_safe_thread_state_get() which tolerates missing TSS.
    PyThreadState* init_tstate = PyGILState_GetThisThreadState();
    if (!init_tstate) {
        // TSS not set up yet - get the actual thread state and set it up
        init_tstate = PyThreadState_Get();
        // Also update mainThreadState for later use
        mainThreadState = init_tstate;
    }
    _qore_PyGILState_SetThisThreadState(init_tstate);
    // We have the GIL at this point after Py_InitializeEx()
    _QORE_SET_GIL_HELD(true);
#endif

    if (init_global_qore_python_pgm()) {
        // the thread is registered with Qore here, so this is not expected
        PyErr_Clear();
        throw QoreStandardException("PYTHON-MODULE-ERROR", "failed to initialize \"python\" module");
    }

#ifdef Py_GIL_DISABLED
    // In free-threading mode, use PyGILState_Ensure to properly set up the thread for Python ops
    // This ensures the mimalloc heap is properly initialized for this thread
    PyGILState_STATE gstate = PyGILState_Ensure();
    //printd(5, "python_module_init_intern() after PyGILState_Ensure: current: %p gstate: %d\n",
    //    PyGILState_GetThisThreadState(), (int)gstate);
#endif

    if (QorePythonProgram::staticInit()) {
#ifdef Py_GIL_DISABLED
        PyGILState_Release(gstate);
#endif
        throw QoreStandardException("PYTHON-MODULE-ERROR", "failed to initialize \"python\" module");
    }

#ifdef Py_GIL_DISABLED
    PyGILState_Release(gstate);
    //printd(5, "python_module_init_intern() after PyGILState_Release: current: %p\n",
    //    PyGILState_GetThisThreadState());
#endif

    if (python_wrap_environment_exec_slots()) {
        PyErr_Clear();
        throw QoreStandardException("PYTHON-MODULE-ERROR", "cannot initialize the environment functions of the "
            "posix and time modules");
    }

    // Initialize the process-wide bridge types in the main interpreter. If the
    // first import happens in a transient subinterpreter, the static type objects
    // and cached loader callables outlive the interpreter that created them.
    {
        QorePythonReferenceHolder loader_module(PyImport_ImportModule("qoreloader"));
        if (!loader_module) {
            PyErr_Clear();
            throw QoreStandardException("PYTHON-MODULE-ERROR", "cannot initialize qoreloader in the main interpreter");
        }
    }

#ifndef Py_GIL_DISABLED
    mainThreadState = PyThreadState_Get();
    if (python_initialized) {
#if PY_VERSION_HEX >= 0x030D0000
        // Python 3.13+ changed thread state management significantly; the main thread state, which other threads
        // use, is not left bound to this thread (see qore_python_release_context())
        printd(5, "python_module_init: before release, mainThreadState: %p TSS: %p GIL: %d\n",
            mainThreadState, PyGILState_GetThisThreadState(), PyGILState_Check());
        assert(PyThreadState_Get() == mainThreadState);
        qore_python_release_context();
        printd(5, "python_module_init: after release, TSS: %p GIL: %d\n",
            PyGILState_GetThisThreadState(), PyGILState_Check());
#else
        // release the current thread state after initialization
#if PY_VERSION_HEX >= 0x030C0000
        // the main thread state, which other threads use, is not left bound to this thread (see
        // qore_python_release_context())
        assert(PyThreadState_Get() == mainThreadState);
        qore_python_release_context();
#else
        _qore_release_thread_state(mainThreadState);
#endif
        // Our tracking should be cleared now
        assert(!_qore_PyRuntimeGILState_GetThreadState());
        _qore_PyGILState_SetThisThreadState(nullptr);
        // NOTE: In Python 3.12, PyEval_ReleaseThread does NOT clear PyGILState_GetThisThreadState()
        // because it doesn't update the autoTSSkey. This is different from earlier Python versions.
        // We only check haveGil() which uses our own tracking.
        assert(!QorePythonProgram::haveGil());
#endif
    }
#else
    if (python_initialized) {
        PyThreadState_Swap(nullptr);
    }
#endif

    if (!repeat) {
        tclist.push(QorePythonProgram::pythonThreadCleanup, nullptr);
    }

    return nullptr;
}

static void python_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
    QoreProgram* pgm = getProgram();
    assert(pgm->getRootNS() == rns);
    if (!pgm->getExternalData(QORE_PYTHON_MODULE_NAME)) {
        QoreNamespace* pyns = PNS->copy();
        rns->addNamespace(pyns);
        // NOTE: Use QoreProgramContextHelper instead of QoreExternalProgramContextHelper
        // because QoreExternalProgramContextHelper uses runtime=true which triggers
        // doTopLevelInstantiation(), setting tlpd->inst = true. This happens before the
        // user's top-level local variables are defined (during %requires processing), so
        // no variables are actually instantiated. When the helper destructs, tlpd->inst
        // stays true even though no variables were instantiated. Later when runTopLevel()
        // is called, it skips doTopLevelInstantiation because tlpd->inst is true, causing
        // crashes when accessing top-level local variables defined after %requires python.
        // QoreProgramContextHelper just sets the current program without triggering
        // thread-local variable instantiation.
        //
        // QoreProgramContextHelper only manages the program context (save/restore), which raises no exceptions
        QoreProgramContextHelper pch(pgm);
        // an error is raised in the Program that loads the module; the invalid program is kept, so that its
        // interpreter is released when the Program is deleted
        pgm->setExternalData(QORE_PYTHON_MODULE_NAME, new QorePythonProgram(pgm, pyns, &xsink));
    }

#ifndef Py_GIL_DISABLED
#if PY_VERSION_HEX < 0x030C0000
    // In Python 3.12+, PyGILState_Check() behavior changed - it returns 1 even after
    // releasing the GIL because PyEval_ReleaseThread doesn't clear the TSS.
    // In Python 3.13+, sub-interpreters also affect this behavior.
    assert(!python_initialized || !PyGILState_Check());
#endif
    // haveGil() uses our own tracking which should be accurate
    assert(!python_initialized || !QorePythonProgram::haveGil());
#endif
}

static void python_module_delete() {
    if (qore_python_pgm) {
        qore_python_pgm->doDeref();
        qore_python_pgm = nullptr;
    }
    if (PNS) {
        delete PNS;
        PNS = nullptr;
    }
    python_module_shutdown();
    python_unwrap_environment_exec_slots();
#ifdef QORE_PYTHON_HAVE_FORK_GIL_CHECK
    qore_unregister_fork_handler(python_gil_fork_reg);
    python_gil_fork_reg = nullptr;
#endif
    qpy_unregister_fork_handlers();
    QorePythonProgram::unregisterForkHandlers();
}

static void python_module_parse_cmd(const QoreString& cmd, ExceptionSink* xsink) {
    //printd(5, "python_module_parse_cmd() cmd: '%s'\n", cmd.c_str());

    const char* p = strchr(cmd.c_str(), ' ');
    QoreString str;
    QoreString arg;
    if (p) {
        QoreString nstr(&cmd, p - cmd.c_str());
        str = nstr;
        arg = cmd;
        arg.replace(0, p - cmd.c_str() + 1, (const char*)nullptr);
        arg.trim();
    } else {
        str = cmd;
        str.trim();
    }

    mcmap_t::const_iterator i = mcmap.find(str.c_str());
    if (i == mcmap.end()) {
        QoreStringNode* desc = new QoreStringNodeMaker("unrecognized command '%s' in '%s' (valid commands: ", str.c_str(), cmd.c_str());
        for (mcmap_t::const_iterator i = mcmap.begin(), e = mcmap.end(); i != e; ++i) {
            if (i != mcmap.begin())
                desc->concat(", ");
            desc->sprintf("'%s'", i->first.c_str());
        }
        desc->concat(')');
        xsink->raiseException("PYTHON-PARSE-COMMAND-ERROR", desc);
        return;
    }

    QoreProgram* pgm = getProgram();
    // Python code is not subject to Qore's restrictions, so every command, which gives access to Python, is an
    // uncontrolled API, like the Python classes and functions that the module imports, and "parse" also parses code at
    // runtime; the restrictions of the Program are checked here, as a Program that inherits the module from its parent
    // can issue commands without loading the module
    if (pgm->getParseOptions() & PO_NO_UNCONTROLLED_APIS) {
        xsink->raiseException("PYTHON-PARSE-COMMAND-ERROR", "python module command '%s' is not allowed in a Program "
            "restricted with PO_NO_UNCONTROLLED_APIS", str.c_str());
        return;
    }
    if (str == "parse" && (pgm->getParseOptions() & PO_NO_EMBEDDED_LOGIC)) {
        xsink->raiseException("PYTHON-PARSE-COMMAND-ERROR", "python module command 'parse' is not allowed in a "
            "Program restricted with PO_NO_EMBEDDED_LOGIC");
        return;
    }

    if (i->second.requires_arg) {
        if (arg.empty()) {
            xsink->raiseException("PYTHON-PARSE-COMMAND-ERROR", "missing argument / command name in parse command: '%s'", cmd.c_str());
            return;
        }
    } else {
        if (!arg.empty()) {
            xsink->raiseException("PYTHON-PARSE-COMMAND-ERROR", "extra argument / command name in parse command: '%s'", cmd.c_str());
            return;
        }
    }

    QorePythonProgram* pypgm = static_cast<QorePythonProgram*>(pgm->getExternalData(QORE_PYTHON_MODULE_NAME));
    //printd(5, "parse-cmd '%s' pypgm: %p pythonns: %p\n", arg.c_str(), pypgm, pypgm->getPythonNamespace());
    if (!pypgm) {
        QoreNamespace* pyns = PNS->copy();
        pgm->getRootNS()->addNamespace(pyns);
        pypgm = new QorePythonProgram(pgm, pyns, xsink);
        pgm->setExternalData(QORE_PYTHON_MODULE_NAME, pypgm);
        pgm->addFeature(QORE_PYTHON_MODULE_NAME);
        if (*xsink) {
            return;
        }
    }

    i->second.cmd(xsink, arg, pypgm);
}

// %module-cmd(python) import
static void py_mc_import(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    // process import statement
    //printd(5, "py_mc_import() pypgm: %p arg: %s\n", pypgm, arg.c_str());

    QorePythonHelper qph(pypgm, xsink);
    if (qph.wasInterrupted()) {
        return;
    }

    // see if there is a dot (.) in the name
    qore_offset_t i = arg.find('.');
    if (i < 0 || i == static_cast<qore_offset_t>(arg.size() - 1)) {
        pypgm->import(xsink, arg.c_str());
        return;
    }

    const char* symbol = arg.c_str() + i + 1;
    arg.replaceChar(i, '\0');

    arg.terminate(i);
    if (!strcmp(symbol, "*")) {
        pypgm->import(xsink, arg.c_str());
        return;
    }

    pypgm->import(xsink, arg.c_str(), symbol);
}

// %module-cmd(python) import-ns <qore-namespace> <python-module-path>
static void py_mc_import_ns(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    // find end of qore namespace
    qore_offset_t end = arg.find(' ');
    if (end == -1) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "syntax: import-ns <qore-namespace> " \
            "<python-module-path>: missing python module path argument; value given: '%s'", arg.c_str());
    }

    QoreString qore_ns(&arg, end);
    QoreString py_mod_path(arg.c_str() + end + 1);

    QoreProgram* pgm = getProgram();
    if (!pgm) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "import-ns error: no current Program context");
    }

    QoreNamespace* ns = pgm->findNamespace(qore_ns);
    if (!ns || ns == pgm->getRootNS()) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "import-ns error: Qore namespace '%s' not found",
            qore_ns.c_str());
    }

    pypgm->importQoreNamespaceToPython(*ns, py_mod_path, xsink);
}

// %module-cmd(python) alias <python-source-path> <python-target-path>
static void py_mc_alias(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    // find end of qore namespace
    qore_offset_t end = arg.find(' ');
    if (end == -1 || (size_t)end == (arg.size() - 1)) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "syntax: alias <python-source-path> " \
            "<python-target-path: python target path argument; value given: '%s'", arg.c_str());
    }

    QoreString source_path(&arg, end);
    QoreString target_path(arg.c_str() + end + 1);

    pypgm->aliasDefinition(source_path, target_path);
}

// %module-cmd(python) parse <label> <source code>
static void py_mc_parse(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    // find end of qore namespace
    qore_offset_t end = arg.find(' ');
    if (end == -1 || (size_t)end == (arg.size() - 1)) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "syntax: alias <python-source-path> " \
            "<python-target-path: python target path argument; value given: '%s'", arg.c_str());
    }

    QoreString source_label(&arg, end);
    QoreString source_code(arg.c_str() + end + 1);

    ValueHolder val(pypgm->eval(xsink, source_code, source_label, Py_file_input, false), xsink);
}

// %module-cmd(python) export-class <python path>
/** export a Python class to Qore
*/
static void py_mc_export_class(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    pypgm->exportClass(xsink, arg);
}

// %module-cmd(python) export-func <python path>
/** export a Python function to Qore
*/
static void py_mc_export_func(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    pypgm->exportFunction(xsink, arg);
}

// %module-cmd(python) add-module-path <fs path>
/** add a path to the module path
*/
static void py_mc_add_module_path(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    pypgm->addModulePath(xsink, arg);
}

#if 0
// %module-cmd(python) reset-python
static void py_mc_reset_python(ExceptionSink* xsink, QoreString& arg, QorePythonProgram* pypgm) {
    q_reset_python(xsink);
}
#endif

// exported function
extern "C" int python_module_import(ExceptionSink* xsink, QoreProgram* pgm, const char* module, const char* symbol) {
    QorePythonProgram* pypgm = static_cast<QorePythonProgram*>(pgm->getExternalData(QORE_PYTHON_MODULE_NAME));
    if (!pypgm) {
        QoreNamespace* pyns = PNS->copy();
        pgm->getRootNS()->addNamespace(pyns);
        pypgm = new QorePythonProgram(pgm, pyns, xsink);
        pgm->setExternalData(QORE_PYTHON_MODULE_NAME, pypgm);
        pgm->addFeature(QORE_PYTHON_MODULE_NAME);
        if (*xsink) {
            return -1;
        }
    }
    // the following call adds the class to the current program as well
    QorePythonHelper qph(pypgm, xsink);
    if (qph.wasInterrupted()) {
        return -1;
    }
    return pypgm->import(xsink, module, symbol);
}

thread_local const QorePythonHelper* QorePythonHelper::current = nullptr;

// returns the thread state that the current thread has attached, if any
static inline PyThreadState* qore_python_attached_thread_state() {
#if PY_VERSION_HEX >= 0x030D0000
    return PyThreadState_GetUnchecked();
#else
    return _PyThreadState_UncheckedGet();
#endif
}

bool QorePythonHelper::canNest(const QorePythonProgram* pypgm, ExceptionSink* xsink) {
    const QorePythonHelper* outer = current;
    // the thread state that the outer helper attached is still attached: the GIL (or the attachment in free-threaded
    // builds) has not been released since, and no other interpreter's state has been swapped in
    if (!outer || outer->new_pypgm != pypgm || !outer->attached || python_shutdown || python_fork_lost
        || !pypgm->isValid() || qore_python_attached_thread_state() != outer->attached) {
        return false;
    }
    // an interrupt is raised by the normal path (setContext())
    return !xsink || !qore_check_cancel(nullptr, "Python execution");
}

QorePythonHelper::QorePythonHelper(const QorePythonProgram* pypgm, ExceptionSink* xsink)
        : nested(canNest(pypgm, xsink)),
            old_pgm(nested ? (void*)pypgm : q_swap_thread_local_data(python_u_tld_key, (void*)pypgm)),
            old_state(nested ? QorePythonThreadInfo{nullptr, nullptr, nullptr, nullptr, PyGILState_UNLOCKED, false}
                : pypgm->setContext(xsink != nullptr)),
            new_pypgm(pypgm), prev(current) {
    current = this;
    //printd(5, "QorePythonHelper::QorePythonHelper() new: %p old: %p\n", pypgm, old_pgm);
    if (nested) {
        attached = prev->attached;
        if (xsink) {
            registerInterrupt();
        }
        return;
    }
    if (old_state.valid) {
        attached = qore_python_attached_thread_state();
    }
    if (xsink) {
        if (python_fork_lost) {
            qore_python_raise_fork_error(xsink);
        } else if (wasInterrupted()) {
            xsink->raiseException("PROGRAM-INTERRUPTED",
                "program execution was interrupted while acquiring the Python GIL");
        } else if (!old_state.valid && !new_pypgm->isValid()) {
            // The interpreter has been deleted - raise an exception
            xsink->raiseException("PYTHON-INTERPRETER-DELETED",
                "cannot execute Python callable: the Python interpreter has been deleted");
        } else if (old_state.valid) {
            registerInterrupt();
        }
    }
}

QorePythonHelper::~QorePythonHelper() {
    deactivateInterrupt();
    if (!nested) {
        new_pypgm->releaseContext(old_state);
        q_swap_thread_local_data(python_u_tld_key, (void*)old_pgm);
    }
    assert(current == this);
    current = prev;
    deregisterInterrupt();
}

void QorePythonHelper::registerInterrupt() {
    // the first helper of the program in the thread registers the callback: an inner one would clear the interrupt
    // raised for the outer one
    for (const QorePythonHelper* h = prev; h; h = h->prev) {
        if (h->new_pypgm == new_pypgm && h->interrupt) {
            return;
        }
    }
    // constructed in place: most programs have no sandbox manager, and the helper is created for each call
    interrupt_smh.emplace(QoreSandboxManagerHelper::All);
    if (!*interrupt_smh) {
        interrupt_smh.reset();
        return;
    }
    interrupt = std::make_shared<InterruptState>();
    interrupt->thread_id = PyThread_get_thread_ident();
    interrupt->pypgm = new_pypgm;
    std::shared_ptr<InterruptState> state = interrupt;
    // the callback is called on the thread that requests the interrupt, possibly after it was removed
    (*interrupt_smh)->registerCancelCallback(interrupt.get(), [state]() -> bool {
        std::lock_guard<std::mutex> al(state->lck);
        if (!state->active.load()) {
            return false;
        }
        // the interpreter's context on the calling thread; the program exists while the callback is registered
        QorePythonHelper qph(state->pypgm);
        if (!qph.isValid() || !state->active.load()) {
            return false;
        }
        // KeyboardInterrupt is a BaseException: except Exception does not catch it; checkPythonException() raises the
        // Qore interrupt for it
        bool rv = PyThreadState_SetAsyncExc(state->thread_id, PyExc_KeyboardInterrupt) > 0;
        if (rv) {
            state->raised.store(true);
        }
        return rv;
    });
}

void QorePythonHelper::deactivateInterrupt() {
    if (!interrupt) {
        return;
    }
    // with the GIL held, so the callback cannot raise the interrupt after this
    interrupt->active.store(false);
    // an interrupt that was not delivered must not be raised in other Python code that the thread runs later
    if (interrupt->raised.load()) {
        PyThreadState_SetAsyncExc(interrupt->thread_id, nullptr);
    }
}

void QorePythonHelper::deregisterInterrupt() {
    if (!interrupt) {
        return;
    }
    // without the GIL: waits for a callback that is running, which can be waiting for the GIL
    {
        std::lock_guard<std::mutex> al(interrupt->lck);
    }
    (*interrupt_smh)->unregisterCancelCallback(interrupt.get());
}

bool QorePythonHelper::isValid() const {
    return nested || old_state.valid || !new_pypgm->isValid();
}

bool QorePythonHelper::wasInterrupted() const {
    return !nested && !old_state.valid && new_pypgm->isValid();
}

bool _qore_has_gil(PyThreadState* t_state) {
    if (!_qore_PyCeval_GetGilLockedStatus()) {
        return false;
    }
#if PY_VERSION_HEX < 0x030C0000
    // In Python < 3.12, PyGILState_Check() is reliable. If it says we have the GIL,
    // we have it even if t_state is NULL (can happen during early init before TSS is set).
    if (t_state == nullptr && PyGILState_Check()) {
        return true;
    }
#endif
    return _qore_PyCeval_GetThreadState() == t_state;
}

#ifndef Py_GIL_DISABLED
static bool _qore_has_gil(PyThreadState* state0, PyThreadState* state1) {
    if (!_qore_PyCeval_GetGilLockedStatus()) {
        return false;
    }
#if PY_VERSION_HEX < 0x030C0000
    // In Python < 3.12, PyGILState_Check() is reliable. If it says we have the GIL,
    // we have it even if both states are NULL (can happen during early init).
    if (state0 == nullptr && state1 == nullptr && PyGILState_Check()) {
        return true;
    }
#endif
    PyThreadState* gs = _qore_PyCeval_GetThreadState();
    return gs == state0 || gs == state1;
}
#endif

#if !defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030C0000
namespace {
// the idle thread state of this thread: a state of this thread in the main interpreter, which only this thread uses,
// attached before the thread releases the GIL (see qore_python_release_context()); created on first use, and deleted
// when the thread ends (see qore_python_delete_idle_thread_state())
thread_local PyThreadState* python_idle_tstate = nullptr;
}
#endif

#ifndef Py_GIL_DISABLED
void qore_python_release_context() {
#if PY_VERSION_HEX >= 0x030C0000
    // Detaching does not clear CPython's binding of the state to the thread for the GIL state API (its auto-TSS):
    // a state of another interpreter left bound to a thread that does not hold the GIL dangles when another thread
    // destroys the interpreter, which cannot clear this thread's binding, and a state that other threads use (the
    // main thread state) must not be bound to the thread either, since its bound_gilstate flag is then changed by
    // other threads.  The idle state of the thread, in the main interpreter, which ends only when Python is
    // finalized, is attached instead before the GIL is released: CPython moves the binding to it, with the GIL held.
    // This uses no internal CPython API, whose binding moved in Python 3.15 from _PyRuntime.autoTSSkey to a hidden
    // thread-local variable.  Previously the shared main thread state was bound to every thread that released the
    // GIL here, and its flag was cleared by threads that did not hold the GIL.
    PyThreadState* tstate = PyThreadState_Get();
    if (tstate == mainThreadState || qore_python_get_interpreter(tstate) != PyInterpreterState_Main()) {
        if (!python_idle_tstate) {
            python_idle_tstate = PyThreadState_New(PyInterpreterState_Main());
        }
        // without memory for it, the state stays bound to the thread
        if (python_idle_tstate) {
            PyThreadState_Swap(python_idle_tstate);
        }
    }
#endif
    PyEval_SaveThread();
    _qore_PyGILState_SetThisThreadState(nullptr);
    _QORE_SET_GIL_HELD(false);
}

void qore_python_delete_idle_thread_state() {
#if PY_VERSION_HEX >= 0x030C0000
    PyThreadState* tstate = python_idle_tstate;
    if (!tstate) {
        return;
    }
    python_idle_tstate = nullptr;
    // after Python is finalized, or in a child of fork() that cannot use Python, the state is left as it is
    if (python_shutdown || python_fork_lost || !Py_IsInitialized()) {
        return;
    }
#if PY_VERSION_HEX >= 0x030D0000
    PyThreadState* current = PyThreadState_GetUnchecked();
#else
    PyThreadState* current = _PyThreadState_UncheckedGet();
#endif
    if (current) {
        // the thread holds the GIL with another state: a thread created by Python is deregistered from Qore while
        // its own state is cleared when it ends; the idle state, which has not run code, is deleted while that state
        // stays attached (deleting it unbinds it from the thread if it is bound)
        assert(current != tstate);
        PyThreadState_Clear(tstate);
        PyThreadState_Delete(tstate);
        return;
    }
    PyEval_RestoreThread(tstate);
    PyThreadState_Clear(tstate);
    // unbinds the state from the thread and releases the GIL
    PyThreadState_DeleteCurrent();
#endif
}
#endif

QorePythonGilHelper::QorePythonGilHelper(PyThreadState* new_thread_state)
    : new_thread_state(new_thread_state), state(nullptr), t_state(nullptr), release_gil(true) {
    // CRITICAL: Check if Python is shutting down or not initialized before ANY Python API calls.
    // After Py_FinalizeEx(), calling PyGILState_Ensure() will crash because the TSS points
    // to a stale thread state with a freed interpreter.
    if (python_shutdown || !Py_IsInitialized()) {
        printd(5, "QorePythonGilHelper ctor: Python is shutting down, skipping initialization\n");
        return;
    }
    assert(new_thread_state);

#ifdef Py_GIL_DISABLED
    // Attach the requested interpreter without consulting the auto-TSS cache,
    // which can name a detached state from a different interpreter.
    t_state = PyThreadState_GetUnchecked();
    if (t_state != new_thread_state) {
        PyThreadState_Swap(new_thread_state);
    }
#else
    state = _qore_PyRuntimeGILState_GetThreadState();
    t_state = PyGILState_GetThisThreadState();
    release_gil = !_qore_has_gil(t_state, new_thread_state);
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ GIL mode - use PyEval_AcquireThread/ReleaseThread which properly
    // handle TSS and fast TLS synchronization
    if (release_gil) {
        // No bound_gilstate flag is changed without the GIL: other threads use the state (the main thread state)
        // with the GIL held; CPython binds it to this thread when it is attached unless it is bound to another
        // thread (see qore_python_release_context())
        // Need to acquire the GIL with our specific thread state
        // CRITICAL: If there's a stale TSS from a previous interpreter, we must clear it first.
        // PyEval_AcquireThread -> _PyThreadState_Attach tries to detach any existing thread state,
        // and if the TSS doesn't match, it will assert.
        PyEval_AcquireThread(new_thread_state);
    } else {
        // Already have the GIL - swap to our thread state if needed
        if (t_state != new_thread_state) {
            printd(5, "QorePythonGilHelper ctor: swapping from %p to %p\n", t_state, new_thread_state);
            PyThreadState_Swap(new_thread_state);
        }
    }
    // the attached state tells that the thread holds the GIL with new_thread_state (no tracking in 3.13+)
    assert(PyThreadState_GetUnchecked() == new_thread_state);
#else
    if (release_gil) {
        // No bound_gilstate flag or binding is changed without the GIL: other threads use the state (the main thread
        // state) with the GIL held; CPython binds it to this thread when it is attached unless it is bound to another
        // thread (see qore_python_release_context())
        _qore_acquire_thread_state(new_thread_state);
        // Use _qore_tss_tstate for assertion since Python's TSS (PyGILState_GetThisThreadState)
        // might be stale from a deleted interpreter in Python 3.12
        assert(_qore_tss_tstate == new_thread_state);
    } else {
        // NOTE: In Python 3.12, t_state (from PyGILState_GetThisThreadState()) may be stale
        // after PyEval_ReleaseThread() since Python's TSS isn't cleared.
        // _qore_has_gil() verified we have the GIL with either t_state or new_thread_state,
        // so check against both possibilities.
        [[maybe_unused]] PyThreadState* ceval_ts = _qore_PyCeval_GetThreadState();
        assert(ceval_ts == t_state || ceval_ts == new_thread_state);
    }
    // NOTE: even if the current thread state is equal to the new one, we still need to set all thread states in all
    // locations

    _QORE_PYTHREAD_STATE_SWAP(new_thread_state);

    // set this thread state
    _qore_PyGILState_SetThisThreadState(new_thread_state);
#if PY_VERSION_HEX < 0x030C0000
    // NOTE: In Python 3.12+, PyGILState_GetThisThreadState() can be unreliable because
    // PyEval_ReleaseThread() doesn't clear the autoTSSkey. Our tracking is authoritative.
    assert(PyGILState_GetThisThreadState() == new_thread_state);
    assert(PyGILState_Check());
#else
    // Python 3.12+: PyGILState_Check() and PyGILState_GetThisThreadState() rely on Python's
    // internal TSS (autoTSSkey) which can be stale after PyEval_ReleaseThread() or corrupted
    // by external modules like JNI. Our thread-local tracking (_qore_tss_tstate) is reliable.
    assert(_qore_PyCeval_GetGilLockedStatus());
#endif
#endif
#endif
    initialized = true;
}

QorePythonGilHelper::~QorePythonGilHelper() {
    // CRITICAL: If Python was shutting down during construction, skip all cleanup
    if (!initialized) {
        return;
    }
#ifdef Py_GIL_DISABLED
    PyThreadState_Swap(t_state);
#elif PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ GIL mode
    printd(5, "QorePythonGilHelper dtor: release_gil: %d t_state: %p new_thread_state: %p\n",
        release_gil, t_state, new_thread_state);

    if (release_gil) {
        // We acquired the GIL, so release it
        // Use PyEval_SaveThread which properly releases the GIL
        qore_python_release_context();
    } else if (t_state != new_thread_state) {
        // We already had the GIL with t_state - attach it again, as the constructor attached new_thread_state
        PyThreadState_Swap(t_state);
    }
#else

    if (release_gil) {
        //printd(5, "QorePythonGilHelper::~QorePythonGilHelper() releasing %llx state: %llx t_state: %llx\n",
        //    new_thread_state, state, t_state);
        // We acquired the GIL with new_thread_state in the constructor.
        // During our lifetime, setContext() might have changed the ceval thread state to a different
        // PythonProgram's thread state. We need to ensure we release the correct state.
        // Swap to new_thread_state before releasing to avoid "wrong thread state" error.
        // Use _qore_PyCeval_GetThreadState() instead of PyThreadState_Get() - the latter crashes
        // in Python 3.11 if the GIL is not held properly (e.g., after PyInterpreterState_Delete).
        PyThreadState* current = _qore_PyCeval_GetThreadState();
        if (current != new_thread_state) {
            PyThreadState_Swap(new_thread_state);
        }
        qore_python_release_context();
        // _qore_release_thread_state already cleared _qore_tss_tstate to nullptr.
    } else {
        //printd(5, "QorePythonGilHelper::~QorePythonGilHelper() swapping %llx state: %llx t_state: %llx\n",
        //    new_thread_state, state, t_state);
        // We already had the GIL - restore to original state
        _QORE_PYTHREAD_STATE_SWAP(state);
        _qore_PyCeval_SwapThreadState(t_state);
        // restore the old TLD state - only when we already had the GIL
        _qore_PyGILState_SetThisThreadState(t_state);
    }
#endif
}

#ifdef Py_GIL_DISABLED
void QorePythonGilHelper::releaseBeforeSubInterpreter() {
    // Creation attaches the new interpreter; the helper restores the original
    // attachment on exit, after the new program has finished initialization.
    PyThreadState_Swap(nullptr);
}

#endif

void QorePythonGilHelper::set(PyThreadState* other_state) {
    // This function is called after Py_NewInterpreter() to switch from the main interpreter's
    // thread state (which was used to acquire the GIL in the constructor) to the new sub-interpreter's
    // thread state, which the destructor then releases.  The module does not change the gilstate_counter of thread
    // states: CPython creates every state with a counter of 1, and only PyGILState_Release() changes it (see
    // design/python-module.md).
#ifdef Py_GIL_DISABLED
    // In free-threading mode, Py_NewInterpreterFromConfig already attached the new thread state.
    // Just update our tracking variable so the destructor restores correctly.
    // Do NOT call PyThreadState_Swap again as it may corrupt the thread's heap state.
    new_thread_state = other_state;
#else
    assert(_qore_PyCeval_GetGilLockedStatus() && _qore_PyCeval_GetThreadState());

    // Update new_thread_state so the destructor releases the correct thread state
    new_thread_state = other_state;

    _QORE_PYTHREAD_STATE_SWAP(other_state);
    _qore_PyCeval_SwapThreadState(other_state);
    _qore_PyGILState_SetThisThreadState(other_state);
#endif
}

bool qore_python_release_runs_qore_code(const QoreValue& v) {
    if (!v.hasNode()) {
        return false;
    }
    const AbstractQoreNode* n = v.getInternalNode();
    // releasing a reference that is not the last one only decrements the reference count
    if (n->reference_count() != 1) {
        return false;
    }
    switch (n->getType()) {
        case NT_STRING:
        case NT_NUMBER:
        case NT_DATE:
        case NT_BINARY:
        case NT_NULL:
            return false;

        case NT_LIST: {
            ConstListIterator i(static_cast<const QoreListNode*>(n));
            while (i.next()) {
                if (qore_python_release_runs_qore_code(i.getValue())) {
                    return true;
                }
            }
            return false;
        }

        case NT_HASH: {
            ConstHashIterator i(static_cast<const QoreHashNode*>(n));
            while (i.next()) {
                if (qore_python_release_runs_qore_code(i.get())) {
                    return true;
                }
            }
            return false;
        }

        default:
            // objects, closures, call references and any other value
            return true;
    }
}

void qore_python_release(ValueHolder& v) {
    if (qore_python_release_runs_qore_code(*v)) {
        QorePythonReleaseGilHelper prgh;
        v = QoreValue();
    }
}

void qore_python_release(ReferenceHolder<QoreListNode>& l) {
    if (l && qore_python_release_runs_qore_code(*l)) {
        QorePythonReleaseGilHelper prgh;
        l = nullptr;
    }
}
