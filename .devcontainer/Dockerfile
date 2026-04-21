# Use Fedora 43 for a rock-solid GCC 15 experience
FROM fedora:43

# Install development tools and dependencies
# Fedora 43+ uses dnf5 by default, which is significantly faster
RUN dnf install -y \
    gcc \
    gcc-c++ \
    libstdc++-devel \
    gdb \
    make \
    ninja-build \
    git \
    pkg-config \
    xmake \
    clang \
    clang-tools-extra \
    llvm \
    llvm-devel \
    lld \
    libcxx \
    libcxx-devel \
    libcxx-static \
    cmake \
    mariadb \
    mariadb-server \
    mariadb-devel \
    google-crc32c-devel \
    python3-pip \
    python3-devel \
    && dnf clean all

# Keep devcontainer behavior aligned with wheel/manylinux builds:
# xmake's Python target imports nanobind during target loading.
RUN python3 -m pip install --no-cache-dir --upgrade pip \
    && python3 -m pip install --no-cache-dir nanobind pytest auditwheel

# Verify the environment
RUN gcc --version && xmake --version --root

# Set environment variables for the compiler (helps Xmake and CMake)
ENV CC=gcc
ENV CXX=g++
ENV BYTECASK_PYTHON=python3

ARG USERNAME=gustavoamigo
RUN useradd -m $USERNAME || echo "User already exists"