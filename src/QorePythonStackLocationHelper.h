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

#include <vector>

// forward reference
class QorePythonProgram;

//! Marks a call from Qore into Python code on the current thread; must be created with the GIL held
/** Records the Python frame that is current when Qore calls Python code: the Python frames of a
    QorePythonStackLocationHelper (the Python code that called Qore) are the frames of its call from Qore, which end at
    the frame that is current when the call from Qore starts, and begin at the frame that is current when the next
    call from Qore starts; the
    Python stack of a thread is continuous across nested calls between Qore and Python, so that without these limits
    each call was given the Python frames of all the calls (issue 5533)
*/
class QorePythonEntryFrame {
public:
    DLLLOCAL QorePythonEntryFrame();
    DLLLOCAL ~QorePythonEntryFrame();

    QorePythonEntryFrame(const QorePythonEntryFrame&) = delete;
    QorePythonEntryFrame& operator=(const QorePythonEntryFrame&) = delete;

    //! a call from Qore into Python code
    struct Entry {
        //! the Python frame that is current when the call starts (borrowed: it exists while the call runs), if any
        PyObject* frame;
        //! the interpreter of the call
        PyInterpreterState* interp;
    };

    //! returns the number of calls from Qore into Python code on the current thread
    DLLLOCAL static size_t getCount() {
        return entries.size();
    }

    //! returns the calls from Qore into Python code on the current thread
    DLLLOCAL static const std::vector<Entry>& getEntries() {
        return entries;
    }

private:
    //! false if the call could not be recorded (memory error)
    bool recorded = false;

    DLLLOCAL static thread_local std::vector<Entry> entries;
};

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
    //! the number of calls from Qore into Python code when the Python code called Qore: the last one is the call of
    //! this Python code
    size_t entry_count = QorePythonEntryFrame::getCount();
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

    //! reads the frames of the Python stack of the current thread, if the context of the program can be acquired
    DLLLOCAL void readPythonStack() const;
};

#endif