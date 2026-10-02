#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Check retained Python callables after their owning Qore program shuts down."""
import atexit
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def child(report):
    retained = []

    def after_qore_shutdown():
        results = []
        for label, callback, args in retained:
            try:
                callback(*args)
            except RuntimeError as error:
                results.append([label, str(error)])
            else:
                results.append([label, "unexpected success"])
        report.write_text(json.dumps(results), encoding="utf-8")

    # Registration before importing the bridge makes this callback run after
    # qoreloader's atexit handler has deleted its Qore program.
    atexit.register(after_qore_shutdown)
    import qoreloader  # noqa: F401
    from qore.__root__.Qore.Thread import Counter
    from qore import sprintf

    counter = Counter()
    method = counter.waitForZero
    method()
    assert sprintf("%d", 73) == "73"
    retained.extend([("function", sprintf, ("%d", 1)), ("method", method, ())])


def main():
    with tempfile.TemporaryDirectory(prefix="qore-python-lifecycle-") as directory:
        report = Path(directory) / "shutdown.json"
        completed = subprocess.run(
            [sys.executable, "-B", "-W", "error", __file__, "--child", str(report)],
            check=True, capture_output=True, text=True, env=os.environ.copy(),
        )
        assert not completed.stderr, completed.stderr
        assert not completed.stdout, completed.stdout
        expected = "the owning Qore program has been deleted"
        assert json.loads(report.read_text(encoding="utf-8")) == [
            ["function", expected], ["method", expected],
        ]
    print("Standalone callable shutdown ownership PASS")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        child(Path(sys.argv[2]))
    else:
        main()
