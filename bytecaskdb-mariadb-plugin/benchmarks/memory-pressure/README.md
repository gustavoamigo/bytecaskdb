# Memory-pressure benchmark

Runs the MariaDB plugin under a memory limit and compares the three ByteCaskDB
back-ends (`buffer_pool`, `mmap`, `pread`) with InnoDB. Each engine's
`mariadbd` runs inside a cgroup with `memory.max` set, so the page cache, the
key directory and the buffer pool all have to fit the same number. This is the
regime the buffer pool exists for; see
[`docs/buffer_pool_design.md`](../../../docs/buffer_pool_design.md) §8 for
results and what they mean.

## Requirements

- `mariadbd`, `mariadb` and `sysbench` on the `PATH`; the plugin is built by
  the script (`cmake`, Release).
- `systemd-run`, and cgroup v2 with the memory controller delegated to your
  systemd user session (the default): `grep memory
  /sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/cgroup.subtree_control`.
  Each server runs in a transient scope with `MemoryMax=` set, and the page
  cache is dropped per data file with `posix_fadvise(DONTNEED)`, so nothing
  needs root. Everything runs as you.
- `libjemalloc.so.2`: every server runs under jemalloc so its RSS is the
  engine's, not memory glibc's arenas keep after a free. `MARIADB_MALLOC=none`
  opts out.
- A swap device, only if you pass `--swap-limit`. The host needs one for the
  limit to mean anything; the script refuses otherwise.

Sizing the limit: the steady state is the key directory (~50 B per key,
two keys per row with the secondary index) plus the pool plus ~300 MB of
server, but two transients sit on top. Recovery builds per-worker trees and
merges them, so its peak is above the steady state (watch `memory.peak`).
Under a write load vacuum's compaction snapshot pins the key directory nodes
retired while it runs — up to a few hundred MB at 10 M rows; `SHOW ENGINE
BYTECASKDB STATUS` reports `keydir_versions_live` and `keydir_nodes_parked`
for exactly that.

## Running

```bash
# Defaults: 10 M rows, memory.max = 2.5 GiB, 512 MiB pool, 1.5 GiB InnoDB
# buffer pool, all four engines, four workloads, 1/8/16 threads, 30 s
# warm-up + 60 s measured per cell. About 75 minutes plus two prepares.
./run-memory-pressure.sh --data-root=/mnt/data/memory-pressure

# One back-end, two workloads, shorter cells.
./run-memory-pressure.sh --data-root=/mnt/data/memory-pressure \
  --engines=bytecaskdb-pool,bytecaskdb-mmap \
  --workloads=oltp_point_select,oltp_read_write --threads=8,16 \
  --warmup=20 --time=40

# A key directory larger than the limit, with swap allowed.
./run-memory-pressure.sh --data-root=/mnt/data/memory-pressure \
  --engines=bytecaskdb-pool --mem-limit=1073741824 \
  --pool-bytes=268435456 --swap-limit=2147483648 \
  --workloads=oltp_point_select --threads=8,16 --warmup=20 --time=40
```

Put `--data-root` on the disk you want the data files on; the default is
`results/` under this directory, which is git-ignored. Every run wipes the
engine directories under it and prepares from scratch — nothing from an
earlier run is reused, so a directory an earlier run's OOM kill left behind
cannot be handed to the next one. Within a run the three ByteCaskDB back-ends
share one prepare (InnoDB has its own), as long as the previous cell's server
shut down cleanly. Don't put a swapfile inside the data root.

| option | default | meaning |
|---|---|---|
| `--rows=N` | 10 M | rows in `sbtest1`: ~3.2 GB of ByteCaskDB data files and ~1.9 GB of key directory for its 20 M keys. Below ~6 M rows everything fits the default limit and nothing is under pressure. |
| `--mem-limit=BYTES` | 2560 MiB | the cgroup's `memory.max`. The key directory is anonymous memory the kernel cannot reclaim without swap, so the limit has to hold it plus the pool plus ~120 MB of server; the page cache gets what is left. |
| `--swap-limit=BYTES\|max` | 0 | the cgroup's `memory.swap.max`. `0` means exceeding the limit is an OOM kill; a value lets the kernel page the key directory out instead. |
| `--pool-bytes=BYTES` | 512 MiB | `bytecaskdb_buffer_pool_size` for `bytecaskdb-pool`. |
| `--engines=LIST` | all four | `bytecaskdb-pool`, `bytecaskdb-mmap`, `bytecaskdb-pread`, `innodb`. |
| `--workloads=LIST` | 4 OLTP mixes | sysbench `oltp_*` workloads. |
| `--threads=LIST` | 1,8,16 | |
| `--warmup=S` / `--time=S` | 30 / 60 | unmeasured run, then the measured run, per cell. |
| `--start-timeout=S` | 900 | how long to wait for `mariadbd` to accept connections. Recovery takes seconds with memory to spare and minutes when the key directory is being swapped. |
| `--out=FILE` | `<data-root>/memory_pressure_<timestamp>.csv` | |

Every engine restarts `mariadbd` inside the cgroup: the back-end sysvars are
read-only, memory is only charged to a cgroup from the moment a process joins
it, and recovery under the limit is part of what is measured.

## Watching a run

The script prints one line per cell and a table at the end. While the server
is starting under a tight limit the only live signal is the cgroup:

```bash
watch -n 2 'cd /sys/fs/cgroup/bytecaskdb-mempressure && \
  echo "mem  $(cat memory.current)"; echo "swap $(cat memory.swap.current)"; \
  grep -E "^(pgmajfault|anon|file) " memory.stat; grep oom_kill memory.events'
```

`memory.swap.current` is how much of the process is on disk, `pgmajfault`
counts the times a page had to come back, `oom_kill` says whether it died.
Once the socket is up, `SHOW ENGINE BYTECASKDB STATUS` on
`/tmp/bytecaskdb-mempressure.sock` has `recovery_duration_us` and the
`pool_hits` / `pool_misses` counters the hit ratio is computed from.

## Output

One CSV row per engine × workload × thread count:

`engine, workload, threads, rows, mem_limit_bytes, swap_limit, pool_bytes,
tps, avg_ms, p95_ms, pool_hit_ratio, rss_bytes, cgroup_memory_bytes,
cgroup_swap_bytes, major_faults, oom_kills, startup_s`

`major_faults` is how many pages the cgroup read back from swap during the
measured run; divided by the transactions in it, that is the per-query cost of
not being resident, and it is zero without swap.

`tps` is sysbench's transactions per second (not the run total).
`pool_hit_ratio` is empty for engines without a pool. `startup_s` is how long
the server took to accept connections inside the cgroup.

## Measured so far (4-vCPU Azure VM, 10 M rows, 3.2 GB of data files)

`memory.max = 2.5 GiB`, no swap, 16 threads:

| engine | point select | read-write (p95) |
|---|---:|---:|
| pread | 43.7 K/s | 460 tps (68 ms) |
| mmap | 43.4 K/s | 193 tps (117 ms) |
| buffer pool 512 MiB | 48.6 K/s | 1042 tps (22 ms) |
| InnoDB, 1.5 GiB buffer pool | 46.9 K/s | 993 tps (44 ms) |

Without a limit the three ByteCaskDB back-ends are within a few percent of
each other; the pool keeps 84–90 % of that under the limit.

`memory.max = 1 GiB`, `memory.swap.max = 2 GiB`, 256 MiB pool — a key
directory (~1.9 GB) larger than the limit:

| threads | point select | avg / p95 | pool hit | swapped | startup |
|---:|---:|---:|---:|---:|---:|
| 8 | 42.0 K/s | 0.19 / 0.42 ms | 85 % | 1.42 GB | 644 s |
| 16 | 46.1 K/s | 0.35 / 0.75 ms | 85 % | 1.44 GB | 644 s |

Steady state held 93–95 % of the no-swap throughput because sysbench's
default `special` distribution keeps the hot part of the key directory
resident; recovery, which touches every key once, took 644 s instead of
about 5 s, and a clean shutdown about four minutes. A uniform distribution
and the write mix have not been run under swap.
