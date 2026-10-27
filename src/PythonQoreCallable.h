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

#ifndef _QORE_PYTHON_QORECALLABLE_H

#define _QORE_PYTHON_QORECALLABLE_H

#include "python-module.h"

// qore callable type
struct PyQoreCallable {
    PyObject_HEAD
    ResolvedCallReferenceNode* callable;
};

//! returns a new type of the Python objects of Qore callables for the current interpreter (see
//! QorePythonInterpreterTypes)
DLLLOCAL PyObject* qore_python_create_callable_type();

//! returns the type of the Python objects of Qore callables of the current interpreter (a borrowed reference), or
//! nullptr with a Python exception if the qoreloader module is not initialized in the interpreter
DLLLOCAL PyTypeObject* qore_python_get_callable_type();

DLLLOCAL bool PyQoreCallable_Check(PyObject* obj);

#endif