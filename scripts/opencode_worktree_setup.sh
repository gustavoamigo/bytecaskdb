#!/bin/bash
# Startup script OpenCode runs in each new worktree, right after it creates it
# (`Project.Commands.start`, see "Worktrees" in CONTRIBUTING.md). A fresh
# worktree is a bare checkout: no build directory, so no C++23 module BMIs and
# no compile_commands.json. Without them the first build pays for a full
# dependency fetch and every `import bytecask.*` is unresolved in the editor.
#
# Writes only gitignored paths (.xmake/, build/, compile_commands.json, .cache/),
# so it never dirties the worktree it runs in.
#
# Never fails the worktree creation: OpenCode runs this with `bash -lc` and a
# non-zero exit would surface as a creation error. Anything it cannot do here
# is reported and left to the human, with the command to run.
#
# That covers every failure inside the script. The one failure a script cannot
# catch — its own absence at a ref that predates it — is guarded one level up by
# the `commands.start` wrapper in CONTRIBUTING.md: it runs this script from the
# worktree checkout when the ref has one, from the main checkout otherwise, and
# exits 0 either way.
set -uo pipefail

cd "${OPENCODE_WORKTREE_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}" || exit 0

log() { printf '[worktree-setup] %s\n' "$*" >&2; }
skip() { log "$*"; exit 0; }

command -v xmake >/dev/null 2>&1 || skip "xmake not found — skipping build setup"

# Prebuilt object and BMI paths in compile_commands.json are absolute, so a
# reused worktree directory (the same name created twice) must start clean.
rm -rf build .xmake compile_commands.json

# clangd cannot read module files built by a different clang, so refuse to
# generate an index from a mismatched pair rather than produce one that is
# silently wrong. Only a warning: the build below still works.
clang_bin="$(command -v clang 2>/dev/null || true)"
clangd_bin="$(command -v clangd 2>/dev/null || true)"
if [ -n "$clang_bin" ] && [ -n "$clangd_bin" ]; then
  clang_ver="$("$clang_bin" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -1)"
  clangd_ver="$("$clangd_bin" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -1)"
  if [ -n "$clang_ver" ] && [ -n "$clangd_ver" ] && [ "${clang_ver%%.*}" != "${clangd_ver%%.*}" ]; then
    log "warning: clang ${clang_ver} and clangd ${clangd_ver} differ in major version;"
    log "         clangd will not resolve this worktree's module files"
  fi
fi

if ! xmake f -y >&2; then
  log "xmake configure failed — run 'xmake f' by hand to see why"
  exit 0
fi

# Compiling the engine produces the module BMIs that compile_commands.json
# points at. This is the slow step on a cold cache (dependencies are fetched
# and built).
if ! xmake build bytecask_tests >&2; then
  log "build failed — fix the error above, then re-run scripts/opencode_worktree_setup.sh"
  exit 0
fi

if ./scripts/gen_compile_commands.sh >&2; then
  log "ready — build and test with: xmake build bytecask_tests && xmake run bytecask_tests"
else
  log "index generation failed — re-run scripts/gen_compile_commands.sh by hand"
fi

exit 0
