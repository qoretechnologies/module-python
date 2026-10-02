/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file QorePythonProgram.cpp defines the QorePythonProgram class */
/*
    QorePythonStackLocationHelper.cpp

    Qore Programming Language

    Copyright 2020 - 2026 Qore Technologies, s.r.o.

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

#include "QorePythonStackLocationHelper.h"
#include "QorePythonProgram.h"

#include <frameobject.h>

std::string QorePythonStackLocationHelper::python_no_call_name = "<python_module_no_runtime_stack_info>";
QoreExternalProgramLocationWrapper QorePythonStackLocationHelper::python_loc_builtin("<python_module_unknown>", -1,
    -1);

QorePythonStackLocationHelper::QorePythonStackLocationHelper(QorePythonProgram* py_pgm) : py_pgm(py_pgm) {
}

const std::string& QorePythonStackLocationHelper::getCallName() const {
    if (tid != q_gettid()) {
        return python_no_call_name;
    }
    checkInit();
    assert((unsigned)current < size());
    //printd(5, "QorePythonStackLocationHelper::getCallName() this: %p %d/%d '%s'\n", this, (int)current, (int)size,
    //    stack_call[current].c_str());
    return stack_call[current];
}

qore_call_t QorePythonStackLocationHelper::getCallType() const {
    if (tid != q_gettid()) {
        return CT_BUILTIN;
    }
    checkInit();
    assert((unsigned)current < size());
    return CT_USER;
}

const QoreProgramLocation& QorePythonStackLocationHelper::getLocation() const {
    if (tid != q_gettid()) {
        return python_loc_builtin.get();
    }
    checkInit();
    assert((unsigned)current < size());
    //printd(5, "QorePythonStackLocationHelper::getLocation() %s:%d (%s)\n", stack_loc[current].getFile(), stack_loc[current].getStartLine());
    return stack_loc[current].get();
}

const QoreStackLocation* QorePythonStackLocationHelper::getNext() const {
    if (tid != q_gettid()) {
        return stack_next;
    }
    checkInit();
    assert((unsigned)current < size());
    // issue #3169: reset the pointer after iterating all the information in the stack
    // the exception stack can be iterated multiple times
    ++current;
    if ((unsigned)current < size()) {
        return this;
    }
    current = 0;
    return stack_next;
}

void QorePythonStackLocationHelper::checkInit() const {
    assert(tid == q_gettid());
    if (init) {
        return;
    }
    init = true;

    QorePythonHelper qph(py_pgm);

    // Resolve callables in the current interpreter. A cached main-interpreter
    // function carries its module dictionary and code objects into other interpreters.
    QorePythonReferenceHolder sys(PyImport_ImportModule("sys"));
    QorePythonReferenceHolder getframe(sys ? PyObject_GetAttrString(*sys, "_getframe") : nullptr);
    if (!getframe) {
        PyErr_Clear();
    }
    // start at depth = 1 or the first two entries will be identical
    int depth = 1;
    while (getframe) {
        QorePythonReferenceHolder args(PyTuple_New(1));
        PyTuple_SET_ITEM(*args, 0, PyLong_FromLong(depth));

        QorePythonReferenceHolder frame_obj(PyObject_CallObject(*getframe, *args));
        if (PyErr_Occurred()) {
            PyErr_Clear();
            break;
        }

        PyFrameObject* frame = reinterpret_cast<PyFrameObject*>(*frame_obj);

        const char* filename;
        int line;
        const char* funcname;
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION > 10
        // get line number
        QorePythonReferenceHolder code((PyObject*)PyFrame_GetCode(frame));
        line = PyCode_Addr2Line((PyCodeObject*)*code, PyFrame_GetLasti(frame));
        QorePythonReferenceHolder filename_obj(PyObject_GetAttrString(*code, "co_filename"));
        filename = PyUnicode_AsUTF8(*filename_obj);

        QorePythonReferenceHolder funcname_obj(PyObject_GetAttrString(*code, "co_qualname"));
        funcname = PyUnicode_AsUTF8(*funcname_obj);
#else
        line = PyCode_Addr2Line(frame->f_code, frame->f_lasti);
        filename = QorePythonProgram::getCString(frame->f_code->co_filename);
        funcname = QorePythonProgram::getCString(frame->f_code->co_name);
#endif
        // get normalized path
        QorePythonReferenceHolder np_obj(normalizePath(filename));
        if (!np_obj) {
            break;
        }
        filename = QorePythonProgram::getCString(*np_obj);

        QoreExternalProgramLocationWrapper loc(filename, line, line, nullptr, 0, QORE_PYTHON_LANG_NAME);
        stack_loc.push_back(loc);
        stack_call.push_back(funcname);
        ++depth;
    }

    if (!size()) {
        stack_call.push_back(python_no_call_name);
        stack_loc.push_back(python_loc_builtin);
    }
}

PyObject* QorePythonStackLocationHelper::normalizePath(const char* path) {
    // normalize path
    QorePythonReferenceHolder path_obj(PyUnicode_FromString(path));
    return normalizePath(path_obj.release());
}

PyObject* QorePythonStackLocationHelper::normalizePath(PyObject* path_obj) {
    QorePythonReferenceHolder argument(path_obj);
    if (!argument) {
        return nullptr;
    }
    QorePythonReferenceHolder path_module(PyImport_ImportModule("os.path"));
    QorePythonReferenceHolder normpath(path_module ? PyObject_GetAttrString(*path_module, "normpath") : nullptr);
    if (!normpath) {
        PyErr_Clear();
        return nullptr;
    }
    QorePythonReferenceHolder normpath_args(PyTuple_Pack(1, *argument));
    if (!normpath_args) {
        return nullptr;
    }
    QorePythonReferenceHolder normalized(PyObject_CallObject(*normpath, *normpath_args));
    if (!normalized) {
        PyErr_Clear();
    }
    return normalized.release();
}
