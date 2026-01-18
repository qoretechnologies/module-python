/* indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2020 Qore Technologies, s.r.o.

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

#ifndef _QORE_PYTHON_PYTHONQORECLASS_H

#define _QORE_PYTHON_PYTHONQORECLASS_H

#include "python-module.h"

#include <vector>

// qore object type
struct PyQoreObject {
    PyObject_HEAD
    QoreObject* qobj;
};

class PythonQoreClass {
public:
    DLLLOCAL PythonQoreClass(QorePythonProgram* pypgm, const char* module_name, const QoreClass& qcls,
        py_cls_map_t::iterator i);

    //! called for pure Python types to allow them to be extended in Qore
    DLLLOCAL PythonQoreClass(QorePythonProgram* pypgm, PyTypeObject* type, const QoreClass& qcls);

    DLLLOCAL ~PythonQoreClass();

    //! Release Python reference before interpreter is cleared
    /** This must be called while the interpreter is still valid.
        After this call, the PythonQoreClass can be safely deleted even after
        the interpreter is deleted.
    */
    DLLLOCAL void release();

    //! Clear method objects from the type dictionary
    /** This removes all method objects from py_type->tp_dict.
        Must be called with the correct interpreter's thread state active.
    */
    DLLLOCAL void clearMethods();

    DLLLOCAL PyTypeObject* getPythonType() {
        return py_type;
    }

    //! wraps a Qore object as a Python object of this class
    /** obj will be referenced for the assignment
    */
    DLLLOCAL PyObject* wrap(QoreObject* obj);

    DLLLOCAL static const QoreClass* getQoreClass(PyTypeObject* type);

private:
    QoreString name;
    QoreString doc;

    typedef std::vector<PyMethodDef> py_meth_vec_t;
    py_meth_vec_t py_normal_meth_vec;
    py_meth_vec_t py_static_meth_vec;
    typedef std::vector<QoreString> strvec_t;
    strvec_t strvec;
    typedef std::vector<QorePythonReferenceHolder> py_obj_vec_t;
    py_obj_vec_t py_normal_meth_obj_vec;
    py_obj_vec_t py_static_meth_obj_vec;
    typedef std::set<const char*, ltstr> cstrset_t;
    typedef std::set<const QoreClass*> clsset_t;

    PyTypeObject* py_type;

    DLLLOCAL void populateClass(QorePythonProgram* pypgm, const QoreClass& qcls, clsset_t& cls_set,
        cstrset_t& meth_set, bool skip_first = true);

    DLLLOCAL static int newQoreObject(ExceptionSink& xsink, PyQoreObject* pyself, QoreObject* qobj,
        const QoreClass* qcls, QorePythonProgram* qore_python_pgm);

    // finds the Qore class for the Python type
    DLLLOCAL static const QoreClass* findQoreClass(PyObject* self);

    // class methods
    DLLLOCAL static PyObject* exec_qore_method(PyObject* method_capsule, PyObject* args);
    DLLLOCAL static PyObject* exec_qore_static_method(PyObject* method_capsule, PyObject* args);

    DLLLOCAL static PyObject* exec_qore_static_method(const QoreMethod& m, PyObject* args, size_t offset = 0);

    // Python type methods
    DLLLOCAL static int py_init(PyObject* self, PyObject* args, PyObject* kwds);
    DLLLOCAL static PyObject* py_new(PyTypeObject* type, PyObject* args, PyObject* kw);
    DLLLOCAL static void py_dealloc(PyQoreObject* self);
    DLLLOCAL static PyObject* py_repr(PyObject* obj);
    DLLLOCAL static void py_free(PyQoreObject* self);
    // get attribute
    DLLLOCAL static PyObject* py_getattro(PyObject* self, PyObject* attr);
};

/*
//! Holds a Python exception and restores it on exit
class PythonExceptionHolder {
public:
    DLLLOCAL PythonExceptionHolder() {
        PyErr_Fetch(&error_type, &error_value, &error_traceback);
    }

    DLLLOCAL ~PythonExceptionHolder() {
        PyErr_Restore(error_type, error_value, error_traceback);
    }

protected:
    PyObject* error_type,
        * error_value,
        * error_traceback;
};
*/

DLLLOCAL bool PyQoreObject_Check(PyObject* obj);

DLLLOCAL bool PyQoreObjectType_Check(PyTypeObject* type);

DLLLOCAL extern PyTypeObject PythonQoreException_Type;

#endif
