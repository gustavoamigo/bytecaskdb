# ByteCaskDB MariaDB Storage Engine Plugin

> **Status: early development.** The plugin works and passes correctness validation, but the storage format and SQL feature coverage may change before a stable release. Not recommended for production use yet.

A MariaDB storage engine plugin that exposes ByteCaskDB as a SQL-accessible table engine. Supports full DML (INSERT, UPDATE, DELETE, SELECT), secondary indexes, transactions with savepoints, and bulk loading.

## Differences from InnoDB

Read this before pointing an application written for InnoDB at the engine.

- **Optimistic concurrency, not locking.** Nothing blocks. A transaction reads from a snapshot taken at its first read or write and buffers its writes in memory. At `COMMIT`, a write to a row that another transaction changed since that snapshot fails the whole transaction with error 1213 (`ER_LOCK_DEADLOCK`). The server does not retry it; the application must re-read and redo the transaction. Under contention on hot rows, expect 1213 as a normal outcome rather than waiting.
- **`SELECT ... FOR UPDATE` and `LOCK IN SHARE MODE` are plain snapshot reads.** They take no locks and register no conflict guard, so a read-then-write pattern that relies on `FOR UPDATE` to serialise is only protected for rows the transaction actually writes. Write skew between two transactions that read overlapping rows and write disjoint ones is possible.
- **Foreign keys are not enforced.** `FOREIGN KEY` clauses are accepted, stored, and listed in `information_schema`, so DDL round-trips through dump and restore, but no referential check runs on `INSERT`, `UPDATE` or `DELETE`, and there are no cascades.
- **Transactions are buffered in RAM until commit.** A transaction that modifies millions of rows holds all of them in memory. `ALTER TABLE ... ALGORITHM=COPY` and `CREATE INDEX` are exempt: they flush in batches.
- **Long-lived snapshots defer vacuum.** `mysqldump --single-transaction` and any long transaction pin the data files they can see; space from overwritten or deleted rows is reclaimed only after they finish.
- **Every table's key directory lives in memory.** The engine's key directory holds no key bytes, so a row costs about 14–19 bytes per index (primary and secondary) whatever the key's length: 14 measured on structured keys, which index encodings are, 19 on random ones, both at 1M keys. Budget 20 bytes per row per index of resident memory, in addition to MariaDB's own and `bytecaskdb_buffer_pool_size` if the pool is on. The row data itself stays on disk.
- **Not supported:** `FULLTEXT` and `SPATIAL` indexes, `LOCK TABLES` blocking semantics, `HANDLER`, `INSERT DELAYED`, table-level lock priorities, `CHECKSUM TABLE ... QUICK`.

## Configuration

All settings are global. Set them in `my.cnf` under `[mariadbd]` or, for the dynamic ones, with `SET GLOBAL`.

| Variable | Default | Range | Dynamic | Description |
|---|---|---|---|---|
| `bytecaskdb_io_backend` | `pread` | `pread`, `mmap`, `buffer_pool` | no | How sealed data files are read. The active file is written the same way in every mode. |
| `bytecaskdb_buffer_pool_size` | `0` | bytes | no | Pool size when `io_backend = buffer_pool`. Total footprint, not just frame bytes. Must be at least `2 x bytecaskdb_max_file_bytes`. |
| `bytecaskdb_buffer_pool_direct_io` | `ON` | — | no | Fill the pool with `O_DIRECT`, so it is the only consumer of memory for sealed-file data. Falls back to buffered fills per file where the filesystem refuses. |
| `bytecaskdb_max_file_bytes` | 64 MiB | 1 MiB – 4 GiB | no | Active data file rotation threshold. Sealed files are the unit of vacuum: smaller files reclaim space sooner at the cost of more files. |
| `bytecaskdb_sync` | `AT_EVERY_COMMIT` | `AT_EVERY_COMMIT`, `AT_INTERVAL`, `AT_FILE_ROTATION` | yes | When committed transactions reach disk. See [Durability](#durability). |
| `bytecaskdb_sync_interval_ms` | 1000 | 1 – 60,000 | yes | With `bytecaskdb_sync = AT_INTERVAL`, how often committed transactions are synced. |
| `bytecaskdb_verify_checksums` | `ON` | — | yes | CRC-verify every value read from disk. Turn off only for benchmarking; recovery still verifies hint files. |
| `bytecaskdb_vacuum_fragmentation_threshold` | 0.5 | 0.0 – 1.0 | yes | Fraction of dead bytes a sealed file must reach before background vacuum rewrites it. |
| `bytecaskdb_vacuum_busy_interval_ms` | 500 | 10 – 3,600,000 | yes | Pause between vacuum passes while files are being reclaimed. |
| `bytecaskdb_vacuum_idle_interval_ms` | 30000 | 100 – 86,400,000 | yes | Pause between vacuum passes when the last pass found nothing to reclaim. |
| `bytecaskdb_bulk_copy_flush_bytes` | 64 MiB | 4 KiB – 4 GiB | yes | Buffered bytes per batch during `ALTER TABLE ... ALGORITHM=COPY` and `CREATE INDEX`. |

Changes to the vacuum variables take effect on the next vacuum pass, at most one pause of the previous length later.

### Durability

A committed transaction is always written to the data file before `COMMIT` returns, so a mariadbd crash loses nothing in any mode. `bytecaskdb_sync` decides when it also reaches the disk, which is what an OS crash or power loss needs:

| `bytecaskdb_sync` | Synced to disk | Lost on an OS crash | InnoDB | PostgreSQL |
|---|---|---|---|---|
| `AT_EVERY_COMMIT` (default) | before `COMMIT` returns (`fdatasync`, shared by concurrent commits) | nothing | `innodb_flush_log_at_trx_commit = 1` | `synchronous_commit = on` |
| `AT_INTERVAL` | by a background thread every `bytecaskdb_sync_interval_ms` | at most one interval | `= 2` | `= off` |
| `AT_FILE_ROTATION` | when the active data file fills and rotates, and at shutdown | up to one data file (`bytecaskdb_max_file_bytes`), however old | — | — |

DDL is always synced, in every mode. Changing either variable at runtime takes effect at the next commit; a shorter interval also triggers a sync at once. Set the mode by name: MariaDB numbers enum values from 0, so `SET GLOBAL bytecaskdb_sync = 1` means `AT_INTERVAL`.

## Examples

Ready-to-run Docker Compose setups demonstrating ByteCaskDB as a drop-in storage engine:

- [`examples/wordpress/`](examples/wordpress/) — WordPress backed by ByteCaskDB
- [`examples/metabase/`](examples/metabase/) — Metabase analytics backed by ByteCaskDB

Each example includes a `compose.yml` and tuned `mariadb.cnf`.

## Backup and Restore

ByteCaskDB's append-only architecture makes backup simple: sealed data files are immutable, so backup reduces to sealing the active file and copying the sealed files. No redo log replay, no prepare step, no rollback.

The plugin hooks into MariaDB's `BACKUP STAGE` protocol. When `BACKUP STAGE START` fires, the plugin pauses vacuum, seals the active file, and writes a `backup_manifest.txt` listing exactly which files to copy. When `BACKUP STAGE END` fires, the manifest is removed and vacuum resumes. Reads and writes continue normally during the backup — only vacuum is paused.

### Method 1: Script-based backup

The simplest approach. No external tools required.

**Backup:**

```sql
BACKUP STAGE START;
-- Plugin seals the active file and writes backup_manifest.txt
```

```bash
cd /var/lib/mysql/bytecaskdb/
rsync --files-from=backup_manifest.txt . /backup/bytecaskdb/
cp backup_manifest.txt /backup/bytecaskdb/
```

```sql
BACKUP STAGE END;
-- Manifest removed, vacuum resumed
```

**Restore:**

1. Stop MariaDB.
2. Replace `datadir/bytecaskdb/` with the backup copy (which includes `backup_manifest.txt`).
3. Start MariaDB. The plugin detects the manifest, moves any unlisted `.data`/`.hint` files to `discarded/`, deletes the manifest, and opens normally.

### Method 2: mariabackup

mariabackup handles `.frm` files, InnoDB/Aria data, and binlog position. It does not copy ByteCaskDB data files (it only knows about InnoDB, Aria, MyISAM, and CSV file extensions). The operator copies `bytecaskdb/` separately using the manifest.

**Backup:**

```bash
# 1. mariabackup captures .frm files, InnoDB, Aria, and binlog position.
#    This runs BACKUP STAGE internally, which triggers the plugin to
#    seal the active file and write backup_manifest.txt.
mariabackup --backup --target-dir=/backup --user=root

# 2. Copy ByteCaskDB data files listed in the manifest.
cd /var/lib/mysql/bytecaskdb/
rsync --files-from=backup_manifest.txt . /backup/bytecaskdb/
cp backup_manifest.txt /backup/bytecaskdb/

# 3. Prepare the backup (InnoDB redo log apply).
mariabackup --prepare --target-dir=/backup
```

**Restore:**

```bash
# 1. Stop MariaDB.
systemctl stop mariadb

# 2. Replace the data directory with the backup.
mariabackup --copy-back --target-dir=/backup
# or: rm -rf /var/lib/mysql && cp -r /backup /var/lib/mysql

# 3. Start MariaDB. The plugin applies the manifest automatically:
#    unlisted files are moved to bytecaskdb/discarded/, the manifest
#    is deleted, and the database opens with only the sealed files.
systemctl start mariadb
```

See [`docs/backup_design.md`](docs/backup_design.md) for the full design, vacuum interaction, crash safety, and limitations.

## Tests

```bash
# Unit tests (C++ plugin internals)
./tests/run-unit-tests.sh

# Functional tests (SQL-level correctness via pytest)
./tests/run-functional-tests.sh

# MTR tests (MariaDB Test Runner integration)
./tests/run-mtr-tests.sh

# Sysbench OLTP benchmarks (ByteCaskDB vs InnoDB vs RocksDB)
./benchmarks/run-sysbench.sh [--engines=bytecaskdb,innodb,rocksdb] [--table-size=N] [--threads=LIST] [--time=S]
```

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/mariadb_engine_design.md`](docs/mariadb_engine_design.md) | Storage engine architecture: table mapping, index strategy, transaction model |
| [`docs/mariadb_plugin_guide.md`](docs/mariadb_plugin_guide.md) | Build, install, and configure the plugin |
| [`docs/mariadb_plugin_plan.md`](docs/mariadb_plugin_plan.md) | Project plan and task tracker |
| [`docs/correctness_validation.md`](docs/correctness_validation.md) | Proof test matrix for DML/txn/failure scenarios |
| [`docs/backup_design.md`](docs/backup_design.md) | Backup and restore design |

## Sysbench OLTP Benchmarks

ByteCaskDB vs InnoDB vs RocksDB (MyRocks) on standard sysbench OLTP workloads.

### Configuration

All engines run with `sync_binlog=0`, `skip-log-bin`, and `performance-schema=OFF`. Each engine runs in its own dedicated MariaDB instance.

| Engine | Parameters |
|--------|-----------|
| ByteCaskDB | Default (`sync=true` per write, all keys in memory, 64 MiB file rotation) |
| InnoDB | `buffer_pool_size=1G`, `log_file_size=256M`, `flush_log_at_trx_commit=1`, `flush_method=O_DIRECT`, `io_capacity=2000`, `io_capacity_max=4000` |
| RocksDB | `block_cache_size=1G`, `max_background_jobs=4` |

Sysbench: 1 table, `--report-interval=0`, `--time=10`.

### 50K Rows

| Workload | Threads | BC tps | avg (ms) | p95 (ms) | InnoDB tps | avg (ms) | p95 (ms) | RocksDB tps | avg (ms) | p95 (ms) |
|----------|--------:|-------:|---------:|---------:|-----------:|---------:|---------:|------------:|---------:|---------:|
| oltp_point_select | 1 | 455,750 | 0.02 | 0.03 | 390,433 | 0.03 | 0.03 | 352,535 | 0.03 | 0.04 |
| oltp_point_select | 16 | 2,875,907 | 0.06 | 0.09 | 2,644,972 | 0.06 | 0.10 | 2,349,744 | 0.07 | 0.09 |
| oltp_read_only | 1 | 14,946 | 0.67 | 0.80 | 15,456 | 0.65 | 0.75 | 11,205 | 0.89 | 1.04 |
| oltp_read_only | 16 | 125,677 | 1.27 | 1.79 | 131,588 | 1.21 | 1.70 | 103,481 | 1.55 | 1.89 |
| oltp_write_only | 1 | 4,166 | 2.40 | 2.61 | 3,941 | 2.54 | 2.71 | 4,131 | 2.42 | 2.61 |
| oltp_write_only | 16 | 21,611 | 7.41 | 14.21 | 11,501 | 13.92 | 14.46 | 9,696 | 16.51 | 40.37 |
| oltp_insert | 1 | 1,573 | 6.36 | 6.67 | 1,483 | 6.74 | 7.17 | 1,519 | 6.58 | 7.43 |
| oltp_insert | 16 | 10,990 | 14.57 | 15.55 | 11,444 | 13.99 | 15.55 | 11,920 | 13.43 | 15.27 |
| oltp_read_write | 1 | 1,339 | 7.47 | 8.58 | 1,269 | 7.88 | 8.90 | 1,157 | 8.64 | 9.73 |
| oltp_read_write | 16 | 9,362 | 17.11 | 30.81 | 10,606 | 15.10 | 17.01 | 8,786 | 18.22 | 41.10 |

### 1M Rows

| Workload | Threads | BC tps | avg (ms) | p95 (ms) | InnoDB tps | avg (ms) | p95 (ms) | RocksDB tps | avg (ms) | p95 (ms) |
|----------|--------:|-------:|---------:|---------:|-----------:|---------:|---------:|------------:|---------:|---------:|
| oltp_point_select | 1 | 406,881 | 0.02 | 0.03 | 380,116 | 0.03 | 0.03 | 293,231 | 0.03 | 0.04 |
| oltp_point_select | 16 | 3,225,410 | 0.05 | 0.06 | 2,814,282 | 0.06 | 0.08 | 2,013,372 | 0.08 | 0.13 |
| oltp_read_only | 1 | 14,797 | 0.68 | 0.77 | 15,582 | 0.64 | 0.75 | 10,209 | 0.98 | 1.16 |
| oltp_read_only | 16 | 130,041 | 1.23 | 1.58 | 131,720 | 1.21 | 1.67 | 91,158 | 1.75 | 2.30 |
| oltp_write_only | 1 | 4,006 | 2.50 | 2.66 | 3,973 | 2.52 | 2.76 | 3,991 | 2.51 | 2.66 |
| oltp_write_only | 16 | 23,690 | 6.76 | 14.21 | 11,705 | 13.68 | 16.71 | 11,438 | 14.00 | 14.21 |
| oltp_insert | 1 | 1,532 | 6.53 | 6.79 | 1,543 | 6.48 | 6.91 | 1,559 | 6.41 | 6.67 |
| oltp_insert | 16 | 11,354 | 14.10 | 13.95 | 12,381 | 12.93 | 13.70 | 11,988 | 13.35 | 13.95 |
| oltp_read_write | 1 | 1,271 | 7.87 | 8.28 | 1,332 | 7.51 | 8.28 | 1,169 | 8.56 | 9.56 |
| oltp_read_write | 16 | 9,671 | 16.57 | 17.63 | 10,782 | 14.85 | 16.71 | 10,345 | 15.48 | 17.63 |

### Observations

- **Point selects**: ByteCaskDB leads at both dataset sizes and scales well with threads (3.2M tps at 16 threads, 1M rows). The in-memory radix tree lookup avoids block cache misses that affect InnoDB and RocksDB at larger sizes.
- **Write-only (16 threads)**: ByteCaskDB's group commit delivers 2x the throughput of InnoDB and RocksDB at 16 threads. Single-writer serialization with batched `fdatasync` amortizes the dominant cost.
- **Read-only (range scans)**: InnoDB leads slightly due to contiguous sorted storage. ByteCaskDB fetches each value individually from disk (known trade-off).
- **Insert (single-row)**: All three engines are close — limited by `fdatasync` round-trip latency per row.
- **Read-write mixed**: InnoDB leads at 16 threads. ByteCaskDB's p95 at 16 threads (30.81 ms at 50K, 17.63 ms at 1M) reflects contention on the single write mutex under mixed load.

### Running

`run-sysbench.sh` loads each engine's table once up front, then runs every
workload/thread cell against a freshly started server, stopping it again
afterwards — so only the engine under test is running and no other engine's
background flushing or compaction competes for the disk. Each cell records the
block-layer bytes it read and wrote, taken from the instance's cgroup
`io.stat`, alongside tps and latency in `sysbench_results.csv`.

Because the table is loaded once per engine rather than once per cell,
workloads that mutate it (`oltp_insert`, `oltp_write_only`, `oltp_read_write`)
leave it changed for whatever runs next: results depend on the order workloads
are listed in.

```bash
# All engines (default)
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh

# ByteCaskDB only
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh --engines=bytecaskdb

# Custom parameters
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh --engines=bytecaskdb,innodb --table-size=1000000 --threads=1,4,16 --time=30

# Under a memory limit: each engine's mariadbd in a cgroup with memory.max
# (default 2.5 GiB, 10 M rows; needs passwordless sudo). Compares the three
# ByteCaskDB back-ends (buffer_pool, mmap, pread) with InnoDB.
./bytecaskdb-mariadb-plugin/benchmarks/memory-pressure/run-memory-pressure.sh
```

## HammerDB TPROC-C

`run-hammerdb.sh` runs HammerDB's TPROC-C workload (TPC-C derived, stored
procedures) against ByteCaskDB and InnoDB, using the same `bytecaskdb.cnf` and
`innodb.cnf` as the sysbench runs. It builds the schema once per engine, keeps
a copy of the data directory, and restores that copy before every
virtual-user count, so each cell starts from the same database. Results go to
`hammerdb_results.csv`: NOPM and TPM from HammerDB, plus the same I/O and
memory columns as the sysbench runs, sampled over the measured window only.

HammerDB is not packaged by most distributions. Unpack a release tarball from
<https://github.com/TPC-Council/HammerDB/releases> into `~/HammerDB-<version>`,
or pass `--hammerdb-home`.

TPROC-C contends on a few hot rows: every Payment updates its warehouse row,
and every New-Order increments a district's next order id. InnoDB makes those
transactions wait for each other. ByteCaskDB aborts all but one of them at
`COMMIT` with 1213, and HammerDB drops the aborted transaction and starts the
next one. NOPM counts only committed new orders. The `aborts` column counts the
dropped transactions.

```bash
# 20 warehouses, 16 virtual users, 2 min ramp-up, 5 min measured (defaults)
./bytecaskdb-mariadb-plugin/benchmarks/run-hammerdb.sh --data-root=/mnt/bench

# Several virtual-user counts. --reuse-data keeps the built schema at exit and
# reuses it on the next run instead of rebuilding it.
./bytecaskdb-mariadb-plugin/benchmarks/run-hammerdb.sh --warehouses=50 --vus=8,16,32 --data-root=/mnt/bench --reuse-data
```

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (469 MiB/s read, 450 MiB/s write), 31 GiB RAM. Each result is the mean of a single 10s sysbench run._
