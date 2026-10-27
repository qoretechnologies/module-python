#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check qoreloader with sub-interpreters, the arguments of its finder and loaders, and the paths of its packages."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SUB_CODE = """
import qoreloader
from qore.__root__.Qore.Thread import Counter
c = Counter(1)
c.dec()
assert c.getCount() == 0
"""


def run_in_subinterpreter(code, threads=False):
    """runs the code in a new sub-interpreter, which is closed afterwards; returns None or the error's text

    threads: the code starts threads, which an isolated sub-interpreter of Python 3.11 does not allow
    """
    try:
        from concurrent import interpreters
    except ImportError:
        interpreters = None
    if interpreters:
        interp = interpreters.create()
        try:
            interp.exec(code)
        except interpreters.ExecutionFailed as error:
            return str(error)
        finally:
            interp.close()
        return None
    try:
        import _interpreters as low_level
    except ImportError:
        # Python 3.11 and 3.12
        import _xxsubinterpreters as low_level
    interp = low_level.create(isolated=False) if threads and sys.version_info < (3, 12) else low_level.create()
    try:
        if hasattr(low_level, "exec"):
            rv = low_level.exec(interp, code)
            return None if rv is None else str(rv)
        try:
            low_level.run_string(interp, code)
        except low_level.RunFailedError as error:
            return str(error)
        return None
    finally:
        low_level.destroy(interp)


def subinterpreters_supported():
    try:
        from concurrent import interpreters  # noqa: F401
        return True
    except ImportError:
        pass
    for name in ("_interpreters", "_xxsubinterpreters"):
        try:
            __import__(name)
            return True
        except ImportError:
            pass
    return False


def child_sub_first():
    # the process-wide state of qoreloader belongs to the main interpreter: a sub-interpreter cannot import it first
    error = run_in_subinterpreter(SUB_CODE)
    assert error is not None and "must be imported in the main Python interpreter" in error, error
    # the failed import leaves no state behind: the main interpreter imports it, then sub-interpreters can
    import qoreloader  # noqa: F401
    from qore.__root__.Qore.Thread import Counter
    c = Counter(2)
    c.dec()
    assert c.getCount() == 1
    # each sub-interpreter has its own Python program and Qore Program, whose Python objects are not shared with the
    # main interpreter (the Python types of the main interpreter's Qore classes were used in sub-interpreters, which
    # crashed free-threaded Python, and with Python 3.11 the main interpreter could not call Qore after a
    # sub-interpreter was destroyed: qoretechnologies/qore#5536)
    for _ in range(3):
        error = run_in_subinterpreter(SUB_CODE)
        assert error is None, error
    # a Python thread of a sub-interpreter calls Qore (methods and functions): the sub-interpreter can be closed (with
    # Python 3.11 and 3.12, a second thread state of the thread was created, so that it could not end; with Python 3.11,
    # the main thread then had no state for the GIL state API, and qoreloader's exit handler waited forever)
    error = run_in_subinterpreter(SUB_THREAD_CODE, threads=True)
    assert error is None, error
    # closing the sub-interpreters did not shut Qore down
    c.dec()
    assert c.getCount() == 0
    print("sub-first ok")


SUB_THREAD_CODE = """
import threading
import qoreloader
from qore.__root__.Qore.Thread import Counter
from qore.__root__.Qore import get_module_hash
out = []
def run():
    c = Counter(2)
    c.dec()
    out.append((c.getCount(), "python" in get_module_hash()))
t = threading.Thread(target=run)
t.start()
t.join()
assert out == [(1, True)], out
"""


def child_own_gil():
    # with Python 3.12, a thread that waits for the GIL of an isolated sub-interpreter (its own GIL) does not make the
    # thread that holds the shared GIL release it: the thread released it for a thread that never took it, and waited
    import threading
    import time
    import _xxsubinterpreters as low_level
    import qoreloader  # noqa: F401
    with tempfile.TemporaryDirectory() as tmp:
        flag = os.path.join(tmp, "flag")
        sub = ("import os\nfrom qore.__root__.Qore import usleep\n"
            "while not os.path.exists(%r):\n    usleep(0)\n" % flag)
        started = threading.Event()

        def runner():
            interp = low_level.create(isolated=True)
            try:
                low_level.run_string(interp, "import qoreloader")
                started.set()
                low_level.run_string(interp, sub)
            finally:
                low_level.destroy(interp)
        t = threading.Thread(target=runner)
        t.start()
        assert started.wait(60)
        # the main thread runs Python code while the sub-interpreter calls Qore, then ends its loop
        end = time.monotonic() + 0.5
        n = 0
        while time.monotonic() < end:
            n += 1
        Path(flag).write_text("")
        t.join(60)
        assert not t.is_alive()
    print("own-gil ok")


def child_reimport():
    # the module cannot be initialized again in an interpreter while the module initialized before exists (the types
    # of the Qore objects made by it were replaced, so that they were no longer recognized)
    import qoreloader  # noqa: F401
    from qore.__root__.Qore.Thread import Counter
    c = Counter(1)
    del sys.modules["qoreloader"]
    try:
        import qoreloader  # noqa: F401,F811
        raise AssertionError("no ImportError")
    except ImportError as error:
        assert "already initialized" in str(error), str(error)
    c.dec()
    assert c.getCount() == 0
    Counter(1).dec()
    print("reimport ok")


def child_paths():
    # the packages of Qore namespaces have an empty __path__: a directory named like a character of the namespace
    # path in the current directory (ex: o/ for 'Qore') does not replace Qore modules
    import qoreloader  # noqa: F401
    import qore
    import qore.Thread
    assert qore.__path__ == [], qore.__path__
    assert qore.Thread.__path__ == [], qore.Thread.__path__
    assert not getattr(qore.Thread, "__file__", None), qore.Thread.__file__
    assert hasattr(qore.Thread, "Counter")
    print("paths ok")


def child_arguments():
    import qoreloader  # noqa: F401
    import qore
    finder = [f for f in sys.meta_path if type(f).__name__ == "QoreMetaPathFinder"][0]
    cases = [
        ("find_spec()", lambda: finder.find_spec(), TypeError),
        ("find_spec(1)", lambda: finder.find_spec(1, None), TypeError),
        ("exec_module(42)", lambda: qore.__loader__.exec_module(42), TypeError),
        ("exec_module(sys)", lambda: qore.__loader__.exec_module(sys), ImportError),
    ]
    for label, call, expected in cases:
        try:
            call()
        except expected:
            pass
        else:
            raise AssertionError(label + ": no " + expected.__name__)
    # the finder still finds Qore modules, and a name it does not handle gives None
    assert finder.find_spec("qore", None) is not None
    assert finder.find_spec("not_qore", None) is None
    print("arguments ok")


def child_module():
    # the Qore python module is this binary, not another python module that the module manager finds in its module
    # path (ex: an installed one), which would be a second copy of the module in the process
    import qoreloader
    from qore.__root__.Qore import get_module_hash
    info = get_module_hash()["python"]
    assert os.path.realpath(info["filename"]) == os.path.realpath(qoreloader.__file__), \
        (info["filename"], qoreloader.__file__)
    print("module ok")


def run_child(mode, cwd=None):
    proc = subprocess.run([sys.executable, "-B", "-W", "error", os.path.abspath(__file__), mode], cwd=cwd,
        capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, "%s: rc %d\nstdout: %s\nstderr: %s" % (mode, proc.returncode, proc.stdout,
        proc.stderr)
    assert mode.replace("child-", "") + " ok" in proc.stdout, proc.stdout


def main():
    if len(sys.argv) > 1:
        {"child-sub-first": child_sub_first, "child-paths": child_paths,
            "child-arguments": child_arguments, "child-module": child_module, "child-own-gil": child_own_gil,
            "child-reimport": child_reimport}[sys.argv[1]]()
        return
    if subinterpreters_supported():
        run_child("child-sub-first")
    else:
        print("sub-interpreters: skipped (no interpreters module)")
    # isolated sub-interpreters with their own GIL while the interpreters of the module share one: Python 3.12
    if sys.version_info[:2] == (3, 12):
        run_child("child-own-gil")
    run_child("child-reimport")
    with tempfile.TemporaryDirectory() as tmp:
        # a module that would replace qore.Thread if the characters of the path 'Qore' were searched
        for d in ("Q", "o", "r", "e"):
            Path(tmp, d).mkdir()
            Path(tmp, d, "Thread.py").write_text("raise RuntimeError('loaded from the file system')\n",
                encoding="utf-8")
        run_child("child-paths", cwd=tmp)
    run_child("child-arguments")
    run_child("child-module")
    print("standalone-loader: OK")


if __name__ == "__main__":
    main()
