/* -*- mode: c++; indent-tabs-mode: nil -*- */
/** @file QorePythonSignalWakeup.cpp interrupts the Qore calls of the main thread when Python receives a signal */
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

#include "QorePythonSignalWakeup.h"

#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace {
//! the state of the wakeup fd; the members that the reader thread uses are guarded by lck
struct SignalWakeupState {
    //! true while the wakeup fd is installed; written by the main thread with the GIL
    std::atomic<bool> installed{false};
    //! the Python thread identifier of the main thread
    unsigned long main_ident = 0;
    //! the pipe registered as the wakeup fd: read end and write end
    int pipe_r = -1;
    int pipe_w = -1;
    //! the wakeup fd set before, which is given the bytes written to the pipe, or -1
    int prev_fd = -1;
    //! the thread that reads the pipe
    pthread_t reader;
    //! true if the reader thread runs; guarded by lck (in a child of fork(), the reader thread does not exist, and
    //! the pipe is shared with the parent, whose reader thread reads it, so signals are handled when the Qore calls
    //! return)
    bool reader_running = false;
    //! true if the fork handlers locked lck (see install())
    bool fork_locked = false;

    //! guards the members below
    std::mutex lck;
    //! the number of Qore calls that the main thread runs (Qore code called from Python, which can call Python)
    int depth = 0;
    //! the Qore TID of the main thread, set when it runs Qore code
    int main_tid = 0;
    //! true if a signal cancelled the main thread's Qore call
    bool cancelled = false;

    //! true if the main thread's last Qore call was cancelled by a signal; used by the main thread only
    bool pending = false;
};

SignalWakeupState& get_state() {
    // not destroyed, so that it can be used while the process exits
    static SignalWakeupState* state = new SignalWakeupState;
    return *state;
}

bool is_main_thread(const SignalWakeupState& state) {
    return state.installed.load(std::memory_order_acquire) && PyThread_get_thread_ident() == state.main_ident;
}

void* reader_main(void* arg) {
    SignalWakeupState* state = static_cast<SignalWakeupState*>(arg);
    char buf[64];
    while (true) {
        ssize_t n = read(state->pipe_r, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // end of file: the write end was closed (see uninstall())
        if (n <= 0) {
            return nullptr;
        }
        // the wakeup fd set before gets the signal numbers as before; it is non-blocking, as Python requires
        if (state->prev_fd >= 0) {
            [[maybe_unused]] ssize_t rc = write(state->prev_fd, buf, n);
        }
        std::lock_guard<std::mutex> al(state->lck);
        // only a Qore call of the main thread is cancelled: a cancellation requested while the thread runs Python
        // code would apply to its next Qore call
        if (state->depth && !state->cancelled && !qore_cancel_thread(state->main_tid, "a signal was received")) {
            state->cancelled = true;
        }
    }
}

//! starts the reader thread; lck must be held
void start_reader(SignalWakeupState& state) {
    // if the thread cannot be created, signals are handled when the Qore calls return
    state.reader_running = !pthread_create(&state.reader, nullptr, reader_main, &state);
}

// the fork handlers: the lock is not held by the reader thread in the child, where the reader thread does not exist
void fork_prepare() {
    SignalWakeupState& state = get_state();
    if (state.installed.load()) {
        state.lck.lock();
        state.fork_locked = true;
    }
}

void fork_parent() {
    SignalWakeupState& state = get_state();
    if (state.fork_locked) {
        state.fork_locked = false;
        state.lck.unlock();
    }
}

void fork_child() {
    SignalWakeupState& state = get_state();
    if (state.fork_locked) {
        // the thread does not exist in the child: its handle is not used
        state.reader_running = false;
        state.cancelled = false;
        state.fork_locked = false;
        state.lck.unlock();
    }
}

//! calls signal.set_wakeup_fd(); returns the previous wakeup fd, or -2 with no Python exception set on error
int set_wakeup_fd(int fd) {
    QorePythonReferenceHolder signal_mod(PyImport_ImportModule("signal"));
    QorePythonReferenceHolder func(signal_mod ? PyObject_GetAttrString(*signal_mod, "set_wakeup_fd") : nullptr);
    QorePythonReferenceHolder args(func ? Py_BuildValue("(i)", fd) : nullptr);
    QorePythonReferenceHolder kwargs(args ? Py_BuildValue("{s:O}", "warn_on_full_buffer", Py_False) : nullptr);
    QorePythonReferenceHolder rv(kwargs ? PyObject_Call(*func, *args, *kwargs) : nullptr);
    long prev = rv ? PyLong_AsLong(*rv) : -2;
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return -2;
    }
    return static_cast<int>(prev);
}
}

void qore_python_signal_enter_qore() {
    QorePythonSignalWakeup::enterQore();
}

void qore_python_signal_leave_qore() {
    QorePythonSignalWakeup::leaveQore();
}

void QorePythonSignalWakeup::install() {
    SignalWakeupState& state = get_state();
    if (state.installed.load()) {
        return;
    }
    int fds[2];
    if (pipe(fds)) {
        return;
    }
    // the write end must be non-blocking (signal.set_wakeup_fd()); neither end is inherited by child processes
    if (fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK) || fcntl(fds[0], F_SETFD, FD_CLOEXEC)
        || fcntl(fds[1], F_SETFD, FD_CLOEXEC)) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    // fails (ex: ValueError) if the thread is not the main thread of the main interpreter
    int prev = set_wakeup_fd(fds[1]);
    if (prev == -2) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    state.pipe_r = fds[0];
    state.pipe_w = fds[1];
    state.prev_fd = prev;
    state.main_ident = PyThread_get_thread_ident();
    {
        std::lock_guard<std::mutex> al(state.lck);
        start_reader(state);
    }
    static bool fork_handlers = false;
    if (!fork_handlers) {
        fork_handlers = !pthread_atfork(fork_prepare, fork_parent, fork_child);
    }
    state.installed.store(true, std::memory_order_release);
}

void QorePythonSignalWakeup::uninstall() {
    SignalWakeupState& state = get_state();
    if (!state.installed.load()) {
        return;
    }
    state.installed.store(false, std::memory_order_release);
    // the previous wakeup fd is restored, unless the application has set another one since
    int current = set_wakeup_fd(state.prev_fd);
    if (current >= 0 && current != state.pipe_w) {
        set_wakeup_fd(current);
    }
    // closing the write end ends the reader thread
    close(state.pipe_w);
    bool running;
    {
        std::lock_guard<std::mutex> al(state.lck);
        running = state.reader_running;
        state.reader_running = false;
    }
    if (running) {
        QorePythonReleaseGilHelper prgh;
        pthread_join(state.reader, nullptr);
    }
    close(state.pipe_r);
    state.pipe_r = state.pipe_w = state.prev_fd = -1;
}

void QorePythonSignalWakeup::enterQore() {
    SignalWakeupState& state = get_state();
    if (!is_main_thread(state)) {
        return;
    }
    std::lock_guard<std::mutex> al(state.lck);
    if (!state.depth++) {
        state.main_tid = q_gettid();
        state.pending = false;
    }
}

void QorePythonSignalWakeup::leaveQore() {
    SignalWakeupState& state = get_state();
    if (!is_main_thread(state)) {
        return;
    }
    std::lock_guard<std::mutex> al(state.lck);
    if (state.depth && !--state.depth && state.cancelled) {
        // the cancellation applied to the Qore call that the thread has left
        state.cancelled = false;
        state.pending = true;
        qore_clear_thread_cancel();
    }
}

bool QorePythonSignalWakeup::checkSignal() {
    SignalWakeupState& state = get_state();
    if (!is_main_thread(state) || !state.pending) {
        return false;
    }
    state.pending = false;
    // runs the Python signal handlers; KeyboardInterrupt is raised by the default SIGINT handler
    return PyErr_CheckSignals() < 0;
}
