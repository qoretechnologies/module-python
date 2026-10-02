RPM packaging
=============

Copyright 2026 Qore Technologies, s.r.o.

qore-python-module.spec builds the native bridge and its CPython extension alias
for Fedora, Enterprise Linux and openSUSE. It requires Qore 3.0 development
packages and qore-rpm-macros from the same repository. The distribution's normal
compiler flags, ELF stripping and separate debug packages remain enabled.
Documentation is built with warnings treated as errors and packaged separately.

The runtime package installs python-api-*.qmod, compiler metadata, and a relative
qoreloader extension symlink in the default interpreter's architecture-specific
site-packages directory. The extension suffix and exact python(abi) requirement
come from that interpreter. This is a full CPython ABI bridge; a package built
for one Python minor version must be rebuilt for another. Free-threaded Python
has separate native test coverage but is not substituted for the default Python
in this RPM.

Prepare a committed source bundle from the qore-packaging repository::

    python3 tools/packaging.py prepare --repo ../module-python --ref COMMIT \
      --name qore-python-module --version 1.3.0 \
      --spec qore-python-module.spec --output work/python-source
    python3 tools/build-local.py --source work/python-source \
      --image TARGET_SDK_IMAGE --output results/python-build --jobs 2

The default build runs all 30 embedded cases, with at least 270 assertions, the
standalone JSON and callback suite, and retained-callable shutdown checks. XML
is mandatory for qualification and is loaded before the suite. Only the two
optional JNI cases may skip here; JNI/Python interoperability is a separate
repository qualification gate. The fixture rejects missing or ambiguous module
artifacts, incomplete coverage, unexpected skips and runtime warnings.

After installing the runtime RPM and the XML test dependency, run from a source
bundle as an unprivileged user::

    /usr/bin/python3 -B -W error rpm/run-tests.py --installed

With the Qore SDK installed, also exercise the native compiler::

    /usr/bin/python3 -B -W error rpm/run-tests.py --installed --compiler

Each invocation copies the tests to a fresh temporary directory and clears
ambient Qore, Python and dynamic-loader overrides. It verifies that Python loads
the extension belonging to the selected native module. Build-tree tests instead
create an isolated temporary extension alias. No host databases or network
services are required. --without tests and --without docs are diagnostic options;
repository qualification uses both defaults.

Native memory qualification retains the full Valgrind output. Its separately
approved external diagnostics are documented by qore-packaging: CPython 3.12/3.13
immortal-string cleanup retention, the measured static-type baseline, glibc's
one-block 40-byte loader cleanup leak, and GCC 16 bug 125913's nonzero-offset
delete warnings. These exceptions do not admit invalid memory accesses,
additional bridge leaks or other warnings, and do not disable compiler checks.
