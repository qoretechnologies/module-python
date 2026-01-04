/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    PythonCallableCallReferenceNode.cpp

    Qore Programming Language

    Copyright (C) 2020 - 2021 Qore Technologies, s.r.o.

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

#include "PythonCallableCallReferenceNode.h"
#include "QorePythonProgram.h"

PythonCallableCallReferenceNode::PythonCallableCallReferenceNode(QorePythonProgram* pypgm, PyObject* val, PyObject* self)
        : pypgm(pypgm), val(val), self(self) {
}

QoreValue PythonCallableCallReferenceNode::execValue(const QoreListNode* args, ExceptionSink* xsink) const {
    //printd(5, "PythonCallableCallReferenceNode::execValue() f: %p self: %p\n", *val, *self);
    QorePythonHelper qph(*pypgm, xsink);
    if (*xsink) {
        return QoreValue();
    }
    //QorePythonGilHelper qpgh;
    return pypgm->callInternal(xsink, *val, args, 0, *self);
}

int PythonCallableCallReferenceNode::getAsString(QoreString& str, int foff, ExceptionSink* xsink) const {
    str.sprintf("python callable %p", *val);
    return 0;
}

QoreString* PythonCallableCallReferenceNode::getAsString(bool& del, int foff, ExceptionSink* xsink) const {
    del = true;
    QoreString* rv = new QoreString;
    getAsString(*rv, foff, xsink);
    return rv;
}
