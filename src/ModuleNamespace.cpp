/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file ModuleNamespace.cpp defines the Python ModuleNamespace class */
/*
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

#include "ModuleNamespace.h"
#include "QorePythonInterpreterTypes.h"

static PyMethodDef ModuleNamespace_methods[] = {
    {nullptr, nullptr},
};

PyDoc_STRVAR(ModuleNamespace_doc,
"ModuleNamespace()\n\
\n\
Python modules for imported Qore namespaces.");

static PyObject* ModuleNamespace_getattro(PyObject* self, PyObject* key);

// the modules of namespaces are modules (a subtype of PyModule_Type) with the namespace after the module's data
static int ModuleNamespace_traverse(PyObject* self, visitproc visit, void* arg) {
    // the object of a heap type holds a reference to its type
    Py_VISIT(Py_TYPE(self));
    return PyModule_Type.tp_traverse(self, visit, arg);
}

static void ModuleNamespace_dealloc(PyObject* self) {
    PyTypeObject* type = Py_TYPE(self);
    PyModule_Type.tp_dealloc(self);
    Py_DECREF(type);
}

PyObject* ModuleNamespace_CreateType() {
    PyType_Slot slots[] = {
        {Py_tp_dealloc, (void*)ModuleNamespace_dealloc},
        {Py_tp_traverse, (void*)ModuleNamespace_traverse},
        {Py_tp_clear, (void*)PyModule_Type.tp_clear},
        {Py_tp_getattro, (void*)ModuleNamespace_getattro},
        {Py_tp_doc, (void*)ModuleNamespace_doc},
        {Py_tp_methods, (void*)ModuleNamespace_methods},
        {0, nullptr},
    };
    PyType_Spec spec = {
        .name = "qoreloader.ModuleNamespace",
        .basicsize = static_cast<int>(PyModule_Type.tp_basicsize + sizeof(ModuleNamespace)),
        .itemsize = 0,
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
        .slots = slots,
    };
    QorePythonReferenceHolder bases(PyTuple_Pack(1, reinterpret_cast<PyObject*>(&PyModule_Type)));
    return bases ? PyType_FromSpecWithBases(&spec, *bases) : nullptr;
}

bool ModuleNamespace_Check(PyObject* obj) {
    const QorePythonInterpreterTypes* types = qore_python_get_interpreter_types();
    return types && PyObject_TypeCheck(obj, types->module_namespace);
}

static ModuleNamespace* get_namespace(PyObject* py_mns) {
    return reinterpret_cast<ModuleNamespace*>(reinterpret_cast<char*>(py_mns) + PyModule_Type.tp_basicsize);
}

PyObject* ModuleNamespace_New(const char* name, const QoreNamespace* ns) {
    // Create the module by calling the type with the name argument
    // This properly calls tp_new and tp_init, ensuring the module dictionary is initialized
    QorePythonReferenceHolder args(PyTuple_New(1));
    PyTuple_SET_ITEM(*args, 0, PyUnicode_FromString(name));
    const QorePythonInterpreterTypes* types = qore_python_get_interpreter_types();
    if (!types) {
        PyErr_SetString(PyExc_RuntimeError, "the qoreloader module is not initialized in the current interpreter");
        return nullptr;
    }
    QorePythonReferenceHolder self(PyObject_Call(reinterpret_cast<PyObject*>(types->module_namespace), *args,
        nullptr));
    if (!self) {
        return nullptr;
    }
    assert(PyModule_Check(*self));

    ModuleNamespace* mns = get_namespace(*self);
    assert(!mns->ns);
    mns->ns = const_cast<QoreNamespace*>(ns);
    //printd(5, "ModuleNamespace_New() %s: %p (%s)\n", name, *self, ns->getName());
    return self.release();
}

static PyObject* ModuleNamespace_getattro(PyObject* self, PyObject* key) {
    // first check if the attribute is defined
    PyObject* attr = PyObject_GenericGetAttr(self, key);
    if (attr) {
        return attr;
    }
    // now try to resolve it
    assert(PyUnicode_Check(key));
    const char* key_str = PyUnicode_AsUTF8(key);
    // do not try to look up dunder attributes
    if (key_str[0] == '_' && key_str[1] == '_') {
        return nullptr;
    }
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    ModuleNamespace* mns = get_namespace(self);
    printd(5, "ModuleNamespace_getattro() obj: %p ns: %p (%s) attr: %s: %p\n", self, mns->ns, mns->ns->getName(), key_str, attr);

    QoreProgram* qpgm = mns->ns->getProgram();
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    ExceptionSink xsink;
    QoreExternalProgramContextHelper pch(&xsink, qpgm);
    if (xsink) {
        PyErr_Clear();
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    CurrentProgramRuntimeExternalParseContextHelper prpch;
    QoreClass* qc = mns->ns->findLocalClass(key_str);
    if (!qc) {
        // try to load the class dynamically
        qc = mns->ns->findLoadLocalClass(key_str);
    }
    printd(5, "ModuleNamespace_getattro() %s.%s qc: %p\n", mns->ns->getName(), key_str, qc);
    if (!qc) {
        return nullptr;
    }
    PyErr_Clear();
    if (qc && qore_python_pgm->importQoreClassToPython(self, *qc, mns->ns->getName())) {
        return nullptr;
    }
    return PyModule_Type.tp_getattro(self, key);
}
