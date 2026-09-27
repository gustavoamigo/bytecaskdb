# Write path under relaxed sync: investigation

Status: investigation report, the foundation for a design. Measurements from
2026-09-27, on `main` at `08114ea2` plus #202 (`Snapshot::count_keys`) and
#197 (`--profile=fast`).

## Question

With commits that do not wait for `fdatasync` — the plugin's
`bytecaskdb_sync = AT_INTERVAL`, the engine's `sync = false` — ByteCaskDB
leads on one thread and falls behind with many:

| | 1 thread | 16 threads |
|---|---:|---:|
| sysbench `oltp_write_only`, vs InnoDB (`flush_log_at_trx_commit = 2`) | **+7%** | **−24%** |
| sysbench `oltp_insert`, vs InnoDB | **+6%** | **−12%** |
| `engine_bench` concurrent unsynced puts (8 threads), vs RocksDB | — | **0.44×** |

With synced commits the ranking reverses — group commit makes ByteCaskDB
2–7× RocksDB on `PutMT/Sync` — so the question is what the unsynced path
pays that the synced one hides behind the disk.

## Setup

- Ryzen 7 3700X (8 cores, 16 threads), 31 GiB, MariaDB 10.11.16,
  HammerDB/sysbench client on the same host.
- sysbench 1.0.20 `oltp_write_only`, 1M rows, 16 threads, `--profile=fast`
  (both engines 16 GiB cache, buffered I/O, commit survives a mariadbd
  crash), data on `/mnt/bench` (SATA SSD, btrfs), 30 s warm-up, 60 s
  measured.
- `engine_bench` `PutMT/PeriodicSync` with its periodic sync disabled
  (8 threads of `put(sync = false)`), 50,000 keys.
- Tools: engine counters (`SHOW ENGINE BYTECASKDB STATUS`), `perf` CPU
  profiles with DWARF stacks, scheduler-switch stacks, and uprobes on the
  entry and return of the commit path's functions, paired per thread into
  durations. Uprobes add a few µs per probe hit; ByteCaskDB's commit carries
  ~10 hits, InnoDB's two, so the ByteCaskDB durations below are inflated by
  up to ~20 µs. Throughput figures are from runs without probes.

## Findings

### 1. The filesystem, the disk and the syncs are not the cause

`engine_bench`, 8 threads, unsynced puts:

| Where | ByteCaskDB | RocksDB | CPU used, BC / RocksDB |
|---|---:|---:|---:|
| btrfs | 192K ops/s | 422K | 236% / 730% |
| ext4 | 223K | 441K | 226% / 587% |
| tmpfs | 229K | 580K | 295% / 672% |
| btrfs, sync every 50 ms | 123K | 219K | |

The gap is there on tmpfs, and is no smaller without the periodic sync.
ByteCaskDB's 8 threads never use more than ~3 cores: its writers are
serialised or waiting, not computing. A single thread does ~135K ops/s, so 8
threads add 1.4×.

### 2. An unsynced commit takes 15× InnoDB's

The `COMMIT` statement, timed at `trans_commit` for both engines, sysbench
`oltp_write_only`, 16 threads:

| | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| InnoDB | 30 µs | 23 µs | 51 µs | 106 µs |
| ByteCaskDB | 450 µs | 360 µs | 559 µs | 692 µs |

Inside ByteCaskDB's commit, per commit:

| Phase | Calls / commit | mean | p50 |
|---|---:|---:|---:|
| `WriteGroup::submit` — waiting for the batch in progress, then running its own if it leads | 1.0 | 279 µs | 257 µs |
| `execute_slots` — one batch, serial under `write_mu_` (~3.7 commits) | 0.27 | 125 µs | 106 µs |
| `commit_wait` — waiting for the batch to be published | 1.0 | 155 µs | 86 µs |

Per transaction, mariadbd's threads sleep 7.13 times against InnoDB's 5.88;
the rest of both is the client round trips of the transaction's statements.
Scheduler-switch stacks put ByteCaskDB's extra sleeps in the commit: 28.5% of
all context switches, 18.3% as a follower in `WriteGroup::submit` and 10.2%
in `commit_wait`.

### 3. `commit_wait` is mostly the settle wait, which unsynced commits do not need

`flush_once` yields while the write group is busy, up to `kFlushSettleMax`
(200 µs), so the flush covers writers still in stage 1. That was measured to
raise commits per `fdatasync` from 4.15 to 7.78 for synced writers. With 16
writers arriving continuously the group is always busy, so every flush waits
out the cap — and an unsynced flush has no `fdatasync` to share.

Setting the cap to 0, same run:

| | settle 200 µs | settle 0 |
|---|---:|---:|
| Throughput (no probes) | 21,814 tps | **25,919 tps (+19%)** |
| `COMMIT` mean / p50 | 450 / 360 µs | 357 / 282 µs |
| `commit_wait` mean / p50 | 155 / 86 µs | **46 / 3 µs** |
| `WriteGroup::submit` mean | 279 µs | 292 µs |
| `execute_slots` per batch | 125 µs (~3.7 commits) | 161 µs (~4.5 commits) |

### 4. The serial section is now the ceiling

With the settle gone, `WriteGroup::submit` is ~290 µs whether or not it
waits: a writer waits out the batch in progress and then runs its own. A
batch of ~4.5 commits takes ~160 µs — **~35 µs of serial work per commit**,
which caps commit attempts at about 28,000 per second whatever the
coordination costs. InnoDB's whole commit takes 30 µs.

What the serial work is (CPU samples inside `execute_slot`, the per-commit
part of the batch; the append is another 8% of the batch, building the new
tree version ~5%):

| | Share |
|---|---:|
| Applying writes to the key directory (`apply_writes`) | 64% |
| &nbsp;&nbsp;inserts, `kd_put` — a record read to place each key | 30% |
| &nbsp;&nbsp;erases, blind `erase` — a record read to confirm each key | 23% |
| Conflict validation (`validate_preconditions`) | 33% |
| &nbsp;&nbsp;the key's version in the transaction's **snapshot** | 22% |
| &nbsp;&nbsp;the key's version in the current head | 10% |

The snapshot lookup reads an immutable version and needs no lock: a third
of validation, about a fifth of the serial work, sits under `write_mu_` only
because that is where validation runs.

Building a tree version per batch — the cost group commit amortises — is
~5% of the batch here, and ~2% of `engine_bench`'s samples. Batches hold
3.8–4.5 commits in both sync modes: the pipeline appends the next batch
while the previous `fdatasync` runs, so syncing shares the `fdatasync`, not
the tree building. Unsynced commits lose nothing on that front.

### 5. Coordination costs more CPU than the work in `engine_bench`

Without MariaDB, the handoffs dominate. `engine_bench`, 8 threads, unsynced,
tmpfs, CPU samples (symbolised):

| | Share |
|---|---:|
| In the kernel at the sample (futex waits and wakes, scheduler) | 47% |
| The batch (`execute_slots`) | 22% |
| Followers waiting in the write group | ~14% |
| `commit_wait`, including the settle yields | 11% |
| Loading the engine state (`std::atomic_load` of a `shared_ptr`) | 6.9% |
| `notify_all` | 2.5% |

`std::atomic_load` on a `shared_ptr` takes one of libstdc++'s global pool of
mutexes, which every writer and the flusher contend for. Through MariaDB the
same coordination is ~3.5% of CPU but still costs the latency in finding 2.

### 6. The aborts are real conflicts, from sysbench's skew

`oltp_write_only` at 16 threads aborts 11% of transactions at the default
settle, and a third with the settle at 0; 0 with one client. With
`--rand-type=uniform` (sysbench 1.0 defaults to `special`: 75% of accesses to
1% of the rows) they fall from 13,429/s to 144/s, and throughput stays at
24,442 tps. InnoDB waits on row locks and never aborts. This is optimistic
concurrency under a skewed hot set, not a bug; a shorter commit narrows the
window in which a conflicting write can land, but the rate also grows with
throughput, as the settle-0 run shows.

## What does not explain the gap

- **The filesystem or disk** (finding 1).
- **`fdatasync`**: the unsynced gap is as large without any.
- **Building a tree version per commit**: ~2–5%.
- **RocksDB's design lesson**: RocksDB serialises its WAL append and lets
  writers insert into the memtable, a mutable concurrent skiplist, in
  parallel. ByteCaskDB's data-file append is the WAL append's counterpart
  and is not the bottleneck (8% of the batch). Its key directory is a
  persistent tree that yields one immutable version per batch; writers
  cannot insert into it in parallel. The lesson is only which part of the
  serial section is avoidable.

## Targets

In order of measured effect:

1. **The settle wait on flushes that owe no `fdatasync`** — +19% throughput,
   `commit_wait` p50 86 → 3 µs, when removed outright (finding 3). A settle
   only earns its keep when a sync is owed.
2. **Serial work per commit, ~35 µs** — the ceiling once 1 is fixed
   (finding 4). The snapshot-side validation (~a fifth) can leave the lock;
   the key-directory updates' record reads (~half) are inherent to blind
   leaves and stay.
3. **The second sleep per commit** — an unsynced batch whose head owes no
   sync can be published by its own leader, so its writers return from
   stage 1 without `commit_wait`; the first sleep, waiting for the leader,
   remains while the section is serial (findings 2, 5).
4. **The shared-state load** — `std::atomic_load` on a `shared_ptr`, a
   global mutex pool (finding 5).

Not targets: the aborts (finding 6), which are the workload's; the synced
path, where group commit already wins.

## Not investigated

- `engine_bench` `CasMT/NoSync` (read-modify-write, unsynced), ~6× RocksDB's
  CPU per operation: a separate profile.
- InnoDB's internal commit breakdown: only its `trans_commit` total was
  timed.
- Configurations other than 16 clients and this machine.
