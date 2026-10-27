/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file QorePythonInterpreterTypes.h defines the per-interpreter types of the qoreloader module */
/*
    QorePythonInterpreterTypes.h

    Qore Programming Language python Module

    Copyright (C) 2026 Qore Technologies, s.r.o.

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

#ifndef _QORE_PYTHON_INTERPRETER_TYPES_H

#define _QORE_PYTHON_INTERPRETER_TYPES_H

#include "python-module.h"

//! The types of the module, and the objects of its loaders, in an interpreter
/** Each interpreter that imports the qoreloader module has its own types (heap types), created when the module is
    initialized in the interpreter and kept in the state of the module: Python objects, types included, cannot be
    shared by interpreters (an isolated sub-interpreter has its own GIL and its own memory allocator, and with
    free-threaded Python each interpreter counts the references of its types itself).  The types were static types
    of the extension shared by all interpreters (issue 5536).

    The types of an interpreter are found with qore_python_get_interpreter_types(); the types do not reference the
    module, so that they do not keep it alive.
*/
struct QorePythonInterpreterTypes {
    //! the base type of the Python types of Qore classes
    PyTypeObject* qore_object_base = nullptr;
    //! the type of the Python objects of Qore closures and call references
    PyTypeObject* qore_callable = nullptr;
    //! the type of the modules of Java namespaces
    PyTypeObject* module_namespace = nullptr;
    //! the type of the loader of Qore modules
    PyTypeObject* qore_loader = nullptr;
    //! the type of the loader of Java modules
    PyTypeObject* java_loader = nullptr;
    //! the type of the finder of Qore and Java modules in sys.meta_path
    PyTypeObject* meta_path_finder = nullptr;
    //! the type of the registrations of Python threads with Qore
    PyTypeObject* thread_sentinel = nullptr;
    //! the loader of Qore modules
    PyObject* qore_loader_obj = nullptr;
    //! the loader of Java modules
    PyObject* java_loader_obj = nullptr;
    //! the interpreter that the types belong to, while they are registered
    PyInterpreterState* interp = nullptr;
};

//! Creates the types of the current interpreter and registers them; must be called with the GIL held
/** @return 0 for OK, -1 with a Python exception set; the types created are released on error
*/
DLLLOCAL int qore_python_interpreter_types_init(QorePythonInterpreterTypes& types);

//! Deregisters and releases the types of an interpreter; must be called with the GIL held in the interpreter
DLLLOCAL void qore_python_interpreter_types_clear(QorePythonInterpreterTypes& types);

//! Visits the types of an interpreter for Python's cycle collector
DLLLOCAL int qore_python_interpreter_types_traverse(QorePythonInterpreterTypes& types, visitproc visit, void* arg);

//! Returns the types of the current interpreter, or nullptr if the qoreloader module is not initialized in it
/** Must be called with the GIL (or an attached thread state with free-threaded Python) of the interpreter
*/
DLLLOCAL const QorePythonInterpreterTypes* qore_python_get_interpreter_types();

//! Registers the fork handler of the lock of the registry of the types; returns 0 or -1 on error
DLLLOCAL int qore_python_interpreter_types_register_fork_handler();

//! Unregisters the fork handler of the lock of the registry of the types
DLLLOCAL void qore_python_interpreter_types_unregister_fork_handler();

#endif
