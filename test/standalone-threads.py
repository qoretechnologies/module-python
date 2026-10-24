#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check that threads created by a Python process hosting Qore can call Qore.

A thread created by Python has no Qore thread data until it is registered with Qore; it stays registered while it
runs and is deregistered when it ends.  Previously a Qore function or constructor called on such a thread crashed the
process.
"""
import threading

import qoreloader  # noqa: F401
from qore import gettid, sprintf, thread_list
from qore.__root__.Qore.Thread import Counter, Mutex


def in_thread(fn):
    results = []

    def target():
        try:
            results.append(fn())
        except Exception as error:  # reported to the main thread
            results.append("error: " + repr(error))

    thread = threading.Thread(target=target)
    thread.start()
    thread.join()
    return results[0]


def construct_and_use():
    counter = Counter(2)
    counter.dec()
    return counter.getCount()


def lock_across_calls():
    mutex = Mutex()
    mutex.lock()
    mutex.unlock()
    return "unlocked"


def concurrent_rounds():
    """Threads created in successive rounds call a Qore user function concurrently.

    Each round's threads get the Qore thread IDs of the previous round's threads, which have ended.  The cleanup of
    an ended thread was skipped when another thread held the module's thread lock, so the thread state cached for its
    Qore thread ID was then used by the new thread, although it was the state of the ended thread (or the memory of
    the state of another running thread), which aborted free-threaded Python ("_PyThreadState_Attach: non-NULL old
    thread state", or a critical section unlocked on the wrong state).  The skipped cleanup depends on the timing of
    the threads, so this is a stress test; debug builds of the module assert that a thread uses its own state.
    """
    from qore.__root__.Qore import Program

    program = Program()
    program.parse("%new-style\nint sub twice(int i) { return i * 2; }\n", "rounds")
    threads_per_round = 8
    calls = 400
    expected = sorted(sum((i + k) * 2 for i in range(calls)) for k in range(threads_per_round))
    for _round in range(40):
        barrier = threading.Barrier(threads_per_round)
        lock = threading.Lock()
        results = []

        def worker(offset):
            barrier.wait()
            total = 0
            for i in range(calls):
                total += program.callFunction("twice", i + offset)
            with lock:
                results.append(total)

        threads = [threading.Thread(target=worker, args=(k,)) for k in range(threads_per_round)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        assert sorted(results) == expected, "concurrent calls in round %d: %r" % (_round, results)
    return "ok"


def main():
    assert in_thread(lambda: sprintf("%d", 73)) == "73", "a Qore function called in a thread"
    assert in_thread(construct_and_use) == 1, "a Qore object created and used in a thread"
    assert in_thread(lock_across_calls) == "unlocked", "a Qore lock held across calls in a thread"
    tid = in_thread(gettid)
    assert tid > 0, "the thread had a Qore thread ID"
    assert tid not in thread_list(), "the ended thread is deregistered from Qore"
    assert concurrent_rounds() == "ok", "threads reusing the Qore thread IDs of ended threads call Qore"
    print("Standalone Python threads PASS")


if __name__ == "__main__":
    main()
