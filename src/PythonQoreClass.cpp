/* indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2020 - 2022 Qore Technologies, s.r.o.

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

#include "PythonQoreClass.h"
#include "QoreLoader.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"

#include <string.h>
#include <memory>
#include <set>

#include <frameobject.h>

#include <pthread.h>

static constexpr const char* QCLASS_KEY = "__$QCLS__";

static int qore_exception_init(PyObject* self, PyObject* args, PyObject* kwds) {
    //QorePythonReferenceHolder argstr(PyObject_Repr(args));
    //printd(5, "qore_exception_init() self: %p args: %s\n", self, PyUnicode_AsUTF8(*argstr));

    assert(PyTuple_Check(args));
    Py_ssize_t size = PyTuple_Size(args);
    if (!size) {
        return -1;
    }
    PyObject* err = PyTuple_GetItem(args, 0);
    if (!PyUnicode_Check(err)) {
        QorePythonReferenceHolder err_repr(PyObject_Repr(err));
        if (PyObject_SetAttrString(self, "err", *err_repr) < 0) {
            return -1;
        }
    } else {
        if (PyObject_SetAttrString(self, "err", err) < 0) {
            return -1;
        }
    }
    if (size > 1) {
        PyObject* desc = PyTuple_GetItem(args, 1);
        if (!PyUnicode_Check(desc)) {
            QorePythonReferenceHolder desc_repr(PyObject_Repr(desc));
            if (PyObject_SetAttrString(self, "desc", *desc_repr) < 0) {
                return -1;
            }
        } else {
            if (PyObject_SetAttrString(self, "desc", desc) < 0) {
                return -1;
            }
        }
        if (size > 2) {
            if (PyObject_SetAttrString(self, "arg", PyTuple_GetItem(args, 2)) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

PyTypeObject PythonQoreException_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
#if !defined(__clang__) && __GNUC__ < 8
    // g++ 5.4.0 does not accept the short-form initialization below :(
    "QoreException",                // tp_name
    sizeof(PyBaseExceptionObject),  // tp_basicsize
    0,                              // tp_itemsize
    nullptr,                        // tp_dealloc
    0,                              // tp_vectorcall_offset/
    0,                              // tp_getattr
    0,                              // tp_setattr
    0,                              // tp_as_async
    nullptr,                        // tp_repr
    0,                              // tp_as_number
    0,                              // tp_as_sequence
    0,                              // tp_as_mapping
    0,                              // tp_hash
    0,                              // tp_call
    0,                              // tp_str
    0,                              // tp_getattro
    0,                              // tp_setattro
    0,                              // tp_as_buffer
    Py_TPFLAGS_DEFAULT,             // tp_flags
    "Qore exception class",         // tp_doc
    0,                              // tp_traverse
    0,                              // tp_clear
    0,                              // tp_richcompare
    0,                              // tp_weaklistoffset
    0,                              // tp_iter
    0,                              // tp_iternext
    0,                              // tp_methods
    0,                              // tp_members
    0,                              // tp_getset
    reinterpret_cast<PyTypeObject*>(PyExc_Exception),   // tp_base
    0,                              // tp_dict
    0,                              // tp_descr_get
    0,                              // tp_descr_set
    0,                              // tp_dictoffset
    qore_exception_init,            // tp_init
    0,                              // tp_alloc
    0,                              // tp_new
#else
    .tp_name = "QoreException",
    .tp_basicsize = sizeof(PyBaseExceptionObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Qore exception class",
    .tp_base = reinterpret_cast<PyTypeObject*>(PyExc_Exception),
    .tp_init = qore_exception_init,
#endif
};

void PythonQoreClass::py_free(PyQoreObject* self) {
    //printd(5, "PythonQoreClass::py_free() self: %p '%s'\n", self, Py_TYPE(self)->tp_name);
    PyObject_Del(self);
}

bool PyQoreObject_Check(PyObject* obj) {
    return obj && PyObject_TypeCheck(obj, &PythonQoreObjectBase_Type);
}

bool PyQoreObjectType_Check(PyTypeObject* type) {
    assert(type);
    // FIXME: use PyDict_Contains() instead
    return type->tp_dict && PyDict_GetItemString(type->tp_dict, QCLASS_KEY);
}

PythonQoreClass::PythonQoreClass(QorePythonProgram* pypgm, PyTypeObject* type, const QoreClass& qcls) {
    Py_INCREF((PyObject*)type);
    py_type = type;
    pypgm->insertClass(&qcls, this);
    // do not save the qore class to the python class, as the python class may be a builtin class and the Qore class
    // can be deleted afterwards
}

PythonQoreClass::PythonQoreClass(QorePythonProgram* pypgm, const char* module_name, const QoreClass& qcls, py_cls_map_t::iterator i) {
    //printd(5, "PythonQoreClass::PythonQoreClass() %s.%s py_type: %p\n", module_name, qcls.getName(), &py_type);

    name.sprintf("%s.%s", module_name, qcls.getName());
    const char* namestr = pypgm->saveString(name.c_str());

    doc.sprintf("Python wrapper class for Qore class %s", qcls.getName());
    const char* docstr = pypgm->saveString(doc.c_str());

    PyType_Slot slots[] = {
        {Py_tp_doc, (void*)docstr},
        {Py_tp_dealloc, (void*)PythonQoreClass::py_dealloc},
        {Py_tp_repr, (void*)PythonQoreClass::py_repr},
        {Py_tp_getattro, (void*)PythonQoreClass::py_getattro},
        {Py_tp_base, (void*)&PythonQoreObjectBase_Type},
        {Py_tp_alloc, (void*)PyType_GenericAlloc},
        {Py_tp_init, (void*)PythonQoreClass::py_init},
        {Py_tp_new, (void*)PythonQoreClass::py_new},
        {Py_tp_free, (void*)PythonQoreClass::py_free},
        {0, nullptr},
    };

    PyType_Spec spec = {
        .name = namestr,
        .basicsize = sizeof(PyQoreObject),
        .itemsize = 0,
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
        .slots = slots,
    };

    clsset_t cls_set;

    // get single base class - Python and Qore's multiple inheritance models are not compatible
    // we can only set a single class for the Python base class, so if there are multiple
    // base classes, then we populate methods instead directly
    QorePythonReferenceHolder bases;
    {
        PythonQoreClass* base_cls = nullptr;
        QoreParentClassIterator ci(qcls);
        while (ci.next()) {
            if (ci.getAccess() > Private) {
                continue;
            }

            base_cls = pypgm->findCreatePythonClass(ci.getParentClass(), module_name);
            cls_set.insert(&ci.getParentClass());
            bases = PyTuple_New(1);
            PyObject* py_base_cls = reinterpret_cast<PyObject*>(base_cls->getPythonType());
            Py_INCREF(py_base_cls);
            PyTuple_SET_ITEM(*bases, 0, py_base_cls);
            break;
        }
    }

    py_type = bases
        ? reinterpret_cast<PyTypeObject*>(PyType_FromSpecWithBases(&spec, *bases))
        : reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&spec));
    //printd(5, "PythonQoreClass::PythonQoreClass() %s py_type: %p\n", qcls.getName(), py_type);

    assert(py_type);
    assert(py_type->tp_dict);

    // set of normal method names
    cstrset_t meth_set;

    pypgm->insertClass(i, &qcls, this);

    // setup class
    populateClass(pypgm, qcls, cls_set, meth_set);

    // add normal methods
    for (size_t i = 0; i < py_normal_meth_vec.size(); ++i) {
        PyMethodDef& md = py_normal_meth_vec[i];
        QorePythonReferenceHolder method_capsule(py_normal_meth_obj_vec[i].release());
        QorePythonReferenceHolder func(PyCFunction_New(&md, *method_capsule));
        QorePythonReferenceHolder meth(PyInstanceMethod_New(*func));
        assert(meth);
        const QoreMethod* m = reinterpret_cast<const QoreMethod*>(PyCapsule_GetPointer(*method_capsule, nullptr));
        PyDict_SetItemString(py_type->tp_dict, m->getName(), *meth);
    }
    py_normal_meth_obj_vec.clear();

    // add static methods
    for (size_t i = 0; i < py_static_meth_vec.size(); ++i) {
        PyMethodDef& md = py_static_meth_vec[i];
        QorePythonReferenceHolder method_capsule(py_static_meth_obj_vec[i].release());
        QorePythonReferenceHolder func(PyCFunction_New(&md, *method_capsule));
        QorePythonReferenceHolder meth(PyStaticMethod_New(*func));
        assert(meth);
        const QoreMethod* m = reinterpret_cast<const QoreMethod*>(PyCapsule_GetPointer(*method_capsule, nullptr));
        PyDict_SetItemString(py_type->tp_dict, m->getName(), *meth);
    }
    py_static_meth_obj_vec.clear();

    // add Qore class to type dictionary
    QorePythonReferenceHolder qore_class(PyCapsule_New((void*)&qcls, nullptr, nullptr));
    PyDict_SetItemString(py_type->tp_dict, QCLASS_KEY, *qore_class);
}

PythonQoreClass::~PythonQoreClass() {
    printd(5, "PythonQoreClass::~PythonQoreClass() this: %p '%s'\n", this, name.c_str());
    // py_type should have been released via release() before interpreter deletion
    // Only decref if it's still set (shouldn't happen in normal cleanup)
    if (py_type) {
        Py_DECREF(py_type);
    }
}

void PythonQoreClass::release() {
    // Release Python reference before interpreter is cleared
    // This must be called while the interpreter is still valid
    if (py_type) {
        Py_DECREF(py_type);
        py_type = nullptr;
    }
}

void PythonQoreClass::clearMethods() {
    // CRITICAL: Clear method objects from the type dictionary.
    // Method objects created with PyCFunction_New reference PyMethodDef structures stored
    // in py_normal_meth_vec and py_static_meth_vec. If the type survives (e.g., due to
    // references from the main interpreter), these method objects will be traversed during
    // GC after the PyMethodDef structures are freed, causing use-after-free crashes.
    // By clearing the dict, we ensure method objects are decreffed while the interpreter
    // is still valid.
    if (py_type && py_type->tp_dict) {
        // Remove all method objects we added
        for (const auto& md : py_normal_meth_vec) {
            if (md.ml_name) {
                PyDict_DelItemString(py_type->tp_dict, md.ml_name);
                PyErr_Clear();  // Ignore errors if key doesn't exist
            }
        }
        for (const auto& md : py_static_meth_vec) {
            if (md.ml_name) {
                PyDict_DelItemString(py_type->tp_dict, md.ml_name);
                PyErr_Clear();  // Ignore errors if key doesn't exist
            }
        }
        // Also remove the Qore class capsule
        PyDict_DelItemString(py_type->tp_dict, QCLASS_KEY);
        PyErr_Clear();
    }
}

void PythonQoreClass::populateClass(QorePythonProgram* pypgm, const QoreClass& qcls, clsset_t& cls_set,
        cstrset_t& meth_set, bool skip_first) {
    //printd(5, "PythonQoreClass::populateClass() cls: %s cs: %d ms: %d\n", qcls.getName(), (int)cls_set.size(),
    //  (int)meth_set.size());

    {
        QoreMethodIterator i(qcls);
        while (i.next()) {
            const QoreMethod* m = i.getMethod();
            if (m->getAccess() > Private) {
                continue;
            }

            //printd(5, "PythonQoreClass::populateClass() adding %s -> %s::%s()\n", name.c_str(), qcls.getName(),
            //  m->getName());
            cstrset_t::iterator mi = meth_set.lower_bound(m->getName());
            if (mi == meth_set.end() || strcmp(*mi, m->getName())) {
                meth_set.insert(mi, m->getName());
                QoreStringMaker mdoc("Python wrapper for Qore class method %s::%s()", qcls.getName(), m->getName());
                const char* mdocstr = pypgm->saveString(mdoc.c_str());
                py_normal_meth_vec.push_back({m->getName(), (PyCFunction)exec_qore_method, METH_VARARGS, mdocstr});
                py_normal_meth_obj_vec.push_back(PyCapsule_New((void*)m, nullptr, nullptr));
            }
        }
    }

    {
        QoreStaticMethodIterator i(qcls);
        while (i.next()) {
            const QoreMethod* m = i.getMethod();
            if (m->getAccess() > Private) {
                continue;
            }
            // issue #4397: check if there is an accessible normal method with the same name, if so, skip it
            {
                ClassAccess access;
                const QoreMethod* nm = qcls.findMethod(m->getName(), access);
                if (nm && access < Internal) {
                    continue;
                }
            }

            //printd(5, "PythonQoreClass::populateClass() adding %s -> static %s::%s()\n", name.c_str(),
            //  qcls.getName(), m->getName());
            cstrset_t::iterator mi = meth_set.lower_bound(m->getName());
            if (mi == meth_set.end() || strcmp(*mi, m->getName())) {
                meth_set.insert(mi, m->getName());
                QoreStringMaker mdoc("Python wrapper for Qore static class method %s::%s()", qcls.getName(),
                    m->getName());
                const char* mdocstr = pypgm->saveString(mdoc.c_str());
                py_static_meth_vec.push_back({m->getName(), (PyCFunction)exec_qore_static_method, METH_VARARGS,
                    mdocstr});
                py_static_meth_obj_vec.push_back(PyCapsule_New((void*)m, nullptr, nullptr));
            }
        }
    }

    ExceptionSink xsink;
    {
        QoreClassConstantIterator i(qcls);
        while (i.next()) {
            const QoreExternalConstant& c = i.get();
            if (c.getAccess() > Private) {
                continue;
            }
            cstrset_t::iterator mi = meth_set.lower_bound(c.getName());
            if (mi == meth_set.end() || strcmp(*mi, c.getName())) {
                meth_set.insert(mi, c.getName());
                ValueHolder qoreval(c.getReferencedValue(), &xsink);
                if (xsink) {
                    // a constant that cannot be read must not abort the import of the entire class; skip it
                    xsink.clear();
                    continue;
                }
                QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
                QorePythonReferenceHolder val(qore_python_pgm->getPythonValue(*qoreval, &xsink));
                if (xsink) {
                    // a constant that has no Python representation (ex: java.time.Instant::MIN, whose year lies
                    // outside the range supported by datetime.datetime) must not abort the import of the entire
                    // class; skip the constant instead
                    xsink.clear();
                    PyErr_Clear();
                    continue;
                }
                assert(val);
                PyDict_SetItemString(py_type->tp_dict, c.getName(), *val);
            }
        }
    }

    bool first = false;
    QoreParentClassIterator ci(qcls);
    while (ci.next()) {
        if (ci.getAccess() > Private) {
            continue;
        }

        // skip the first class, as it's already been added as a base class
        if (skip_first && !first) {
            first = true;
            continue;
        }

        const QoreClass& parent_cls = ci.getParentClass();

        clsset_t::iterator i = cls_set.lower_bound(&parent_cls);
        if (i != cls_set.end() && ((*i) == &parent_cls)) {
            //printd(5, "PythonQoreClass::populateClass() %s skipping parent %s\n", qcls.getName(), parent_cls.getName());
            continue;
        }
        //printd(5, "PythonQoreClass::populateClass() %s parent <- %s\n", qcls.getName(), parent_cls.getName());
        cls_set.insert(i, &parent_cls);
        populateClass(pypgm, parent_cls, cls_set, meth_set, false);
    }
}

PyObject* PythonQoreClass::wrap(QoreObject* obj) {
    PyQoreObject* self = (PyQoreObject*)py_type->tp_alloc(py_type, 0);
    obj->tRef();
    self->qobj = obj;
    // save a strong reference to the Qore object
    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    qore_python_pgm->saveQoreObjectFromPython(obj, xsink);
    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
    }
    //printd(5, "PythonQoreClass::wrap() obj: %p (%s)\n", obj, obj->getClassName());
    return (PyObject*)self;
}

PyObject* PythonQoreClass::exec_qore_method(PyObject* method_capsule, PyObject* args) {
    // Save the thread state Python had when calling us - must restore before returning
    PyThreadState* entry_tstate = _qore_safe_thread_state_get();

    QoreForeignThreadHelper qfth;

    // get method
    const QoreMethod* m = reinterpret_cast<const QoreMethod*>(PyCapsule_GetPointer(method_capsule, nullptr));
    assert(PyTuple_Check(args));
    QoreObject* obj;
    // check if this could be a static method call
    if (!PyTuple_Size(args)) {
        obj = nullptr;
    } else {
        // returns a borrowed reference
        PyObject* py_obj = PyTuple_GetItem(args, 0);
        if (!PyQoreObject_Check(py_obj)) {
            obj = nullptr;
        } else {
            obj = reinterpret_cast<PyQoreObject*>(py_obj)->qobj;
            // if the class is not accessible, do not make the call
            if (obj && obj->getClassAccess(*m->getClass()) > Private) {
                obj = nullptr;
            }
        }
    }

#if 0
    {
        QorePythonReferenceHolder argstr(PyObject_Repr(args));
        printd(5, "PythonQoreClass::exec_qore_method() %s::%s() obj: %p (%s) args: %s\n", m->getClassName(),
            m->getName(), obj, obj ? obj->getClassName() : "n/a", PyUnicode_AsUTF8(*argstr));
    }
#endif
    if (!obj) {
        // see if a static method with the same name is available
        ClassAccess access;
        const QoreMethod* static_meth = m->getClass()->findStaticMethod(m->getName(), access);
        if (!static_meth || access > Private) {
            QoreStringMaker desc("cannot call normal method '%s::%s()' without a 'self' object argument that " \
                "inherits '%s'", m->getClassName(), m->getName(), m->getClassName());
            PyErr_SetString(PyExc_ValueError, desc.c_str());
            return nullptr;
        }

        //printd(5, "about to call exec_qore_static_method()\n");
        return exec_qore_static_method(*static_meth, args, 0);
    }

    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    {
        // issue #4330: get Qore program from method if possible
        QoreProgram* pgm = m->getClass()->getProgram();
        if (!pgm) {
            pgm = qore_python_pgm->getQoreProgram();
        }

        QorePythonHelper qph(qore_python_pgm, &xsink);
        if (!xsink) {
            QoreExternalProgramContextHelper pch(&xsink, pgm);
            if (!xsink) {
                ReferenceHolder<QoreListNode> qargs(qore_python_pgm->getQoreListFromTuple(&xsink, args, 1), &xsink);
                if (!xsink) {
                    ValueHolder rv(&xsink);
                    {
                        QorePythonReleaseGilHelper prgh;

                        QorePythonStackLocationHelper slh(qph.getOldProgram());

                        rv = obj->evalMethod(*m, *qargs, &xsink);
                    }
                    if (!xsink) {
                        QorePythonReferenceHolder py_rv(qore_python_pgm->getPythonValue(*rv, &xsink));
                        if (!xsink) {
                            assert(py_rv);
                            return py_rv.release();
                        }
                    }
                }
            }
        }
    }

    // issue #4329: exception must be thrown in the calling context
    // IMPORTANT: Restore the entry thread state before setting the exception.
    // QorePythonHelper may have changed thread states, but Python expects the
    // exception to be on the thread state it passed when calling us.
    PyThreadState* exit_tstate = _qore_safe_thread_state_get();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }

    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    // Fallback - should not reach here normally
    return nullptr;
}

PyObject* PythonQoreClass::exec_qore_static_method(PyObject* method_capsule, PyObject* args) {
    printd(5, "exec_qore_static_method() args: %p\n", args);
    QoreForeignThreadHelper qfth;

    // get method
    const QoreMethod* m = reinterpret_cast<const QoreMethod*>(PyCapsule_GetPointer(method_capsule, nullptr));
    assert(PyTuple_Check(args));
#if 0
    {
        QorePythonReferenceHolder argstr(PyObject_Repr(args));
        assert(PyUnicode_Check(*argstr));
        printd(1, "PythonQoreClass::exec_qore_static_method() %s::%s() args: %s\n", m->getClassName(), m->getName(),
            PyUnicode_AsUTF8(*argstr));
    }
#endif

    return exec_qore_static_method(*m, args);
}

PyObject* PythonQoreClass::exec_qore_static_method(const QoreMethod& m, PyObject* args, size_t offset) {
    // Save the thread state Python had when calling us - must restore before returning
    PyThreadState* entry_tstate = _qore_safe_thread_state_get();

    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    {
        // issue #4330: get Qore program from method if possible
        QoreProgram* pgm = m.getClass()->getProgram();
        if (!pgm) {
            pgm = qore_python_pgm->getQoreProgram();
        }

        QorePythonHelper qph(qore_python_pgm, &xsink);
        if (!xsink) {
            QoreExternalProgramContextHelper pch(&xsink, pgm);
            if (!xsink) {
                ReferenceHolder<QoreListNode> qargs(qore_python_pgm->getQoreListFromTuple(&xsink, args, offset), &xsink);
                if (!xsink) {
                    ValueHolder rv(&xsink);
                    {
                        QorePythonReleaseGilHelper prgh;

                        QorePythonStackLocationHelper slh(qph.getOldProgram());

                        rv = QoreObject::evalStaticMethod(m, m.getClass(), *qargs, &xsink);
                    }
                    if (!xsink) {
                        QorePythonReferenceHolder py_rv(qore_python_pgm->getPythonValue(*rv, &xsink));
                        if (!xsink) {
                            assert(py_rv);
                            return py_rv.release();
                        }
                    }
                }
            }
        }
    }

    // issue #4329: exception must be thrown in the calling context
    // IMPORTANT: Restore the entry thread state before setting the exception.
    // QorePythonHelper may have changed thread states, but Python expects the
    // exception to be on the thread state it passed when calling us.
    PyThreadState* exit_tstate = _qore_safe_thread_state_get();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }

    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    // Fallback - should not reach here normally
    return nullptr;
}

int PythonQoreClass::py_init(PyObject* self, PyObject* args, PyObject* kwds) {
    assert(PyQoreObject_Check(self));
    assert(PyTuple_Check(args));
    //QorePythonReferenceHolder argstr(PyObject_Repr(args));
    //printd(5, "PythonQoreClass::py_init() self: %p '%s' args: %p (%d: %s) kwds: %p\n", self, Py_TYPE(self)->tp_name,
    //  args, (int)PyTuple_Size(args), PyUnicode_AsUTF8(*argstr), kwds);

    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();

    ExceptionSink xsink;

    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return -1;
    }

    // the current Qore class
    const QoreClass* qcls;
    // the Qore class to be constructed
    const QoreClass* constructor_cls;

    PyTypeObject* type = Py_TYPE(self);
    if (!PyQoreObjectType_Check(type)) {
        constructor_cls = findQoreClass(self);
        assert(type->tp_base);
        // create Qore type for Python type
        qcls = qore_python_pgm->getCreateQorePythonClass(&xsink, type);
        printd(5, "PythonQoreClass::py_init() self: %p type: %s got context pypgm: %p\n", self, type->tp_name,
            qore_python_pgm);
    } else {
        constructor_cls = qcls = getQoreClass(type);
    }

    PyQoreObject* pyself = reinterpret_cast<PyQoreObject*>(self);
    // returns a borrowed reference
    QoreObject* qobj = QorePythonImplicitQoreArgHelper::getQoreObject();
    //printd(5, "PythonQoreClass::py_init() self: %p py_cls: '%s' qcls: '%s' (%d) cq: '%s' (%d) qobj: %p args: %p\n",
    //  self, type->tp_name, qcls->getName(), qcls->getID(), constructor_cls->getName(), constructor_cls->getID(),
    //  qobj, args);
    if (qobj && qobj->getClass() == qcls) {
        qobj->tRef();
        pyself->qobj = qobj;
        return 0;
    }

    // issue #4044: do not allow abstract classes to be constructed
    if (!qcls->runtimeCheckInstantiateClass(&xsink)) {
        ReferenceHolder<QoreListNode> qargs(qore_python_pgm->getQoreListFromTuple(&xsink, args, 0, true), &xsink);
        if (!xsink) {
            QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
            if (!xsink) {
                QoreObject* created_obj = nullptr;
                {
                    QorePythonReleaseGilHelper prgh;

                    QorePythonStackLocationHelper slh(qore_python_pgm);

                    ReferenceHolder<QoreObject> qobj(constructor_cls->execConstructor(*qcls, *qargs, true, &xsink),
                        &xsink);
                    if (!xsink) {
                        printd(5, "PythonQoreClass::py_init() self: %p created Qore %s object (args: %p %d): %p (%s)\n",
                            self, qcls->getName(), *qargs, qargs ? (int)qargs->size() : 0, *qobj, qobj->getClassName());
                        created_obj = qobj.release();
                    }
                }
                // GIL is now held again, safe to call newQoreObject which calls Py_INCREF
                if (created_obj) {
                    return newQoreObject(xsink, pyself, created_obj, qcls == constructor_cls ? nullptr : qcls,
                        qore_python_pgm);
                }
            }
        }
    }

    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return -1;
}

int PythonQoreClass::newQoreObject(ExceptionSink& xsink, PyQoreObject* pyself, QoreObject* qobj,
        const QoreClass* qcls, QorePythonProgram* qore_python_pgm) {
    qobj->tRef();
    pyself->qobj = qobj;

    if (qcls) {
        // add private data for python class
        Py_INCREF(pyself);
        qobj->setPrivate(qcls->getID(), new QorePythonPrivateData((PyObject*)pyself));
    }
    // save a strong reference to the Qore object
    qore_python_pgm->saveQoreObjectFromPython(qobj, xsink);
    if (!xsink) {
        return 0;
    }

    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return -1;
}

PyObject* PythonQoreClass::py_getattro(PyObject* self, PyObject* attr) {
    // first try to get python attribute
    PyObject* pyrv = PyObject_GenericGetAttr(self, attr);
    if (pyrv) {
        return pyrv;
    }
    // if that failed, then clear the error and try to get a Qore member
    PyErr_Clear();

    assert(PyQoreObject_Check(self));
    assert(PyUnicode_Check(attr));
    const QoreClass* qcls = findQoreClass(self);
    if (!qcls) {
        return nullptr;
    }

    const char* member = PyUnicode_AsUTF8(attr);
    ExceptionSink xsink;
    QoreObject* obj = reinterpret_cast<PyQoreObject*>(self)->qobj;
    if (!obj) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    //printd(5, "PythonQoreClass::py_getattro() obj %p %s.%s\n", obj, qcls->getName(), member);
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (!xsink) {
        ValueHolder v(&xsink);
        {
            QorePythonReleaseGilHelper prgh;

            QorePythonStackLocationHelper slh(qore_python_pgm);

            v = obj->evalMember(member, &xsink);
        }
        printd(5, "PythonQoreClass::py_getattro() obj %p %s.%s = %s\n", obj, qcls->getName(), member, v->getFullTypeName());
        if (!xsink) {
            QorePythonReferenceHolder rv(qore_python_pgm->getPythonValue(*v, &xsink));
            if (!xsink) {
                return rv.release();
            }
        }
    }
    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return nullptr;
}

const QoreClass* PythonQoreClass::findQoreClass(PyObject* self) {
    PyTypeObject* type = Py_TYPE(self);
    // get base Qore class
    while (!PyQoreObjectType_Check(type)) {
        if (!type->tp_base) {
            ExceptionSink xsink;
            xsink.raiseException("QORE-ERROR", "cannot initialize Python class '%s' as a derived class of a Qore " \
                "base class; no Qore base class found", Py_TYPE(self)->tp_name);
            qore_python_pgm->raisePythonException(xsink);
            return nullptr;
        }
        type = type->tp_base;
    }
    return getQoreClass(type);
}

const QoreClass* PythonQoreClass::getQoreClass(PyTypeObject* type) {
    assert(type->tp_dict);
    PyObject* obj = PyDict_GetItemString(type->tp_dict, QCLASS_KEY);
    assert(obj);
    assert(PyCapsule_CheckExact(obj));
    const QoreClass* qcls = reinterpret_cast<const QoreClass*>(PyCapsule_GetPointer(obj, nullptr));
    assert(qcls);
    return qcls;
}

PyObject* PythonQoreClass::py_new(PyTypeObject* type, PyObject* args, PyObject* kw) {
    return type->tp_alloc(type, 0);
}

void PythonQoreClass::py_dealloc(PyQoreObject* self) {
    if (self->qobj) {
        self->qobj->tDeref();
        self->qobj = nullptr;
    }
    Py_TYPE(self)->tp_free(self);
}

PyObject* PythonQoreClass::py_repr(PyObject* obj) {
    QoreStringMaker str("Qore %s object %p", Py_TYPE(obj)->tp_name, obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}
