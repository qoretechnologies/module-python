/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file QorePythonSignalWakeup.h interrupts the Qore calls of the main thread when Python receives a signal */
/*
    Qore Programming Language python Module

    Copyright (C) 2026 Qore Technologies, s.r.o.

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

#ifndef _QORE_PYTHON_QOREPYTHONSIGNALWAKEUP_H
#define _QORE_PYTHON_QOREPYTHONSIGNALWAKEUP_H

#include "python-module.h"

//! Interrupts the Qore calls that the main thread makes from Python when a signal arrives
/** When Python is the host (\c import \c qoreloader), Qore runs without its signal handling, and Python's C signal
    handler only sets a flag that the main thread checks when it runs Python code: a Qore call that blocks (ex:
    \c Queue::get()) would not be interrupted by \c SIGINT.  A pipe is registered with \c signal.set_wakeup_fd(), and
    a native thread that reads it cancels the Qore call of the main thread (\c qore_cancel_thread()); when the call's
    exception is raised in Python, Python's signal handlers run, and an exception raised by a handler (ex:
    \c KeyboardInterrupt) replaces the Qore exception.

    A wakeup fd set before is given the bytes written to the pipe; a wakeup fd set later replaces the pipe.
*/
class QorePythonSignalWakeup {
public:
    //! installs the wakeup fd; called with the GIL by the main thread of the main interpreter
    /** Does nothing if it cannot be installed (ex: \c signal.set_wakeup_fd() fails); never raises an exception
    */
    DLLLOCAL static void install();

    //! restores the previous wakeup fd and stops the reader thread; called with the GIL by the main thread
    DLLLOCAL static void uninstall();

    //! called when the current thread runs Qore code without the GIL
    DLLLOCAL static void enterQore();

    //! called when the current thread returns to Python after running Qore code
    DLLLOCAL static void leaveQore();

    //! called with the GIL when a Qore exception is raised in Python
    /** @return true if the main thread's Qore call was cancelled by a signal and a Python signal handler raised an
        exception, which is set and replaces the Qore exception
    */
    DLLLOCAL static bool checkSignal();
};

#endif
