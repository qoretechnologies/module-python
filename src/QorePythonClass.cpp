/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QorePythonClass.h

    Qore Programming Language python Module

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

#include "QorePythonClass.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"

#include <structmember.h>

type_vec_t QorePythonClass::gateParamTypeInfo = { stringTypeInfo };

static constexpr const char* PYOBJ_KEY = "__$PYCLS__";

QorePythonClass::QorePythonClass(const char* name, const char* path)
        : QoreBuiltinClass(name, path, QDOM_UNCONTROLLED_API), pypgm(nullptr) {
}

QorePythonClass::QorePythonClass(QorePythonProgram* pypgm, const char* name, const char* path) :
        QoreBuiltinClass(name, path, QDOM_UNCONTROLLED_API), pypgm(pypgm) {
    pypgm->weakRef();
    addMethod(nullptr, "memberGate", (q_external_method_t)memberGate, Public, QCF_NO_FLAGS, QDOM_UNCONTROLLED_API,
        autoTypeInfo, gateParamTypeInfo);
    addMethod(nullptr, "methodGate", (q_external_method_t)methodGate, Public, QCF_USES_EXTRA_ARGS, QDOM_UNCONTROLLED_API,
        autoTypeInfo, gateParamTypeInfo);

    addMember(PYOBJ_KEY, Internal, autoTypeInfo);
    // assignments to the members of the object and their removal and deletion are Python attribute writes
    setMemberWriteGate(memberWriteGate);

    setPublicMemberFlag();
}

QorePythonClass::QorePythonClass(const QorePythonClass& old) : QoreBuiltinClass(old), pypgm(old.pypgm) {
    if (pypgm) {
        pypgm->weakRef();
    }
}

QorePythonClass::~QorePythonClass() {
    if (pypgm) {
        pypgm->weakDeref();
    }
}

void QorePythonClass::addObj(PyObject* obj) {
    pypgm->addObj(obj);
}

PyObject* QorePythonClass::getPyObject(QoreObject* self, ExceptionSink* xsink) const {
    ValueHolder v(self->getReferencedMemberNoMethod(PYOBJ_KEY, this, xsink), xsink);
    if (*xsink) {
        assert(!v);
        return nullptr;
    }
    if (!v) {
        return nullptr;
    }
    if (v->getType() != NT_INT) {
        xsink->raiseException("PYTHON-OBJECT-ERROR", "invalid type '%s' saved to internal data key '%s'",
            v->getFullTypeName(), PYOBJ_KEY);
        return nullptr;
    }
    return (PyObject*)v->getAsBigInt();
}

int QorePythonClass::setPyObject(QoreObject* self, PyObject* pyself, ExceptionSink* xsink) const {
    return self->setMemberValue(PYOBJ_KEY, this, (int64)pyself, xsink);
}

// static method
QoreValue QorePythonClass::methodGate(const QoreMethod& meth, void* m, QoreObject* self, QorePythonPrivateData* pd,
    const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    assert(args && args->size() >= 1);
    assert(args->retrieveEntry(0).getType() == NT_STRING);

    QoreStringValueHelper mname(args->retrieveEntry(0));

    //printd(5, "QorePythonClass::methodGate() %s()\n", mname);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    if (!pypgm) {
        assert(*xsink);
        return QoreValue();
    }

    const QorePythonClass* cls = static_cast<const QorePythonClass*>(meth.getClass());
    // the arguments of the call follow the method name
    return cls->callPythonMethod(xsink, pypgm, mname->c_str(), args, pd, 1);
}

// static method
QoreValue QorePythonClass::memberGate(const QoreMethod& meth, void* m, QoreObject* self, QorePythonPrivateData* pd,
    const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    assert(args && args->size() == 1);
    assert(args->retrieveEntry(0).getType() == NT_STRING);

    QoreStringValueHelper mname(args->retrieveEntry(0));

    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    if (!pypgm) {
        assert(*xsink);
        return QoreValue();
    }

    const QorePythonClass* cls = static_cast<const QorePythonClass*>(meth.getClass());
    return cls->getPythonMember(pypgm, mname->c_str(), pd, xsink);
}

// static function
QoreValue QorePythonClass::memberWriteGate(QoreObject* self, AbstractPrivateData* data, const char* member,
        QoreValue value, QoreMemberWriteOp operation, ExceptionSink* xsink) {
    QorePythonPrivateData* pd = static_cast<QorePythonPrivateData*>(data);
    QorePythonProgram* pypgm = pd->getOwner();
    if (!pypgm) {
        xsink->raiseException("PYTHON-OBJECT-ERROR", "cannot write member '%s': the object has no Python object",
            member);
        return QoreValue();
    }
    QorePythonHelper qph(pypgm, xsink);
    if (*xsink || pypgm->checkValid(xsink)) {
        return QoreValue();
    }
    PyObject* pyobj = pd->get();
    assert(pyobj);

    if (operation == QoreMemberWriteOp::Assign) {
        QorePythonReferenceHolder pyval(pypgm->getPythonValue(value, xsink));
        if (*xsink) {
            return QoreValue();
        }
        int rc;
        {
            // attribute writes can run Python code (ex: properties), which can call Qore
            QorePythonEntryFrame entry;
            rc = PyObject_SetAttrString(pyobj, member, *pyval);
        }
        if (rc < 0) {
            pypgm->checkPythonException(xsink);
        }
        return QoreValue();
    }

    // removal and deletion return the previous value; a missing attribute has no value, as a missing member
    QorePythonReferenceHolder old_value;
    {
        QorePythonEntryFrame entry;
        old_value = PyObject_GetAttrString(pyobj, member);
    }
    if (!old_value) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            pypgm->checkPythonException(xsink);
        } else {
            PyErr_Clear();
        }
        return QoreValue();
    }
    ValueHolder rv(pypgm->getQoreValue(xsink, old_value), xsink);
    if (*xsink) {
        return QoreValue();
    }
    int rc;
    {
        QorePythonEntryFrame entry;
        rc = PyObject_DelAttrString(pyobj, member);
    }
    if (rc < 0) {
        pypgm->checkPythonException(xsink);
        return QoreValue();
    }
    return rv.release();
}

QoreValue QorePythonClass::callPythonMethod(ExceptionSink* xsink, QorePythonProgram* pypgm, const char* mname,
        const QoreListNode* args, QorePythonPrivateData* pd, size_t arg_offset) const {
    //printd(5, "QorePythonClass::callPythonMethod() %s::%s()\n", pname.c_str(), mname);
    QorePythonHelper qph(pypgm, xsink);
    if (*xsink || pypgm->checkValid(xsink)) {
        return QoreValue();
    }
    PyObject* pyobj = pd->get();
    assert(pyobj);
    // the attribute of the object, as in Python: a method bound to the object, a class method bound to its class, a
    // static method, or a callable attribute of the object (ex: a lambda or a functools.partial object)
    QorePythonReferenceHolder attr;
    {
        // attribute reads can run Python code (ex: __getattr__()), which can call Qore
        QorePythonEntryFrame entry;
        attr = PyObject_GetAttrString(pyobj, mname);
    }
    if (!attr) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
            pypgm->checkPythonException(xsink);
            return QoreValue();
        }
        PyErr_Clear();
        xsink->raiseException("METHOD-DOES-NOT-EXIST", "Python value of type '%s' has no method or member '%s'",
            Py_TYPE(pyobj)->tp_name, mname);
        return QoreValue();
    }
    if (!PyCallable_Check(*attr)) {
        xsink->raiseException("METHOD-DOES-NOT-EXIST", "the attribute '%s' of the Python value of type '%s' is not "
            "callable (type '%s')", mname, Py_TYPE(pyobj)->tp_name, Py_TYPE(*attr)->tp_name);
        return QoreValue();
    }
    return pypgm->callInternal(xsink, *attr, args, arg_offset);
}

QoreValue QorePythonClass::getPythonMember(QorePythonProgram* pypgm, const char* mname, QorePythonPrivateData* pd,
    ExceptionSink* xsink) const {
    QorePythonHelper qph(pypgm, xsink);
    if (*xsink || pypgm->checkValid(xsink)) {
        return QoreValue();
    }

    {
        PyMemberDef* m = getPythonMember(mname);
        if (m) {
            QorePythonReferenceHolder value(PyMember_GetOne(reinterpret_cast<const char*>(pd->get()), m));
            if (!value && pypgm->checkPythonException(xsink)) {
                return QoreValue();
            }
            return pypgm->getQoreValue(xsink, value);
        }
    }

    return pypgm->getQoreAttr(pd->get(), mname, xsink);
}
