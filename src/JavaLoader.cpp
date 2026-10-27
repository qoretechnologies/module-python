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
#include "QorePythonInterpreterTypes.h"

#include <dlfcn.h>

#include <memory>


static PyMethodDef JavaLoader_methods[] = {
    {"create_module", JavaLoader::create_module, METH_VARARGS, "JavaLoader.create_module() implementation"},
    {"exec_module", JavaLoader::exec_module, METH_VARARGS, "JavaLoader.exec_module() implementation"},
    {nullptr, nullptr},
};

PyDoc_STRVAR(JavaLoader_doc,
"JavaLoader()\n\
\n\
Python modules for Java through the Qore library.");

static PyType_Slot JavaLoader_slots[] = {
    {Py_tp_dealloc, (void*)JavaLoader::dealloc},
    {Py_tp_repr, (void*)JavaLoader::repr},
    {Py_tp_doc, (void*)JavaLoader_doc},
    {Py_tp_methods, (void*)JavaLoader_methods},
    {0, nullptr},
};

static PyType_Spec JavaLoader_spec = {
    .name = "qoreloader.JavaLoader",
    .basicsize = sizeof(PyObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = JavaLoader_slots,
};

PyObject* JavaLoader::createType() {
    return PyType_FromSpec(&JavaLoader_spec);
}

PyObject* JavaLoader::getLoaderRef() {
    PyObject* loader = getLoader();
    Py_XINCREF(loader);
    return loader;
}

PyObject* JavaLoader::getLoader() {
    const QorePythonInterpreterTypes* types = qore_python_get_interpreter_types();
    if (!types) {
        PyErr_SetString(PyExc_RuntimeError, "the qoreloader module is not initialized in the current interpreter");
        return nullptr;
    }
    return types->java_loader_obj;
}

void JavaLoader::dealloc(PyObject* self) {
    // the object of a heap type holds a reference to its type
    PyTypeObject* type = Py_TYPE(self);
    type->tp_free(self);
    Py_DECREF(type);
}

PyObject* JavaLoader::repr(PyObject* obj) {
    QoreStringMaker str("JavaLoader object %p", obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

// class method functions
PyObject* JavaLoader::create_module(PyObject* self, PyObject* args) {
    typedef QoreNamespace* (*jni_module_find_create_java_namespace_t)(QoreString& arg, QoreProgram* pgm);
    static jni_module_find_create_java_namespace_t jni_module_find_create_java_namespace = nullptr;

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
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
    const char* name_str = PyUnicode_AsUTF8(*name);
    if (!name_str) {
        return nullptr;
    }
    if (strcmp(name_str, "java") && strncmp(name_str, "java.", 5)) {
        PyErr_Format(PyExc_ImportError, "JavaLoader.create_module(): module '%s' is not a Java module", name_str);
        return nullptr;
    }
    QoreString mname(name_str);
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
    // exec_module(module); a borrowed reference
    PyObject* mod;
    if (!PyArg_ParseTuple(args, "O!:exec_module", &PyModule_Type, &mod)) {
        return nullptr;
    }
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
#ifdef _QORE_PYTHON_DEBUG_EXEC_MODULE_ARGS
    QorePythonReferenceHolder argstr(PyObject_Repr(args));
    assert(PyUnicode_Check(*argstr));
    printd(5, "JavaLoader::exec_module() self: %p args: %s\n", self, PyUnicode_AsUTF8(*argstr));
#endif

    // get module name
    QorePythonReferenceHolder name(PyObject_GetAttrString(mod, "__name__"));
    if (!name) {
        return nullptr;
    }
    const char* name_str = PyUnicode_Check(*name) ? PyUnicode_AsUTF8(*name) : nullptr;
    if (!name_str) {
        if (!PyErr_Occurred()) {
            PyErr_Format(PyExc_TypeError, "JavaLoader.exec_module(): the name of the module is not a string (type "
                "'%s')", Py_TYPE(*name)->tp_name);
        }
        return nullptr;
    }
    if (strcmp(name_str, "java") && strncmp(name_str, "java.", 5)) {
        PyErr_Format(PyExc_ImportError, "JavaLoader.exec_module(): module '%s' is not a Java module", name_str);
        return nullptr;
    }

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
