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

#include <string>
#include <vector>
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

    // no Python stack can be read if Python cannot be used
    if (!python_shutdown) {
        readPythonStack();
    }

    if (!size()) {
        stack_call.push_back(python_no_call_name);
        stack_loc.push_back(python_loc_builtin);
    }
}

void QorePythonStackLocationHelper::readPythonStack() const {
    QorePythonHelper qph(py_pgm);
    // the context cannot be acquired if the program is deleted by another thread (ex: an exception raised in this
    // thread when the deletion cancels its Qore call): the Python stack cannot be read without it
    if (!qph.isValid() || !qore_python_attached_thread_state()) {
        return;
    }

    // Resolve callables in the current interpreter. A cached main-interpreter
    // function carries its module dictionary and code objects into other interpreters.
    QorePythonReferenceHolder sys(PyImport_ImportModule("sys"));
    QorePythonReferenceHolder getframe(sys ? PyObject_GetAttrString(*sys, "_getframe") : nullptr);
    if (!getframe) {
        PyErr_Clear();
    }
    // start with the innermost frame (depth 0): the Python frame that called Qore
    int depth = 0;
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
        // get line number
        QorePythonReferenceHolder code((PyObject*)PyFrame_GetCode(frame));
        line = PyCode_Addr2Line((PyCodeObject*)*code, PyFrame_GetLasti(frame));
        QorePythonReferenceHolder filename_obj(PyObject_GetAttrString(*code, "co_filename"));
        filename = PyUnicode_AsUTF8(*filename_obj);

        QorePythonReferenceHolder funcname_obj(PyObject_GetAttrString(*code, "co_qualname"));
        funcname = PyUnicode_AsUTF8(*funcname_obj);
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
}

PyObject* QorePythonStackLocationHelper::normalizePath(const char* path) {
    // normalize path
    QorePythonReferenceHolder path_obj(PyUnicode_FromString(path));
    return normalizePath(path_obj.release());
}

//! normalizes a POSIX path as posixpath.normpath() does: removes redundant separators and "." components, and
//! resolves ".." components where possible
static std::string qore_python_normpath(const std::string& path) {
    if (path.empty()) {
        return ".";
    }
    // POSIX keeps two initial slashes, and one for one or more than two
    size_t initial = 0;
    while (initial < path.size() && path[initial] == '/') {
        ++initial;
    }
    if (initial > 2) {
        initial = 1;
    }
    std::vector<std::string> comps;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        std::string comp = path.substr(start, end - start);
        if (comp.empty() || comp == ".") {
            // skipped
        } else if (comp != "..") {
            comps.push_back(comp);
        } else if (!comps.empty() && comps.back() != "..") {
            comps.pop_back();
        } else if (!initial) {
            // ".." above the start of a relative path is kept; above the root, it is dropped
            comps.push_back(comp);
        }
        start = end + 1;
    }
    std::string rv(initial, '/');
    for (size_t i = 0; i < comps.size(); ++i) {
        if (i) {
            rv += '/';
        }
        rv += comps[i];
    }
    return rv.empty() ? "." : rv;
}

PyObject* QorePythonStackLocationHelper::normalizePath(PyObject* path_obj) {
    QorePythonReferenceHolder argument(path_obj);
    if (!argument) {
        return nullptr;
    }
    // normalized in C++: calling os.path.normpath(), which is Python code with Python 3.11, would run pending signal
    // handlers, whose exceptions would be lost (ex: KeyboardInterrupt while the stack of a Qore exception is read)
    Py_ssize_t size;
    const char* str = PyUnicode_Check(*argument) ? PyUnicode_AsUTF8AndSize(*argument, &size) : nullptr;
    if (!str) {
        PyErr_Clear();
        return nullptr;
    }
    std::string normalized = qore_python_normpath(std::string(str, size));
    return PyUnicode_FromStringAndSize(normalized.c_str(), normalized.size());
}
