#!/usr/bin/env bash
# Build ByteCaskDB as a WebAssembly binary for Node.js with real file I/O.
#
# Prerequisites:
#   - Emscripten SDK activated (source emsdk_env.sh)
#   - Internet access (clones google/crc32c and google/benchmark on first run)
#
# Usage:
#   cd emscripten && bash build.sh
#
# Output:
#   build/bytecask_node.js         — smoke test (Node.js + NODEFS)
#   build/engine_bench_nodefs.js   — benchmarks (Node.js + NODEFS)
#   build/bytecask.mjs             — Embind JS module (Node.js + NODEFS)
#
# Run:
#   node build/bytecask_node.js
#   BC_DATASET_SIZE=100000 node build/engine_bench_nodefs.js

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD="$SCRIPT_DIR/build"
PCM="$BUILD/pcm-nodefs"
CRC32C_SRC="$BUILD/crc32c-src"
CRC32C_PREFIX="$BUILD/crc32c-wasm"
BENCH_SRC="$BUILD/benchmark-src"
BENCH_PREFIX="$BUILD/benchmark-wasm"

CFLAGS="-std=c++23 -O2 -DNDEBUG -DBYTECASK_SINGLE_THREADED -fwasm-exceptions"
CRC32C_INCLUDE="-I$CRC32C_PREFIX/include"
BENCH_INCLUDE="-I$BENCH_PREFIX/include"

mkdir -p "$PCM"

# ── Step 1a: Cross-compile crc32c to WASM ─────────────────────────────────────
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

# ── Step 1b: Cross-compile Google Benchmark to WASM ──────────────────────────
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

# ── Step 2: Precompile C++20 modules in dependency order ──────────────────────
echo "=== Precompiling modules ==="
MODS=""

precompile() {
  local name="$1" file="$2"
  echo "  $name"
  emcc $CFLAGS --precompile $MODS $CRC32C_INCLUDE "$ROOT/$file" -o "$PCM/$name.pcm"
  MODS="$MODS -fmodule-file=$name=$PCM/$name.pcm"
}

precompile bytecask.types        bytecaskdb/types.cppm
precompile bytecask.util         bytecaskdb/util.cppm
precompile bytecask.serialization bytecaskdb/serialization.cppm
precompile bytecask.data_entry   bytecaskdb/data_entry.cppm
precompile bytecask.hint_entry   bytecaskdb/hint_entry.cppm
precompile bytecask.radix_tree   bytecaskdb/radix_tree.cppm
precompile bytecask.u32_map      bytecaskdb/u32_map.cppm
precompile bytecask.data_file    bytecaskdb/data_file.cppm
precompile bytecask.hint_file    bytecaskdb/hint_file.cppm
precompile bytecask.concurrency  bytecaskdb/concurrency.cppm
precompile bytecask:internals    bytecaskdb/internals.cppm
precompile bytecask              bytecaskdb/bytecask.cppm

# ── Step 3: Compile object files ──────────────────────────────────────────────
echo "=== Compiling objects ==="
CPPM_FILES=(types util serialization data_entry hint_entry radix_tree u32_map data_file hint_file concurrency internals)

for f in "${CPPM_FILES[@]}"; do
  emcc $CFLAGS -c $MODS $CRC32C_INCLUDE "$ROOT/bytecaskdb/${f}.cppm" -o "$PCM/${f}.o"
done

emcc $CFLAGS -c $MODS $CRC32C_INCLUDE "$ROOT/bytecaskdb/bytecask.cppm" -o "$PCM/bytecask_ifc.o"
emcc $CFLAGS -c $MODS $CRC32C_INCLUDE "$ROOT/bytecaskdb/bytecask.cpp" -o "$PCM/bytecask_impl.o"
emcc $CFLAGS -c $MODS $CRC32C_INCLUDE "$SCRIPT_DIR/test_node.cpp" -o "$PCM/test.o"
emcc $CFLAGS -c $MODS $CRC32C_INCLUDE "$SCRIPT_DIR/bytecask_embind.cpp" -o "$PCM/bytecask_embind.o"
emcc $CFLAGS -c $MODS $CRC32C_INCLUDE $BENCH_INCLUDE \
  -DBENCH_NO_LEVELDB -DBENCH_NO_ROCKSDB -DBENCH_NO_MT \
  "$ROOT/benchmarks/engine_bench.cpp" -o "$PCM/engine_bench.o"

# ── Step 4: Link ──────────────────────────────────────────────────────────────
echo "=== Linking ==="
LIB_OBJ=()
for f in "${CPPM_FILES[@]}"; do
  LIB_OBJ+=("$PCM/${f}.o")
done
LIB_OBJ+=("$PCM/bytecask_ifc.o" "$PCM/bytecask_impl.o")

LINK_COMMON=($CFLAGS
  -L"$CRC32C_PREFIX/lib" -lcrc32c
  -sNODERAWFS=1 -sENVIRONMENT=node -lnoderawfs.js
  -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH
  --pre-js "$SCRIPT_DIR/pre.js")

# Smoke test
emcc "${LINK_COMMON[@]}" \
  "${LIB_OBJ[@]}" "$PCM/test.o" \
  -o "$BUILD/bytecask_node.js"

# Benchmarks
emcc "${LINK_COMMON[@]}" \
  "${LIB_OBJ[@]}" "$PCM/engine_bench.o" \
  -L"$BENCH_PREFIX/lib" -lbenchmark -lbenchmark_main \
  -o "$BUILD/engine_bench_nodefs.js"

# Embind JS module
emcc $CFLAGS \
  -L"$CRC32C_PREFIX/lib" -lcrc32c \
  -lembind \
  -sNODERAWFS=1 -sENVIRONMENT=node -lnoderawfs.js \
  -sALLOW_MEMORY_GROWTH \
  -sMODULARIZE=1 -sEXPORT_NAME=createByteCask \
  --pre-js "$SCRIPT_DIR/pre.js" \
  "${LIB_OBJ[@]}" "$PCM/bytecask_embind.o" \
  -o "$BUILD/bytecask.mjs"

echo "=== Build complete ==="
echo "Run smoke test:  node $BUILD/bytecask_node.js"
echo "Run benchmarks:  BC_DATASET_SIZE=100000 node $BUILD/engine_bench_nodefs.js"
echo "Embind module:   $BUILD/bytecask.mjs"
