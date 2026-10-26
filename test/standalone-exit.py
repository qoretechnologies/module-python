#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check that a Python process hosting Qore exits cleanly while Python still holds Qore objects.

qoreloader's atexit handler shuts the Qore library down; Python finalization runs after it and releases the Python
objects that are still alive, such as Qore objects held by class attributes.  Releasing their Qore objects then ran
their destructors in the library that had been torn down, and the process crashed at exit.  Each child process holds
a Qore object of a class whose destructor runs Qore code, compiled in each execution mode, a builtin Qore object
whose destructor closes a socket, and the Program, in class attributes and in a global, and exits.
"""
import os
import subprocess
import sys

MODES = ("ast", "ir", "jit", "tiered")

CLASS_SOURCE = """
class Holder {
    destructor() {
        string s = sprintf("%d", 42);
        if (s != "42") {
            throw "EXIT-TEST", s;
        }
    }
}
Holder sub make() { return new Holder(); }
"""


def child(mode):
    import qoreloader  # noqa: F401
    from qore.__root__.Qore import (Program, HTTPClient, ExecMode, PO_NEW_STYLE, PO_REQUIRE_TYPES, PO_STRICT_ARGS,
                                    PO_STRONG_ENCAPSULATION)

    exec_modes = {"ast": ExecMode.EM_AST, "ir": ExecMode.EM_IR, "jit": ExecMode.EM_JIT, "tiered": ExecMode.EM_TIERED}
    pgm = Program(PO_NEW_STYLE | PO_REQUIRE_TYPES | PO_STRICT_ARGS | PO_STRONG_ENCAPSULATION)
    pgm.setExecMode(exec_modes[mode])
    pgm.parse(CLASS_SOURCE, "standalone-exit")

    class Keeper:
        holder = pgm.callFunction("make")
        client = HTTPClient({"url": "http://localhost:1"})
        program = pgm

    # module globals are also released by Python finalization, after Qore has been shut down
    global kept
    kept = (Keeper, pgm.callFunction("make"))
    print("child ok")


def main():
    for mode in MODES:
        completed = subprocess.run(
            [sys.executable, "-B", "-W", "error", __file__, "--child", mode],
            capture_output=True, text=True, env=os.environ.copy(),
        )
        assert completed.returncode == 0, "%s: exit code %d; stderr: %s" % (mode, completed.returncode,
                                                                           completed.stderr)
        assert completed.stdout == "child ok\n", "%s: stdout: %r" % (mode, completed.stdout)
        assert not completed.stderr, "%s: stderr: %s" % (mode, completed.stderr)
    print("Standalone exit with live Qore objects PASS")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        child(sys.argv[2])
    else:
        main()
