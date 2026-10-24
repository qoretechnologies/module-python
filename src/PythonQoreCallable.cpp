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

#include "PythonQoreCallable.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"

static int qore_callable_init(PyObject* self, PyObject* args, PyObject* kwds);
static PyObject* qore_callable_new(PyTypeObject* type, PyObject* args, PyObject* kw);
static void qore_callable_dealloc(PyQoreCallable* self);
static PyObject* qore_callable_repr(PyObject* obj);
static PyObject* qore_callable_call(PyQoreCallable* self, PyObject* args, PyObject* kwargs);
static void qore_callable_free(PyQoreCallable* self);

PyTypeObject PythonQoreCallable_Type = {
#if !defined(__clang__) && __GNUC__ < 8
    PyVarObject_HEAD_INIT(nullptr, 0)
    // g++ 5.4.0 does not accept the short-form initialization below :(
    "QoreCallable",                 // tp_name
    sizeof(PyQoreCallable),         // tp_basicsize
    0,                              // tp_itemsize
    (destructor)qore_callable_dealloc, // tp_dealloc
    0,                              // tp_vectorcall_offset/
    0,                              // tp_getattr
    0,                              // tp_setattr
    0,                              // tp_as_async
    qore_callable_repr,             // tp_repr
    0,                              // tp_as_number
    0,                              // tp_as_sequence
    0,                              // tp_as_mapping
    0,                              // tp_hash
    (ternaryfunc)qore_callable_call, // tp_call
    0,                              // tp_str
    0,                              // tp_getattro
    0,                              // tp_setattro
    0,                              // tp_as_buffer
    Py_TPFLAGS_DEFAULT,             // tp_flags
    "Qore callable type",           // tp_doc
    0,                              // tp_traverse
    0,                              // tp_clear
    0,                              // tp_richcompare
    0,                              // tp_weaklistoffset
    0,                              // tp_iter
    0,                              // tp_iternext
    0,                              // tp_methods
    0,                              // tp_members
    0,                              // tp_getset
    nullptr,                        // tp_base
    0,                              // tp_dict
    0,                              // tp_descr_get
    0,                              // tp_descr_set
    0,                              // tp_dictoffset
    qore_callable_init,             // tp_init
    0,                              // tp_alloc
    qore_callable_new,              // tp_new
    (freefunc)qore_callable_free,   // tp_free
#else
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "QoreCallable",
    .tp_basicsize = sizeof(PyQoreCallable),
    .tp_dealloc = (destructor)qore_callable_dealloc,
    .tp_repr = qore_callable_repr,
    .tp_call = (ternaryfunc)qore_callable_call,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Qore callable type",
    .tp_init = qore_callable_init,
    .tp_new = qore_callable_new,
    .tp_free = (freefunc)qore_callable_free,
#endif
};

bool PyQoreCallable_Check(PyObject* obj) {
    return obj && PyObject_TypeCheck(obj, &PythonQoreCallable_Type);
}

int qore_callable_init(PyObject* self, PyObject* args, PyObject* kwds) {
    assert(PyQoreCallable_Check(self));
    assert(PyTuple_Check(args));

    //QorePythonReferenceHolder argstr(PyObject_Repr(args));
    //printd(5, "qore_callable_init() self: %p '%s' args: %p (%d: %s) kwds: %p\n", self, Py_TYPE(self)->tp_name, args, (int)PyTuple_Size(args), PyUnicode_AsUTF8(*argstr), kwds);

    Py_ssize_t size = PyTuple_Size(args);
    if (size) {
        QoreStringMaker desc("invalid args to __init__() on internal class");
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return -1;
    }

    PyQoreCallable* pyself = reinterpret_cast<PyQoreCallable*>(self);
    // returns a borrowed reference
    ResolvedCallReferenceNode* callable = QorePythonImplicitQoreArgHelper::getQoreCallable();
    //printd(5, "PythonQoreClass::py_init() self: %p py_cls: '%s' qcls: '%s' cq: '%s' qobj: %p args: %p\n", self, type->tp_name, qcls->getName(), constructor_cls->getName(), qobj, args);
    if (callable) {
        pyself->callable = callable->refRefSelf();
        return 0;
    }

    QoreStringMaker desc("invalid __init__() call to an internal class");
    PyErr_SetString(PyExc_ValueError, desc.c_str());
    return -1;
}

PyObject* qore_callable_new(PyTypeObject* type, PyObject* args, PyObject* kw) {
    return type->tp_alloc(type, 0);
}

void qore_callable_dealloc(PyQoreCallable* self) {
    if (self->callable) {
        // releasing the callable can run Qore code (destructors), which needs a thread registered with Qore and runs
        // without the GIL; if the thread cannot be registered, the callable is not released
        QorePythonQoreEntryHelper qeh;
        if (qeh.ok()) {
            ExceptionSink xsink;
            ValueHolder callable(self->callable, &xsink);
            qore_python_release(callable);
        }
        self->callable = nullptr;
    }
    Py_TYPE(self)->tp_free(self);
}

PyObject* qore_callable_repr(PyObject* obj) {
    QoreStringMaker str("Qore callable %p", obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}

PyObject* qore_callable_call(PyQoreCallable* self, PyObject* args, PyObject* kwargs) {
    if (!self->callable) {
        QoreStringMaker desc("Error: Qore callback ovject missing callable ptr");
        PyErr_SetString(PyExc_ValueError, desc.c_str());
        return nullptr;
    }
    // Qore code takes positional arguments; keyword arguments are not ignored
    if (qore_python_check_no_kwargs(kwargs, "a Qore callable")) {
        return nullptr;
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    // the thread state that Python called us with; it must be current again before returning
    PyThreadState* entry_tstate = _qore_safe_thread_state_get();

    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();

    ExceptionSink xsink;
    {
        QorePythonHelper qph(qore_python_pgm, &xsink);
        if (!xsink) {
            ReferenceHolder<QoreListNode> qargs(qore_python_pgm->getQoreListFromTuple(&xsink, args), &xsink);
            if (!xsink) {
                ValueHolder rv(&xsink);
                {
                    // the callable runs Qore code, which can wait for other Python threads, so it runs without the
                    // GIL, as Qore methods and functions called from Python do
                    QorePythonReleaseGilHelper prgh;

                    QorePythonStackLocationHelper slh(qph.getOldProgram());

                    rv = self->callable->execValue(*qargs, &xsink);
                }
                QorePythonReferenceHolder py_rv;
                if (!xsink) {
                    py_rv = qore_python_pgm->getPythonValue(*rv, &xsink);
                }
                // the arguments and the result of the call are released without the GIL if this can run Qore code
                qore_python_release(rv);
                qore_python_release(qargs);
                if (!xsink) {
                    assert(py_rv);
                    return py_rv.release();
                }
            }
        }
    }

    // the exception must be raised in the thread state that Python called us with
    PyThreadState* exit_tstate = _qore_safe_thread_state_get();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }

    assert(xsink);
    qore_python_pgm->raisePythonException(xsink);
    return nullptr;
}

void qore_callable_free(PyQoreCallable* self) {
    PyObject_Del(self);
}
