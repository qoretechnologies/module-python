#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check qoreloader with sub-interpreters, the arguments of its finder and loaders, and the paths of its packages."""
import os
from pathlib import Path
import subprocess
import sys
import sysconfig
import tempfile

SUB_CODE = """
import qoreloader
from qore.__root__.Qore.Thread import Counter
c = Counter(1)
c.dec()
assert c.getCount() == 0
"""


def run_in_subinterpreter(code):
    """runs the code in a new sub-interpreter, which is closed afterwards; returns None or the error's text"""
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
    interp = low_level.create()
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
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        # closing such a sub-interpreter crashes with free-threaded Python (qoretechnologies/qore#5536)
        print("sub-interpreters after the main interpreter: skipped (issue 5536)")
    else:
        for _ in range(2):
            error = run_in_subinterpreter(SUB_CODE)
            assert error is None, error
    # closing the sub-interpreters did not shut Qore down
    c.dec()
    assert c.getCount() == 0
    print("sub-first ok")


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
            "child-arguments": child_arguments, "child-module": child_module}[sys.argv[1]]()
        return
    if subinterpreters_supported():
        run_child("child-sub-first")
    else:
        print("sub-interpreters: skipped (no interpreters module)")
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
