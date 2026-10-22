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


def main():
    assert in_thread(lambda: sprintf("%d", 73)) == "73", "a Qore function called in a thread"
    assert in_thread(construct_and_use) == 1, "a Qore object created and used in a thread"
    assert in_thread(lock_across_calls) == "unlocked", "a Qore lock held across calls in a thread"
    tid = in_thread(gettid)
    assert tid > 0, "the thread had a Qore thread ID"
    assert tid not in thread_list(), "the ended thread is deregistered from Qore"
    print("Standalone Python threads PASS")


if __name__ == "__main__":
    main()
