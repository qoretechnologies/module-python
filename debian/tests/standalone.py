#!/usr/bin/python3
# Copyright (C) 2026 David Nichols
# SPDX-License-Identifier: MIT
"""Exercise a standalone Python interpreter importing the installed bridge."""
import qoreloader
from qore.__root__.Qore.Thread import Counter
from qore.json import parse_json, make_json
from qore.python import PythonProgram

counter = Counter()
counter.waitForZero()
value = {"unicode": "Příliš žluťoučký", "array": [1, True, None], "nested": {"v": 42}}
assert parse_json(make_json(value)) == value
program = PythonProgram("def echo(value):\n    return value", "standalone.py")
assert program.callFunction("echo", value) == value
print("Standalone qoreloader counter, JSON and Python roundtrip PASS")
