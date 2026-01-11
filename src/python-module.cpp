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
#include "QC_PythonProgram.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"

static QoreStringNode* python_module_init();
static void python_module_ns_init(QoreNamespace* rns, QoreNamespace* qns);
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
    mod_info.license = QL_MIT;
    mod_info.license_str = "MIT";

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

static QoreStringNode* python_module_init() {
    return python_module_init_intern(false);
}

static QoreStringNode* python_module_init_intern(bool repeat) {
    if (!PNS) {
        PNS = new QoreNamespace("Python");
        PNS->addSystemClass(initPythonProgramClass(*PNS));
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

    // initialize python library; do not register signal handlers
    if (!Py_IsInitialized()) {
        if (PyImport_AppendInittab("qoreloader", PyInit_qoreloader) == -1) {
            throw QoreStandardException("PYTHON-MODULE-ERROR", "cannot append the qoreloader module to Python");
        }

        Py_InitializeEx(0);
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
                // ignore errors; already assigned to another module
                err->deref();
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

        python_u_tld_key = q_get_unique_thread_local_data_key();
        python_qobj_key = q_get_unique_thread_local_data_key();
    }

    // ensure that runtime version matches compiled version
    check_python_version();

    // Initialize thread-local state tracking to match Python's state
    // This must be done before creating any QorePythonProgram instances
#ifdef Py_GIL_DISABLED
    // In free-threading mode, ensure main thread state is attached before any Python API calls
    mainThreadState = PyThreadState_Get();
    //printd(5, "python_module_init_intern() mainThreadState: %p current: %p\n",
    //    mainThreadState, PyGILState_GetThisThreadState());
    if (!PyGILState_GetThisThreadState()) {
        PyThreadState_Swap(mainThreadState);
    }
#else
    // In GIL mode, PyGILState_GetThisThreadState() might return NULL during early init
    // even though we have the GIL. Use PyThreadState_Get() which works when we have the GIL.
    PyThreadState* init_tstate = PyGILState_GetThisThreadState();
    if (!init_tstate) {
        // TSS not set up yet - get the actual thread state and set it up
        init_tstate = PyThreadState_Get();
        // Also update mainThreadState for later use
        mainThreadState = init_tstate;
    }
    _qore_PyGILState_SetThisThreadState(init_tstate);
#endif

    if (init_global_qore_python_pgm()) {
        throw QoreStandardException("PYTHON-MODULE-ERROR", "failed to initialize \"python\" module");
    }

#ifdef Py_GIL_DISABLED
    // In free-threading mode, use PyGILState_Ensure to properly set up the thread for Python ops
    // This ensures the mimalloc heap is properly initialized for this thread
    PyGILState_STATE gstate = PyGILState_Ensure();
    //printd(5, "python_module_init_intern() after PyGILState_Ensure: current: %p gstate: %d\n",
    //    PyGILState_GetThisThreadState(), (int)gstate);
#endif

    if (QorePythonProgram::staticInit() || QorePythonStackLocationHelper::staticInit()) {
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

#ifndef Py_GIL_DISABLED
    mainThreadState = PyThreadState_Get();
    if (python_initialized) {
#if PY_VERSION_HEX >= 0x030D0000
        // Python 3.13+ changed thread state management significantly
        // Use PyEval_ReleaseThread which properly clears both TSS and fast TLS
        printd(5, "python_module_init: before release, mainThreadState: %p TSS: %p GIL: %d\n",
            mainThreadState, PyGILState_GetThisThreadState(), PyGILState_Check());
        PyEval_ReleaseThread(mainThreadState);
        printd(5, "python_module_init: after release, TSS: %p GIL: %d\n",
            PyGILState_GetThisThreadState(), PyGILState_Check());
        _qore_PyGILState_SetThisThreadState(nullptr);
#else
        // release the current thread state after initialization
        _qore_release_thread_state(mainThreadState);
        // Our tracking should be cleared by _qore_release_thread_state
        assert(!_qore_PyRuntimeGILState_GetThreadState());
        _qore_PyGILState_SetThisThreadState(nullptr);
        // NOTE: In Python 3.12, PyEval_ReleaseThread does NOT clear PyGILState_GetThisThreadState()
        // because it doesn't update the autoTSSkey. This is different from earlier Python versions.
        // We only check haveGil() which uses our own tracking.
        assert(!QorePythonProgram::haveGil());
#endif
    }
#else
    // In free-threading mode, don't release the thread state after initialization
    // We keep the main thread state attached for Python operations
#endif

    if (!repeat) {
        tclist.push(QorePythonProgram::pythonThreadCleanup, nullptr);
    }

    return nullptr;
}

static void python_module_ns_init(QoreNamespace* rns, QoreNamespace* qns) {
    QoreProgram* pgm = getProgram();
    assert(pgm->getRootNS() == rns);
    if (!pgm->getExternalData(QORE_PYTHON_MODULE_NAME)) {
        QoreNamespace* pyns = PNS->copy();
        rns->addNamespace(pyns);
        // issue #4153: in case we only have the calling context here
        ExceptionSink xsink;
        QoreExternalProgramContextHelper pch(&xsink, pgm);
        if (!xsink) {
            pgm->setExternalData(QORE_PYTHON_MODULE_NAME, new QorePythonProgram(pgm, pyns));
        }
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

    QoreProgram* pgm = getProgram();
    QorePythonProgram* pypgm = static_cast<QorePythonProgram*>(pgm->getExternalData(QORE_PYTHON_MODULE_NAME));
    //printd(5, "parse-cmd '%s' pypgm: %p pythonns: %p\n", arg.c_str(), pypgm, pypgm->getPythonNamespace());
    if (!pypgm) {
        QoreNamespace* pyns = PNS->copy();
        pgm->getRootNS()->addNamespace(pyns);
        pypgm = new QorePythonProgram(pgm, pyns);
        pgm->setExternalData(QORE_PYTHON_MODULE_NAME, pypgm);
        pgm->addFeature(QORE_PYTHON_MODULE_NAME);
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
        pypgm = new QorePythonProgram(pgm, pyns);
        pgm->setExternalData(QORE_PYTHON_MODULE_NAME, pypgm);
        pgm->addFeature(QORE_PYTHON_MODULE_NAME);
    }
    // the following call adds the class to the current program as well
    QorePythonHelper qph(pypgm, xsink);
    if (qph.wasInterrupted()) {
        return -1;
    }
    return pypgm->import(xsink, module, symbol);
}

QorePythonHelper::QorePythonHelper(const QorePythonProgram* pypgm, ExceptionSink* xsink)
        : old_pgm(q_swap_thread_local_data(python_u_tld_key, (void*)pypgm)),
            old_state(pypgm->setContext(xsink != nullptr)), new_pypgm(pypgm) {
    //printd(5, "QorePythonHelper::QorePythonHelper() new: %p old: %p\n", pypgm, old_pgm);
    if (xsink && wasInterrupted()) {
        xsink->raiseException("PROGRAM-INTERRUPTED",
            "program execution was interrupted while acquiring the Python GIL");
    }
}

QorePythonHelper::~QorePythonHelper() {
    new_pypgm->releaseContext(old_state);
    q_swap_thread_local_data(python_u_tld_key, (void*)old_pgm);
}

bool QorePythonHelper::isValid() const {
    return old_state.valid || !new_pypgm->isValid();
}

bool QorePythonHelper::wasInterrupted() const {
    return !old_state.valid && new_pypgm->isValid();
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

QorePythonGilHelper::QorePythonGilHelper(PyThreadState* new_thread_state)
    : new_thread_state(new_thread_state), state(_qore_PyRuntimeGILState_GetThreadState()),
        t_state(PyGILState_GetThisThreadState()),
        release_gil(!_qore_has_gil(t_state, new_thread_state)) {
    assert(new_thread_state);
#ifdef Py_GIL_DISABLED
    // In free-threading mode, use PyGILState_Ensure to properly
    // initialize the thread and ensure a valid thread state is attached.
    gstate = PyGILState_Ensure();
    // Now we have a thread state attached
    t_state = PyGILState_GetThisThreadState();
    if (t_state != new_thread_state) {
        PyThreadState_Swap(new_thread_state);
    }
    _qore_PyGILState_SetThisThreadState(new_thread_state);
    _QORE_GILSTATE_COUNTER_INC(new_thread_state);
#elif PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ GIL mode - use PyEval_AcquireThread/ReleaseThread which properly
    // handle TSS and fast TLS synchronization
    printd(5, "QorePythonGilHelper ctor: release_gil: %d t_state: %p new_thread_state: %p\n",
        release_gil, t_state, new_thread_state);
    if (release_gil) {
        // Need to acquire the GIL with our specific thread state
        PyEval_AcquireThread(new_thread_state);
    } else {
        // Already have the GIL - swap to our thread state if needed
        if (t_state != new_thread_state) {
            printd(5, "QorePythonGilHelper ctor: swapping from %p to %p\n", t_state, new_thread_state);
            PyThreadState_Swap(new_thread_state);
        }
    }
    _QORE_GILSTATE_COUNTER_INC(new_thread_state);
    _qore_PyGILState_SetThisThreadState(new_thread_state);
#else
    if (release_gil) {
        _qore_acquire_thread_state(new_thread_state);
        assert(PyThreadState_Get() == new_thread_state);
    } else {
        // NOTE: In Python 3.12, t_state (from PyGILState_GetThisThreadState()) may be stale
        // after PyEval_ReleaseThread() since Python's TSS isn't cleared.
        // _qore_has_gil() verified we have the GIL with either t_state or new_thread_state,
        // so check against both possibilities.
        PyThreadState* ceval_ts = _qore_PyCeval_GetThreadState();
        assert(ceval_ts == t_state || ceval_ts == new_thread_state);
    }
    // NOTE: even if the current thread state is equal to the new one, we still need to set all thread states in all
    // locations

    _QORE_GILSTATE_COUNTER_INC(new_thread_state);
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
}

QorePythonGilHelper::~QorePythonGilHelper() {
#ifdef Py_GIL_DISABLED
    if (gstate_released) {
        // gstate was released in releaseBeforeSubInterpreter() for sub-interpreter creation.
        // The sub-interpreter now owns the thread state - we don't restore or release anything.
        // When the sub-interpreter is destroyed, it will clean up its own thread state.
        return;
    }
    // First swap back to the original thread state if needed
    PyThreadState* current = PyGILState_GetThisThreadState();
    if (current && current != t_state && t_state) {
        PyThreadState_Swap(t_state);
    }
    // Decrement counter before release
    _QORE_GILSTATE_COUNTER_DEC(new_thread_state);
    // Release the GIL state
    PyGILState_Release(gstate);
    _qore_PyGILState_SetThisThreadState(nullptr);
#elif PY_VERSION_HEX >= 0x030D0000
    // Python 3.13+ GIL mode
    printd(5, "QorePythonGilHelper dtor: release_gil: %d t_state: %p new_thread_state: %p\n",
        release_gil, t_state, new_thread_state);
    _QORE_GILSTATE_COUNTER_DEC(new_thread_state);

    if (release_gil) {
        // We acquired the GIL, so release it
        // Use PyEval_SaveThread which properly releases the GIL
        PyEval_SaveThread();
        _qore_PyGILState_SetThisThreadState(nullptr);
    } else {
        // We already had the GIL - restore the original tracking
        _qore_PyGILState_SetThisThreadState(t_state);
    }
#else
    _QORE_GILSTATE_COUNTER_DEC(new_thread_state);

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
        _qore_release_thread_state(new_thread_state);
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
    // Prepare for sub-interpreter creation in free-threading mode.
    // In free-threading mode with mimalloc, each thread state has thread-local heap data.
    // PyGILState_Ensure() set up heap data for the main interpreter. Before creating a
    // sub-interpreter, we need to detach so Py_NewInterpreterFromConfig can properly
    // initialize heap data for the new interpreter.

    // Swap to NULL thread state to detach from main interpreter's thread state.
    // This allows Py_NewInterpreterFromConfig to properly attach a new thread state
    // with correctly initialized mimalloc heap for the sub-interpreter.
    PyThreadState_Swap(nullptr);
    gstate_released = true;
}
#endif

void QorePythonGilHelper::set(PyThreadState* other_state) {
    // as this is called after creating a new interpreter, we cannot assert that we hold the GIL here
#ifdef Py_GIL_DISABLED
    // In free-threading mode, Py_NewInterpreterFromConfig already attached the new thread state.
    // Just update our tracking variable so the destructor restores correctly.
    // Do NOT call PyThreadState_Swap again as it may corrupt the thread's heap state.
    new_thread_state = other_state;
#else
    assert(_qore_PyCeval_GetGilLockedStatus() && _qore_PyCeval_GetThreadState());

    // Update new_thread_state so the destructor releases the correct thread state
    // This is critical when called after Py_NewInterpreter() which creates a new thread state
    new_thread_state = other_state;

    _QORE_PYTHREAD_STATE_SWAP(other_state);
    _qore_PyCeval_SwapThreadState(other_state);
    _qore_PyGILState_SetThisThreadState(other_state);
#endif
}
