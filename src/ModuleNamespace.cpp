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

static PyMethodDef ModuleNamespace_methods[] = {
    {nullptr, nullptr},
};

PyDoc_STRVAR(ModuleNamespace_doc,
"ModuleNamespace()\n\
\n\
Python modules for imported Qore namespaces.");

static PyObject* ModuleNamespace_getattro(PyObject* self, PyObject* key);

PyTypeObject ModuleNamespace_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "ModuleNamespace",            // tp_name
    0,                            // tp_basicsize
    0,                            // tp_itemsize
    // Slots
    nullptr,                      // tp_dealloc
    0,                            // tp_vectorcall_offset
    0,                            // tp_getattr
    0,                            // tp_setattr
    0,                            // tp_as_async
    nullptr,                      // tp_repr
    0,                            // tp_as_number
    0,                            // tp_as_sequence
    0,                            // tp_as_mapping
    0,                            // tp_hash
    0,                            // tp_call
    0,                            // tp_str
    ModuleNamespace_getattro,     // tp_getattro
    nullptr,                      // tp_setattro
    0,                            // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, // tp_flags
    ModuleNamespace_doc,          // tp_doc
    0,                            // tp_traverse
    0,                            // tp_clear
    0,                            // tp_richcompare
    0,                            // tp_weaklistoffset
    0,                            // tp_iter
    0,                            // tp_iternext
    ModuleNamespace_methods,      // tp_methods
    0,                            // tp_members
    0,                            // tp_getset
    &PyModule_Type,               // tp_base
    0,                            // tp_dict
    0,                            // tp_descr_get
    0,                            // tp_descr_set
    0,                            // tp_dictoffset
    nullptr,                      // tp_init
    nullptr,                      // tp_alloc
    nullptr,                      // tp_new
    nullptr,                      // tp_free
};

int initModuleNamespace() {
    ModuleNamespace_Type.tp_basicsize = PyModule_Type.tp_basicsize + sizeof(ModuleNamespace);
    ModuleNamespace_Type.tp_dictoffset = PyModule_Type.tp_dictoffset;
    ModuleNamespace_Type.tp_traverse = PyModule_Type.tp_traverse;
    ModuleNamespace_Type.tp_clear = PyModule_Type.tp_clear;

    if (PyType_Ready(&ModuleNamespace_Type) < 0) {
        printd(5, "initModuleNamespace() module type initialization failed\n");
        return -1;
    }

    return 0;
}

bool ModuleNamespace_Check(PyObject* obj) {
    return PyObject_TypeCheck(obj, &ModuleNamespace_Type);
}

static ModuleNamespace* get_namespace(PyObject* py_mns) {
    return reinterpret_cast<ModuleNamespace*>(reinterpret_cast<char*>(py_mns) + PyModule_Type.tp_basicsize);
}

PyObject* ModuleNamespace_New(const char* name, const QoreNamespace* ns) {
    // Create the module by calling the type with the name argument
    // This properly calls tp_new and tp_init, ensuring the module dictionary is initialized
    QorePythonReferenceHolder args(PyTuple_New(1));
    PyTuple_SET_ITEM(*args, 0, PyUnicode_FromString(name));
    QorePythonReferenceHolder self(PyObject_Call((PyObject*)&ModuleNamespace_Type, *args, nullptr));
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
