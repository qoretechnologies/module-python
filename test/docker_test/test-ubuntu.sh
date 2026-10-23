#!/bin/bash
# Copyright (C) 2026 Qore Technologies, s.r.o.

set -e
set -x

ENV_FILE=/tmp/env.sh

. ${ENV_FILE}

# setup MODULE_SRC_DIR env var
cwd=`pwd`
if [ -z "${MODULE_SRC_DIR}" ]; then
    if [ -e "$cwd/src/python-module.cpp" ]; then
        MODULE_SRC_DIR=$cwd
    else
        MODULE_SRC_DIR=$WORKDIR/module-python
    fi
fi
echo "export MODULE_SRC_DIR=${MODULE_SRC_DIR}" >> ${ENV_FILE}

echo "export QORE_UID=999" >> ${ENV_FILE}
echo "export QORE_GID=999" >> ${ENV_FILE}

. ${ENV_FILE}

export MAKE_JOBS=4

# a job that tests a Python version that the image does not provide builds it from a python.org source release
# (PYTHON_SRC_VERSION, PYTHON_SRC_SHA256 and the configure options in PYTHON_SRC_CONFIGURE, ex: --disable-gil)
PYTHON=python3
if [ -n "${PYTHON_SRC_VERSION}" ]; then
    echo && echo "-- building CPython ${PYTHON_SRC_VERSION} ${PYTHON_SRC_CONFIGURE} --"
    PYTHON_PREFIX=/opt/cpython
    ${MODULE_SRC_DIR}/test/docker_test/install-cpython.sh "${PYTHON_SRC_VERSION}" "${PYTHON_SRC_SHA256}" \
        ${PYTHON_PREFIX} ${PYTHON_SRC_CONFIGURE}
    PYTHON=${PYTHON_PREFIX}/bin/python3
    export Python3_ROOT=${PYTHON_PREFIX}
    export Python3_EXECUTABLE=${PYTHON}
fi

# build module and install
echo && echo "-- building module --"
mkdir -p ${MODULE_SRC_DIR}/build
cd ${MODULE_SRC_DIR}/build
cmake .. -DCMAKE_BUILD_TYPE=debug -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} 2>&1 | tee cmake.log
test ${PIPESTATUS[0]} -eq 0
if [ -n "${PYTHON_SRC_VERSION}" ]; then
    # the module must be built with the Python that was built, and with all of its features: a Python version that
    # changes the internals that the module uses gives a CMake warning
    grep -q "Found Python3: ${PYTHON}" cmake.log
    if grep -q "CMake Warning" cmake.log; then
        echo "CMake gave a warning with Python ${PYTHON_SRC_VERSION}" >&2
        exit 1
    fi
fi
make -j${MAKE_JOBS}
make install
ln -s python*.qmod qoreloader.so
# issue #4398: test the qoreloader.so module
${PYTHON} -c "import qoreloader;from qore.__root__.Qore.Thread import Counter;c = Counter();c.waitForZero()"
PYTHONPATH="$PWD" ${PYTHON} -B -W error ../test/standalone-lifecycle.py
PYTHONPATH="$PWD" ${PYTHON} -B -W error ../test/standalone-threads.py

# add Qore user and group
groupadd -o -g ${QORE_GID} qore
useradd -o -m -d /home/qore -u ${QORE_UID} -g ${QORE_GID} qore

# own everything by the qore user
chown -R qore:qore ${MODULE_SRC_DIR}

# run the tests
export QORE_MODULE_DIR=${MODULE_SRC_DIR}/qlib:${QORE_MODULE_DIR}
cd ${MODULE_SRC_DIR}
for test in test/*.qtest; do
    gosu qore:qore qore $test -vv
done
