# Code map

Where things live, and which test covers them. Coarse on purpose: modules and
directories, not functions. `scripts/check_code_map.py` fails CI when a path
named here no longer exists. For the layout by license, see
`docs/project_organization.md`; for behavior, `docs/bytecask_design.md`.

## Engine (`bytecaskdb/`, C++23 modules)

`bytecask.cppm` is the primary interface and declares everything; the `db_*.cpp`
files are `DB` member definitions split by concern. Public header for
out-of-tree users: `include/bytecask.hpp`, implemented by `bytecask_hpp.cpp`.

| Concern | Where |
|---|---|
| Open, close, lifecycle, `put`/`del`/`apply_batch` entry, mode, `stats()` | `db.cpp` |
| Commit path: writer executors, pipeline, durability waits | `db_commit.cpp` |
| Group commit batching, background worker, solo writer | `concurrency.cppm` |
| Mutable state the write path builds and freezes | `engine_state.cpp` |
| Checking a plan by its read sequence: guards, the W-W check | `engine_state.cpp` (`validate_preconditions`), `delete_log.cppm` (keys recent writes removed); tests `[validate_preconditions]`, `[read_sequence]`, `tests/range_conflict_test.cpp` |
| Publishing the immutable state, the read cache | `db_state.cpp` |
| Point reads, snapshots, iterators | `db_read.cpp`, `batch_iterator.cppm` |
| Recovery at `open` (hint replay, hint rebuild, torn tail) | `db_recovery.cpp` |
| File rotation, hint generation, hint backlog cap | `db_rotation.cpp` |
| Degraded state, `resume()` | `db_degraded.cpp` |
| Vacuum | `db_vacuum.cpp` |
| Replication: modes, manifests, `changes_since`, `ingest` | `db_replication.cpp`, `change_iterator.cppm`, `change_index.cppm` |
| Data file I/O, `fdatasync`, directory sync | `data_file.cppm` |
| Entry layout and CRC | `data_entry.cppm`, `serialization.cppm`; format in `docs/file_format.md` |
| Hint files | `hint_entry.cppm`, `hint_file.cppm` |
| Key directory (default): blind-leaf B+ tree | `blind_btree.cppm` |
| Key directory (`BYTECASK_KEYDIR=btree`): keyed B+ tree | `btree.cppm` |
| Tree support: version chains, node maps | `version_chain.cppm`, `patch_map.cppm`, `u32_map.cppm` |
| Buffer pool (`IoBackend::BufferPool`) | `buffer_pool.cppm` |
| Counters behind `stats()` | `counters.cppm` |
| Fault-injection points | `fault_injector.h` |
| Types shared by every unit | `types.cppm`, `internals.cppm`, `util.cppm` |

## Tests (`tests/`)

Run one test, not the suite: `xmake run bytecask_tests "<test name>"` or a tag,
`xmake run bytecask_tests "[tag]"`. In a git worktree add `-P .` to every xmake
command, or it builds the main checkout.

| Behavior | File (Catch2 tag) |
|---|---|
| Engine behavior, recovery, vacuum, replication, model-based recovery | `tests/bytecask_test.cpp` (`[bytecask]`, `[model]`) |
| Group commit, commit delay | `tests/concurrency_test.cpp` (`[concurrency]`) |
| Data file and hint file formats | `tests/data_file_test.cpp`, `tests/hint_file_test.cpp`, `tests/data_entry_test.cpp` |
| Key directory trees | `tests/blind_btree_test.cpp` (`[blind]`), `tests/btree_test.cpp` (`[btree]`) |
| Buffer pool | `tests/buffer_pool_test.cpp` (`[buffer_pool]`) |
| Replication markers and index | `tests/change_marker_test.cpp`, `tests/change_index_test.cpp` (`[replication]`) |
| Allocation failures, fault sweeps | `tests/alloc_sweep_test.cpp` (`[alloc_sweep]`), `tests/fault_sweep_test.cpp` (`[fault_sweep]`) |
| Invariants after `resume()` and recovery | `tests/invariants_test.cpp` (`[invariants]`) |
| Range guards | `tests/range_conflict_test.cpp` |
| Soak | `tests/soak_test.cpp` |
| Crash consistency | `tests/crash/crash_consistency.cpp`, model in `tests/crash/crash_model.h` |
| Chaos rig (fault-injecting filesystem) | `tests/chaos/run_chaos.py` |
| Isolation checking (Elle) | `tests/elle/isolation_history.cpp`, `scripts/run_isolation_check.py` |
| Fuzzers | `tests/fuzz/` |
| Generated proof tests: edit the generator, not the output | `tests/proof/generate_tests.py` produces `tests/proof/generated/` |
| Deliberate breaks proving the rigs catch things | `tests/soak_mutations/`, `tests/chaos_mutations/`, `tests/durability_mutations/` |

A guarantee's test is named in its row of `CONTRACT.md`. Rules for each kind of
test: `tests/CLAUDE.md`.

## Around the engine

| What | Where |
|---|---|
| Python binding | `bytecaskdb-python/src/bytecaskdb_module.cpp`, tests in `bytecaskdb-python/tests/`, model in `bytecaskdb-python/reference/bytecask_ref.py` |
| Node.js package (WASM and native) | `bytecaskdb-node/src/` |
| MariaDB plugin (GPL-2.0) | `bytecaskdb-mariadb-plugin/` (`bytecaskdb-mariadb-plugin/ha_bytecaskdb.cc`) |
| SQL layer (own `CONTRACT.md` and `docs/`) | `bytecaskdb-sql-layer/`; modules listed in `docs/project_organization.md` |
| Benchmarks | `benchmarks/engine_bench.cpp`, `scripts/run_engine_bench.py` |

## Docs worth finding by topic

| Topic | Doc |
|---|---|
| Whole design | `docs/bytecask_design.md` |
| Write-path failure classes and proof matrix | `docs/correctness_validation.md` |
| Commit pipeline, group commit | `docs/commit_pipeline_design.md` |
| Replication | `docs/replication_primitives_design.md` |
| Key directory | `docs/blind_leaf_btree_design.md`, `docs/persistent_btree_design.md` |
| Buffer pool | `docs/buffer_pool_design.md` |
| Chaos and isolation checking | `docs/chaos_testing_design.md`, `docs/isolation_checking_design.md` |
