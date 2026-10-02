#!/usr/bin/env bash
# Replays one recorded trace (record.sh) through every policy at pool sizes
# given as multiples of the live data at the start of the run.
#   sweep.sh <trace dir> <sim binary> [multiples...]
# With ABS=1, the sizes are MiB instead of multiples.
set -euo pipefail
DIR="$1" SIM="$2"; shift 2
MULTS=("${@:-1 1.5 2 3 4}")
[[ $# -gt 0 ]] && MULTS=("$@")
LIVE=$("$SIM" stats "$DIR/sorted.bin" "$DIR/phases.txt" | tee "$DIR/stats.txt" |
       sed -n 's/^at run start: live \([0-9.]*\) MiB.*/\1/p')
POLICIES=(pool wnv deadfirst sparse=0.25 sparse=0.5 ra ra_dense=0.5 ra_young=4 ra_live
          deadfirst+sparse=0.25+wnv)
BASE=$LIVE; [[ -n "${ABS:-}" ]] && BASE=1
OUT="$DIR/sweep.csv"
"$SIM" header > "$OUT"
jobs=()
for m in "${MULTS[@]}"; do
  mib=$(python3 -c "print(max(1, round($BASE * $m)))")
  for p in "${POLICIES[@]}"; do jobs+=("$mib ${p}"); done
done
printf '%s\n' "${jobs[@]}" |
  xargs -P 6 -L 1 sh -c 'SIM_NO_HEADER=1 "$0" run "$1" "$2" "$3" "$4"' "$SIM" "$DIR/sorted.bin" "$DIR/phases.txt" >> "$OUT"
# Belady needs several GB per run: one at a time.
for m in "${MULTS[@]}"; do
  mib=$(python3 -c "print(max(1, round($BASE * $m)))")
  SIM_NO_HEADER=1 "$SIM" run "$DIR/sorted.bin" "$DIR/phases.txt" "$mib" opt >> "$OUT"
done
echo "live at run start: $LIVE MiB" >> "$DIR/stats.txt"
