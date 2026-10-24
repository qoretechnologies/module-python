
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

#ifndef _QORE_PYTHON_QOREPYTHONCLASS_H

#define _QORE_PYTHON_QOREPYTHONCLASS_H

#include "python-module.h"
#include "QorePythonPrivateData.h"

#include <string>
#include <vector>
#include <memory>
#include <map>

// forward references
class QorePythonProgram;

//! Represents a Python class in Qore
class QorePythonClass : public QoreBuiltinClass {
public:
    //! only for the base python class
    DLLLOCAL QorePythonClass(const char* name, const char* path);

    DLLLOCAL QorePythonClass(QorePythonProgram* pypgm, const char* name, const char* path);

    DLLLOCAL QorePythonClass(const QorePythonClass& old);

    DLLLOCAL virtual ~QorePythonClass();

    //! Called when a class is copied for import
    DLLLOCAL virtual QoreClass* copyImport() {
        return new QorePythonClass;
    }

    //! Called when a class is copied
    DLLLOCAL virtual QoreClass* copy() {
        return new QorePythonClass(*this);
    }

    DLLLOCAL void addObj(PyObject* obj);

    DLLLOCAL void addPythonMember(std::string member, PyMemberDef* memdef) {
        assert(mem_map.find(member) == mem_map.end());
        mem_map[member] = memdef;
    }

    DLLLOCAL PyMemberDef* getPythonMember(std::string member) const {
        mem_map_t::const_iterator i = mem_map.find(member);
        /*
        if (i == mem_map.end()) {
            for (auto& ix : mem_map) {
                printd(0, "MEM %s\n", ix.first.c_str());
            }
        }
        */
        return i == mem_map.end() ? nullptr : i->second;
    }

    DLLLOCAL QorePythonProgram* getPythonProgram() const {
        return pypgm;
    }

    DLLLOCAL PyObject* getPyObject(QoreObject* self, ExceptionSink* xsink) const;

    DLLLOCAL int setPyObject(QoreObject* self, PyObject* pyself, ExceptionSink* xsink) const;

    DLLLOCAL QoreValue getPythonMember(QorePythonProgram* pypgm, const char* mname, QorePythonPrivateData* pd,
        ExceptionSink* xsink) const;

    DLLLOCAL QoreValue callPythonMethod(ExceptionSink* xsink, QorePythonProgram* pypgm, const char* mname, const QoreListNode* args,
        QorePythonPrivateData* pd, size_t arg_offset = 0) const;

    DLLLOCAL static QoreValue memberGate(const QoreMethod& meth, void* m, QoreObject* self, QorePythonPrivateData* pd,
        const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink);

    //! Assigns, removes or deletes a Python attribute of the object for a member that the class does not declare
    /** @return NOTHING for an assignment, or the previous value of the attribute for removal and deletion (NOTHING if
        the object does not have it)
    */
    DLLLOCAL static QoreValue memberWriteGate(QoreObject* self, AbstractPrivateData* data, const char* member,
        QoreValue value, QoreMemberWriteOp operation, ExceptionSink* xsink);

    DLLLOCAL static QoreValue methodGate(const QoreMethod& meth, void* m, QoreObject* self, QorePythonPrivateData* pd,
        const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink);

private:
    QorePythonProgram* pypgm = nullptr;

    // map of builtin members: name -> member
    typedef std::map<std::string, PyMemberDef*> mem_map_t;
    mem_map_t mem_map;
    std::string pname;

    static type_vec_t gateParamTypeInfo;

    DLLLOCAL QorePythonClass() {
    }
};

#endif
