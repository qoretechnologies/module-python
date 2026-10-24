#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Report the cost of calls from Python to Qore in a Python process hosting Qore (not run in CI; use release builds):
    PYTHONPATH=<module build with qoreloader.so> python3 test/benchmark-calls.py [calls]
"""
import sys
import time

import qoreloader  # noqa: F401
from qore.__root__.Qore import abs as qabs
from qore.__root__.Qore.Thread import Counter

ROUNDS = 5
CALLS = int(sys.argv[1]) if len(sys.argv) > 1 else 100000


def f(x):
    return x


def best(measure):
    """Return the shortest time of the rounds in seconds."""
    rv = None
    for _ in range(ROUNDS):
        start = time.perf_counter()
        measure()
        secs = time.perf_counter() - start
        rv = secs if rv is None else min(rv, secs)
    return rv


def report(label, secs):
    print(f"{label:42s} {secs * 1e6 / CALLS:8.3f} us/call")


counter = Counter()
report("Python -> Python", best(lambda: [f(i) for i in range(CALLS)]))
report("Python -> Qore builtin function", best(lambda: [qabs(i) for i in range(CALLS)]))
report("Python -> Qore method", best(lambda: [counter.getCount() for _ in range(CALLS)]))
report("Python creates a Qore object", best(lambda: [Counter() for _ in range(CALLS)]))
