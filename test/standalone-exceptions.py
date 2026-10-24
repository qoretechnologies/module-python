#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check the exceptions raised between Python and Qore in a Python process hosting Qore."""
import os
import subprocess
import sys


def child_exit():
    import qoreloader  # noqa: F401
    from qore import call_function
    call_function(lambda: sys.exit(7))
    # not reached: SystemExit raised in a Python callback of Qore code exits the process
    print("SystemExit did not exit the process")


def main():
    from qoreloader import QoreException
    from qore import call_function
    from qore.__root__.Qore.Thread import Mutex

    # a Qore exception is a qoreloader.QoreException, a RuntimeError, with the location of the Python call (the module
    # gives normalized paths of Python files)
    assert QoreException.__module__ == "qoreloader", QoreException.__module__
    assert issubclass(QoreException, RuntimeError)
    try:
        line = sys._getframe().f_lineno + 1
        Mutex().unlock()
        raise AssertionError("no exception")
    except QoreException as e:
        assert e.err == "LOCK-ERROR", e.err
        assert "unlock" in e.desc, e.desc
        assert e.arg is None, e.arg
        assert str(e) == "LOCK-ERROR: " + e.desc, str(e)
        assert e.file == os.path.normpath(__file__), (e.file, __file__)
        assert e.line == line, e.line
        assert e.lang == "Python", e.lang
        assert [x["function"] for x in e.callstack] == ["Mutex::unlock", "main", "<module>"], e.callstack
        assert e.__cause__ is None

    # a Python exception raised in a Python callback of Qore code is raised again
    class Marker(Exception):
        pass

    marker = Marker("m")

    def raise_marker():
        raise marker

    try:
        call_function(raise_marker)
        raise AssertionError("no exception")
    except Marker as e:
        assert e is marker

    # also exceptions that are not subclasses of Exception
    def interrupt():
        raise KeyboardInterrupt

    try:
        call_function(interrupt)
        raise AssertionError("no exception")
    except KeyboardInterrupt:
        pass

    # a QoreException raised in Python is a Qore exception with its err, desc and arg
    def raise_qore():
        raise QoreException("PY-ERR", "py desc", {"a": [1, 2]})

    line = raise_qore.__code__.co_firstlineno + 1

    try:
        call_function(raise_qore)
        raise AssertionError("no exception")
    except QoreException as e:
        assert (e.err, e.desc, e.arg) == ("PY-ERR", "py desc", {"a": [1, 2]}), (e.err, e.desc, e.arg)
        assert e.file == os.path.normpath(__file__), (e.file, __file__)
        assert e.line == line, e.line

    # SystemExit raised in a Python callback of Qore code exits the process
    completed = subprocess.run(
        [sys.executable, "-B", "-W", "error", __file__, "--child-exit"],
        capture_output=True, text=True, env=os.environ.copy(),
    )
    assert completed.returncode == 7, (completed.returncode, completed.stdout, completed.stderr)
    assert not completed.stdout, completed.stdout
    assert not completed.stderr, completed.stderr
    print("Standalone exception mapping PASS")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--child-exit":
        child_exit()
    else:
        main()
