#!/bin/bash
# SessionStart hook — provisions the build toolchain for Claude Code on the web.
#
# Local checkouts and the Dev Container already have everything (see
# .devcontainer/Dockerfile, which installs clang + xmake via dnf on Fedora).
# The web containers start from a bare Ubuntu image, so without this hook a
# session cannot build or test the project at all.
#
# Idempotent: re-running is a no-op once the container state is cached.
set -euo pipefail

[ "${CLAUDE_CODE_REMOTE:-}" = "true" ] || exit 0

log() { printf '[session-start] %s\n' "$*" >&2; }
strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }
project_dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# xmake refuses to run as root without this, and these containers run as root.
export XMAKE_ROOT=y
[ -n "${CLAUDE_ENV_FILE:-}" ] && echo 'export XMAKE_ROOT=y' >> "$CLAUDE_ENV_FILE"

apt_install() {
  apt-get update -qq >&2
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$@" >&2
}

# xmake 3.x or newer. Ubuntu noble/universe ships 2.8.7, which cannot load the
# current xmake-repo ("attempt to call a nil value (global 'on_source')"), so
# apt is not an option here.
xmake_is_current() {
  command -v xmake >/dev/null 2>&1 || return 1
  # xmake colours its --version output, so the banner does not start with
  # "xmake" — match it anywhere on the line rather than anchoring.
  local major
  major=$(xmake --version 2>/dev/null | sed -n 's/.*xmake v\([0-9][0-9]*\).*/\1/p' | head -1)
  [ -n "$major" ] && [ "$major" -ge 3 ]
}

if xmake_is_current; then
  log "xmake present: $(xmake --version 2>/dev/null | strip_ansi | head -1 | cut -d, -f1)"
else
  # https://xmake.io is not reachable from these containers (the egress policy
  # 403s it), so the official installer is out. Building from the git clone
  # works — git to GitHub is allowed even though plain HTTPS archive
  # downloads are not.
  log "building xmake from source (a few minutes, cached afterwards)"
  apt_install git build-essential libreadline-dev
  src=$(mktemp -d)
  git clone --depth 1 --recursive https://github.com/xmake-io/xmake.git "$src" >&2
  (
    cd "$src"
    ./configure >&2
    make --no-print-directory -j"$(nproc)" >&2
    make --no-print-directory install PREFIX=/usr/local >&2
  )
  rm -rf "$src"
  hash -r
fi

# Clang is the only supported compiler — the engine is C++23 modules throughout.
# Pinned: Ubuntu's unversioned `clang` is 18, older than CI's (Fedora 43).
# The unversioned names are linked in /usr/local/bin (ahead of /usr/bin on
# PATH) so xmake, clang-scan-deps and clangd are one version: clangd cannot
# read module files built by a different clang.
llvm_version=20
llvm_bin="/usr/lib/llvm-${llvm_version}/bin"
if [ ! -x "$llvm_bin/clangd" ] || [ ! -x "$llvm_bin/clang-scan-deps" ]; then
  log "installing clang ${llvm_version}"
  apt_install "clang-${llvm_version}" "clangd-${llvm_version}" "clang-tools-${llvm_version}"
fi
for tool in clang clang++ clang-scan-deps clangd; do
  ln -sf "$llvm_bin/$tool" "/usr/local/bin/$tool"
done
hash -r

# xmake loads the Python extension target during configure, which imports
# nanobind; without it, configure fails outright.
python3 -c 'import nanobind' >/dev/null 2>&1 || {
  log "installing nanobind"
  pip install --quiet --break-system-packages nanobind >&2
}

# Configure now so the xmake packages (crc32c, zstd, catch2, benchmark,
# jemalloc) are fetched into the cached container state rather than on first
# build. GitHub archive downloads 403 here; xmake falls back to git clone.
log "configuring xmake (fetches crc32c, zstd, catch2, benchmark, jemalloc)"
cd "$project_dir"
xmake f --toolchain=clang \
        --cxflags="-resource-dir=$(clang --print-resource-dir)" \
        -m debug -y >&2

# Serena — symbol-level code navigation over MCP (.mcp.json). Best-effort:
# without it the session still starts, and the MCP entry serves an empty
# server. Installed with the system Python (noble's 3.12 satisfies Serena's
# >=3.11), since uv's managed Pythons download from GitHub releases, which
# 403 here.
if ! command -v serena >/dev/null 2>&1; then
  log "installing serena (cached afterwards)"
  {
    command -v uv >/dev/null 2>&1 \
      || pip install --quiet --break-system-packages uv
    UV_TOOL_BIN_DIR=/usr/local/bin uv tool install --python-preference only-system serena-agent
  } >&2 || log "serena install failed; continuing without it"
fi

echo "Toolchain ready. Build and test with:"
echo "  xmake build bytecask_tests && xmake run bytecask_tests"
