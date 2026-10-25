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
    print("Standalone signal handling PASS")


if __name__ == "__main__":
    main()
