# Builds libbytecask.a for the benchmark scripts. Sourced, not executed; no
# side effects at source time.
#
# Every build starts from nothing. xmake.lua compiles with -march=native, and
# instance types change between sessions when one is not available: objects
# left by a build on another CPU can use instructions this one lacks (an
# AVX-512 build on an AMD EPYC 7571 dies with SIGILL on its first write), and
# an incremental build does not notice, because the flags read the same.

# The key directory a tree is benchmarked with: BYTECASK_KEYDIR when set,
# else what its last build here recorded, else what its archive was built
# with as far as can be told (buffered or not), else blind.
bench_keydir() {  # tree root
  local root="$1" lib="$1/build/linux/$(uname -m)/release/libbytecask.a"
  if [[ -n "${BYTECASK_KEYDIR:-}" ]]; then echo "$BYTECASK_KEYDIR"
  elif [[ -s "$root/build/bench_keydir" ]]; then cat "$root/build/bench_keydir"
  elif [[ -f "$lib" ]] && strings "$lib" | grep -q 'buffered key directory'; then echo buffered
  else echo blind
  fi
}

# Deletes the tree's build directory and xmake configuration, then builds
# libbytecask.a, release, with -g so perf can name inlined functions (it does
# not change the generated code). Output goes to stdout.
build_bytecask_lib() {  # tree root, key directory
  local root="$1" kd="$2" lib="$1/build/linux/$(uname -m)/release/libbytecask.a"
  echo "=== Building libbytecask.a from scratch: keydir=$kd, for $(lscpu | sed -n 's/^Model name: *//p') ==="
  rm -rf "$root/build" "$root/.xmake" || return 1
  (cd "$root" && BYTECASK_KEYDIR="$kd" xmake f -c -y -m release --cxflags=-g &&
     BYTECASK_KEYDIR="$kd" xmake build -y bytecask) || return 1
  local is_buffered=no
  strings "$lib" | grep -q 'buffered key directory' && is_buffered=yes
  if [[ "$kd" == buffered && "$is_buffered" == no ]] ||
     [[ "$kd" != buffered && "$is_buffered" == yes ]]; then
    echo "ERROR: $lib is not the $kd key directory"; return 1
  fi
  echo "$kd" > "$root/build/bench_keydir"
}
