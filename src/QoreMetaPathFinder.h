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

#ifndef _QORE_PYTHON_QOREMETAPATHFINDER_H

#define _QORE_PYTHON_QOREMETAPATHFINDER_H

#include "python-module.h"

#include <map>

class QoreMetaPathFinder {
public:
    //! initializer function
    DLLLOCAL static int init();

    //! module initializer function
    DLLLOCAL static int setupModules();

    //! destructor function

    //! type functions
    DLLLOCAL static void dealloc(PyObject* self);
    DLLLOCAL static PyObject* repr(PyObject* obj);

    //! class methods
    DLLLOCAL static PyObject* find_spec(PyObject* self, PyObject* args);

    //! Retuns a new module spec object
    /** @param qore qore or java loader
        @param name the name of the new module
        @param loader either nullptr (None will be used) or an already-referenced loader
    */
    DLLLOCAL static PyObject* newModuleSpec(bool qore, const QoreString& name, PyObject* loader = nullptr);

private:

    DLLLOCAL static PyObject* getQorePackageModuleSpec();
    DLLLOCAL static PyObject* getJavaPackageModuleSpec();
    DLLLOCAL static PyObject* getQoreRootModuleSpec(const QoreString& mname);
    DLLLOCAL static PyObject* tryLoadModule(const QoreString& full_name, const char* mod_name);
    DLLLOCAL static PyObject* getJavaNamespaceModule(const QoreString& full_name, const char* mod_name);
};

class PythonThreadStateHelper : QorePythonGilHelper {
public:
    DLLLOCAL PythonThreadStateHelper() : QorePythonGilHelper(PyGILState_GetThisThreadState()) {
    }
};

#endif
