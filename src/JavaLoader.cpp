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

#include "JavaLoader.h"
#include "QoreMetaPathFinder.h"
#include "QorePythonProgram.h"
#include "PythonQoreClass.h"
#include "ModuleNamespace.h"

#include <dlfcn.h>

#include <memory>

QorePythonManualReferenceHolder JavaLoader::loader_cls;
QorePythonManualReferenceHolder JavaLoader::loader;

static PyMethodDef JavaLoader_methods[] = {
    {"create_module", JavaLoader::create_module, METH_VARARGS, "JavaLoader.create_module() implementation"},
    {"exec_module", JavaLoader::exec_module, METH_VARARGS, "JavaLoader.exec_module() implementation"},
    {nullptr, nullptr},
};

PyDoc_STRVAR(JavaLoader_doc,
"JavaLoader()\n\
\n\
Python modules for Java through the Qore library.");

PyTypeObject JavaLoader_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "JavaLoader",                 // tp_name
    0,                            // tp_basicsize
    0,                            // tp_itemsize
    // Slots
    (destructor)JavaLoader::dealloc,  // tp_dealloc
    0,                            // tp_vectorcall_offset
    0,                            // tp_getattr
    0,                            // tp_setattr
    0,                            // tp_as_async
    JavaLoader::repr,             // tp_repr
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
    JavaLoader_doc,               // tp_doc
    0,                            // tp_traverse
    0,                            // tp_clear
    0,                            // tp_richcompare
    0,                            // tp_weaklistoffset
    0,                            // tp_iter
    0,                            // tp_iternext
    JavaLoader_methods,           // tp_methods
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

int JavaLoader::init() {
    if (initModuleNamespace()) {
        return -1;
    }

    if (PyType_Ready(&JavaLoader_Type) < 0) {
        printd(5, "JavaLoader::init() type initialization failed\n");
        return -1;
    }

    QorePythonReferenceHolder args(PyTuple_New(0));
    loader = PyObject_CallObject((PyObject*)&JavaLoader_Type, *args);
    //loader = PyObject_CallObject((PyObject*)*loader_cls, *args);

    if (PyType_Ready(&PythonQoreException_Type) < 0) {
        return -1;
    }

    return 0;
}

void JavaLoader::del() {
    loader.purge();
    loader_cls.purge();
}

PyObject* JavaLoader::getLoaderRef() {
    assert(*loader);
    Py_INCREF(*loader);
    return *loader;
}

PyObject* JavaLoader::getLoader() {
    assert(*loader);
    return *loader;
}

void JavaLoader::dealloc(PyObject* self) {
    //PyObject_GC_UnTrack(self);
    Py_TYPE(self)->tp_free(self);
}

PyObject* JavaLoader::repr(PyObject* obj) {
    QoreStringMaker str("JavaLoader object %p", obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

// class method functions
PyObject* JavaLoader::create_module(PyObject* self, PyObject* args) {
    typedef QoreNamespace* (*jni_module_find_create_java_namespace_t)(QoreString& arg, QoreProgram* pgm);
    static jni_module_find_create_java_namespace_t jni_module_find_create_java_namespace = nullptr;

    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    if (!jni_module_find_create_java_namespace) {
        if (load_jni_module(qore_python_pgm)) {
            return nullptr;
        }

        jni_module_find_create_java_namespace = (jni_module_find_create_java_namespace_t)dlsym(RTLD_DEFAULT,
            "jni_module_find_create_java_namespace");
        if (!jni_module_find_create_java_namespace) {
            PyErr_SetString(PyExc_ValueError, "cannot find required symbol 'jni_module_find_create_java_namespace'");
            return nullptr;
        }
    }

    QorePythonReferenceHolder argstr(PyObject_Repr(args));
    printd(5, "JavaLoader::create_module() self: %p args: %s\n", self, PyUnicode_AsUTF8(*argstr));

    if (!args || !PyTuple_Check(args) || !PyTuple_Size(args)) {
        PyErr_SetString(PyExc_ValueError, "missing ModuleSpec arg to 'JavaLoader.create_module()'");
        return nullptr;
    }
    // returns a borrowed reference
    PyObject* spec = PyTuple_GetItem(args, 0);

    // get name
    QorePythonReferenceHolder name(PyObject_GetAttrString(spec, "name"));
    if (!name || !PyUnicode_Check(*name)) {
        PyErr_SetString(PyExc_ValueError, "ModuleSpec has no 'name' attribute");
        return nullptr;
    }
    QoreString mname(PyUnicode_AsUTF8(*name));
    assert(mname.equalPartial("java"));
    // create new namespace
    [[maybe_unused]] QoreProgram* mod_pgm = qore_python_pgm->getQoreProgram();
    if (mname == "java") {
        return qore_python_pgm->newModule("java", mod_pgm->getRootNS()->findLocalNamespace("Jni"));
    }
    // get the full Python module name
    QoreString py_name = mname;
    // remove "java." from the beginning of the string
    mname.replace(0, 5, "");
    // replace "." with "::"
    mname.replaceAll(".", "::");

    QoreNamespace* ns = jni_module_find_create_java_namespace(mname, mod_pgm);
    if (!ns) {
        QoreStringMaker desc("failed to create Java namespace '%s' in JavaLoader.create_module()", mname.c_str());
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return nullptr;
    }

    printd(5, "JavaLoader::create_module() '%s' ns: %s\n", py_name.c_str(), ns->getName());
    return qore_python_pgm->newModule(py_name.c_str(), ns);
}

PyObject* JavaLoader::exec_module(PyObject* self, PyObject* args) {
    assert(PyTuple_Check(args));
#ifdef _QORE_PYTHON_DEBUG_EXEC_MODULE_ARGS
    QorePythonReferenceHolder argstr(PyObject_Repr(args));
    assert(PyUnicode_Check(*argstr));
    printd(5, "JavaLoader::exec_module() self: %p args: %s\n", self, PyUnicode_AsUTF8(*argstr));
#endif

    // get module
    // returns a borrowed reference
    PyObject* mod = PyTuple_GetItem(args, 0);
    assert(PyModule_Check(mod));

    // get module name
    assert(PyObject_HasAttrString(mod, "__name__"));
    QorePythonReferenceHolder name(PyObject_GetAttrString(mod, "__name__"));
    assert(PyUnicode_Check(*name));
    [[maybe_unused]] const char* name_str = PyUnicode_AsUTF8(*name);

    printd(5, "JavaLoader::exec_module() mod: '%s'\n", name_str);
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    [[maybe_unused]] QoreProgram* mod_pgm = qore_python_pgm->getQoreProgram();
    printd(5, "JavaLoader::exec_module() qore_python_pgm: %p mod pgm: %p\n", qore_python_pgm, mod_pgm);

    if (load_jni_module(qore_python_pgm)) {
        return nullptr;
    }

    Py_INCREF(Py_None);
    return Py_None;
}
