/* indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2020 - 2022 Qore Technologies, s.r.o.

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

#include "QoreLoader.h"

#include <algorithm>
#include "QoreMetaPathFinder.h"
#include "QorePythonProgram.h"
#include "PythonQoreClass.h"

#include <memory>

QorePythonManualReferenceHolder QoreLoader::loader_cls;
QorePythonManualReferenceHolder QoreLoader::loader;

static bool moduleNameMatches(const char* item_mod, const char* name) {
    return item_mod && !strcmp(item_mod, name);
}

static bool namespaceHasDirectItemFromModule(const QoreNamespace& ns, const char* name) {
    QoreNamespaceFunctionIterator fi(ns);
    while (fi.next()) {
        if (moduleNameMatches(fi.get().getModuleName(), name)) {
            return true;
        }
    }

    QoreNamespaceConstantIterator ci(ns);
    while (ci.next()) {
        if (moduleNameMatches(ci.get().getModuleName(), name)) {
            return true;
        }
    }

    QoreNamespaceClassIterator class_i(ns);
    while (class_i.next()) {
        if (moduleNameMatches(class_i.get().getModuleName(), name)) {
            return true;
        }
    }

    QoreNamespaceTypedHashIterator hash_i(ns);
    while (hash_i.next()) {
        if (moduleNameMatches(hash_i.get().getModuleName(), name)) {
            return true;
        }
    }

    QoreNamespaceEnumIterator enum_i(ns);
    while (enum_i.next()) {
        if (moduleNameMatches(enum_i.get().getModuleName(), name)) {
            return true;
        }
    }

    return false;
}

static PyMethodDef QoreLoader_methods[] = {
    {"create_module", QoreLoader::create_module, METH_VARARGS, "QoreLoader.create_module() implementation"},
    {"exec_module", QoreLoader::exec_module, METH_VARARGS, "QoreLoader.exec_module() implementation"},
    {nullptr, nullptr},
};

PyDoc_STRVAR(QoreLoader_doc,
"QoreLoader()\n\
\n\
Python modules for Qore code.");

PyTypeObject QoreLoader_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "QoreLoader",                 // tp_name
    0,                            // tp_basicsize
    0,                            // tp_itemsize
    // Slots
    (destructor)QoreLoader::dealloc,  // tp_dealloc
    0,                            // tp_vectorcall_offset
    0,                            // tp_getattr
    0,                            // tp_setattr
    0,                            // tp_as_async
    QoreLoader::repr,             // tp_repr
    0,                            // tp_as_number
    0,                            // tp_as_sequence
    0,                            // tp_as_mapping
    0,                            // tp_hash
    0,                            // tp_call
    0,                            // tp_str
    PyObject_GenericGetAttr,      // tp_getattro
    0,                            // tp_setattro
    0,                            // tp_as_buffer
    Py_TPFLAGS_DEFAULT,           // tp_flags
    QoreLoader_doc,               // tp_doc
    0,                            // tp_traverse
    0,                            // tp_clear
    0,                            // tp_richcompare
    0,                            // tp_weaklistoffset
    0,                            // tp_iter
    0,                            // tp_iternext
    QoreLoader_methods,           // tp_methods
    0,                            // tp_members
    0,                            // tp_getset
    &PyBaseObject_Type,           // tp_base
    0,                            // tp_dict
    0,                            // tp_descr_get
    0,                            // tp_descr_set
    0,                            // tp_dictoffset
    0,                            // tp_init
    PyType_GenericAlloc,          // tp_alloc
    PyType_GenericNew,            // tp_new
    PyObject_Del,                 // tp_free
};

int QoreLoader::init() {
    if (PyType_Ready(&QoreLoader_Type) < 0) {
        printd(5, "QoreLoader::init() type initialization failed\n");
        return -1;
    }

    QorePythonReferenceHolder args(PyTuple_New(0));
    loader = PyObject_CallObject((PyObject*)&QoreLoader_Type, *args);
    //loader = PyObject_CallObject((PyObject*)*loader_cls, *args);

    if (PyType_Ready(&PythonQoreException_Type) < 0) {
        return -1;
    }

    return 0;
}

void QoreLoader::del() {
    loader.purge();
    loader_cls.purge();
}

PyObject* QoreLoader::getLoaderRef() {
    assert(*loader);
    Py_INCREF(*loader);
    return *loader;
}

PyObject* QoreLoader::getLoader() {
    assert(*loader);
    return *loader;
}

void QoreLoader::dealloc(PyObject* self) {
    //PyObject_GC_UnTrack(self);
    Py_TYPE(self)->tp_free(self);
}

PyObject* QoreLoader::repr(PyObject* obj) {
    QoreStringMaker str("QoreLoader object %p", obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

// class method functions
PyObject* QoreLoader::create_module(PyObject* self, PyObject* args) {
    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* QoreLoader::exec_module(PyObject* self, PyObject* args) {
    assert(PyTuple_Check(args));
#ifdef _QORE_PYTHON_DEBUG_EXEC_MODULE_ARGS
    QorePythonReferenceHolder argstr(PyObject_Repr(args));
    assert(PyUnicode_Check(*argstr));
    printd(5, "QoreLoader::exec_module() self: %p args: %s\n", self, PyUnicode_AsUTF8(*argstr));
#endif

    // get module
    // returns a borrowed reference
    PyObject* mod = PyTuple_GetItem(args, 0);
    assert(PyModule_Check(mod));

    // get module name
    assert(PyObject_HasAttrString(mod, "__name__"));
    QorePythonReferenceHolder name(PyObject_GetAttrString(mod, "__name__"));
    assert(PyUnicode_Check(*name));
    const char* orig_name_str = PyUnicode_AsUTF8(*name);
    const char* name_str = orig_name_str;

    printd(5, "QoreLoader::exec_module() mod: '%s'\n", name_str);
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    QoreProgram* mod_pgm = qore_python_pgm->getQoreProgram();
    printd(5, "QoreLoader::exec_module() qore_python_pgm: %p mod pgm: %p\n", qore_python_pgm, mod_pgm);

    // get the root namespace(s) backing the Python module; a Qore module can declare items in more than one root
    // namespace, in which case all of them have to be imported, otherwise the items the module declares in the
    // namespaces that are left out are not visible in Python at all
    nsvec_t nsvec;
    if (!strcmp(name_str, "qore")) {
        nsvec.push_back(mod_pgm->getQoreNS());
    } else {
        assert(!strncmp(name_str, "qore.", 5));
        name_str += 5;
        if (!strchr(name_str, '.')) {
            if (!strcmp(name_str, "__root__")) {
                nsvec.push_back(mod_pgm->getRootNS());
            } else {
                getModuleRootNsList(name_str, mod_pgm, nsvec);
            }
        }
    }

    if (!nsvec.empty()) {
        QoreProgramContextHelper pch(mod_pgm);
        for (const QoreNamespace* ns : nsvec) {
            printd(5, "QoreLoader::exec_module() found '%s' NS %p: '::%s'\n", name_str, ns, ns->getName());
            qore_python_pgm->importQoreToPython(mod, *ns, name_str);
            if (PyErr_Occurred()) {
                return nullptr;
            }
        }
        // the primary namespace gives the module its path
        std::string nspath = nsvec[0]->getPath();
        QorePythonReferenceHolder py_path(PyUnicode_FromStringAndSize(nspath.c_str(), nspath.size()));
        if (PyObject_SetAttrString(mod, "__path__", *py_path)) {
            return nullptr;
        }
    } else {
        QoreStringMaker desc("cannot find Qore namespace for Python module '%s'", orig_name_str);
        PyErr_SetString(PyExc_NameError, desc.c_str());
        return nullptr;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

void QoreLoader::getModuleRootNsList(const char* name, QoreProgram* mod_pgm, nsvec_t& nsvec) {
    ReferenceHolder<QoreHashNode> all_mod_info(MM.getModuleHash(), nullptr);
    mod_dep_map_t mod_dep_map;

    // otherwise look for a public namespace and then find the ealiest ancestor provided by the module
    const QoreNamespace* root_ns = mod_pgm->getRootNS();
    getModuleRootNsListIntern(name, *root_ns, *all_mod_info, mod_dep_map, true, nsvec);
    if (nsvec.empty()) {
        getModuleRootNsListIntern(name, *root_ns, *all_mod_info, mod_dep_map, false, nsvec);
    }
}

// Returns true when the namespace is directly owned by the named module — either it was added
// as a contributor (isFromModule) or it holds at least one direct item declared by the module.
static bool namespaceMatchesModuleDirect(const QoreNamespace* parent, const char* name) {
    if (!parent || parent->isRoot()) {
        return false;
    }
    return parent->isFromModule(name) || namespaceHasDirectItemFromModule(*parent, name);
}

// Returns true when the namespace is matched to the named module *only* via the module's
// reexport list — i.e. one of the module's reexported dependencies owns the namespace.  Kept
// separate from the direct check so callers can prefer direct-ownership candidates and avoid
// rooting an importing module's Python package at a reexported dependency's namespace.
static bool namespaceMatchesModuleViaReexport(const QoreNamespace* parent, const char* name,
        const QoreHashNode* all_mod_info, mod_dep_map_t& mod_dep_map) {
    if (!parent || parent->isRoot() || !all_mod_info) {
        return false;
    }

    const char* mod = parent->getModuleName();
    const QoreListNode* reexport_list = nullptr;

    // see if we have the reexport list already
    mod_dep_map_t::iterator i;
    if (!mod || (i = mod_dep_map.lower_bound(mod)) == mod_dep_map.end() || strcmp(i->first, mod)) {
        const QoreHashNode* mod_info = all_mod_info->getKeyValue(name).get<QoreHashNode>();
        if (!mod_info) {
            return false;
        }
        reexport_list = mod_info->getKeyValue("reexported-modules").get<QoreListNode>();
        if (!reexport_list) {
            return false;
        }
        if (mod) {
            mod_dep_map.insert(i, mod_dep_map_t::value_type(mod, reexport_list));
        }
    } else {
        reexport_list = i->second;
    }

    ConstListIterator li(reexport_list);
    while (li.next()) {
        const QoreValue v = li.getValue();
        if (v.getType() == NT_STRING) {
            QoreStringValueHelper str(v);
            if (parent->isFromModule(str->c_str()) || namespaceHasDirectItemFromModule(*parent, str->c_str())) {
                return true;
            }
        }
    }

    return false;
}

void QoreLoader::getModuleRootNsListIntern(const char* name, const QoreNamespace& root_ns,
        const QoreHashNode* all_mod_info, mod_dep_map_t& mod_dep_map, bool check_mod, nsvec_t& nsvec) {
    // A Qore module can declare items in any number of root namespaces (for example QorusClientCore declares
    // classes in both ::OMQ and ::RemoteDevelopment), so every namespace that directly owns items declared by
    // the module is collected; returning only one of them would leave everything the module declares in the
    // others invisible to Python, and which one was returned depended on namespace iteration order.
    //
    // Namespaces matched only through the module's reexport list are a fallback: a namespace that owns items
    // declared by `name` is always a better root than a namespace that merely belongs to a module reexported by
    // `name` — otherwise a Python "from qore.X import Y" for a module X that reexports dependency D would pick
    // D's namespace as X's root and hide X's own items (e.g. QorusClientBase reexports HttpClientIo, so
    // ::HttpClientIo would wrongly displace ::OMQ as the root for "qore.QorusClientBase").
    const QoreNamespace* exact_candidate = nullptr;
    const QoreNamespace* reexport_candidate = nullptr;
    QoreNamespaceConstIterator i(root_ns);
    while (i.next()) {
        const QoreNamespace* ns = &i.get();
        if (!check_mod) {
            if (!strcmp(ns->getName(), name)) {
                nsvec.push_back(ns);
                return;
            }
            continue;
        }

        if (!isModule(ns, name, all_mod_info, mod_dep_map)) {
            continue;
        }
        printd(5, "QoreLoader::getModuleRootNsList('%s') found '%s' (%p)\n", name, ns->getPath().c_str(), ns);
        // try to find parent ns
        while (true) {
            const QoreNamespace* parent = ns->getParent();
            if (!isModule(parent, name, all_mod_info, mod_dep_map)) {
                printd(5, "QoreLoader::getModuleRootNsList('%s') invalid parent '%s'\n", name,
                    parent->getPath().c_str());
                break;
            }
            ns = parent;
            printd(5, "QoreLoader::getModuleRootNsList('%s') got parent '%s'\n", name, ns->getPath().c_str());
        }

        if (!namespaceMatchesModuleDirect(ns, name)) {
            if (!reexport_candidate) {
                reexport_candidate = ns;
            }
            continue;
        }

        // the parent walk above can map several namespaces to the same ancestor
        if (std::find(nsvec.begin(), nsvec.end(), ns) != nsvec.end()) {
            continue;
        }
        printd(5, "QoreLoader::getModuleRootNsList('%s') adding direct namespace '%s'\n", name,
            ns->getPath().c_str());
        nsvec.push_back(ns);
        // a namespace with the module's own name is the module's primary namespace
        if (!exact_candidate && !strcmp(ns->getName(), name)) {
            exact_candidate = ns;
        }
    }

    if (nsvec.empty()) {
        if (reexport_candidate) {
            printd(5, "QoreLoader::getModuleRootNsList('%s') returning reexport candidate '%s'\n", name,
                reexport_candidate->getPath().c_str());
            nsvec.push_back(reexport_candidate);
        }
        return;
    }

    // the module's primary namespace gives the Python module its path, so it must come first
    if (exact_candidate && nsvec[0] != exact_candidate) {
        nsvec.erase(std::find(nsvec.begin(), nsvec.end(), exact_candidate));
        nsvec.insert(nsvec.begin(), exact_candidate);
    }
}

bool QoreLoader::isModule(const QoreNamespace* parent, const char* name, const QoreHashNode* all_mod_info,
        mod_dep_map_t& mod_dep_map) {
    return namespaceMatchesModuleDirect(parent, name)
        || namespaceMatchesModuleViaReexport(parent, name, all_mod_info, mod_dep_map);
}
