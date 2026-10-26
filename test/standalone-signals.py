#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check that importing qoreloader leaves the signal handling of a Python process hosting Qore to Python."""
import os
import signal
import subprocess
import sys

MASK = "import signal; print(sorted(int(s) for s in signal.pthread_sigmask(signal.SIG_BLOCK, [])))"


def child_mask():
    # the signal mask of a child process
    completed = subprocess.run([sys.executable, "-I", "-c", MASK], capture_output=True, text=True, check=True)
    return completed.stdout.strip()


def main():
    # Python leaves SIGINT ignored when the process starts with it ignored (as a background job of a non-interactive
    # shell, as in CI), so its handler is installed here
    signal.signal(signal.SIGINT, signal.default_int_handler)
    process_mask = signal.pthread_sigmask(signal.SIG_BLOCK, [])
    mask_before = child_mask()

    # a wakeup fd set before qoreloader is imported still gets the signal numbers
    wakeup_r, wakeup_w = os.pipe()
    os.set_blocking(wakeup_r, False)
    os.set_blocking(wakeup_w, False)
    signal.set_wakeup_fd(wakeup_w, warn_on_full_buffer=False)

    import qoreloader  # noqa: F401
    from qore import sprintf
    assert sprintf("%d", 1) == "1"

    # the signal mask of the process and of its children is unchanged
    assert signal.pthread_sigmask(signal.SIG_BLOCK, []) == process_mask
    assert child_mask() == mask_before, (child_mask(), mask_before)

    # a child process can be terminated
    p = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(30)"])
    p.terminate()
    assert p.wait(timeout=20) == -signal.SIGTERM, p.returncode

    # Python handles the signals of the process
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler, signal.getsignal(signal.SIGINT)
    try:
        signal.raise_signal(signal.SIGINT)
        raise AssertionError("no KeyboardInterrupt")
    except KeyboardInterrupt:
        pass

    received = []
    signal.signal(signal.SIGUSR1, lambda signum, frame: received.append(signum))
    os.kill(os.getpid(), signal.SIGUSR1)
    assert received == [signal.SIGUSR1], received

    # a signal interrupts a Qore call of the main thread that blocks (issue 5527): the exception raised by the
    # signal's Python handler replaces the Qore exception
    check_blocking_qore_call(received, wakeup_r)
    print("Standalone signal handling PASS")


def check_blocking_qore_call(received, wakeup_r):
    import qoreloader
    import threading
    from qore.__root__.Qore import Program
    from qore.__root__.Qore.Thread import Queue

    pgm = Program()
    # the main thread waits in Qore code after telling the other thread that it runs Qore code; the timeout is only a
    # guard: without the interruption, the call raises QUEUE-TIMEOUT
    pgm.parse("%new-style\nsub wait_in_qore(Queue ready, Queue q) { ready.push(1); q.get(30s); }", "signals")

    def interrupt_in_qore(signum):
        ready = Queue()
        q = Queue()

        def send():
            ready.get()
            os.kill(os.getpid(), signum)
        thread = threading.Thread(target=send)
        thread.start()
        try:
            pgm.callFunction("wait_in_qore", ready, q)
            return "returned"
        except KeyboardInterrupt:
            return "KeyboardInterrupt"
        except qoreloader.QoreException as e:
            return str(e).split(":")[0]
        finally:
            thread.join()

    assert interrupt_in_qore(signal.SIGINT) == "KeyboardInterrupt"
    # a handler that does not raise an exception leaves the Qore exception
    del received[:]
    assert interrupt_in_qore(signal.SIGUSR1) == "THREAD-CANCELLED"
    assert received == [signal.SIGUSR1], received

    # the cancellation does not apply to later Qore calls
    q = Queue()
    q.push(1)
    assert q.get() == 1
    try:
        q.get(1)
        raise AssertionError("no exception")
    except qoreloader.QoreException as e:
        assert str(e).startswith("QUEUE-TIMEOUT"), str(e)

    # the wakeup fd set before qoreloader was imported got the signal numbers
    data = os.read(wakeup_r, 100)
    assert signal.SIGINT in data and signal.SIGUSR1 in data, data


if __name__ == "__main__":
    main()
