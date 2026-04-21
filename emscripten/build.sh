#!/usr/bin/env bash
# Build ByteCaskDB as a WebAssembly binary for Node.js with real file I/O.
#
# Prerequisites:
#   - Emscripten SDK activated (source emsdk_env.sh)
#   - Internet access (clones google/crc32c on first run)
#
# Usage:
#   cd emscripten && bash build.sh
#
# Output:
#   build/bytecask_node.js   — Node.js entry point
#   build/bytecask_node.wasm — WebAssembly binary
#
# Run:
#   node build/bytecask_node.js

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD="$SCRIPT_DIR/build"
PCM="$BUILD/pcm"
CRC32C_SRC="$BUILD/crc32c-src"
CRC32C_PREFIX="$BUILD/crc32c-wasm"

CFLAGS="-std=c++23 -pthread -O2"
CRC32C_INCLUDE="-I$CRC32C_PREFIX/include"

mkdir -p "$PCM"

# ── Step 1: Cross-compile crc32c to WASM ──────────────────────────────────────
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

# ── Step 4: Link ──────────────────────────────────────────────────────────────
echo "=== Linking ==="
OBJ_FILES=()
for f in "${CPPM_FILES[@]}"; do
  OBJ_FILES+=("$PCM/${f}.o")
done
OBJ_FILES+=("$PCM/bytecask_ifc.o" "$PCM/bytecask_impl.o" "$PCM/test.o")

emcc $CFLAGS \
  "${OBJ_FILES[@]}" \
  -L"$CRC32C_PREFIX/lib" -lcrc32c \
  -sNODERAWFS=1 -sENVIRONMENT=node -lnoderawfs.js \
  -sEXIT_RUNTIME=1 \
  -o "$BUILD/bytecask_node.js"

echo "=== Build complete ==="
echo "Run: node $BUILD/bytecask_node.js"
