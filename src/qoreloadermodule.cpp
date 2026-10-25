/* indent-tabs-mode: nil -*- */
/*
    qore Python module

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
#include "QoreThreadAttachHelper.h"

#include <qore/QoreForkHandler.h>
#include "QoreLoader.h"
#include "JavaLoader.h"
#include "QoreMetaPathFinder.h"
#include "QorePythonProgram.h"
#include "PythonQoreCallable.h"
#ifdef QORE_PYTHON_HAVE_FORK_GIL_CHECK
#include "python_fork_gil.h"
#endif

#include <dlfcn.h>
#include <cstdlib>
#include <memory>
#include <string>

QorePythonProgram* qore_python_pgm = nullptr;

PyDoc_STRVAR(module_doc, "This module provides dynamic access to Qore APIs.");

static int slot_qoreloader_exec(PyObject* m);

static PyObject* qoreloader_load_java(PyObject* self, PyObject* args);
static PyObject* qoreloader_issue_module_cmd(PyObject* self, PyObject* args);

#if defined(DEBUG) && defined(QORE_PYTHON_HAVE_FORK_GIL_CHECK)
// test hooks of debug builds (see python_fork_gil.h); only for the main interpreter, where the code of a Python process
// hosting Qore runs: the code of Qore Programs, which can be sandboxed, runs in other interpreters
static PyObject* qoreloader_test_hold_gil_mutex(PyObject* self, PyObject* args) {
    if (qore_python_get_interpreter(PyThreadState_Get()) != PyInterpreterState_Main()) {
        PyErr_SetString(PyExc_RuntimeError, "the test hook can only be called in the main interpreter");
        return nullptr;
    }
    if (qore_python_test_hold_gil_mutex()) {
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}

static PyObject* qoreloader_test_release_gil_mutex(PyObject* self, PyObject* args) {
    if (qore_python_get_interpreter(PyThreadState_Get()) != PyInterpreterState_Main()) {
        PyErr_SetString(PyExc_RuntimeError, "the test hook can only be called in the main interpreter");
        return nullptr;
    }
    if (qore_python_test_release_gil_mutex()) {
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}
#endif

static PyMethodDef qoreloader_methods[] = {
    {"load_java", qoreloader_load_java, METH_VARARGS, "Import the given Java path to the parent Qore program if " \
        "possible; args: import_str (ex: 'org.qore.lang.restclient.*')"},
    {"issue_module_cmd", qoreloader_issue_module_cmd, METH_VARARGS, "Issue the given module command on the given " \
        "module; args: module_name, cmd"},
#if defined(DEBUG) && defined(QORE_PYTHON_HAVE_FORK_GIL_CHECK)
    {"_test_hold_gil_mutex", qoreloader_test_hold_gil_mutex, METH_NOARGS, "Test hook of debug builds: a thread "
        "holds the mutex of the GIL until _test_release_gil_mutex() is called; the calling thread must not release "
        "the GIL meanwhile"},
    {"_test_release_gil_mutex", qoreloader_test_release_gil_mutex, METH_NOARGS, "Test hook of debug builds: "
        "releases the mutex held since _test_hold_gil_mutex()"},
#endif
    {nullptr, nullptr, 0, nullptr},
};

bool qore_needs_shutdown = false;

PyTypeObject PythonQoreObjectBase_Type = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "PythonQoreObjectBase",
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = "base class for Python objects based on Qore classes",
};

static int init_count = 0;

//! the state of the qoreloader module in an interpreter
struct qoreloader_state {
    //! the qoreloader.QoreException type of the interpreter
    PyObject* qore_exception_type;
    //! the qoreloader.QoreStaticMember type of the interpreter (the descriptor of the static members of Qore classes)
    PyObject* qore_static_member_type;
    //! true if the module is counted in init_count (the module is freed also when its initialization fails before)
    bool counted;
};

static qoreloader_state* qoreloader_get_state(PyObject* m) {
    return static_cast<qoreloader_state*>(PyModule_GetState(m));
}

static int qoreloader_traverse(PyObject* m, visitproc visit, void* arg) {
    qoreloader_state* st = qoreloader_get_state(m);
    if (st) {
        Py_VISIT(st->qore_exception_type);
        Py_VISIT(st->qore_static_member_type);
    }
    return 0;
}

static int qoreloader_clear(PyObject* m) {
    qoreloader_state* st = qoreloader_get_state(m);
    if (st) {
        Py_CLEAR(st->qore_exception_type);
        Py_CLEAR(st->qore_static_member_type);
    }
    return 0;
}

void qoreloader_free(void* obj) {
    qoreloader_state* st = qoreloader_get_state(static_cast<PyObject*>(obj));
    bool counted = st && st->counted;
    qoreloader_clear(static_cast<PyObject*>(obj));
    if (counted && !--init_count) {
        printd(5, "qoreloader_free() obj: %p qore_needs_shutdown: %d\n", obj, qore_needs_shutdown);

        if (qore_python_pgm) {
            qore_python_pgm->doDeref();
            qore_python_pgm = nullptr;
        }

        QoreLoader::del();
        JavaLoader::del();

        if (qore_needs_shutdown) {
            qore_cleanup();
        }
    }
}

static struct PyModuleDef_Slot qoreloader_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(slot_qoreloader_exec)},
#if PY_VERSION_HEX >= 0x030C0000
    // Python 3.12+ sub-interpreter support (PEP 684)
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ free-threading support (PEP 703)
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, nullptr},
};

static struct PyModuleDef qoreloadermodule = {
    PyModuleDef_HEAD_INIT,
    "qoreloader",        // m_name
    module_doc,          // m_doc
    sizeof(qoreloader_state), // m_size
    qoreloader_methods,  // m_methods
    qoreloader_slots,    // m_slots
    qoreloader_traverse, // m_traverse
    qoreloader_clear,    // m_clear
    qoreloader_free,     // m_free
};

typedef std::set<QorePythonProgram*> qpy_pgm_set_t;
static qpy_pgm_set_t qpy_pgm_set;
QoreThreadLock qpy_lock;

// Global set to track ALL QorePythonProgram pointers for validity checking
static qpy_pgm_set_t qpy_global_set;
static QoreThreadLock qpy_global_lock;
// Counter for how many sub-interpreters have been destroyed
// Used to detect when destructor calls might be unsafe
static std::atomic<int> qpy_destroyed_count{0};

// fork handlers of qpy_lock and qpy_global_lock (see QoreForkHandler.h): their holders only change the sets of
// programs, so they are leaves (QFR_MODULE_LEAF) acquired before fork()
static QoreForkHandlerRegistration* qpy_lock_fork_reg = nullptr;
static QoreForkHandlerRegistration* qpy_global_lock_fork_reg = nullptr;
static QoreForkHandlerRegistration* qpy_subclass_type_lock_fork_reg = nullptr;
static QoreForkHandlerRegistration* qpy_proxy_lock_fork_reg = nullptr;

QoreThreadLock qpy_subclass_type_lock;
QoreThreadLock qpy_proxy_lock;

int qpy_register_fork_handlers() {
    assert(!qpy_lock_fork_reg && !qpy_global_lock_fork_reg);
    qpy_lock_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_program_registry",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock, &qpy_lock);
    qpy_global_lock_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_program_validity",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock, &qpy_global_lock);
    qpy_subclass_type_lock_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_qore_subclass_types",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock,
        &qpy_subclass_type_lock);
    qpy_proxy_lock_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_qore_object_proxies",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock, &qpy_proxy_lock);
    if (!qpy_lock_fork_reg || !qpy_global_lock_fork_reg || !qpy_subclass_type_lock_fork_reg
        || !qpy_proxy_lock_fork_reg || QorePythonDeclHandle::registerForkHandler()) {
        qpy_unregister_fork_handlers();
        return -1;
    }
    return 0;
}

void qpy_unregister_fork_handlers() {
    QorePythonDeclHandle::unregisterForkHandler();
    qore_unregister_fork_handler(qpy_proxy_lock_fork_reg);
    qpy_proxy_lock_fork_reg = nullptr;
    qore_unregister_fork_handler(qpy_subclass_type_lock_fork_reg);
    qpy_subclass_type_lock_fork_reg = nullptr;
    qore_unregister_fork_handler(qpy_global_lock_fork_reg);
    qpy_global_lock_fork_reg = nullptr;
    qore_unregister_fork_handler(qpy_lock_fork_reg);
    qpy_lock_fork_reg = nullptr;
}

void qpy_global_register(QorePythonProgram* p) {
    AutoLocker al(qpy_global_lock);
    qpy_global_set.insert(p);
}

void qpy_global_deregister(QorePythonProgram* p) {
    AutoLocker al(qpy_global_lock);
    qpy_global_set.erase(p);
    // NOTE: We don't increment destroyed count here - it's incremented when the
    // interpreter is actually destroyed in qpy_interpreter_destroyed()
}

void qpy_interpreter_destroyed() {
    ++qpy_destroyed_count;
}

bool qpy_is_valid(QorePythonProgram* p) {
    AutoLocker al(qpy_global_lock);
    return qpy_global_set.find(p) != qpy_global_set.end();
}

int qpy_get_destroyed_count() {
    return qpy_destroyed_count.load();
}

bool qpy_register(QorePythonProgram* p) {
    if (!qore_needs_shutdown) {
        return false;
    }
    printd(5, "qpy_register() p: %p\n", p);
    AutoLocker al(qpy_lock);
    assert(qpy_pgm_set.find(p) == qpy_pgm_set.end());
    qpy_pgm_set.insert(p);
    return true;
}

void qpy_deregister(QorePythonProgram* p) {
    if (!qore_needs_shutdown) {
        return;
    }
    printd(5, "qpy_deregister() p: %p\n", p, qore_needs_shutdown);
    AutoLocker al(qpy_lock);
    qpy_pgm_set_t::iterator i = qpy_pgm_set.find(p);
    if (i != qpy_pgm_set.end()) {
        qpy_pgm_set.erase(i);
    }
}

static PyObject* qoreloader_atexit(PyObject* self, PyObject* args) {
#ifndef Py_GIL_DISABLED
    PyGILState_STATE gstate = PyGILState_Ensure();
#endif

    if (!qpy_pgm_set.empty()) {
        assert(mainThreadState);

        ExceptionSink xsink;
        for (auto& i : qpy_pgm_set) {
            //printd(5, "qoreloader_atexit() p: %p\n", i);
            i->py_destructor(&xsink);
        }
    }

    qore_needs_shutdown = false;
    qore_cleanup();

    assert(mainThreadState);
    _QORE_PYTHREAD_STATE_SWAP(mainThreadState);

    Py_INCREF(Py_None);
#ifndef Py_GIL_DISABLED
    PyGILState_Release(gstate);
#endif
    return Py_None;
}

static PyMethodDef atexit_md = {
    "qoreloader_atexit",
    qoreloader_atexit,
    METH_NOARGS,
    "qoreloader cleanup function",
};

// QoreException.__init__(self, err, desc=None, arg=None)
static PyObject* qore_exception_init(PyObject* /* unused */, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"", "err", "desc", "arg", nullptr};
    PyObject* self;
    PyObject* err;
    PyObject* desc = Py_None;
    PyObject* arg = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|OO:QoreException", const_cast<char**>(kwlist), &self, &err,
        &desc, &arg)) {
        return nullptr;
    }
    // the arguments are (err, desc[, arg]) as for a Qore exception, also when given as keyword arguments
    Py_ssize_t size = arg != Py_None ? 3 : (desc != Py_None ? 2 : 1);
    QorePythonReferenceHolder ex_args(PyTuple_New(size));
    if (!ex_args) {
        return nullptr;
    }
    PyObject* vals[] = {err, desc, arg};
    for (Py_ssize_t i = 0; i < size; ++i) {
        Py_INCREF(vals[i]);
        PyTuple_SET_ITEM(*ex_args, i, vals[i]);
    }
    if (PyObject_SetAttrString(self, "args", *ex_args) < 0
        || PyObject_SetAttrString(self, "err", err) < 0
        || PyObject_SetAttrString(self, "desc", desc) < 0
        || PyObject_SetAttrString(self, "arg", arg) < 0) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

// QoreException.__str__(self): "err: desc", or "err" without a description
static PyObject* qore_exception_str(PyObject* /* unused */, PyObject* self) {
    QorePythonReferenceHolder err(PyObject_GetAttrString(self, "err"));
    if (!err) {
        // ex: a subclass whose __init__() does not call QoreException.__init__()
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return nullptr;
        }
        PyErr_Clear();
        return reinterpret_cast<PyTypeObject*>(PyExc_BaseException)->tp_str(self);
    }
    QorePythonReferenceHolder desc(PyObject_GetAttrString(self, "desc"));
    if (!desc) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            return nullptr;
        }
        PyErr_Clear();
    }
    if (!desc || *desc == Py_None) {
        return PyObject_Str(*err);
    }
    return PyUnicode_FromFormat("%S: %S", *err, *desc);
}

static PyMethodDef qore_exception_init_def = {"__init__", reinterpret_cast<PyCFunction>(
    reinterpret_cast<void(*)(void)>(qore_exception_init)), METH_VARARGS | METH_KEYWORDS,
    "QoreException(err, desc=None, arg=None): sets the err, desc and arg attributes"};

static PyMethodDef qore_exception_str_def = {"__str__", qore_exception_str, METH_O,
    "returns \"err: desc\", or \"err\" without a description"};

PyDoc_STRVAR(qore_exception_doc, "A Qore exception\n\n"
    "Qore exceptions are raised in Python as QoreException objects with the attributes err, desc and arg; "
    "exceptions raised by Qore code also have the attributes file, line, endline, source, offset, lang and "
    "callstack, and the exceptions chained to them are set as __cause__.  A QoreException raised in Python is a "
    "Qore exception with its err, desc and arg in Qore.");

// adds a method implemented in C to the class dictionary given
static int qore_exception_add_method(PyObject* dict, PyMethodDef* def) {
    QorePythonReferenceHolder func(PyCFunction_NewEx(def, nullptr, nullptr));
    if (!func) {
        return -1;
    }
    // an instance method: the object is passed as the first argument
    QorePythonReferenceHolder meth(PyInstanceMethod_New(*func));
    if (!meth) {
        return -1;
    }
    return PyDict_SetItemString(dict, def->ml_name, *meth);
}

// creates the qoreloader.QoreException type of the interpreter executing the module; a subclass of RuntimeError
static int qoreloader_init_exception_type(PyObject* m) {
    qoreloader_state* st = qoreloader_get_state(m);
    assert(st && !st->qore_exception_type);
    QorePythonReferenceHolder dict(PyDict_New());
    if (!dict || qore_exception_add_method(*dict, &qore_exception_init_def)
        || qore_exception_add_method(*dict, &qore_exception_str_def)) {
        return -1;
    }
    st->qore_exception_type = PyErr_NewExceptionWithDoc("qoreloader.QoreException", qore_exception_doc,
        PyExc_RuntimeError, *dict);
    if (!st->qore_exception_type) {
        return -1;
    }
    return PyModule_AddObjectRef(m, "QoreException", st->qore_exception_type);
}

// the descriptor of a static member of a Qore class: gives and sets the current value of the member, also for the
// class (ex: Cls.member); assigning to the attribute of the class replaces the descriptor, as for any class attribute
static const char* qore_static_member_src = R"(
class QoreStaticMember:
    """A static member of a Qore class: its value is the current value of the member"""
    __slots__ = ('name', '_get', '_set')

    def __init__(self, name, get, set):
        self.name = name
        self._get = get
        self._set = set

    def __get__(self, obj, cls=None):
        return self._get()

    def __set__(self, obj, value):
        self._set(value)

    def __repr__(self):
        return '<Qore static member ' + self.name + '>'
)";

// creates the qoreloader.QoreStaticMember type of the interpreter executing the module
static int qoreloader_init_static_member_type(PyObject* m) {
    qoreloader_state* st = qoreloader_get_state(m);
    assert(st && !st->qore_static_member_type);
    QorePythonReferenceHolder globals(PyDict_New());
    if (!globals || PyDict_SetItemString(*globals, "__builtins__", PyEval_GetBuiltins()) < 0) {
        return -1;
    }
    QorePythonReferenceHolder name(PyUnicode_FromString("qoreloader"));
    if (!name || PyDict_SetItemString(*globals, "__name__", *name) < 0) {
        return -1;
    }
    QorePythonReferenceHolder rv(PyRun_String(qore_static_member_src, Py_file_input, *globals, *globals));
    if (!rv) {
        return -1;
    }
    // returns a borrowed reference
    PyObject* type = PyDict_GetItemString(*globals, "QoreStaticMember");
    if (!type) {
        PyErr_SetString(PyExc_RuntimeError, "cannot create the qoreloader.QoreStaticMember type");
        return -1;
    }
    Py_INCREF(type);
    st->qore_static_member_type = type;
    return PyModule_AddObjectRef(m, "QoreStaticMember", type);
}

// returns a new reference to the qoreloader module of the current interpreter, importing it if necessary
static PyObject* qoreloader_get_module() {
    QorePythonReferenceHolder name(PyUnicode_FromString("qoreloader"));
    if (!name) {
        return nullptr;
    }
    QorePythonReferenceHolder mod(PyImport_GetModule(*name));
    if (!mod) {
        if (PyErr_Occurred()) {
            return nullptr;
        }
        mod = PyImport_Import(*name);
        if (!mod) {
            return nullptr;
        }
    }
    if (!PyModule_Check(*mod) || PyModule_GetDef(*mod) != &qoreloadermodule) {
        PyErr_SetString(PyExc_ImportError, "sys.modules['qoreloader'] is not the qoreloader module");
        return nullptr;
    }
    return mod.release();
}

PyObject* qore_python_get_qore_static_member_type() {
    QorePythonReferenceHolder mod(qoreloader_get_module());
    if (!mod) {
        return nullptr;
    }
    qoreloader_state* st = qoreloader_get_state(*mod);
    if (!st || !st->qore_static_member_type) {
        PyErr_SetString(PyExc_ImportError, "the qoreloader module has not been initialized");
        return nullptr;
    }
    Py_INCREF(st->qore_static_member_type);
    return st->qore_static_member_type;
}

PyObject* qore_python_get_qore_exception_type() {
    QorePythonReferenceHolder mod(qoreloader_get_module());
    if (!mod) {
        return nullptr;
    }
    qoreloader_state* st = qoreloader_get_state(*mod);
    if (!st || !st->qore_exception_type) {
        PyErr_SetString(PyExc_ImportError, "the qoreloader module has not been initialized");
        return nullptr;
    }
    Py_INCREF(st->qore_exception_type);
    return st->qore_exception_type;
}

//! returns the resolved path of this binary if it has the extension of binary Qore modules, otherwise an empty string
static std::string qoreloader_get_module_path() {
    Dl_info info;
    if (!dladdr(reinterpret_cast<void*>(&PyInit_qoreloader), &info) || !info.dli_fname) {
        return std::string();
    }
    std::unique_ptr<char, decltype(&free)> resolved(realpath(info.dli_fname, nullptr), &free);
    if (!resolved) {
        return std::string();
    }
    std::string path(resolved.get());
    static constexpr const char ext[] = ".qmod";
    if (path.size() <= sizeof(ext) - 1 || path.compare(path.size() - (sizeof(ext) - 1), sizeof(ext) - 1, ext)) {
        return std::string();
    }
    return path;
}

static int slot_qoreloader_exec(PyObject *m) {
    // when Python is the host, the state of the module for the process (the initialization of the Qore library, the
    // global Qore program and the cleanup at exit) belongs to the main interpreter, which keeps it until the process
    // exits; a sub-interpreter can be closed at any time, which would shut Qore down while the process uses it
    if (!init_count && !q_libqore_initalized()
        && qore_python_get_interpreter(PyThreadState_Get()) != PyInterpreterState_Main()) {
        PyErr_SetString(PyExc_ImportError, "qoreloader must be imported in the main Python interpreter before it "
            "can be imported in a sub-interpreter");
        return -1;
    }
    ++init_count;
    qoreloader_get_state(m)->counted = true;
    printd(5, "slot_qoreloader_exec() ic: %d\n", init_count);
    if (init_count == 1) {
        // initialize qore library if necessary
        if (!q_libqore_initalized()) {
            printd(5, "PyInit_qoreloader() initializing Qore library\n");
            // Python is the host: it owns the process's signal handling, so Qore must not block the signals (which
            // the process's threads and child processes inherit) or handle them in its own thread
            qore_init(QL_MIT, nullptr, false, QLO_DISABLE_SIGNAL_HANDLING);
            qore_needs_shutdown = true;
            printd(5, "PyInit_qoreloader() Qore library initialized\n");

            // ensure that the Qore python module is registered as a Qore module: by the path of this binary, which
            // is the module; with the feature name, the module manager would load the python module that it finds
            // in its module path, which can be another binary, so that the process would have two copies of the
            // module (possibly built for another Python).  The path is resolved, as qoreloader.so is usually a
            // symbolic link, and the module manager loads a binary module only from a file with its extension
            std::string self_path = qoreloader_get_module_path();
            ExceptionSink xsink;
            MM.runTimeLoadModule(&xsink, self_path.empty() ? "python" : self_path.c_str(), nullptr,
                python_qore_module_desc);
            if (xsink) {
                QoreValue err(xsink.getExceptionErr());
                QoreValue desc(xsink.getExceptionDesc());
                QoreStringValueHelper errstr(err);
                QoreStringValueHelper descstr(desc);
                PyErr_Format(PyExc_ImportError, "cannot load the Qore \"python\" module: %s: %s", errstr->c_str(),
                    descstr->c_str());
                xsink.clear();
                return -1;
            }
        }

        if (init_global_qore_python_pgm()) {
            return -1;
        }

        if (PyType_Ready(&PythonQoreObjectBase_Type) < 0) {
            return -1;
        }

        if (PyType_Ready(&PythonQoreCallable_Type) < 0) {
            return -1;
        }

        if (qore_python_thread_registration_init() < 0) {
            return -1;
        }

        if (QoreLoader::init()) {
            return -1;
        }

        if (JavaLoader::init()) {
            return -1;
        }

        if (QoreMetaPathFinder::init()) {
            return -1;
        }

        if (qore_needs_shutdown) {
            // set cleanup function call
            QorePythonReferenceHolder atexit(PyImport_ImportModule("atexit"));
            if (!*atexit) {
                printd(5, "slot_qoreloader_exec() ERROR: no atexit module\n");
                return -1;
            }

            if (!PyObject_HasAttrString(*atexit, "register")) {
                printd(5, "slot_qoreloader_exec() ERROR: no atexit.register() method\n");
                return -1;
            }

            QorePythonReferenceHolder register_func(PyObject_GetAttrString(*atexit, "register"));
            if (!register_func || !PyCallable_Check(*register_func)) {
                printd(5, "slot_qoreloader_exec() ERROR: atexit.register() is not callable\n");
                return -1;
            }

            // call atexit.register()
            QorePythonReferenceHolder args(PyTuple_New(1));
            QorePythonReferenceHolder func(PyCFunction_New(&atexit_md, nullptr));
            PyTuple_SET_ITEM(*args, 0, func.release());
            QorePythonReferenceHolder rv_ignored(PyObject_CallObject(*register_func, *args));
        }
    }

    if (qoreloader_init_exception_type(m) || qoreloader_init_static_member_type(m)) {
        return -1;
    }

    QoreMetaPathFinder::setupModules();

    // changes to os.environ in this interpreter are serialized with the environment changes of Qore
    if (qore_python_install_environment_functions()) {
        return -1;
    }

    // the main interpreter is the only one that can fork() (os.fork(), and Qore's fork() called from Python code)
    if (qore_python_get_interpreter(PyThreadState_Get()) == PyInterpreterState_Main()
        && qore_python_register_at_fork_functions()) {
        return -1;
    }

    return 0;
}

int init_global_qore_python_pgm() {
    if (!qore_python_pgm) {
        // save and restore the Python thread state while initializing the Qore python module
        PythonThreadStateHelper ptsh;

        QoreThreadAttachHelper attach_helper;
        if (attach_helper.attach()) {
            PyErr_SetString(PyExc_ImportError, "cannot register the thread with Qore: the maximum number of Qore "
                "threads has been reached");
            return -1;
        }
        qore_python_pgm = new QorePythonProgram;
    }

    return 0;
}

PyMODINIT_FUNC PyInit_qoreloader() {
    // first check if the runtime library differs from the dynamically-linked one
    if (!Py_TYPE(PyExc_RuntimeError)) {
        fprintf(stderr, "ERROR: the Python runtime library is different than the dynamically linked one; it's not " \
            "possible to raise a Python exception in this case without a crash; aborting\n");
        return nullptr;
    }

    const char* ver = Py_GetVersion();
    if (!ver) {
        fprintf(stderr, "cannot determine Python version; no value returned from Py_GetVersion()'\n");
        return nullptr;
    }
    const char* p = strchr(ver, '.');
    if (!p || p == ver) {
        fprintf(stderr, "cannot determine Python major version from '%s'\n", ver);
        return nullptr;
    }
    int major = atoi(ver);
    int minor = atoi(p + 1);

    if (major != PY_MAJOR_VERSION || minor != PY_MINOR_VERSION) {
        fprintf(stderr, "cannot load the qoreloader module; compiled with '%d.%d.%d'; runtime version is '%s'\n",
            PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION, ver);
        return nullptr;
    }

    return PyModuleDef_Init(&qoreloadermodule);
}

int load_jni_module(QorePythonProgram* qore_python_pgm) {
#ifdef Py_GIL_DISABLED
    // JNI is not compatible with Python free-threading mode due to mimalloc heap conflicts
    // JNI creates threads that don't have proper Python mimalloc heap initialization
    PyErr_SetString(PyExc_RuntimeError,
        "JNI/Java integration is not supported with Python free-threading (PEP 703) builds; "
        "use a GIL-enabled Python build for Java integration");
    return -1;
#else
    static bool jni_loaded = false;

    if (!jni_loaded) {
        QoreProgram* qpgm = qore_python_pgm->getQoreProgram();
        ExceptionSink xsink;
        if (ModuleManager::runTimeLoadModule("jni", qpgm, &xsink)) {
            qore_python_pgm->raisePythonException(xsink);
            return -1;
        }
        printd(5, "load_jni_module() 'jni' module loaded\n");
        jni_loaded = true;
    }
    return 0;
#endif
}

int do_jni_module_import(QorePythonProgram* qore_python_pgm, const char* name_str) {
    typedef int (*jni_module_import_t)(ExceptionSink* xsink, QoreProgram* pgm, const char* import);
    static jni_module_import_t jni_module_import = nullptr;

    if (!jni_module_import) {
        if (load_jni_module(qore_python_pgm)) {
            return -1;
        }
        jni_module_import = (jni_module_import_t)dlsym(RTLD_DEFAULT, "jni_module_import");
        if (!jni_module_import) {
            PyErr_SetString(PyExc_ValueError, "cannot find required symbol 'jni_module_import'");
            return -1;
        }
    }

    {
        QoreString path(name_str);
        if (path.size() > 2 && (path[-2] == '.' && path[-1] == '*')) {
            QoreStringMaker desc("'%s': wildcard imports are not currently supported", name_str);
            PyErr_SetString(PyExc_ValueError, desc.c_str());
            return -1;
        }
    }

    QoreProgram* qpgm = qore_python_pgm->getQoreProgram();
    ExceptionSink xsink;

    // set program context for initialization
    QoreProgramContextHelper pgm_ctx(qpgm);
    if (jni_module_import(&xsink, qpgm, name_str)) {
        assert(xsink);
        qore_python_pgm->raisePythonException(xsink);
        return -1;
    }

    return 0;
}

PyObject* qoreloader_load_java(PyObject* self, PyObject* args) {
    if (!PyTuple_Check(args) || (PyTuple_Size(args) != 1)) {
        PyErr_SetString(PyExc_ValueError, "single string argument required for 'qoreloader.load_java()'");
        return nullptr;
    }
    PyObject* name = PyTuple_GetItem(args, 0);
    if (!PyUnicode_Check(name)) {
        QoreStringMaker desc("single string string argument required for 'qoreloader.load_java(); got type '%s' " \
            "instead", Py_TYPE(name)->tp_name);
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return nullptr;
    }
    // nullptr with a Python exception (ex: UnicodeEncodeError for a lone surrogate)
    const char* name_str = PyUnicode_AsUTF8(name);
    if (!name_str) {
        return nullptr;
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    if (do_jni_module_import(qore_python_pgm, name_str)) {
        return nullptr;
    }
    //printd(5, "qoreloader_load_java() jni module loaded: import '%s' pgm: %p\n", name_str, qpgm);

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* qoreloader_issue_module_cmd(PyObject* self, PyObject* args) {
    if (!PyTuple_Check(args) || (PyTuple_Size(args) != 2)) {
        PyErr_SetString(PyExc_ValueError, "two string arguments required for 'qoreloader.issue_module_cmd()'");
        return nullptr;
    }
    PyObject* module = PyTuple_GetItem(args, 0);
    if (!PyUnicode_Check(module)) {
        QoreStringMaker desc("first agument must be a string when calling 'qoreloader.issue_module_cmd(); got type " \
            "'%s' instead", Py_TYPE(module)->tp_name);
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return nullptr;
    }
    PyObject* cmd = PyTuple_GetItem(args, 1);
    if (!PyUnicode_Check(cmd)) {
        QoreStringMaker desc("second agument must be a string when calling 'qoreloader.issue_module_cmd(); got type " \
            "'%s' instead", Py_TYPE(cmd)->tp_name);
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return nullptr;
    }

    // nullptr with a Python exception (ex: UnicodeEncodeError for a lone surrogate)
    const char* module_str = PyUnicode_AsUTF8(module);
    if (!module_str) {
        return nullptr;
    }
    const char* cmd_str = PyUnicode_AsUTF8(cmd);
    if (!cmd_str) {
        return nullptr;
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    QoreProgram* qpgm = qore_python_pgm->getQoreProgram();
    if (qpgm->issueModuleCmd(module_str, cmd_str, &xsink)) {
        assert(xsink);
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    assert(!xsink);

    Py_INCREF(Py_None);
    return Py_None;
}
