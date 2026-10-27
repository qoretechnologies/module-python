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

#ifndef _QORE_PYTHON_JAVALOADER_H

#define _QORE_PYTHON_JAVALOADER_H

#include "python-module.h"

#include <vector>
#include <map>

// forward references
class PythonQoreClass;

// reexport list map
typedef std::map<const char*, const QoreListNode*, ltstr> mod_dep_map_t;

class JavaLoader {
public:
    //! returns a new type of the loader for the current interpreter (see QorePythonInterpreterTypes)
    DLLLOCAL static PyObject* createType();

    //! returns the loader object of the current interpreter with the reference count incremented; nullptr with a
    //! Python exception if the qoreloader module is not initialized in the interpreter
    DLLLOCAL static PyObject* getLoaderRef();

    //! returns the loader object of the current interpreter; nullptr with a Python exception if the qoreloader module
    //! is not initialized in the interpreter
    DLLLOCAL static PyObject* getLoader();

    //! type functions
    DLLLOCAL static void dealloc(PyObject* self);
    DLLLOCAL static PyObject* repr(PyObject* obj);

    //! class methods
    DLLLOCAL static PyObject* create_module(PyObject* self, PyObject* args);
    DLLLOCAL static PyObject* exec_module(PyObject* self, PyObject* args);

private:
};

#endif
