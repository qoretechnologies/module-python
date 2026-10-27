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

#ifndef _QORE_PYTHON_PYTHONQORECLASS_H

#define _QORE_PYTHON_PYTHONQORECLASS_H

#include "python-module.h"
#include "QorePythonDeclHandle.h"

#include <string>
#include <vector>

// qore object type
struct PyQoreObject {
    PyObject_HEAD
    QoreObject* qobj;
    //! true if the object holds a strong reference to qobj, false for a weak reference (when Qore created qobj for a
    //! Qore class that inherits the object's Python class)
    bool strong;
    //! true if qobj's private data holds a reference to this object (an instance of a Python subclass of a Qore class)
    bool qobj_refs_self;
    //! true if the object is the Python object of qobj in its interpreter (see PythonQoreClass::findProxy())
    bool registered;
    //! the weak references to the object
    PyObject* weakreflist;
};

class PythonQoreClass {
public:
    DLLLOCAL PythonQoreClass(QorePythonProgram* pypgm, const char* module_name, const QoreClass& qcls);

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

    //! returns a new reference to the Python object of the Qore object in the current interpreter, if any
    /** Each Qore object has at most one Python object in an interpreter, so that the same Qore object is the same
        Python object (identity)
    */
    DLLLOCAL static PyObject* findProxy(QoreObject* obj);

    //! returns a new base type of the Python types of Qore classes for the current interpreter (see
    //! QorePythonInterpreterTypes)
    DLLLOCAL static PyObject* createBaseType();

    //! returns the handle of the Qore class of a Python type made for a Qore class
    /** @return the handle, valid as long as the type is, or nullptr with a Python exception set

        The class can only be used while a QorePythonDeclUse of the handle is held
    */
    DLLLOCAL static QorePythonDeclHandle* getQoreClass(PyTypeObject* type);

    //! returns false if the Program of the Qore class has been deleted
    DLLLOCAL bool isValid() const {
        return !handle || handle->isValid();
    }

    //! __dir__(): the Python attributes and the public members of the Qore object
    DLLLOCAL static PyObject* py_dir(PyObject* unused, PyObject* self);

private:
    QoreString name;
    QoreString doc;

    typedef std::vector<PyMethodDef> py_meth_vec_t;
    py_meth_vec_t py_normal_meth_vec;
    py_meth_vec_t py_static_meth_vec;
    //! the names of the methods added to the type, removed by clearMethods()
    std::vector<std::string> meth_names;
    typedef std::vector<QoreString> strvec_t;
    strvec_t strvec;
    typedef std::vector<QorePythonReferenceHolder> py_obj_vec_t;
    py_obj_vec_t py_normal_meth_obj_vec;
    py_obj_vec_t py_static_meth_obj_vec;
    typedef std::set<const char*, ltstr> cstrset_t;
    typedef std::set<const QoreClass*> clsset_t;

    PyTypeObject* py_type;

    //! the handle of the Qore class; nullptr for a Python type extended in Qore
    std::shared_ptr<QorePythonDeclHandle> handle;

    DLLLOCAL void populateClass(QorePythonProgram* pypgm, const QoreClass& qcls, clsset_t& cls_set,
        cstrset_t& meth_set, bool skip_first = true);

    DLLLOCAL static int newQoreObject(ExceptionSink& xsink, PyQoreObject* pyself, QoreObject* qobj,
        const QoreClass* qcls, QorePythonProgram* qore_python_pgm);

    //! finds the handle of the Qore class for the Python type of the object (see getQoreClass())
    DLLLOCAL static QorePythonDeclHandle* findQoreClass(PyObject* self);

    // class methods
    DLLLOCAL static PyObject* exec_qore_method(PyObject* method_capsule, PyObject* args);
    DLLLOCAL static PyObject* exec_qore_static_method(PyObject* method_capsule, PyObject* args);

    DLLLOCAL static PyObject* exec_qore_static_method(const QoreMethod& m, PyObject* args, size_t offset = 0);

    // Python type methods
    DLLLOCAL static int py_init(PyObject* self, PyObject* args, PyObject* kwds);
    DLLLOCAL static PyObject* py_new(PyTypeObject* type, PyObject* args, PyObject* kw);
    DLLLOCAL static void py_dealloc(PyQoreObject* self);
    DLLLOCAL static int py_traverse(PyObject* self, visitproc visit, void* arg);
    DLLLOCAL static int py_clear(PyObject* self);

    //! releases the reference to the Qore object; a strong reference is released without the GIL, as releasing it can
    //! run the object's destructor
    DLLLOCAL static void releaseQoreObject(PyQoreObject* self);
    DLLLOCAL static PyObject* py_repr(PyObject* obj);
    DLLLOCAL static void py_free(PyQoreObject* self);
    // get attribute
    DLLLOCAL static PyObject* py_getattro(PyObject* self, PyObject* attr);
    // set or delete attribute
    DLLLOCAL static int py_setattro(PyObject* self, PyObject* attr, PyObject* value);

    //! makes the object the Python object of its Qore object in the current interpreter
    /** @return the object, or a new reference to the Python object registered by another thread meanwhile, in which
        case the reference to \a self is released
    */
    DLLLOCAL static PyObject* registerProxy(PyQoreObject* self);

    //! removes the object as the Python object of its Qore object; called before the Qore object is released
    DLLLOCAL static void deregisterProxy(PyQoreObject* self);
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

#endif
