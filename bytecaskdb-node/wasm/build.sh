#!/usr/bin/env bash
# Build ByteCaskDB as a WebAssembly binary for Node.js with real file I/O.
#
# Prerequisites:
#   - Emscripten SDK activated (source emsdk_env.sh)
#   - Internet access (clones google/crc32c and google/benchmark on first run)
#
# Usage:
#   cd bytecaskdb-node/wasm && bash build.sh
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
ROOT="$SCRIPT_DIR/../.."
BUILD="$SCRIPT_DIR/build"
PCM="$BUILD/pcm-nodefs"
CRC32C_SRC="$BUILD/crc32c-src"
CRC32C_PREFIX="$BUILD/crc32c-wasm"
BENCH_SRC="$BUILD/benchmark-src"
BENCH_PREFIX="$BUILD/benchmark-wasm"
CATCH2_SRC="$BUILD/catch2-src"
CATCH2_PREFIX="$BUILD/catch2-wasm"

CFLAGS="-std=c++23 -O2 -DNDEBUG -DBYTECASK_SINGLE_THREADED -fwasm-exceptions"
CRC32C_INCLUDE="-I$CRC32C_PREFIX/include"
BENCH_INCLUDE="-I$BENCH_PREFIX/include"
CATCH2_INCLUDE="-I$CATCH2_PREFIX/include"

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

# ── Step 1c: Cross-compile Catch2 to WASM ────────────────────────────────────
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

# ── Step 3b: Compile testing-variant library objects ─────────────────────────
echo "=== Compiling testing-variant objects ==="
TESTING="$PCM/testing"
mkdir -p "$TESTING"

# Precompile modules with BYTECASK_TESTING for test-only exports
echo "  Precompiling testing modules..."
TMODS=""
testing_precompile() {
  local name="$1" file="$2"
  echo "    $name"
  emcc $CFLAGS -DBYTECASK_TESTING --precompile $TMODS $CRC32C_INCLUDE \
    -I"$ROOT/bytecaskdb" "$ROOT/$file" -o "$TESTING/$name.pcm"
  TMODS="$TMODS -fmodule-file=$name=$TESTING/$name.pcm"
}

testing_precompile bytecask.types        bytecaskdb/types.cppm
testing_precompile bytecask.util         bytecaskdb/util.cppm
testing_precompile bytecask.serialization bytecaskdb/serialization.cppm
testing_precompile bytecask.data_entry   bytecaskdb/data_entry.cppm
testing_precompile bytecask.hint_entry   bytecaskdb/hint_entry.cppm
testing_precompile bytecask.radix_tree   bytecaskdb/radix_tree.cppm
testing_precompile bytecask.u32_map      bytecaskdb/u32_map.cppm
testing_precompile bytecask.data_file    bytecaskdb/data_file.cppm
testing_precompile bytecask.hint_file    bytecaskdb/hint_file.cppm
testing_precompile bytecask.concurrency  bytecaskdb/concurrency.cppm
testing_precompile bytecask:internals    bytecaskdb/internals.cppm
testing_precompile bytecask              bytecaskdb/bytecask.cppm

for f in "${CPPM_FILES[@]}"; do
  emcc $CFLAGS -DBYTECASK_TESTING -c $TMODS $CRC32C_INCLUDE \
    -I"$ROOT/bytecaskdb" "$ROOT/bytecaskdb/${f}.cppm" -o "$TESTING/${f}.o"
done

emcc $CFLAGS -DBYTECASK_TESTING -c $TMODS $CRC32C_INCLUDE \
  -I"$ROOT/bytecaskdb" "$ROOT/bytecaskdb/bytecask.cppm" -o "$TESTING/bytecask_ifc.o"
emcc $CFLAGS -DBYTECASK_TESTING -c $TMODS $CRC32C_INCLUDE \
  -I"$ROOT/bytecaskdb" "$ROOT/bytecaskdb/bytecask.cpp" -o "$TESTING/bytecask_impl.o"

# ── Step 3c: Compile Catch2 test files ───────────────────────────────────────
echo "=== Compiling test files ==="
TEST_FILES=(bytecask_test data_file_test data_entry_test hint_file_test
            radix_tree_test invariants_test)
for t in "${TEST_FILES[@]}"; do
  emcc $CFLAGS -DBYTECASK_TESTING -c $TMODS $CRC32C_INCLUDE $CATCH2_INCLUDE \
    -I"$ROOT/bytecaskdb" -I"$ROOT/tests" \
    "$ROOT/tests/${t}.cpp" -o "$TESTING/${t}.o"
done

PROOF_FILES=(prove_apply_batch prove_resume prove_vacuum_compact prove_vacuum_absorb)
for p in "${PROOF_FILES[@]}"; do
  emcc $CFLAGS -DBYTECASK_TESTING -c $TMODS $CRC32C_INCLUDE $CATCH2_INCLUDE \
    -I"$ROOT/bytecaskdb" -I"$ROOT/tests" \
    "$ROOT/tests/proof/generated/${p}.cpp" -o "$TESTING/${p}.o"
done

emcc $CFLAGS -DBYTECASK_TESTING -c $CATCH2_INCLUDE \
  "$SCRIPT_DIR/catch2_stringmakers.cpp" -o "$TESTING/catch2_stringmakers.o"

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

# Catch2 test suite (uses testing-variant library objects with fault injection)
TEST_LIB_OBJ=()
for f in "${CPPM_FILES[@]}"; do
  TEST_LIB_OBJ+=("$TESTING/${f}.o")
done
TEST_LIB_OBJ+=("$TESTING/bytecask_ifc.o" "$TESTING/bytecask_impl.o")

TEST_OBJ=()
for t in "${TEST_FILES[@]}"; do TEST_OBJ+=("$TESTING/${t}.o"); done
for p in "${PROOF_FILES[@]}"; do TEST_OBJ+=("$TESTING/${p}.o"); done
TEST_OBJ+=("$TESTING/catch2_stringmakers.o")

emcc "${LINK_COMMON[@]}" \
  "${TEST_LIB_OBJ[@]}" "${TEST_OBJ[@]}" \
  -L"$CATCH2_PREFIX/lib" -lCatch2Main -lCatch2 \
  -sSTACK_SIZE=2097152 \
  -o "$BUILD/bytecask_tests.js"

echo "=== Build complete ==="
echo "Run smoke test:  node $BUILD/bytecask_node.js"
echo "Run Catch2 tests: node $BUILD/bytecask_tests.js '~[concurrency]' '~[lock]'"
echo "Run benchmarks:  BC_DATASET_SIZE=100000 node $BUILD/engine_bench_nodefs.js"
echo "Embind module:   $BUILD/bytecask.mjs"
