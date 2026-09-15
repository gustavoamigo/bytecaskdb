#!/usr/bin/env bash
# Installs graft (repo context graph) from our fork at
# https://github.com/gustavoamigo/Graft instead of the npm registry. The fork
# recognizes the C++20 module interface extension (.cppm) used throughout
# bytecaskdb/*.cppm; upstream @nanonets/graft does not and silently skips the
# repo's primary source files.
#
# `npm install -g github:...` would be the one-liner, but npm 9 does not make
# devDependencies (tsc) available to the package's `prepare` script on a global
# git install, so the build fails. Instead: clone, build, `npm pack`, and
# install the tarball globally. dist/ is gitignored in the fork, so the build
# step is required.
#
# Idempotent — safe to re-run; re-installs from the fork's current main.
#
# Usage: ./scripts/install_graft.sh [git-ref]
# Installs under NPM_PREFIX (default $HOME/.npm-global).
set -euo pipefail

GRAFT_REPO="${GRAFT_REPO:-https://github.com/gustavoamigo/Graft.git}"
GRAFT_REF="${1:-main}"
NPM_PREFIX="${NPM_PREFIX:-$HOME/.npm-global}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone -q --depth 1 --branch "$GRAFT_REF" "$GRAFT_REPO" "$WORK/graft"
cd "$WORK/graft"
npm install --include dev --no-audit --no-fund --silent
npm run build --silent
TGZ="$(npm pack --silent)"

mkdir -p "$NPM_PREFIX"
npm install -g --prefix "$NPM_PREFIX" --no-audit --no-fund --silent "./$TGZ"
echo "graft $("$NPM_PREFIX/bin/graft" --version) installed from $GRAFT_REPO@$GRAFT_REF into $NPM_PREFIX"
