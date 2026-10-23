#!/bin/bash
# Copyright (C) 2026 Qore Technologies, s.r.o.

# Builds and installs CPython from a python.org source release, for the CI jobs that test the module with a Python
# version that the image does not provide (Ubuntu images).
#
# usage: install-cpython.sh <version> <sha256> <prefix> [configure options...]
#   version: the release, ex: 3.15.1 or 3.16.0a1
#   sha256: the SHA-256 checksum of Python-<version>.tar.xz
#   prefix: the installation prefix; its lib directory is added to the dynamic linker's search path

set -e
set -x

if [ $# -lt 3 ]; then
    echo "usage: $0 <version> <sha256> <prefix> [configure options...]" >&2
    exit 1
fi

VERSION=$1
SHA256=$2
PREFIX=$3
shift 3

# the directory of a release on python.org has no pre-release suffix: 3.16.0a1 -> 3.16.0
BASE_VERSION=${VERSION%%[a-z]*}
TARBALL=Python-${VERSION}.tar.xz

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl xz-utils make gcc pkg-config libffi-dev zlib1g-dev \
    libssl-dev libbz2-dev liblzma-dev libsqlite3-dev

WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT
cd "${WORK_DIR}"

curl -fsSL --retry 5 -o "${TARBALL}" "https://www.python.org/ftp/python/${BASE_VERSION}/${TARBALL}"
echo "${SHA256}  ${TARBALL}" | sha256sum -c -
tar xf "${TARBALL}"

mkdir build
cd build
"../Python-${VERSION}/configure" --prefix="${PREFIX}" --enable-shared --without-ensurepip \
    LDFLAGS="-Wl,-rpath,${PREFIX}/lib" "$@"
make -j$(nproc)
make install

echo "${PREFIX}/lib" > /etc/ld.so.conf.d/cpython-${VERSION}.conf
ldconfig

"${PREFIX}/bin/python3" -c \
    "import sys, sysconfig; print(sys.version, 'Py_GIL_DISABLED:', sysconfig.get_config_var('Py_GIL_DISABLED'))"
