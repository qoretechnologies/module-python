#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check that a child of os.fork() of a Python process hosting Qore can wait for the GIL in Qore calls.

The helper thread that asks the thread holding the GIL to release it for the threads waiting in Qore calls does not
exist in the child, and is started again there.  Previously the child kept the parent's copy of the helper's condition
variable, which still listed the helper thread as a waiter: with musl, signaling it waited forever for that thread,
so the child hung in its first Qore call that released the GIL.
"""
import os
import select
import signal
import threading
import warnings

import qoreloader  # noqa: F401
from qore.__root__.Qore.Thread import Queue

# the time that a child is given to exit
CHILD_TIMEOUT = 60
# the number of forks of each check: the threads of the parent are in different states at each fork
FORKS = 20


def wait_child(pid):
    """Waits for the child to exit and returns its exit code; kills it and fails if it does not exit in time."""
    pidfd = os.pidfd_open(pid)
    try:
        readable, _, _ = select.select([pidfd], [], [], CHILD_TIMEOUT)
        if not readable:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            raise AssertionError("the child %d did not exit in %d seconds" % (pid, CHILD_TIMEOUT))
    finally:
        os.close(pidfd)
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def fork_and_check():
    """Forks; the child waits for the GIL in Qore calls from the main thread and from a new thread."""
    with warnings.catch_warnings():
        # the process has threads (the helper thread, the threads of the check)
        warnings.simplefilter("ignore", DeprecationWarning)
        pid = os.fork()
    if pid == 0:
        rc = 1
        try:
            # a Qore call that releases the GIL starts the helper thread again
            Queue()
            q = Queue()
            # a thread of the child waits for the GIL while the main thread holds it
            thread = threading.Thread(target=lambda: q.push(1))
            thread.start()
            rc = 0 if q.get(CHILD_TIMEOUT * 1000) == 1 else 2
            thread.join()
        finally:
            os._exit(rc)
    rc = wait_child(pid)
    assert rc == 0, rc


def check_idle_helper():
    # the helper thread waits for threads that wait for the GIL when the process forks: the fork handlers hold its
    # lock, which it releases only while it waits on its condition variable
    Queue()
    for _ in range(FORKS):
        fork_and_check()


def check_busy_helper():
    # other threads wait for the GIL in Qore calls when the process forks: the helper thread waits on its condition
    # variable between its requests to release the GIL, and the threads do not exist in the child
    stop = threading.Event()

    def contend():
        while not stop.is_set():
            Queue()
    threads = [threading.Thread(target=contend) for _ in range(3)]
    for thread in threads:
        thread.start()
    try:
        for _ in range(FORKS):
            fork_and_check()
    finally:
        stop.set()
        for thread in threads:
            thread.join()


def main():
    check_idle_helper()
    check_busy_helper()


if __name__ == "__main__":
    main()
