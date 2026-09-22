#!/bin/bash
# dbio.sh [interval] — per-engine I/O rates from /proc/<pid>/io. No root, no packages.
# rchar/wchar = bytes via syscalls (page cache counts)
# read_bytes/write_bytes = bytes that actually reached the block layer
int=${1:-2}
declare -A prev
while :; do
  printf '\n%-11s %10s %10s %10s %10s %9s %9s\n' ENGINE rchar/s wchar/s rblk/s wblk/s rsys/s wsys/s
  for p in $(pgrep -x mariadbd); do
    eng=$(tr '\0' '\n' < /proc/$p/cmdline | grep -oP '_sysbench_\K[a-z]+' | head -1)
    eval "$(awk -F': *' '/^(rchar|wchar|read_bytes|write_bytes|syscr|syscw)/{printf "%s=%s ", $1, $2}' /proc/$p/io)"
    k=$p; cur="$rchar $wchar $read_bytes $write_bytes $syscr $syscw"
    if [[ -n ${prev[$k]} ]]; then
      read -r a b c d e f <<< "${prev[$k]}"; read -r A B C D E F <<< "$cur"
      printf '%-11s %10s %10s %10s %10s %9d %9d\n' "${eng:-$p}" \
        "$(numfmt --to=iec $(( (A-a)/int )))" "$(numfmt --to=iec $(( (B-b)/int )))" \
        "$(numfmt --to=iec $(( (C-c)/int )))" "$(numfmt --to=iec $(( (D-d)/int )))" \
        "$(( (E-e)/int ))" "$(( (F-f)/int ))"
    fi
    prev[$k]=$cur
  done
  sleep "$int"
done
