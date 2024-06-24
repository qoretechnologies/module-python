/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QorePythonClass.h

    Qore Programming Language python Module

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

#include "QorePythonClass.h"
#include "QorePythonProgram.h"

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

    const QoreStringNode* mname = args->retrieveEntry(0).get<QoreStringNode>();

    //printd(5, "QorePythonClass::methodGate() %s()\n", mname);
    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    if (!pypgm) {
        assert(*xsink);
        return QoreValue();
    }

    const QorePythonClass* cls = static_cast<const QorePythonClass*>(meth.getClass());
    return cls->callPythonMethod(xsink, pypgm, mname->c_str(), args, pd, 2);
}

// static method
QoreValue QorePythonClass::memberGate(const QoreMethod& meth, void* m, QoreObject* self, QorePythonPrivateData* pd,
    const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink) {
    assert(args && args->size() == 1);
    assert(args->retrieveEntry(0).getType() == NT_STRING);

    const QoreStringNode* mname = args->retrieveEntry(0).get<QoreStringNode>();

    QorePythonProgram* pypgm = QorePythonProgram::getPythonProgramFromMethod(meth, xsink);
    if (!pypgm) {
        assert(*xsink);
        return QoreValue();
    }

    const QorePythonClass* cls = static_cast<const QorePythonClass*>(meth.getClass());
    return cls->getPythonMember(pypgm, mname->c_str(), pd, xsink);
}

QoreValue QorePythonClass::callPythonMethod(ExceptionSink* xsink, QorePythonProgram* pypgm, const char* mname,
        const QoreListNode* args, QorePythonPrivateData* pd, size_t arg_offset) const {
    //printd(5, "QorePythonClass::callPythonMethod() %s::%s()\n", pname.c_str(), mname);
    PyObject* pyobj = pd->get();
    PyTypeObject* mtype = Py_TYPE(pyobj);
    QorePythonHelper qph(pypgm);
    if (pypgm->checkValid(xsink)) {
        return QoreValue();
    }
    // returns a borrowed reference
    PyObject* attr = PyDict_GetItemString(mtype->tp_dict, mname);
    if (!attr) {
        xsink->raiseException("METHOD-DOES-NOT-EXIST", "Python value of type '%s' has no method or member '%s'",
            mtype->tp_name, mname);
        return QoreValue();
    }
    return pypgm->callPythonMethod(xsink, attr, pyobj, args, 2);
}

QoreValue QorePythonClass::getPythonMember(QorePythonProgram* pypgm, const char* mname, QorePythonPrivateData* pd,
    ExceptionSink* xsink) const {
    QorePythonHelper qph(pypgm);
    if (pypgm->checkValid(xsink)) {
        return QoreValue();
    }

    {
        PyMemberDef* m = getPythonMember(mname);
        if (m) {
            return pypgm->getQoreValue(xsink, PyMember_GetOne((const char*)pd->get(), m));
        }
    }

    return pypgm->getQoreAttr(pd->get(), mname, xsink);
}
