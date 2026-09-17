# Memory-pressure findings — 17 September 2026

What happens to ByteCaskDB when its key directory does not fit in memory, and
whether the buffer pool earns its keep. Every number here comes from
`run-memory-pressure.sh` on the branch `single-wait-epoch-buffer-pool`, on
one host, in one day, and every raw row is in
[`data/2026-09-17-results.csv`](data/2026-09-17-results.csv). The scripts
that produced each section are in [`experiments/`](experiments/).

**Host:** Azure VM, 4 vCPU (AMD EPYC 7763), 31 GB RAM, ext4 on a virtual
disk with ~160 µs read latency, Linux 6.17, MariaDB 10.11.14, sysbench 1.0.
An 8 GiB swapfile at `/mnt/swapfile` (not inside the data root). Every
measured cell is a single run of 30 s after a 15 s warm-up, 8 sysbench
threads, unless stated. Treat differences under ~5 % as noise.

**Two trees.** `single-wait-epoch-buffer-pool` carries the epoch-reclamation
radix tree (see `docs/radix_tree_epoch_reclamation_design.md`); "refcount"
is the buffer pool PR branch with the original reference-counted tree plus
these benchmark commits cherry-picked, so both sides ran the same script.

## Summary

1. **Keys should fit in memory. Swap is a guardrail, not a strategy.**
   Without swap, exceeding `memory.max` is an OOM kill that crash-loops
   through recovery. With swap every run survived, at a cost that ranges
   from negligible to severe depending on access locality.
2. **The refcounted tree collapses under swap on random-order writes
   (92 tps, 76 major faults per write); the epoch tree degrades gracefully
   (3803 tps, 1.3 faults).** Holding key count and limit fixed, insertion
   order alone is worth 56× on the refcounted tree and 1.4× on the epoch one.
3. **Recovery is the sharp edge:** 619 s instead of 5 s when the key
   directory is swapped and keys are random. Fewer recovery threads help
   (1.8× at one thread); key order helps 70×.
4. **The buffer pool is the only ByteCaskDB back-end that holds its
   throughput under a limit** (96–99 % kept, against 67–90 % for `mmap` and
   `pread`), but it is slower than both when memory is not scarce.
5. **A right-sized InnoDB beats the pool on two of three workloads at 20 M
   keys under 1 GiB**, because InnoDB can evict index pages and ByteCaskDB
   cannot: the key directory alone exceeds the limit. An oversized InnoDB
   pool loses a third of its throughput to swap.

---

## 1. Swap as a guardrail

10 M rows with sysbench's secondary index = 20 M keys, ~1.9 GB of key
directory, 3.2 GB of data files. `memory.max = 1 GiB`, 256 MiB pool.

| swap | outcome |
|---|---|
| none (`memory.swap.max = 0`) | `mariadbd` OOM-killed during recovery (`anon-rss:1048516kB`), twice; restarts and dies at the same point |
| 2 GiB | survives; 1.4 GB swapped; point selects 42 K/s (93 % of unlimited); startup 644 s |

`oom_kills` was zero in every swap-enabled run of the day. The cost is in
§2–§4; the sizing rule is that swap must cover the overshoot with headroom,
not the whole key directory.

## 2. Refcount tree versus epoch tree under swap

*Script: `experiments/tree-comparison.sh`. Data: experiments A, C, D.*

The hypothesis: cloning a node in the refcounted tree bumps the reference
count of every child, so a Node256 clone touches 256 other nodes wherever
they were allocated. Under swap those are page faults, and dirtied pages
that must be written back. Epoch reclamation touches no children on a clone.
That is a **write-path** cost, so `oltp_write_only` is the test and
`oltp_point_select` the control.

**A. Random-order keys** (10 M rows + secondary index = 20 M keys, 1 GiB
limit, 2 GiB swap, 4 recovery threads):

| tree | arm | write-only tps | p95 | faults / write | recovery |
|---|---|---:|---:|---:|---:|
| epoch | no limit | 5704 | 2.30 ms | 0 | 8 s |
| refcount | no limit | 5013 | 2.81 ms | 0 | 10 s |
| epoch | 1 GiB + swap | 3803 | 3.49 ms | 1.32 | 619 s |
| refcount | 1 GiB + swap | **92** | **116.8 ms** | **76.5** | 626 s |

Without pressure the epoch branch is +14 % on writes (it also carries the
single-wait commit change). Under swap it is **41×**. Point selects are
identical across trees (43.0 K vs 43.2 K, 0.01 faults/tx): reads never
clone. Recovery is identical: it builds through the transient, which
mutates in place, so it never pays the per-child cost either.

**D. The same 20 M keys in sequential order** (20 M rows, no secondary
index, same limit and pool):

| tree | arm | write-only tps | faults / write | recovery |
|---|---|---:|---:|---:|
| epoch | no limit | 5922 | 0 | 5.0 s |
| refcount | no limit | 5460 | 0 | 6.0 s |
| epoch | 1 GiB + swap | 5433 | 0.01 | 7.9 s |
| refcount | 1 GiB + swap | **5186** | 0.02 | **9.1 s** |

Holding key count and limit constant, insertion order alone is worth
**56× for the refcounted tree** and 1.4× for the epoch one. With sequential
keys, consecutive inserts walk the same region of the tree, so the path
nodes and the children a clone touches are still resident; swap evicts only
cold pages. With random keys every insert lands somewhere else.
`oltp_read_write` barely notices pressure on either tree (≈2 %): it is
`fdatasync`-bound at ~1200 tps, so faults hide behind the commit wait.

**C. 10 M rows without the index** (10 M keys): at the same 1 GiB nothing
swaps at all — the engine needs 905 MB instead of 2326 MB. The secondary
index is the whole source of the pressure on this dataset: it doubles the
key count and each secondary key is ~95 B against ~50 B for a primary key,
since it carries the indexed value plus the primary key as a suffix. At a
matched 448 MiB limit both trees lose only ~5 % with 556 MB swapped.

## 3. Recovery under swap

*Script: `experiments/recovery-threads.sh`. Data: experiment B.*

Same 20 M random keys, 1 GiB + 2 GiB swap, epoch tree, engine's own
`recovery_duration_us`:

| recovery threads | recovery | swap used |
|---:|---:|---:|
| 4 (the plugin's previous hardcoded value) | 612.8 s | 1348 MB |
| 2 | 536.0 s | 1145 MB |
| 1 | **342.0 s** | 1032 MB |

Each thread builds a partial key directory and the results are merged, so
four threads hold four partial trees plus the merge at the moment memory is
shortest. With memory to spare the opposite holds (README: 10 M keys in
3.00 s at 1 thread, 0.51 s at 16). `bytecaskdb_recovery_threads` is now a
sysvar. Even at one thread this is ~70× the unpressured cost; §2-D shows
the same 20 M keys recovering in 8–9 s when they arrive in order. Hint
entries are replayed in write order, so **sorting them by key before replay
is the change these numbers point at** (untested).

## 4. Buffer pool versus the page cache

*Script: `experiments/backend-comparison.sh`. Data: experiment E.*

Epoch tree, 20 M sequential keys, 4.9 GB data files, key directory ~1040 MB.
Under 1 GiB + 2 GiB swap the three back-ends are three answers to "who gets
the memory left after the key directory": the pool takes 256 MiB explicitly
and fills it `O_DIRECT`; `mmap` and `pread` let the kernel weigh 4.9 GB of
file pages against an anonymous key directory it knows nothing about.

| workload | back-end | no limit | 1 GiB | kept | faults / tx |
|---|---|---:|---:|---:|---:|
| point select | pool | 42620 | 42142 | **99 %** | 0.00 |
| | mmap | 46635 | 35545 | 76 % | 0.31 |
| | pread | 46954 | 31535 | 67 % | 0.00 |
| read write | pool | 1239 | 1220 | **99 %** | 0.04 |
| | mmap | 1450 | 1173 | 81 % | 7.50 |
| | pread | 1029 | 930 | 90 % | 0.06 |
| write only | pool | 6036 | 5814 | **96 %** | 0.01 |
| | mmap | 6637 | 5399 | 81 % | 1.49 |
| | pread | 6396 | 5784 | 90 % | 0.06 |

Unpressured the pool is last or near-last, as its design says it must be:
an `mmap` hit is an address and a memcpy, the pool hashes and probes an
index first. Under the limit it wins all three, and the win is stability
rather than speed — it keeps 96–99 % of its own throughput while the
kernel-managed back-ends keep 67–90 %. `mmap` suffers most because its
mapped file pages compete directly with the key directory for reclaim;
`pread` faults little but its reads become device reads once the page
cache is squeezed out.

## 5. InnoDB on the same workload

*Script: `experiments/innodb-sizing.sh`. Data: experiment F.*

A first attempt gave InnoDB a 1.5 GiB buffer pool inside a 1 GiB limit,
which the benchmark's `--pool-bytes` did not touch; that was a
misconfiguration, not a result, and it is why `--innodb-pool-bytes` exists.
Sweeping the size inside the limit (`O_DIRECT`, so the pool is InnoDB's
only data cache):

| InnoDB pool | point select | read write | write only | RSS | swapped |
|---:|---:|---:|---:|---:|---:|
| 384 MiB | 36765 | 1327 | 5620 | 480 MB | 0 |
| 512 MiB | 39063 | 1370 | 5805 | 610 MB | 0 |
| 640 MiB | 39117 | 1311 | 5944 | 738 MB | 0 |
| 768 MiB | 40125 | 1423 | 6126 | 870 MB | 0 |
| **896 MiB** | **41244** | **1462** | **6238** | 998 MB | 0 |
| 1536 MiB (over the limit) | 30834 | 1151 | 5205 | 992 MB | 969 MB |
| 6 GiB, no limit (ceiling) | 45932 | 1598 | 6264 | 3204 MB | 0 |

InnoDB improves monotonically to 896 MiB and never swaps, up to 998 MB
inside a 1024 MB limit; oversizing costs 25–33 %. At its best size against
ByteCaskDB with the pool, same limit:

| workload | ByteCaskDB + pool | InnoDB @ 896 MiB | |
|---|---:|---:|---|
| point select | 42142 | 41244 | pool +2 % |
| read write | 1220 | 1462 | **InnoDB +20 %** |
| write only | 5814 | 6238 | **InnoDB +7 %** |

This is a fair test and it favours InnoDB, for a structural reason:
ByteCaskDB must keep ~1040 MB of keys resident, which exceeds the whole
budget, so ~850 MB goes to swap however the pool is sized; InnoDB's index
pages are evictable and it fits entirely. Two things compress every engine
difference here: the MariaDB layer costs ~95 % of each query (the engine
does >1 M point reads/s standalone against ~42 K/s through SQL), and
sysbench rows are small (~250 B per row against ~52 B per key), which is the
least favourable shape for a design that keeps all keys in memory. The
regime the design targets — values large relative to keys, so the data
outgrows RAM while the key directory does not — was not tested.

## What changed because of this

- `bytecaskdb_recovery_threads` sysvar (was hardcoded at 4).
- `run-memory-pressure.sh`: `--swap-limit`, `--recovery-threads`,
  `--no-secondary-index`, `--innodb-pool-bytes`, `--start-timeout`; columns
  `major_faults`, `recovery_ms`, `recovery_threads`, `secondary_index`,
  `cgroup_swap_bytes`; refuses a swap limit on a host without swap;
  explains an OOM-killed start.
- The epoch tree merged with the buffer pool on `single-wait-epoch-buffer-pool`
  (1843 tests pass).

## Not done

- Sorting hint entries by key before recovery replay (§3).
- `mlock` on the pool arena so swap can only ever take the key directory.
- A large-value dataset, where the all-keys-resident design should lead.
- Repeated runs; every cell above is n = 1.
