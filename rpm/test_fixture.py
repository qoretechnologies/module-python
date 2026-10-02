#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Reject missing or ambiguous native package artifacts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

loader = importlib.util.spec_from_file_location('fixture', Path(__file__).with_name('run-tests.py'))
fixture = importlib.util.module_from_spec(loader)
loader.loader.exec_module(fixture)


class ArtifactTests(unittest.TestCase):
    def test_missing_native_module_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'exactly one'):
                fixture.native_module([Path(directory)])

    def test_selects_exact_native_module_among_other_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = root / 'python-api-2.0.qmod'
            expected.touch()
            (root / 'xml-api-2.0.qmod').touch()
            self.assertEqual(expected, fixture.native_module([root]))

    def test_duplicate_abi_artifacts_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('python-api-2.0.qmod', 'python-api-1.5.qmod'):
                (root / name).touch()
            with self.assertRaisesRegex(RuntimeError, 'exactly one'):
                fixture.native_module([root])


class CoverageTests(unittest.TestCase):
    good = 'Ran 30 test cases, 30 succeeded (270 assertions)\n'

    def test_complete_suite(self):
        fixture.verify_suite(self.good)

    def test_explicit_optional_jni_skip(self):
        fixture.verify_suite('Skipped: java test: 0 assertions (JNI not installed)\n' + self.good)

    def test_missing_or_partial_summary(self):
        for output in ('', 'Ran 30 test cases, 29 succeeded (270 assertions)',
                       'Ran 30 test cases, 30 succeeded (269 assertions)'):
            with self.subTest(output=output), self.assertRaisesRegex(RuntimeError, 'Incomplete'):
                fixture.verify_suite(output)

    def test_unexpected_skip(self):
        with self.assertRaisesRegex(RuntimeError, 'Unexpected skipped'):
            fixture.verify_suite('Skipped: object lifecycle: 0 assertions\n' + self.good)

    def test_warning_rejected(self):
        for warning in ('warning: broken import', 'test.py:1: RuntimeWarning: ignored',
                        'ResourceWarning: leaked file'):
            with self.subTest(warning=warning), self.assertRaisesRegex(RuntimeError, 'diagnostic'):
                fixture.verify_suite(warning + '\n' + self.good)


if __name__ == '__main__':
    unittest.main()
