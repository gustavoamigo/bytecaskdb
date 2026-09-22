#!/bin/bash

# Usage: sys-info.sh [TARGET_PATH]
#
# TARGET_PATH is the directory whose backing disk is described and measured.
# Callers that relocate benchmark data (benchmark_showcase.py --tmpdir) must
# pass it: otherwise the report describes a disk the benchmarks never touched.
# Defaults to ./.tmp, the benchmark data directory used when --tmpdir is absent.
target_path=${1:-$PWD/.tmp}
mkdir -p "$target_path" 2>/dev/null
target_path=$(cd "$target_path" 2>/dev/null && pwd -P) || target_path=$PWD

echo "=========================================="
echo "       SYSTEM HARDWARE CHARACTERISTICS     "
echo "=========================================="

# 1. CPU - Model, Architecture, and Speed
echo -e "\n[CPU INFORMATION]"
lscpu | grep -E 'Model name|Architecture|CPU MHz|Core\(s\) per socket'

# 2. Memory - Installed capacity
echo -e "\n[INSTALLED MEMORY]"
awk '
	/MemTotal:/ {
		printf("Installed RAM: %.1f GiB\n", $2 / 1024 / 1024)
		found = 1
	}
	END {
		if (!found) {
			print "Installed RAM: unavailable"
		}
	}
' /proc/meminfo

# 3. Disk - Hardware description, Model, and Size
# Resolve the benchmark data path's mount to its backing physical disk.
mount_source=$(findmnt -no SOURCE --target "$target_path")
mount_device=${mount_source%%[*}
parent_disk=$(lsblk -no PKNAME "$mount_device" 2>/dev/null)

echo -e "\n[DISK HARDWARE DESCRIPTION]"
echo "Measured path : $target_path"
if [[ -n "$parent_disk" ]]; then
	lsblk -d -o NAME,MODEL,SIZE,FSTYPE,TRAN "/dev/$parent_disk"
elif [[ -n "$mount_device" ]]; then
	lsblk -d -o NAME,MODEL,SIZE,FSTYPE,TRAN "$mount_device"
else
	echo "Unable to resolve backing disk for: $target_path"
fi
echo -e ""
#Gather local machine disk performance
fio_file="$target_path/fio_data"
bw=$(fio --name=single_disk_read_max \
    --filename="$fio_file" \
    --size=250M --rw=read --bs=1M --direct=1 \
    --numjobs=4 --iodepth=16 --ioengine=libaio \
    --group_reporting 2>/dev/null \
  | grep -oP 'bw=\K[^\s,]+')
echo "Sequential read speed : $bw"

bw_write=$(fio --name=single_disk_write_max \
    --filename="$fio_file" \
    --size=250M --rw=write --bs=1M --direct=1 \
    --numjobs=4 --iodepth=16 --ioengine=libaio \
    --group_reporting 2>/dev/null \
  | grep -oP 'bw=\K[^\s,]+')
echo "Sequential write speed: $bw_write"
rm -f "$fio_file"

echo -e "\n=========================================="
