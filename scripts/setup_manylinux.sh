#!/bin/bash
# Tests the build compability with manylinux_2_28_x86_64 
set -e

echo "==> Enabling llvm-toolset module"
dnf module enable -y llvm-toolset:rhel8

echo "==> Installing packages"
dnf install -y \
    clang \
    clang-tools-extra \
    llvm \
    llvm-devel \
    lld \
    make \
    ninja-build \
    git \
    cmake

echo "==> Installing xmake"
curl -fsSL https://xmake.io/shget.text | bash
source ~/.xmake/profile

echo "==> Setting up Python 3.12 from /opt/python"
export PATH=/opt/python/cp312-cp312/bin:$PATH
pip install nanobind pytest

echo "==> Allowing xmake to run as root"
export XMAKE_ROOT=y
export RESOURCE_DIR=$(clang --print-resource-dir)

xmake f --toolchain=llvm \
    --sdk=/usr/lib64/llvm20 \
    --cxflags="-resource-dir=${RESOURCE_DIR}" \
    -y

echo "==> Building"
xmake build bytecask_tests

echo "==> Running tests"
PATH=/opt/python/cp312-cp312/bin:$PATH \
XMAKE_ROOT=y \
xmake run bytecask_tests

echo "==> Building python bindinds"
xmake build bytecaskdb_python

echo "==> Running python test"
PYTHONPATH=/bytecaskdb/bytecaskdb-python python3 -m pytest bytecaskdb-python/tests/ -v

echo "==> Done"

