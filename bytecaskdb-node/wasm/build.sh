#!/usr/bin/env bash
# Cross-compile ByteCaskDB's WASM dependencies (crc32c, zstd, Google
# Benchmark, Catch2) for Emscripten. This is a prerequisite step only — the actual
# ByteCaskDB WASM targets (smoke test, Embind module, benchmarks, tests) are
# built by xmake, not by this script. See the "WASM / Emscripten targets"
# section of ../../xmake.lua for the target list and set_default(false)
# names (wasm_smoke_test, wasm_embind, wasm_engine_bench, wasm_memory_profile,
# wasm_tests), or the "WASM Backend" section of ../README.md for the
# end-to-end command sequence.
#
# Prerequisites:
#   - Emscripten SDK activated (source emsdk_env.sh)
#   - Internet access (clones google/crc32c, facebook/zstd, google/benchmark,
#     catchorg/Catch2 on first run; skipped on subsequent runs once each is
#     built)
#
# Usage:
#   cd bytecaskdb-node/wasm && bash build.sh
#   cd ../..
#   xmake build wasm_embind        # or wasm_smoke_test / wasm_tests / ...

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SCRIPT_DIR/build"
CRC32C_SRC="$BUILD/crc32c-src"
CRC32C_PREFIX="$BUILD/crc32c-wasm"
ZSTD_SRC="$BUILD/zstd-src"
ZSTD_PREFIX="$BUILD/zstd-wasm"
BENCH_SRC="$BUILD/benchmark-src"
BENCH_PREFIX="$BUILD/benchmark-wasm"
CATCH2_SRC="$BUILD/catch2-src"
CATCH2_PREFIX="$BUILD/catch2-wasm"

# ── Step 1: Cross-compile crc32c to WASM ─────────────────────────────────────
if [ ! -f "$CRC32C_PREFIX/lib/libcrc32c.a" ]; then
  echo "=== Building crc32c for WASM ==="
  if [ ! -d "$CRC32C_SRC" ]; then
    git clone --depth 1 https://github.com/google/crc32c.git "$CRC32C_SRC"
    (cd "$CRC32C_SRC" && git submodule update --init --depth 1)
  fi
  mkdir -p "$CRC32C_SRC/build"
  (cd "$CRC32C_SRC/build" && emcmake cmake .. \
    -DCRC32C_BUILD_TESTS=OFF \
    -DCRC32C_BUILD_BENCHMARKS=OFF \
    -DCRC32C_USE_GLOG=OFF \
    -DCRC32C_INSTALL=ON \
    -DCMAKE_INSTALL_PREFIX="$CRC32C_PREFIX")
  (cd "$CRC32C_SRC/build" && emmake make -j"$(nproc)" && emmake make install)
fi

# ── Step 2: Cross-compile zstd to WASM ───────────────────────────────────────
# Hint files are zstd-framed. The tag matches the version xmake builds for
# the native targets.
if [ ! -f "$ZSTD_PREFIX/lib/libzstd.a" ]; then
  echo "=== Building zstd for WASM ==="
  if [ ! -d "$ZSTD_SRC" ]; then
    git clone --depth 1 --branch v1.5.7 https://github.com/facebook/zstd.git "$ZSTD_SRC"
  fi
  mkdir -p "$ZSTD_SRC/build/cmake/wasm-build"
  (cd "$ZSTD_SRC/build/cmake/wasm-build" && emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF \
    -DZSTD_BUILD_SHARED=OFF \
    -DZSTD_BUILD_STATIC=ON \
    -DZSTD_MULTITHREAD_SUPPORT=OFF \
    -DZSTD_LEGACY_SUPPORT=OFF \
    -DCMAKE_INSTALL_PREFIX="$ZSTD_PREFIX")
  (cd "$ZSTD_SRC/build/cmake/wasm-build" && emmake make -j"$(nproc)" && emmake make install)
fi

# ── Step 3: Cross-compile Google Benchmark to WASM ───────────────────────────
if [ ! -f "$BENCH_PREFIX/lib/libbenchmark.a" ]; then
  echo "=== Building Google Benchmark for WASM ==="
  if [ ! -d "$BENCH_SRC" ]; then
    git clone --depth 1 https://github.com/google/benchmark.git "$BENCH_SRC"
  fi
  mkdir -p "$BENCH_SRC/build"
  (cd "$BENCH_SRC/build" && emcmake cmake .. \
    -DBENCHMARK_ENABLE_TESTING=OFF \
    -DBENCHMARK_ENABLE_INSTALL=ON \
    -DBENCHMARK_ENABLE_EXCEPTIONS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$BENCH_PREFIX" \
    -DCMAKE_CXX_FLAGS="-fwasm-exceptions")
  (cd "$BENCH_SRC/build" && emmake make -j"$(nproc)" && emmake make install)
fi

# ── Step 4: Cross-compile Catch2 to WASM ─────────────────────────────────────
if [ ! -f "$CATCH2_PREFIX/lib/libCatch2Main.a" ]; then
  echo "=== Building Catch2 for WASM ==="
  if [ ! -d "$CATCH2_SRC" ]; then
    git clone --depth 1 --branch v3.8.0 https://github.com/catchorg/Catch2.git "$CATCH2_SRC"
  fi
  mkdir -p "$CATCH2_SRC/build"
  (cd "$CATCH2_SRC/build" && emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$CATCH2_PREFIX" \
    -DCMAKE_CXX_FLAGS="-fwasm-exceptions" \
    -DCATCH_BUILD_TESTING=OFF \
    -DCATCH_INSTALL_DOCS=OFF)
  (cd "$CATCH2_SRC/build" && emmake make -j"$(nproc)" && emmake make install)
fi

echo "=== Dependencies ready ==="
echo "Build the WASM targets with xmake from the repository root, e.g.:"
echo "  xmake build wasm_embind        # build/bytecask.mjs (Embind JS module)"
echo "  xmake build wasm_smoke_test    # build/wasm_smoke_test.js"
echo "  xmake build wasm_engine_bench  # build/engine_bench_nodefs.js"
echo "  xmake build wasm_tests         # build/bytecask_tests.js"
