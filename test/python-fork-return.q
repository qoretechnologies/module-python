#!/usr/bin/env qore
# -*- mode: qore; indent-tabs-mode: nil -*-
# Copyright (C) 2026 Qore Technologies, s.r.o.

# run by python-fork.qtest: Python calls a Qore closure that calls fork() while another Python thread holds the GIL;
# in the child, the closure cannot return to Python code, and the child exits with a message on stderr

%modern
%no-child-restrictions

%requires python
%requires FsUtil

TmpDir tmp("python-fork-return-");
string go = tmp.path + DirSep + "go";
string ready = tmp.path + DirSep + "ready";
string block = tmp.path + DirSep + "block";

# the thread waits in os.read() (without the GIL) until it is told to go, then holds the GIL in C calls
# (ctypes.PyDLL does not release it): it signals that it holds the GIL and blocks in a read until the parent writes
PythonProgram p("
import ctypes
import os
from threading import Thread

def make_fifos(*paths):
    for path in paths:
        os.mkfifo(path)

def hold_gil(go, ready, block):
    libc = ctypes.PyDLL(None)
    go_fd = os.open(go, os.O_RDWR)
    ready_fd = os.open(ready, os.O_RDWR)
    block_fd = os.open(block, os.O_RDWR)
    buf = ctypes.create_string_buffer(1)
    def target():
        os.read(go_fd, 1)
        libc.write(ready_fd, b'x', 1)
        libc.read(block_fd, buf, 1)
        for fd in (go_fd, ready_fd, block_fd):
            os.close(fd)
    thread = Thread(target = target)
    thread.start()
    return thread

def join(thread):
    thread.join()

def call(fork_closure):
    return fork_closure()
", "fork-return.py");

p.callFunction("make_fifos", go, ready, block);
auto thread = p.callFunction("hold_gil", go, ready, block);

# Python calls this closure, which runs without the GIL
code fork_closure = int sub () {
    {
        File go_file();
        go_file.open2(go, O_WRONLY);
        go_file.write(<78>);
    }
    # the thread holds the GIL when the byte can be read
    {
        ReadOnlyFile ready_file(ready);
        ready_file.readBinary(1);
    }
    int pid = fork();
    if (pid) {
        # the parent releases the thread
        File block_file();
        block_file.open2(block, O_WRONLY);
        block_file.write(<78>);
    }
    return pid;
};

int pid = p.callFunction("call", fork_closure);
# only the parent gets here
p.callFunction("join", thread);
printf("parent: child %s\n", pid > 0 ? "started" : "not started");
