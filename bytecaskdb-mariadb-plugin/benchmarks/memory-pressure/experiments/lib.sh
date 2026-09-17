# Shared helpers for the experiment drivers in this directory. Sourced.
#
# Every arm of every experiment starts from a byte-identical copy of one
# prepared dataset, because the write workloads mutate it. The copy is made
# once per (engine, rows, secondary-index) shape and kept under the data
# root; --fresh on the main script is not used here, the snapshot is.
#
# Callers set: EXP_NAME. Optional: DATA_ROOT, ROWS, THREADS, WARMUP, TIME,
# WORKLOADS, SECONDARY (on|off).

MP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$MP/../../.." && pwd)"
DATA_ROOT="${DATA_ROOT:-$MP/results}"
ROWS="${ROWS:-20000000}"
THREADS="${THREADS:-8}"
WARMUP="${WARMUP:-15}"
TIME="${TIME:-30}"
WORKLOADS="${WORKLOADS:-oltp_point_select,oltp_read_write,oltp_write_only}"
SECONDARY="${SECONDARY:-off}"
OUT_DIR="$DATA_ROOT/experiments/${EXP_NAME}-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$OUT_DIR"

GiB=$((1024 * 1024 * 1024))
MiB=$((1024 * 1024))
NO_LIMIT="--mem-limit=$((28 * GiB))"   # larger than the host: effectively none

log() { echo "[$(date +%H:%M:%S)] $*"; }

sec_flag() { [[ $SECONDARY == off ]] && echo "--no-secondary-index"; }

# Ensures a pristine snapshot exists for <engine> and returns its path.
pristine() {  # <engine: bytecaskdb|innodb>
  local p="$DATA_ROOT/pristine-$1-${ROWS}-sec${SECONDARY}"
  if [[ ! -d $p ]]; then
    log "preparing $1, $ROWS rows, secondary index $SECONDARY (once)" >&2
    rm -rf "$DATA_ROOT/$1"
    local eng=bytecaskdb-pool; [[ $1 == innodb ]] && eng=innodb
    (cd "$MP" && ./run-memory-pressure.sh --engines=$eng --rows="$ROWS" $(sec_flag) \
      --data-root="$DATA_ROOT" $NO_LIMIT --pool-bytes=$((256 * MiB)) \
      --innodb-pool-bytes=$((4 * GiB)) --workloads=oltp_point_select \
      --threads=1 --warmup=1 --time=2 --out="$OUT_DIR/prepare-$1.csv" \
      > "$OUT_DIR/prepare-$1.log" 2>&1) || { echo "prepare failed: see $OUT_DIR/prepare-$1.log" >&2; exit 1; }
    cp -a "$DATA_ROOT/$1" "$p"
  fi
  echo "$p"
}

# Restores <engine>'s data directory from its pristine snapshot.
restore() {  # <engine>
  local p; p="$(pristine "$1")"
  rm -rf "$DATA_ROOT/$1"; cp -a "$p" "$DATA_ROOT/$1"
}

# Builds the engine library on the current checkout; the main script builds
# the plugin against it.
build_engine() {
  (cd "$ROOT" && xmake build bytecask >/dev/null 2>&1) || { echo "engine build failed"; exit 1; }
}

# Runs one arm and prints its result table. <label> names the output file;
# the rest is passed to run-memory-pressure.sh.
arm() {  # <engine> <label> <args...>
  local engine="$1" label="$2"; shift 2
  local base=bytecaskdb; [[ $engine == innodb ]] && base=innodb
  restore "$base"
  log "=== $label ==="
  (cd "$MP" && ./run-memory-pressure.sh --engines="$engine" --rows="$ROWS" $(sec_flag) \
    --data-root="$DATA_ROOT" --workloads="$WORKLOADS" --threads="$THREADS" \
    --warmup="$WARMUP" --time="$TIME" "$@" --out="$OUT_DIR/$label.csv" \
    > "$OUT_DIR/$label.log" 2>&1)
  # The main script's own results table, without its header banner.
  sed -n '/=== Results/,$p' "$OUT_DIR/$label.log" | grep -vE "^\[|=== Results"
}
