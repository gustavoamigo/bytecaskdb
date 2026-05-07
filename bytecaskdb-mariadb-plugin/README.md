# ByteCaskDB MariaDB Storage Engine Plugin

> **Status: early development.** The plugin works and passes correctness validation, but the storage format and SQL feature coverage may change before a stable release. Not recommended for production use yet.

A MariaDB storage engine plugin that exposes ByteCaskDB as a SQL-accessible table engine. Supports full DML (INSERT, UPDATE, DELETE, SELECT), secondary indexes, transactions with savepoints, and bulk loading.

## Examples

Ready-to-run Docker Compose setups demonstrating ByteCaskDB as a drop-in storage engine:

- [`examples/wordpress/`](examples/wordpress/) — WordPress backed by ByteCaskDB
- [`examples/metabase/`](examples/metabase/) — Metabase analytics backed by ByteCaskDB

Each example includes a `compose.yml` and tuned `mariadb.cnf`.

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

```bash
# All engines (default)
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh

# ByteCaskDB only
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh --engines=bytecaskdb

# Custom parameters
./bytecaskdb-mariadb-plugin/benchmarks/run-sysbench.sh --engines=bytecaskdb,innodb --table-size=1000000 --threads=1,4,16 --time=30
```

---

_Tested on AMD Ryzen 7 3700X (8C/16T), Samsung SSD 860 EVO SATA (469 MiB/s read, 450 MiB/s write), 31 GiB RAM. Each result is the mean of a single 10s sysbench run._
