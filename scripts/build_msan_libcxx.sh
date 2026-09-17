#!/usr/bin/env bash
# Builds an MSan-instrumented libc++/libc++abi and installs it under
# .msan-libcxx/. MemorySanitizer requires every piece of code that touches
# tracked memory to be compiled with -fsanitize=memory, including the C++
# standard library — the system's libcxx-devel package (used by the
# address/thread sanitizer builds) is not instrumented and produces constant
# false positives if linked into an MSan binary. Idempotent: skips the build
# if the prefix already has both static libraries.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX="${MSAN_LIBCXX_PREFIX:-$PROJECT_DIR/.msan-libcxx}"
SRC_DIR="$PROJECT_DIR/.msan-libcxx-src"
BUILD_DIR="$PROJECT_DIR/.msan-libcxx-build"

if [ -f "$PREFIX/lib/libc++.a" ] && [ -f "$PREFIX/lib/libc++abi.a" ]; then
    echo "==> MSan-instrumented libc++ already installed at $PREFIX"
    exit 0
fi

CLANG_MAJOR="$(clang -dumpversion | cut -d. -f1)"
echo "==> Building MSan-instrumented libc++/libc++abi for Clang $CLANG_MAJOR"

BRANCH=""
for candidate in "release/${CLANG_MAJOR}.x" "release/${CLANG_MAJOR}"; do
    if git ls-remote --exit-code --heads https://github.com/llvm/llvm-project.git "$candidate" >/dev/null 2>&1; then
        BRANCH="$candidate"
        break
    fi
done
if [ -z "$BRANCH" ]; then
    echo "ERROR: no llvm-project release branch found matching Clang $CLANG_MAJOR" >&2
    echo "       Set MSAN_LLVM_BRANCH to override branch detection." >&2
    exit 1
fi
BRANCH="${MSAN_LLVM_BRANCH:-$BRANCH}"
echo "==> Using llvm-project branch $BRANCH"

rm -rf "$SRC_DIR" "$BUILD_DIR"

# Partial clone (no blobs until checkout) + sparse-checkout: llvm-project is
# too large to clone in full just for libcxx/libcxxabi. `libc` is needed too —
# libcxx's charconv implementation (from_chars/to_chars for floating point)
# includes LLVM libc's header-only "shared" FP bit-manipulation helpers
# (transitively, several more libc/src/__support and libc/hdr headers), even
# though libc itself is never built.
git clone --filter=blob:none --depth 1 --branch "$BRANCH" --no-checkout \
    https://github.com/llvm/llvm-project.git "$SRC_DIR"
git -C "$SRC_DIR" sparse-checkout init --no-cone
git -C "$SRC_DIR" sparse-checkout set cmake llvm/cmake llvm/utils/llvm-lit runtimes libcxx libcxxabi libc libc
git -C "$SRC_DIR" checkout "$BRANCH"

echo "==> Configuring libcxx/libcxxabi (MemoryWithOrigins)..."
cmake -G Ninja -S "$SRC_DIR/runtimes" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLLVM_USE_SANITIZER=MemoryWithOrigins \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"

echo "==> Building..."
ninja -C "$BUILD_DIR" cxx cxxabi

echo "==> Installing to $PREFIX..."
ninja -C "$BUILD_DIR" install-cxx install-cxxabi

rm -rf "$SRC_DIR" "$BUILD_DIR"

echo "==> MSan-instrumented libc++ ready at $PREFIX"
