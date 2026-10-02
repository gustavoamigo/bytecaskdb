#!/bin/bash
# Keeps Serena's symbol index (https://oraios.github.io/serena/) current for
# this checkout. A silent no-op when Serena is not installed.
#
# Usage: scripts/serena_index.sh [--compile-db-only] [--background]
#   --compile-db-only  only generate compile_commands.json, and only if it is
#                      missing (Serena's activation_command; needs no Serena).
#   --background       detach and return at once; output goes to
#                      .serena/logs/index.log. Used by the xmake build hook
#                      and the Claude Code SessionStart hook.
#
# The index is incremental: Serena caches symbols per file by content, so a
# run after a small change only asks clangd about the files that changed. The
# whole cache is dropped when compile_commands.json changes.
set -uo pipefail

compile_db_only=false
background=false
for arg in "$@"; do
  case "$arg" in
    --compile-db-only) compile_db_only=true ;;
    --background) background=true ;;
    *) echo "usage: $0 [--compile-db-only] [--background]" >&2; exit 2 ;;
  esac
done

root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel) || exit 0
cd "$root" || exit 0

if $compile_db_only; then
  [ -f compile_commands.json ] || scripts/gen_compile_commands.sh
  exit 0
fi

# uv installs tools to ~/.local/bin, which a non-login PATH may lack.
PATH="$PATH:$HOME/.local/bin"
command -v serena >/dev/null 2>&1 || exit 0

if $background; then
  mkdir -p .serena/logs
  setsid nohup "$0" >.serena/logs/index.log 2>&1 </dev/null &
  exit 0
fi

# One index run per checkout at a time; a run that finds another in progress
# waits for it, so the last build's state is the one indexed.
mkdir -p .serena
exec 9>.serena/index.lock
flock 9

[ -f compile_commands.json ] || scripts/gen_compile_commands.sh || exit 0
serena project index --log-level WARNING .
