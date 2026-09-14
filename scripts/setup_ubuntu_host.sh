#!/usr/bin/env bash
# Sets up a bare Ubuntu host to build and test ByteCaskDB, without the Dev
# Container. Translates .devcontainer/Dockerfile (Fedora/dnf) and
# .devcontainer/post-create.sh to apt/Ubuntu, and reuses the xmake-version
# and nanobind checks from .claude/hooks/session-start.sh (which does the
# same thing for Claude Code's bare-Ubuntu remote containers).
#
# Usage: ./scripts/setup_ubuntu_host.sh [--skip-emsdk] [--skip-mariadb] [--skip-rocksdb]
# Run from the repository root. Tested on Ubuntu 24.04 (noble). Idempotent —
# safe to re-run.
set -euo pipefail

SKIP_EMSDK=0
SKIP_MARIADB=0
SKIP_ROCKSDB=0
for arg in "$@"; do
  case "$arg" in
    --skip-emsdk) SKIP_EMSDK=1 ;;
    --skip-mariadb) SKIP_MARIADB=1 ;;
    --skip-rocksdb) SKIP_ROCKSDB=1 ;;
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }

if [ ! -f "xmake.lua" ]; then
  echo "Run this from the ByteCaskDB repository root." >&2
  exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

log "Enabling universe (librocksdb-dev, libcrc32c-dev live there) and updating apt"
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq software-properties-common
$SUDO add-apt-repository -y universe || true

# deb-src is needed further down to fetch a matching MariaDB server source
# tree (bytecaskdb-mariadb-plugin needs handler.h and friends, which no
# Debian/Ubuntu mariadb package ships — see the MariaDB source block below).
# Ubuntu 24.04's deb822 sources file has one "Types: deb" line per stanza;
# flip each to "deb deb-src" so every already-configured mirror/suite gets a
# matching source entry, without hardcoding a mirror URL.
UBUNTU_SOURCES="/etc/apt/sources.list.d/ubuntu.sources"
if [ "$SKIP_MARIADB" -eq 0 ]; then
  if [ -f "$UBUNTU_SOURCES" ]; then
    if ! grep -q '^Types:.*deb-src' "$UBUNTU_SOURCES"; then
      log "Enabling deb-src in $UBUNTU_SOURCES (needed to fetch MariaDB server source)"
      $SUDO sed -i 's/^Types: deb$/Types: deb deb-src/' "$UBUNTU_SOURCES"
    fi
  else
    echo "WARNING: $UBUNTU_SOURCES not found (pre-24.04 sources.list format?)." >&2
    echo "  Enable deb-src manually, then re-run this script, to fetch MariaDB server source." >&2
  fi
fi

$SUDO apt-get update -qq

# Fedora's gcc/gcc-c++/libstdc++-devel/make -> build-essential. clang is the
# only supported compiler for the C++23-modules build (see xmake config
# below); llvm/lld/libc++ match the Dockerfile's clang-tools-extra/llvm-devel/
# libcxx-devel (Ubuntu's package is named "clang-tools", not
# "clang-tools-extra"). mariadb/rocksdb are optional plugin/benchmark deps.
# crc32c is deliberately NOT installed as a system package here: there is no
# libcrc32c-dev on Ubuntu, and xmake.lua already fetches it itself via
# add_requires("crc32c") — the Dockerfile's google-crc32c-devel is Fedora-only
# cache-warming, not a real dependency.
PACKAGES=(
  build-essential gdb make ninja-build git pkg-config cmake curl ca-certificates
  clang clang-tools llvm-dev lld libc++-dev libc++abi-dev
  python3-pip python3-dev python3-venv python-is-python3
  npm nodejs
)
[ "$SKIP_MARIADB" -eq 0 ] && PACKAGES+=(mariadb-server mariadb-client libmariadb-dev libmariadb-dev-compat)
[ "$SKIP_ROCKSDB" -eq 0 ] && PACKAGES+=(librocksdb-dev)

log "Installing packages: ${PACKAGES[*]}"
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${PACKAGES[@]}"

if [ "$SKIP_MARIADB" -eq 0 ]; then
  # bytecaskdb-mariadb-plugin needs handler.h and the rest of MariaDB's
  # server-internal sql/ headers to build. Fedora's mariadb-devel bundles
  # them; no Debian/Ubuntu mariadb package ships them at all. The only way to
  # get them here is to fetch and partially build a matching MariaDB server
  # source tree — bytecaskdb-mariadb-plugin/CMakeLists.txt falls back to it
  # (at .mariadb_server_src / .mariadb_server_build) when it can't find the
  # Fedora-style layout. Only the "sql" static lib and its generated headers
  # are needed, so build just the mariadbd target with the heaviest optional
  # storage engines/plugins (this project doesn't need) turned off.
  MARIADB_SERVER_SRC_DIR="$(pwd)/.mariadb_server_src"
  MARIADB_SERVER_BUILD_DIR="$(pwd)/.mariadb_server_build"
  if [ -f "$MARIADB_SERVER_SRC_DIR/sql/handler.h" ] && [ -f "$MARIADB_SERVER_BUILD_DIR/sql/mariadbd" ]; then
    log "MariaDB server source/build already present at $MARIADB_SERVER_SRC_DIR — skipping (delete it to rebuild)"
  else
    log "Fetching and building matching MariaDB server source (for plugin headers) — several minutes"
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get build-dep -y -qq mariadb-server
    _mariadb_src_tmp="$(mktemp -d)"
    ( cd "$_mariadb_src_tmp" && apt-get source mariadb-server )
    _mariadb_src_extracted="$(find "$_mariadb_src_tmp" -maxdepth 1 -mindepth 1 -type d -name 'mariadb-*')"
    rm -rf "$MARIADB_SERVER_SRC_DIR" "$MARIADB_SERVER_BUILD_DIR"
    mv "$_mariadb_src_extracted" "$MARIADB_SERVER_SRC_DIR"
    rm -rf "$_mariadb_src_tmp"
    cmake -G Ninja -S "$MARIADB_SERVER_SRC_DIR" -B "$MARIADB_SERVER_BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DPLUGIN_MROONGA=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_ROCKSDB=NO -DPLUGIN_SPIDER=NO \
      -DPLUGIN_SPHINX=NO -DPLUGIN_CONNECT=NO -DPLUGIN_S3=NO -DPLUGIN_FEDERATED=NO \
      -DPLUGIN_FEDERATEDX=NO -DPLUGIN_ARCHIVE=NO -DPLUGIN_BLACKHOLE=NO \
      -DPLUGIN_TEST_SQL_DISCOVERY=NO -DPLUGIN_TEST_SQL_SERVICE=NO -DPLUGIN_TEST_VERSIONING=NO \
      -DPLUGIN_TYPE_TEST=NO -DPLUGIN_FUNC_TEST=NO -DPLUGIN_AUTH_TEST_PLUGIN=NO \
      -DWITH_UNIT_TESTS=OFF -DCONC_WITH_UNIT_TESTS=OFF -DWITH_MARIABACKUP=OFF
    cmake --build "$MARIADB_SERVER_BUILD_DIR" --target mariadbd --parallel
  fi
fi

log "Installing xmake"
# Ubuntu noble's apt xmake is 2.8.7, which can't load the current xmake-repo
# ("attempt to call a nil value (global 'on_source')") — same reasoning as
# .claude/hooks/session-start.sh. Unlike Claude's sandboxed remote
# containers, this host has normal internet access, so the official
# installer (also used by scripts/setup_manylinux.sh) works directly.
xmake_is_current() {
  command -v xmake >/dev/null 2>&1 || return 1
  local major
  major=$(xmake --version 2>/dev/null | sed -n 's/.*xmake v\([0-9][0-9]*\).*/\1/p' | head -1)
  [ -n "$major" ] && [ "$major" -ge 3 ]
}
if xmake_is_current; then
  echo "xmake already current: $(xmake --version | head -1)"
else
  curl -fsSL https://xmake.io/shget.text | bash
  # The installer's own ~/.xmake/profile isn't safe to `source` under `set -u`
  # (it checks $FISH_VERSION with no default) and only takes effect in new
  # shells anyway. It installs to ~/.local/bin; put that on PATH directly so
  # the rest of this script can use xmake right away.
  export PATH="$HOME/.local/bin:$PATH"
  xmake_is_current || { echo "xmake install failed or is still too old" >&2; exit 1; }
fi

log "Installing Python build dependencies (nanobind, pytest, hypothesis, auditwheel)"
# xmake's Python target imports nanobind while loading, so configure fails
# without it. --break-system-packages: Ubuntu 24.04 marks the system
# interpreter externally-managed (PEP 668).
pip3 install --quiet --break-system-packages nanobind pytest hypothesis auditwheel
python3 -c "import nanobind; print('nanobind:', nanobind.include_dir())"

if [ "$SKIP_EMSDK" -eq 0 ]; then
  log "Installing Emscripten SDK for WASM builds (bytecaskdb-node) — one-time clone, several hundred MB"
  if [ ! -d "$HOME/emsdk" ]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$HOME/emsdk"
  fi
  (cd "$HOME/emsdk" && ./emsdk install latest && ./emsdk activate latest)
  if ! grep -qF 'emsdk_env.sh' "$HOME/.bashrc" 2>/dev/null; then
    echo 'source "$HOME/emsdk/emsdk_env.sh" > /dev/null' >> "$HOME/.bashrc"
  fi
  # shellcheck disable=SC1091
  source "$HOME/emsdk/emsdk_env.sh" > /dev/null
else
  log "Skipping Emscripten SDK (--skip-emsdk) — bytecaskdb-node WASM target will not build"
fi

log "Configuring xmake with the clang toolchain"
RESOURCE_DIR=$(clang --print-resource-dir)
xmake f --toolchain=clang --cc="$(command -v clang)" --cxx="$(command -v clang++)" \
        --cxflags="-resource-dir=${RESOURCE_DIR}" -y

log "Seeding local Claude settings from defaults"
mkdir -p .claude
cp -n .claude.defaults/settings.json .claude/settings.json 2>/dev/null || true

log "Installing graft (repo context graph) under \$HOME/.npm-global"
NPM_PREFIX="${HOME}/.npm-global"
mkdir -p "$NPM_PREFIX"
export PATH="${NPM_PREFIX}/bin:${PATH}"
if ! command -v graft >/dev/null 2>&1; then
  npm install -g --prefix "$NPM_PREFIX" @nanonets/graft
fi
graft init --agents copilot --agents claude -y

if ! grep -qF 'NPM_PREFIX' "$HOME/.bashrc" 2>/dev/null; then
  {
    echo 'export NPM_PREFIX="${HOME}/.npm-global"'
    echo 'export PATH=$NPM_PREFIX/bin:$PATH'
  } >> "$HOME/.bashrc"
fi

log "Done. Open a new shell (or run 'source ~/.bashrc'), then:"
echo "  xmake build bytecask_tests && xmake run bytecask_tests"
