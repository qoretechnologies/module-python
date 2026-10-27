#!/usr/bin/env qore
# -*- mode: qore; indent-tabs-mode: nil -*-
# Copyright (C) 2026 Qore Technologies, s.r.o.

# run by python.qtest: a Qore thread that called a Python program ends while a thread of another Python program runs
# Python code without blocking.  When the thread ends, its idle thread state is deleted with the GIL; the end must not
# wait until that Python code ends.  The spinning thread is bounded, so the script ends in any case; it prints
# "stopped" if it was stopped after the Qore thread had ended, or "bound" if it reached its bound first, which means
# that the thread's end waited for it.

%modern

%requires python

# the iterations after which the spinning thread stops by itself: much longer than a thread that is not blocked takes
# to end
const SpinBound = 300000000;

# the spinning thread is started by spin(), which returns once it runs; end() stops it and returns the reason why it
# stopped
const SpinSource = sprintf("
import threading
stop = False
thread = None
reason = None
def spin():
    started = threading.Event()
    def run():
        global reason
        started.set()
        i = 0
        while not stop and i < %d:
            i += 1
        reason = 'stopped' if i < %d else 'bound'
    global thread
    thread = threading.Thread(target=run)
    thread.start()
    started.wait()
def end():
    global stop
    stop = True
    thread.join()
    return reason
", SpinBound, SpinBound);

#! prints the reason why the spinning thread stopped when the script's Program is deleted
/** The Program is deleted after all the threads that Qore started have ended, including their cleanup, in which the
    idle thread state of the Python module is deleted
*/
class GilThreadExitReport {
    private {
        PythonProgram spinner;
    }

    constructor(PythonProgram spinner) {
        self.spinner = spinner;
    }

    destructor() {
        printf("%s\n", spinner.callFunction("end"));
    }
}

our PythonProgram spinner(SpinSource, "gil-exit-spin.py");
our PythonProgram callee("def f():\n    return 'called'\n", "gil-exit-call.py");
our GilThreadExitReport report(spinner);

spinner.callFunction("spin");
# the thread calls the other program, so that it has an idle thread state, and ends while the spinning thread runs
background sub () {
    callee.callFunction("f");
}();
