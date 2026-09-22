#!/bin/bash
# iowatch.sh [interval] [dev...]  — per-device read/write bytes + IOPS from /proc/diskstats
# No root, no packages. Fields: 3=name 6=sectors read 10=sectors written (512B units)
int=${1:-1}; shift
devs="${*:-sda nvme0n1}"
declare -A pr pw pri pwi
printf "%-10s %10s %10s %8s %8s\n" DEVICE READ/s WRITE/s R-IOPS W-IOPS
while :; do
  for d in $devs; do
    read -r _ _ name ri _ rs _ wi _ ws _ < <(grep -E " $d " /proc/diskstats)
    if [[ -n ${pr[$d]} ]]; then
      printf "%-10s %10s %10s %8s %8s\n" "$name" \
        "$(numfmt --to=iec $(( (rs - pr[$d]) * 512 / int )))/s" \
        "$(numfmt --to=iec $(( (ws - pw[$d]) * 512 / int )))/s" \
        "$(( (ri - pri[$d]) / int ))" "$(( (wi - pwi[$d]) / int ))"
    fi
    pr[$d]=$rs; pw[$d]=$ws; pri[$d]=$ri; pwi[$d]=$wi
  done
  sleep "$int"
done
