#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check the identity and the attributes of Qore objects in a Python process hosting Qore."""
import weakref

import qoreloader  # noqa: F401
from qore.__root__.Qore.Thread import Queue, Mutex


def main():
    q = Queue()
    m = Mutex()
    # the same Qore object is the same Python object
    q.push(m)
    a = q.get()
    assert a is m
    assert a == m and hash(a) == hash(m)
    # a missing member raises AttributeError
    assert not hasattr(q, "nope")
    assert getattr(q, "nope", 42) == 42
    try:
        q.nope
        raise AssertionError("no exception")
    except AttributeError as e:
        assert str(e) == "'Queue' object has no attribute 'nope'", str(e)
    # Qore objects support weak references
    w = weakref.ref(m)
    assert w() is m
    del a, m
    assert w() is None
    print("Standalone Qore objects PASS")


if __name__ == "__main__":
    main()
