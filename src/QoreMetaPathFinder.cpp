/* indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2020 - 2021 Qore Technologies, s.r.o.

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

#include "QoreMetaPathFinder.h"
#include "QoreLoader.h"
#include "JavaLoader.h"
#include "QorePythonProgram.h"

QorePythonManualReferenceHolder QoreMetaPathFinder::qore_package;
QorePythonManualReferenceHolder QoreMetaPathFinder::java_package;
QorePythonManualReferenceHolder QoreMetaPathFinder::mod_spec_cls;

PyDoc_STRVAR(QoreMetaPathFinder_doc,
"QoreMetaPathFinder()\n\
\n\
Creates Python wrappers for Qore code.");

static PyMethodDef QoreMetaPathFinder_methods[] = {
    {"find_spec", QoreMetaPathFinder::find_spec, METH_VARARGS, "QoreMetaPathFinder.find_spec() implementation"},
    {nullptr, nullptr},
};

PyTypeObject QoreMetaPathFinder_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "QoreMetaPathFinder",         // tp_name
    0,                            // tp_basicsize
    0,                            // tp_itemsize
    // Slots
    (destructor)QoreMetaPathFinder::dealloc,  // tp_dealloc
    0,                            // tp_vectorcall_offset
    0,                            // tp_getattr
    0,                            // tp_setattr
    0,                            // tp_as_async
    QoreMetaPathFinder::repr,     // tp_repr
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
    QoreMetaPathFinder_doc,       // tp_doc
    0,                            // tp_traverse
    0,                            // tp_clear
    0,                            // tp_richcompare
    0,                            // tp_weaklistoffset
    0,                            // tp_iter
    0,                            // tp_iternext
    QoreMetaPathFinder_methods,   // tp_methods
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

int QoreMetaPathFinder::init() {
    if (PyType_Ready(&QoreMetaPathFinder_Type) < 0) {
        printd(5, "QoreMetaPathFinder::init() type initialization failed\n");
        return -1;
    }

    // get importlib.machinery.ModuleSpec class
    QorePythonReferenceHolder mod(PyImport_ImportModule("importlib.machinery"));
    if (!*mod) {
        printd(5, "QoreMetaPathFinder::init() ERROR: no importlib.machinery module\n");
        return -1;
    }

    if (!PyObject_HasAttrString(*mod, "ModuleSpec")) {
        printd(5, "QoreMetaPathFinder::init() ERROR: no ModuleSpec class in importlib.machinery\n");
        return -1;
    }

    mod_spec_cls = PyObject_GetAttrString(*mod, "ModuleSpec");
    printd(5, "mod_spec_cls: %p %s\n", *mod_spec_cls, Py_TYPE(*mod_spec_cls)->tp_name);

    return 0;
}

int QoreMetaPathFinder::setupModules() {
    QorePythonReferenceHolder mpf(PyObject_CallObject((PyObject*)&QoreMetaPathFinder_Type, nullptr));
    printd(5, "QoreMetaPathFinder::setupModules() created finder %p\n", *mpf);

    // get sys module
    QorePythonReferenceHolder mod(PyImport_ImportModule("sys"));
    if (!*mod) {
        printd(5, "QoreMetaPathFinder::setupModules() ERROR: no sys module\n");
        return -1;
    }

    if (!PyObject_HasAttrString(*mod, "meta_path")) {
        printd(5, "QoreMetaPathFinder::setupModules() ERROR: no sys.meta_path\n");
        return -1;
    }

    QorePythonReferenceHolder meta_path(PyObject_GetAttrString(*mod, "meta_path"));
    printd(5, "meta_path: %p %s\n", *meta_path, Py_TYPE(*meta_path)->tp_name);
    if (!PyList_Check(*meta_path)) {
        printd(5, "QoreMetaPathFinder::setupModules() ERROR: sys.meta_path is not a list\n");
        return -1;
    }

    // insert object
    if (PyList_Append(*meta_path, *mpf)) {
        printd(5, "QoreMetaPathFinder::setupModules() ERROR: failed to append finder to sys.meta_path\n");
        return -1;
    }

    return 0;
}

void QoreMetaPathFinder::del() {
    qore_package.release();
    java_package.release();
    mod_spec_cls.purge();
}

void QoreMetaPathFinder::dealloc(PyObject* self) {
    //PyObject_GC_UnTrack(self);
    Py_TYPE(self)->tp_free(self);
}

PyObject* QoreMetaPathFinder::repr(PyObject* obj) {
    QoreStringMaker str("QoreMetaPathFinder object %p", obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

// class method functions
PyObject* QoreMetaPathFinder::find_spec(PyObject* self, PyObject* args) {
#ifdef _QORE_PYTHON_DEBUG_FIND_SPEC_ARGS
    {
        // show args
        QorePythonReferenceHolder argstr(PyObject_Repr(args));
        assert(PyUnicode_Check(*argstr));
        printd(0, "QoreMetaPathFinder::find_spec() args: %s\n", PyUnicode_AsUTF8(*argstr));
    }
#endif

    // returns a borrowed reference
    PyObject* fullname = PyTuple_GetItem(args, 0);
    assert(PyUnicode_Check(fullname));
    const char* fname = PyUnicode_AsUTF8(fullname);
    PyObject* path = PyTuple_GetItem(args, 1);

    // return qore top-level package module spec
    if (path == Py_None) {
        if (!strcmp(fname, "qore")) {
            PyObject* rv = getQorePackageModuleSpec();
            if (rv) {
                return rv;
            }
        } else if (!strcmp(fname, "java")) {
            PyObject* rv = getJavaPackageModuleSpec();
            if (rv) {
                return rv;
            }
        }
    } else {
        QoreString mname(fname);
        if (mname.size() > 5 && mname.equalPartial("qore.")) {
            //mname.replace(0, 5, (const char*)nullptr);
            PyObject* rv = tryLoadModule(mname, mname.c_str() + 5);
            if (rv) {
                return rv;
            }
        } else if (mname.size() > 5 && mname.equalPartial("java.")) {
            //mname.replace(0, 5, (const char*)nullptr);
            PyObject* rv = getJavaNamespaceModule(mname, mname.c_str() + 5);
            if (rv) {
                return rv;
            }
        }
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* QoreMetaPathFinder::newModuleSpec(bool qore, const QoreString& name, PyObject* loader) {
    // create args for ModuleSpec constructor
    QorePythonReferenceHolder args(PyTuple_New(2));
    PyTuple_SET_ITEM(*args, 0, PyUnicode_FromStringAndSize(name.c_str(), name.size()));

    if (!loader) {
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(*args, 1, Py_None);
    } else {
        PyTuple_SET_ITEM(*args, 1, loader);
    }

    QorePythonReferenceHolder kwargs(PyDict_New());
    Py_INCREF(Py_True);
    PyDict_SetItemString(*kwargs, "is_package", Py_True);
    QorePythonReferenceHolder mod_spec(PyObject_Call((PyObject*)*mod_spec_cls, *args, *kwargs));

    PyObject_SetAttrString(*mod_spec, "loader", qore ? QoreLoader::getLoader() : JavaLoader::getLoader());

    assert(mod_spec);
    return mod_spec.release();
}

PyObject* QoreMetaPathFinder::getQorePackageModuleSpec() {
    if (!qore_package) {
        // create qore package
        qore_package = newModuleSpec(true, "qore");

        QorePythonReferenceHolder search_locations(PyList_New(0));
        PyObject_SetAttrString(*qore_package, "submodule_search_locations", *search_locations);
    }

    qore_package.py_ref();
    //printd(5, "QoreMetaPathFinder::getQorePackageModuleSpec() returning qore_package: %p\n", *qore_package);
    return *qore_package;
}

PyObject* QoreMetaPathFinder::getJavaPackageModuleSpec() {
    if (!java_package) {
        // create java package
        java_package = newModuleSpec(false, "java");

        QorePythonReferenceHolder search_locations(PyList_New(0));
        PyObject_SetAttrString(*java_package, "submodule_search_locations", *search_locations);
    }

    java_package.py_ref();
    //printd(5, "QoreMetaPathFinder::getJavaPackageModuleSpec() returning java_package: %p\n", *java_package);
    return *java_package;
}

PyObject* QoreMetaPathFinder::getQoreRootModuleSpec(const QoreString& mname) {
    QorePythonReferenceHolder mod_spec(newModuleSpec(true, mname, QoreLoader::getLoaderRef()));

    QorePythonReferenceHolder search_locations(PyList_New(0));
    // add namespaces as submodule search locations (NOTE: not functionally necessary it seems)
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    const RootQoreNamespace* rns = qore_python_pgm->getQoreProgram()->getRootNS();
    QoreNamespaceNamespaceIterator ni(*rns);
    while (ni.next()) {
        const QoreNamespace& ns = ni.get();
        QoreStringMaker mod_name(ns.getName());
        //printd(5, "QoreMetaPathFinder::getQoreRootModuleSpec(): adding '%s'\n", mod_name.c_str());
        QorePythonReferenceHolder name(PyUnicode_FromStringAndSize(mod_name.c_str(), mod_name.size()));
        PyList_Append(*search_locations, *name);
    }
    PyObject_SetAttrString(*mod_spec, "submodule_search_locations", *search_locations);

    return mod_spec.release();
}

PyObject* QoreMetaPathFinder::tryLoadModule(const QoreString& full_name, const char* mod_name) {
    //printd(5, "QoreMetaPathFinder::tryLoadModule() load '%s' (%s)\n", full_name.c_str(), mod_name);
    if (!strcmp(mod_name, "__root__")) {
        return getQoreRootModuleSpec(full_name);
    }

    // Qore module names do not contain dots; dotted names (e.g. "DataProvider.AbstractDataProcessor") are Python
    // sub-package lookups for classes/objects that should already be attributes of the parent module.
    // runTimeLoadModule() would misinterpret the dot as a path separator and incorrectly match the parent module.
    if (strchr(mod_name, '.')) {
        return nullptr;
    }

    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    ExceptionSink xsink;
    if (ModuleManager::runTimeLoadModule(mod_name, qore_python_pgm->getQoreProgram(), &xsink)) {
#ifdef _QORE_PYTHON_DEBUG_MODULE_ERRORS
        // the exception message is lost, to get it for debugging purposes, enable this block
        QoreStringValueHelper err(xsink.getExceptionErr());
        QoreStringValueHelper desc(xsink.getExceptionDesc());
        printf("%s: %s\n", err->c_str(), desc->c_str());
#endif
        // ignore exceptions and continue
        xsink.clear();
        return nullptr;
    }
    assert(!xsink);

    return newModuleSpec(true, full_name, QoreLoader::getLoaderRef());
}

PyObject* QoreMetaPathFinder::getJavaNamespaceModule(const QoreString& full_name, const char* mod_name) {
    printd(5, "QoreMetaPathFinder::getJavaNamespaceModule() load '%s' (%s)\n", full_name.c_str(), mod_name);
    return newModuleSpec(false, full_name, JavaLoader::getLoaderRef());
}
