#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Run Python bridge tests against exactly one native module and packaged XML."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def native_module(directories):
    modules = [file for directory in directories for file in directory.glob('python-api-*.qmod')]
    if len(modules) != 1:
        raise RuntimeError('Expected exactly one Python bridge native module: ' + repr(modules))
    return modules[0]


def run(build=None, compiler=False):
    source = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    for key in ('QORE_MODULE_DIR', 'QORE_MODULE_DIR_ONLY', 'QORE_INCLUDE_DIR', 'LD_LIBRARY_PATH',
                'LD_PRELOAD', 'PYTHONPATH', 'PYTHONHOME'):
        env.pop(key, None)
    env.update(LC_ALL='C.UTF-8', TZ='UTC', PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1')
    paths = subprocess.check_output(['/usr/bin/qore', '--module-path'], env=env, text=True).strip().split(':')
    module = native_module([build.resolve()] if build else [Path(path) for path in paths])
    env.update(QORE_MODULE_DIR=':'.join(dict.fromkeys([str(module.parent), *paths])), QORE_MODULE_DIR_ONLY='1')
    qore = ['/usr/bin/qore', '-b', '--enable-debug', '-l', 'xml', '-l', str(module)]
    subprocess.run([*qore, '-e', 'Qore::exit(0);'], env=env, check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix='qore-python-rpm-') as directory:
        root = Path(directory)
        suite = (source / 'test/python.qtest').read_text()
        (root / 'python.qtest').write_text(re.sub(r'^%prepend-module-path .*\n', '', suite, flags=re.M))
        for relative in ('test/fib.py', 'test/standalone-lifecycle.py', 'debian/tests/standalone.py'):
            shutil.copyfile(source / relative, root / Path(relative).name)
        completed = subprocess.run([*qore, str(root / 'python.qtest'), '-v'], env=env, cwd=root,
                                   check=True, timeout=600, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(completed.stdout, end='', flush=True)
        verify_suite(completed.stdout)
        if build:
            suffix = subprocess.check_output(['/usr/bin/python3', '-c',
                'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))'], env=env, text=True).strip()
            (root / ('qoreloader' + suffix)).symlink_to(module)
            env['PYTHONPATH'] = str(root)
        # Check the loaded extension's identity for both build-tree and installed runs.
        subprocess.run(['/usr/bin/python3', '-B', '-W', 'error', '-c',
            'import qoreloader,sys; from pathlib import Path; '
            'assert Path(qoreloader.__file__).resolve() == Path(sys.argv[1]).resolve()', str(module)],
            env=env, cwd=root, check=True, timeout=60)
        for script in ('standalone.py', 'standalone-lifecycle.py'):
            subprocess.run(['/usr/bin/python3', '-B', '-W', 'error', str(root / script)],
                           env=env, cwd=root, check=True, timeout=90)
        if compiler:
            code = (source / 'debian/tests/compiler').read_text().split("<<'EOF'\n", 1)[1].split('\nEOF', 1)[0]
            (root / 'python-smoke.q').write_text(code + '\n')
            subprocess.run(['/usr/bin/qcc', '-o', str(root / 'python-smoke'), str(root / 'python-smoke.q')],
                           env=env, cwd=root, check=True, timeout=120)
            subprocess.run([str(root / 'python-smoke')], env=env, cwd=root, check=True, timeout=30)


def verify_suite(output):
    summary = re.search(r'Ran (\d+) test cases, (\d+) succeeded \((\d+) assertions\)', output)
    if not summary or tuple(map(int, summary.groups()))[:2] != (30, 30) or int(summary[3]) < 270:
        raise RuntimeError('Incomplete Python bridge test coverage')
    for line in output.splitlines():
        if line.startswith('Skipped: ') and not line.startswith(('Skipped: java test:', 'Skipped: java date constant test:')):
            raise RuntimeError('Unexpected skipped Python bridge test: ' + line)
        if re.match(r'(?i)^(?:warning:|.*(?:RuntimeWarning|ResourceWarning):)', line):
            raise RuntimeError('Unexpected runtime diagnostic: ' + line)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build-dir', type=Path)
    mode.add_argument('--installed', action='store_true')
    parser.add_argument('--compiler', action='store_true')
    args = parser.parse_args()
    run(args.build_dir, args.compiler)
