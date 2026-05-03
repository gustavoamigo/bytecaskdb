# ByteCaskDB MariaDB Plugin — Docker Image
#
# Based on the official MariaDB Docker image (https://hub.docker.com/_/mariadb).
# Adds the ByteCaskDB storage engine plugin compiled from source.
#
# Build (from repo root):
#   docker build -t mariadb-bytecaskdb -f bytecaskdb-mariadb-plugin/Dockerfile .
#
# Run:
#   docker run -d --name bytecaskdb \
#     -e MARIADB_ALLOW_EMPTY_ROOT_PASSWORD=1 \
#     -p 3306:3306 \
#     mariadb-bytecaskdb
#
# All standard mariadb image features are supported:
#   - MARIADB_ROOT_PASSWORD / MARIADB_ALLOW_EMPTY_ROOT_PASSWORD
#   - MARIADB_DATABASE / MARIADB_USER / MARIADB_PASSWORD
#   - /docker-entrypoint-initdb.d/ (.sh, .sql, .sql.gz, .sql.xz, .sql.zst)
#   - VOLUME /var/lib/mysql
#   - Custom config via mount to /etc/mysql/conf.d/

# ---------------------------------------------------------------------------
# Stage 1: Build libbytecask.a and ha_bytecaskdb.so
#
# Uses quay.io/pypa/manylinux_2_28_x86_64 (AlmaLinux 8, glibc 2.28) because:
#   - mariadb-devel is available and includes the server-side plugin headers
#     (handler.h etc.) that are absent from every Ubuntu Jammy apt package.
#   - Binaries built here only require glibc >= 2.28 symbols, compatible with
#     the mariadb:10.11 runtime (Ubuntu 22.04 Jammy, glibc 2.35).
#   - BYTECASKDB_STATIC_LIBSTDCXX embeds libstdc++ statically, eliminating
#     any C++ runtime version dependency from the final .so.
# ---------------------------------------------------------------------------
FROM quay.io/pypa/manylinux_2_28_x86_64 AS builder

# LLVM 20 toolchain (same as .github/workflows/build-wheels.yml) + build tools
# + static libstdc++ for -static-libstdc++
RUN dnf module enable -y llvm-toolset:rhel8 && \
    dnf install -y \
        clang clang-tools-extra llvm llvm-devel lld \
        make ninja-build git cmake curl \
        autoconf automake libtool \
        libstdc++-static \
    && dnf clean all

# MariaDB 10.11 development headers from the official MariaDB RPM repo.
# AlmaLinux 8 ships MariaDB 10.3 whose handler API differs from 10.11
# (const write_row, page_range type, etc.) — we must match the runtime version.
RUN curl -LsS https://downloads.mariadb.com/MariaDB/mariadb_repo_setup | \
        bash -s -- --mariadb-server-version="mariadb-10.11" \
                   --os-type=rhel --os-version=8 && \
    dnf install -y MariaDB-devel && \
    dnf clean all

# Build google-crc32c from source (not in default AlmaLinux 8 repos).
# GNUInstallDirs on 64-bit RHEL installs libraries to /usr/lib64.
RUN git clone --depth 1 -b 1.1.2 https://github.com/google/crc32c.git /tmp/crc32c && \
    cmake -S /tmp/crc32c -B /tmp/crc32c/build \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCRC32C_BUILD_TESTS=OFF \
          -DCRC32C_BUILD_BENCHMARKS=OFF \
          -DCRC32C_USE_GLOG=OFF \
          -DBUILD_SHARED_LIBS=ON && \
    cmake --build /tmp/crc32c/build -j$(nproc) && \
    cmake --install /tmp/crc32c/build --prefix /usr && \
    ldconfig && \
    rm -rf /tmp/crc32c

# Python + nanobind: xmake's on_load for the python target imports nanobind
# even when only the bytecask static-lib target is being built
ENV PATH="/opt/python/cp312-cp312/bin:${PATH}"
ENV BYTECASK_PYTHON=/opt/python/cp312-cp312/bin/python3
RUN pip install --no-cache-dir nanobind

# Install xmake (latest release via the official installer)
ENV XMAKE_ROOT=y
RUN curl -fsSL https://xmake.io/shget.text | bash
ENV PATH="/root/.local/bin:${PATH}"

WORKDIR /src
COPY . .

# Build the core engine static library (C++23 modules, requires clang/LLVM 20)
RUN . /root/.xmake/profile && \
    xmake repo --update && \
    RESOURCE_DIR=$(clang --print-resource-dir) && \
    xmake f --toolchain=llvm \
            --sdk=/usr/lib64/llvm20 \
            --cxflags="-resource-dir=${RESOURCE_DIR}" -y && \
    xmake build bytecask

# Build the MariaDB storage engine plugin (.so).
# BYTECASKDB_STATIC_LIBSTDCXX=ON passes -static-libstdc++ -static-libgcc so
# the .so embeds the C++ runtime — zero libstdc++ version dependency at runtime.
RUN CC=clang CXX=clang++ \
    cmake -S bytecaskdb-mariadb-plugin -B bytecaskdb-mariadb-plugin/build \
          -DBYTECASK_ROOT=/src \
          -DCMAKE_BUILD_TYPE=Release \
          -DBYTECASKDB_STATIC_LIBSTDCXX=ON && \
    cmake --build bytecaskdb-mariadb-plugin/build --target ha_bytecaskdb

# ---------------------------------------------------------------------------
# Stage 2: Official MariaDB image + ByteCaskDB plugin
# ---------------------------------------------------------------------------
FROM docker.io/library/mariadb:10.11

# libcrc32c is not in Ubuntu Jammy apt repos; copy it from the build stage.
# libstdc++ is NOT copied — it is statically embedded in ha_bytecaskdb.so.
COPY --from=builder /usr/lib64/libcrc32c.so* /usr/lib/x86_64-linux-gnu/
COPY --from=builder /src/bytecaskdb-mariadb-plugin/build/ha_bytecaskdb.so \
     /usr/lib/mysql/plugin/ha_bytecaskdb.so

# Load the plugin automatically on server startup
RUN printf '[mariadb]\nplugin-load-add=bytecaskdb=ha_bytecaskdb.so\n' \
    > /etc/mysql/mariadb.conf.d/99-bytecaskdb.cnf
