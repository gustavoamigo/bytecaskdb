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
command -v clang >/dev/null 2>&1 || { log "installing clang"; apt_install clang; }

# xmake loads the Python extension target during configure, which imports
# nanobind; without it, configure fails outright.
python3 -c 'import nanobind' >/dev/null 2>&1 || {
  log "installing nanobind"
  pip install --quiet --break-system-packages nanobind >&2
}

# Configure now so the four xmake packages (crc32c, catch2, benchmark,
# jemalloc) are fetched into the cached container state rather than on first
# build. GitHub archive downloads 403 here; xmake falls back to git clone.
log "configuring xmake (fetches crc32c, catch2, benchmark, jemalloc)"
cd "$project_dir"
xmake f --toolchain=clang \
        --cxflags="-resource-dir=$(clang --print-resource-dir)" \
        -m debug -y >&2

echo "Toolchain ready. Build and test with:"
echo "  xmake build bytecask_tests && xmake run bytecask_tests"
