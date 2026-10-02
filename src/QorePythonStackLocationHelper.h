/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QorePythonStackLocationHelper.h

    Qore Programming Language

    Copyright (C) 2020 - 2026 Qore Technologies, s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

    Note that the Qore library is released under a choice of three open-source
    licenses: MIT (as above), LGPL 2+, or GPL 2+; see README-LICENSE for more
    information.
*/

#ifndef _QORE_QOREPYTHONSTACKLOCATIONHELPER_H

#define _QORE_QOREPYTHONSTACKLOCATIONHELPER_H

#include "python-module.h"

// forward reference
class QorePythonProgram;

class QorePythonStackLocationHelper : public QoreExternalRuntimeStackLocationHelper {
public:
    DLLLOCAL QorePythonStackLocationHelper(QorePythonProgram* py_pgm);

    //! returns the name of the function or method call
    DLLLOCAL virtual const std::string& getCallName() const;

    //! returns the call type
    DLLLOCAL virtual qore_call_t getCallType() const;

    //! returns the source location of the element
    DLLLOCAL virtual const QoreProgramLocation& getLocation() const;

    //! returns the next location in the stack or nullptr if there is none
    DLLLOCAL virtual const QoreStackLocation* getNext() const;

    // Normalize path with os.path.normpath(); returns a new reference
    DLLLOCAL static PyObject* normalizePath(const char* path);

    // Normalize path with os.path.normpath(); steals path_obj and returns a new reference
    DLLLOCAL static PyObject* normalizePath(PyObject* path_obj);

private:
    QorePythonProgram* py_pgm;
    int tid = q_gettid();
    mutable unsigned current = 0;

    mutable std::vector<std::string> stack_call;
    mutable std::vector<QoreExternalProgramLocationWrapper> stack_loc;

    mutable bool init = false;

    DLLLOCAL static std::string python_no_call_name;
    DLLLOCAL static QoreExternalProgramLocationWrapper python_loc_builtin;

    DLLLOCAL size_t size() const {
        return stack_call.size();
    }

    DLLLOCAL void checkInit() const;
};

#endif