#!/usr/bin/env bash
# compare-refs.sh — HammerDB TPROC-C on several builds of the engine, one
# after another in the same session, with one summary table at the end.
#
# Each REF (a branch or a commit) is checked out into a git worktree of its
# own under --worktrees, built there, and run with that tree's own
# run-hammerdb.sh. The checkout this script is run from is never touched, and
# a ref run again rebuilds incrementally. Every ByteCaskDB cell shares one
# schema (--reuse-data, restored fresh per cell), so the refs differ only in
# the engine.
#
# Usage:
#   compare-refs.sh [options] REF[:KEYDIR] [REF[:KEYDIR] ...]
#
#   REF       a branch on origin (fetched first) or any commit the repository has
#   KEYDIR    the key directory to build (BYTECASK_KEYDIR): blind (default),
#             buffered, btree or radix — e.g. perf/buffered-keydir:buffered
#
# Options (defaults in brackets):
#   --warehouses=N      TPROC-C warehouses [96]
#   --vus=N             virtual users [48]
#   --rampup=MIN        unmeasured minutes before the window [2]
#   --duration=MIN      measured minutes [5]
#   --profile=P         acid | fast [fast]
#   --data-root=PATH    where the servers keep their data [/mnt/disk1]
#   --hammerdb-home=P   HammerDB install [/opt/HammerDB-6.0/]
#   --rounds=N          run the refs N times, alternating: A B A B … [1]
#   --innodb            also run InnoDB once per round, for reference
#   --capture           profile each cell (see run-hammerdb.sh --capture)
#   --repo=PATH         the repository to take refs from [the one holding
#                       this script]
#   --worktrees=PATH    where the per-ref worktrees live [~/bench-trees]
#   --out=PATH          results directory [~/bench-results/<timestamp>]
#   --dry-run           build every ref, run nothing
#
# Example — the frame reserve against the contiguous write, twice each, with
# InnoDB for reference:
#   compare-refs.sh --rounds=2 --innodb perf/pool-reserve perf/contiguous-append
#
# Output: <out>/summary.txt (also printed), one log per cell, and with
# --capture the capture tarballs plus serial-section figures read from them:
# commits/s, how busy the serial section was, its µs per commit, and the
# engine counters that say what it waited on.
set -u

WAREHOUSES=96
VUS=48
RAMPUP=2
DURATION=5
PROFILE=fast
DATA_ROOT=/mnt/disk1
HDB_HOME=/opt/HammerDB-6.0/
ROUNDS=1
INNODB=off
CAPTURE=off
DRY_RUN=off
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WT_ROOT="$HOME/bench-trees"
OUT=""
REFS=()

for arg in "$@"; do
  case "$arg" in
    --warehouses=*)    WAREHOUSES="${arg#*=}" ;;
    --vus=*)           VUS="${arg#*=}" ;;
    --rampup=*)        RAMPUP="${arg#*=}" ;;
    --duration=*)      DURATION="${arg#*=}" ;;
    --profile=*)       PROFILE="${arg#*=}" ;;
    --data-root=*)     DATA_ROOT="${arg#*=}" ;;
    --hammerdb-home=*) HDB_HOME="${arg#*=}" ;;
    --rounds=*)        ROUNDS="${arg#*=}" ;;
    --innodb)          INNODB=on ;;
    --capture)         CAPTURE=on ;;
    --repo=*)          REPO="${arg#*=}" ;;
    --worktrees=*)     WT_ROOT="${arg#*=}" ;;
    --out=*)           OUT="${arg#*=}" ;;
    --dry-run)         DRY_RUN=on ;;
    --help|-h)         sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; /^set -u/d'; exit 0 ;;
    -*)                echo "unknown option: $arg (see --help)"; exit 1 ;;
    *)                 REFS+=("$arg") ;;
  esac
done
[[ ${#REFS[@]} -gt 0 ]] || { echo "give at least one REF (see --help)"; exit 1; }
OUT="${OUT:-$HOME/bench-results/$(date +%Y%m%d_%H%M%S)}"

# --- checks before building anything -------------------------------------
fail() { echo "ERROR: $*"; exit 1; }
git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1 || fail "--repo=$REPO is not a git repository"
if [[ "$DRY_RUN" == off ]]; then
  [[ -d "$DATA_ROOT" ]] || fail "--data-root=$DATA_ROOT does not exist"
  [[ -d "$HDB_HOME" ]] || fail "--hammerdb-home=$HDB_HOME does not exist"
  if [[ "$CAPTURE" == on ]]; then
    [[ $(cat /proc/sys/kernel/perf_event_paranoid) -le 1 ]] ||
      fail "--capture needs: sudo sysctl kernel.perf_event_paranoid=-1"
  fi
  if pgrep -x mariadbd >/dev/null; then
    fail "a mariadbd is already running; stop it first (pgrep -a mariadbd)"
  fi
fi
mkdir -p "$OUT" "$WT_ROOT" || fail "cannot create $OUT or $WT_ROOT"
free -g | sed -n 1,2p
echo "results: $OUT"

# --- per ref: resolve, check out, build ----------------------------------
# REF[:KEYDIR] -> ref, keydir, and a directory name for its worktree.
ref_of()    { echo "${1%%:*}"; }
keydir_of() { [[ "$1" == *:* ]] && echo "${1##*:}" || echo blind; }
slug_of()   { echo "$1" | tr '/:' '__'; }

resolve() {  # ref -> commit sha
  git -C "$REPO" fetch -q origin "$1" 2>/dev/null
  git -C "$REPO" rev-parse --verify -q "origin/$1^{commit}" 2>/dev/null ||
    git -C "$REPO" rev-parse --verify -q "$1^{commit}" 2>/dev/null
}

prepare() {  # spec -> builds its worktree; prints nothing on success
  local spec="$1" ref kd slug wt sha
  ref=$(ref_of "$spec"); kd=$(keydir_of "$spec"); slug=$(slug_of "$spec")
  wt="$WT_ROOT/$slug"
  sha=$(resolve "$ref") || { echo "cannot resolve $ref"; return 1; }
  if [[ -d "$wt" ]]; then
    git -C "$wt" checkout -q --detach "$sha" || return 1
  else
    git -C "$REPO" worktree add -q --detach "$wt" "$sha" || return 1
  fi
  echo "=== build $spec -> $(git -C "$wt" log --oneline -1) (keydir=$kd)"
  local cxflags=()
  [[ "$CAPTURE" == on ]] && cxflags=(--cxflags=-g)  # perf needs the symbols
  (cd "$wt" && BYTECASK_KEYDIR="$kd" xmake f -y -m release "${cxflags[@]}" >/dev/null &&
     BYTECASK_KEYDIR="$kd" xmake build -y bytecask) >"$OUT/build_$slug.log" 2>&1 ||
    { echo "build failed: see $OUT/build_$slug.log"; return 1; }
  # xmake remembers the key directory: check the archive is the one asked for.
  local lib="$wt/build/linux/$(uname -m)/release/libbytecask.a"
  local is_buffered=no
  strings "$lib" | grep -q 'buffered key directory' && is_buffered=yes
  if [[ "$kd" == buffered && "$is_buffered" == no ]] ||
     [[ "$kd" != buffered && "$is_buffered" == yes ]]; then
    echo "libbytecask.a in $wt is not the $kd key directory"; return 1
  fi
  echo "$sha" >"$OUT/sha_$slug"
}

for spec in "${REFS[@]}"; do
  prepare "$spec" || fail "could not prepare $spec"
done
[[ "$DRY_RUN" == on ]] && { echo "dry run: every ref built, nothing run"; exit 0; }

# --- run -----------------------------------------------------------------
# One HammerDB cell of engine with the tree of spec; writes the log and, with
# --capture, the tarball into $OUT.
cell() {  # spec engine label
  local spec="$1" engine="$2" label="$3" wt args
  wt="$WT_ROOT/$(slug_of "$spec")"
  args=(--engines="$engine" --warehouses="$WAREHOUSES" --vus="$VUS"
        --rampup="$RAMPUP" --duration="$DURATION" --profile="$PROFILE"
        --reuse-data --data-root="$DATA_ROOT" --hammerdb-home="$HDB_HOME")
  [[ "$CAPTURE" == on ]] && args+=(--capture --capture-dir="$OUT/captures_$label")
  echo "=== $label: $engine on $spec  $(date +%T)"
  (cd "$wt/bytecaskdb-mariadb-plugin/benchmarks" && ./run-hammerdb.sh "${args[@]}") \
    >"$OUT/$label.log" 2>&1
  grep -E "ByteCaskDB:|InnoDB:|FAILED|ERROR" "$OUT/$label.log" | head -5
  pgrep -x mariadbd >/dev/null && { echo "a mariadbd was left running; stopping"; pkill -x mariadbd; sleep 5; }
}

LABELS=()
for ((round = 1; round <= ROUNDS; ++round)); do
  for spec in "${REFS[@]}"; do
    label="r${round}_$(slug_of "$spec")"
    cell "$spec" bytecaskdb "$label"
    LABELS+=("$label|$spec|bytecaskdb")
  done
  if [[ "$INNODB" == on ]]; then
    label="r${round}_innodb"
    cell "${REFS[0]}" innodb "$label"
    LABELS+=("$label|innodb|innodb")
  fi
done

# --- summary -------------------------------------------------------------
# The serial section over a capture's measured window, from its engine-status
# snapshots. Empty when there is no capture.
serial_of() {  # capture dir
  local cap tmp
  cap=$(ls -t "$1"/capture_bytecaskdb_*.tar.gz 2>/dev/null | head -1)
  [[ -n "$cap" ]] || return 0
  tmp=$(mktemp -d)
  tar -xzf "$cap" -C "$tmp" --wildcards '*/status_start.txt' '*/status_end.txt' 2>/dev/null
  python3 - "$tmp"/*/ <<'EOF'
import re, sys
def load(f):
    lines = open(f).read().splitlines(); d = {}
    for l in lines:
        m = re.search(r'(bytecask\.\w+): (-?\d+)$', l.strip())
        if m: d[m.group(1)] = int(m.group(2))
    return float(lines[0]), d
try:
    t0, a = load(sys.argv[1] + 'status_start.txt'); t1, b = load(sys.argv[1] + 'status_end.txt')
except Exception:
    sys.exit(0)
g = lambda k: b.get(k, 0) - a.get(k, 0); el = t1 - t0
co = g('bytecask.group_writer_coalesced'); busy = g('bytecask.group_writer_busy_us')
if co == 0: sys.exit(0)
out = f"{co/el:>9.0f} {100*busy/(el*1e6):>6.1f}% {busy/co:>8.2f}"
out += f" {g('bytecask.pool_append_locked')/el:>10.0f}"
stall = g('bytecask.keydir_buffer_stall_us')
out += f" {stall/co:>9.2f}" if 'bytecask.keydir_buffer_stall_us' in b else f" {'-':>9}"
print(out)
EOF
  rm -rf "$tmp"
}

{
  echo "HammerDB TPROC-C: $WAREHOUSES warehouses, $VUS VUs, ramp $RAMPUP + $DURATION min, profile $PROFILE, data $DATA_ROOT, capture $CAPTURE"
  echo "host: $(uname -n), $(nproc) CPUs, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  printf '%-34s %-10s %10s %10s' label commit NOPM TPM
  [[ "$CAPTURE" == on ]] && printf ' %9s %7s %8s %10s %9s' commits/s serial us/commit locked/s stall_us
  echo
  for entry in "${LABELS[@]}"; do
    IFS='|' read -r label spec engine <<<"$entry"
    line=$(grep -hE "(ByteCaskDB|InnoDB):" "$OUT/$label.log" | head -1)
    nopm=$(echo "$line" | awk '{print $2}'); tpm=$(echo "$line" | awk '{print $5}')
    sha=-
    [[ "$engine" == bytecaskdb ]] && sha=$(cut -c1-10 "$OUT/sha_$(slug_of "$spec")")
    printf '%-34s %-10s %10s %10s' "$label" "$sha" "${nopm:-FAILED}" "${tpm:--}"
    if [[ "$CAPTURE" == on && "$engine" == bytecaskdb ]]; then
      printf ' %s' "$(serial_of "$OUT/captures_$label")"
    fi
    echo
  done
  echo
  echo "refs:"
  for spec in "${REFS[@]}"; do
    echo "  $spec  $(git -C "$WT_ROOT/$(slug_of "$spec")" log --oneline -1)"
  done
} | tee "$OUT/summary.txt"
echo "=== done $(date)  ($OUT)"
