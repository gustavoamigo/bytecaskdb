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
    rocksdb-devel \
    sudo \
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

# Install Emscripten SDK for WebAssembly builds
RUN cd /opt && git clone --depth 1 https://github.com/emscripten-core/emsdk.git \
    && cd emsdk && ./emsdk install latest && ./emsdk activate latest \
    && ln -s $(ls -d /opt/emsdk/node/*/bin | head -1)/node /usr/local/bin/node

ENV EMSDK=/opt/emsdk
ENV EM_CONFIG=/opt/emsdk/.emscripten
ENV PATH="/opt/emsdk:/opt/emsdk/upstream/emscripten:/opt/emsdk/upstream/bin:${PATH}"

ARG USERNAME=gustavoamigo
RUN useradd -m $USERNAME || echo "User already exists"
RUN echo "$USERNAME ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME