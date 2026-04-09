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
    libcxx \
    libcxx-devel \
    libcxx-static \
    cmake \
    mariadb-devel \
    google-crc32c-devel \
    && dnf clean all

# Verify the environment
RUN gcc --version && xmake --version --root

# Set environment variables for the compiler (helps Xmake and CMake)
ENV CC=gcc
ENV CXX=g++

ARG USERNAME=gustavoamigo
RUN useradd -m $USERNAME || echo "User already exists"