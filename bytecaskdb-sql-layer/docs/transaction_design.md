# Transactions in the SQL layer

Status: **design, for review (#491).** Nothing here is built. It decides how
the layer runs several statements as one transaction, which the MVP needs:
TPROC-C's New-Order is about thirty statements that read their own writes
(#487, *MVP*). Each decision is marked **MVP** or **later**. Open questions
are listed at the end, each with a recommendation.

The layer gets a transaction engine of its own, built on the engine's
primitives (`Snapshot`, `WritePlan`, `apply_batch`; the engine's
`docs/transaction_design.md`). The MariaDB plugin's `MariaDBTxn` is a
reference, not a dependency. This design keeps its proven parts: the
snapshot taken at first touch, an ordered effect log beside a sorted lookup
map, a two-pointer merge of buffer and snapshot in both directions, and
statement rollback as a log mark. It also keeps the shape of its tests.
It does not take the parts that serve MariaDB (deferred-insert mode,
bulk-copy mode, row counters, `HA_ERR_*` mapping), and it fixes the plugin's
known faults: a read error taken as "absent" (#524), and the statement and
reverse-scan gaps #491 listed.

## What exists today

`Database::execute` runs each DML statement as one *execution*:
`bc_execute` takes a snapshot, runs the plan, and commits its effects as one
`WritePlan`. On `BC_CONFLICT` it retries up to three times
(`docs/plan_api_design.md`, *Model*; `CONTRACT.md`, *Transaction
isolation*). Within an execution:

- reads see the snapshot, never the plan's own writes;
- a second write of one row is refused (`BC_E_HANDLE`), and a second insert
  of one key is `BC_C_UNIQUE`;
- `bc_update` and `bc_delete` derive the index entries they remove from the
  stored row they are given;
- the commit guards the catalog record of every table it writes, so a DDL
  committed since the snapshot makes it a conflict.

A transaction stretches the execution's snapshot and write plan over many
statements, adds a view that merges the snapshot with the earlier
statements' writes, and moves the second-write rule from the execution to
the statement.

## Shape — MVP

An in-process transaction handle on the embedded API:

```cpp
class Transaction {                    // from Database::begin(); move-only
public:
  auto execute(const Statement&, std::span<const Value> params) -> Rows;
  void commit();                       // throws SerializationFailure (40001)
  void rollback();
  ~Transaction();                      // rolls back if still open
};
auto Database::begin(TransactionOptions = {}) -> Transaction;
```

- **One handle, one thread at a time.** Each handle owns its own `bc_ctx`,
  so transactions run concurrently on different threads. This is what the
  harness needs at 16 and 48 threads. Prepared statements are shared
  read-only across handles; compiling one is serialised. This is the part
  of #494's per-thread contexts the MVP needs.
- **`Database::execute` stays autocommit,** a one-statement transaction
  retried as today.
- **The snapshot is taken at the first statement,** not at `begin()`, as
  `MariaDBTxn` did. An idle handle then pins nothing. The catalog version is
  pinned with it (see *DDL*).
- **`commit()` returns or throws.** A transaction with no writes and no
  guards commits nothing and returns.
- **After `commit()`, `rollback()` or a serialization failure,** the handle
  is closed; any further call throws.

**Later:**
- the server's token form, where `BEGIN` returns a token naming a
  snapshot and a pending log held with a TTL, and statements carry it
  across requests on any thread (#494);
- procedures that run a whole transaction inside the runtime and retry it
  there (#495).

Both reuse this engine. The handle is the token's in-process half.

## The transaction's view — MVP

Every read a plan makes inside a transaction goes through one view: the
snapshot overlaid with the writes of the transaction's committed statements.
The calls and their signatures don't change; the context reads through the
view instead of the bare snapshot.

**The overlay holds:**
- point entries, a key mapped to a value or a tombstone, in a sorted map;
- range tombstones, from `bc_del_range`.

A point entry is always newer than any range tombstone over it: a range
delete erases the point entries inside it as it is recorded. So a key's
answer is:
1. its point entry, if there is one;
2. otherwise absent, if a range tombstone covers it;
3. otherwise the snapshot's.

| Call | Through the view |
|---|---|
| `bc_get`, `bc_exists` | The point entry, else the range tombstone, else the snapshot. |
| `bc_scan_open` / `bc_scan_next`, forward and reverse, tables and indexes | A two-pointer merge of the snapshot cursor and the overlay, in the scan's direction. On a tie the overlay wins and a tombstone hides the snapshot's key. Snapshot keys inside a range tombstone are skipped. `MariaDBTxn::MergeIterator`'s algorithm, with ranges added. |
| `bc_scan_row` on an index | The row is fetched through the view, so an index entry an earlier statement wrote finds the row it wrote. |
| `bc_count_range` | The snapshot's `count_keys`, then corrected: one for each overlay point entry in range whose presence differs from the snapshot's, and minus `count_keys` over each range tombstone's part of the range. Still bounded, at two record reads plus one existence check per overlay entry in range, all charged. |
| Unique checks in `bc_insert` and `bc_update` | Against the view: a key or prefix an earlier statement freed is free, and one it took is taken. The range guard over the prefix stays, for concurrent transactions. |

- **Budget.** A read the overlay answers is charged as if it had read a
  record, so a plan's budget doesn't depend on what the transaction has
  buffered.
- **Lifetime.** The overlay lives in the transaction, outside the context's
  arena, which is still reset after every statement.
- **Errors propagate. No read error is ever taken as "absent".** An I/O,
  CRC or memory error in any read through the view, overlay or snapshot,
  sets the context's sticky error (`BC_E_IO`, `BC_E_CORRUPT`,
  `BC_E_MEMORY`), and the statement fails. The plugin's version of this bug
  is #524. Here it is a requirement with a test: inject a read failure under
  each call in the table above.

## Statements inside a transaction — MVP

**A statement sees the snapshot plus the writes of earlier statements,
never its own.** A statement's effects go to its own pending list, as an
execution's do today. The view it reads is the overlay as it stood when the
statement began. When the statement succeeds, its effects are appended to
the transaction's log and applied to the overlay.

Take `UPDATE … SET qty = qty - 1` on a row an earlier statement wrote:
- the plan reads the row through the view and gets the earlier statement's
  version;
- `bc_update` takes that as the old row, and the index entries it removes
  are derived from it, which are the entries the earlier statement wrote;
- a second read of the row in the same statement still sees the version
  from before the statement;
- a second write of it is refused.

The next statement sees the new row. This is PostgreSQL's statement
semantics, and it's why `bc_update` and `bc_delete` stay correct with no
change to their contract: a "stored row" now means "the row as the
transaction sees it".

**The second-write rule applies per statement.** `written_rows` is cleared
at each statement's start, not each transaction's. Within a statement it
works as today: `BC_E_HANDLE`, or `BC_C_UNIQUE` for a second insert of one
key. A later statement may write a row an earlier one wrote, and the
cross-statement refusal that `docs/plan_api_design.md` describes is removed.

**Statement atomicity: a savepoint per statement.** A statement that fails
— a constraint, the budget, `BC_E_LIMIT`, a read error — discards its
pending effects; the overlay and the log are untouched. The transaction
continues, and the caller decides whether to go on or roll back.
PostgreSQL's default is different: a failed statement aborts the whole
transaction (25P02 until `ROLLBACK`). See open question 2.

User `SAVEPOINT` / `ROLLBACK TO` are **later**: TPROC-C doesn't use them.
When they come they are a mark in the log plus a copy of the overlay's
state at the mark, as `MariaDBTxn` keeps them; nothing here blocks that.

## Commit and conflicts — MVP

**Commit replays the log into one `WritePlan` built on the transaction's
snapshot.** The log holds every put, delete, range delete and guard, in the
order the statements made them. The engine applies a plan's operations in
order (engine `CONTRACT.md`, *Ordered*), so a range delete followed by a put
inside it keeps the put. The commit also adds the catalog-record guard of
every table the transaction wrote, as `bc_ctx_commit` does today.

The engine checks every written key against the snapshot, so the whole
transaction is first-committer-wins. A row read and then written, in any
statements, needs no guard.

| Outcome | Autocommit (`Database::execute`) | Transaction (`commit()`) |
|---|---|---|
| Committed | Rows returned | Returns |
| Conflict | Retried on a fresh snapshot, up to three times, as today; then `ExecError` with `conflict()` | `SerializationFailure`, SQLSTATE **40001**. Nothing written; the handle is closed. The caller reruns the transaction. The layer never retries it: it can't rerun the caller's logic. |
| Constraint or other error | `ExecError`, never retried | Raised by `execute()` for the failing statement; `commit()` itself can only conflict or fail with an engine error |

Conflicts are detected only at commit. A statement never waits and never
fails because another transaction holds a row, which differs from
PostgreSQL's REPEATABLE READ, where the second updater waits and then gets
40001.

The commit outcome's shape is the one #522 drafted: a status of
committed, conflict or engine error, with the caught exception kept apart
from the status. One point from that work carries over: the outcome's code
is fixed before any message is built. Building a message can fail to
allocate, and that drops the message; it never turns a conflict or a
constraint into an engine error. The plugin gets this order wrong in the
same way: `MariaDBTxn::commit` renders a duplicate inside its try block,
before its cleanup.

## Isolation — MVP

- **Snapshot isolation by default.** One snapshot per transaction. Writes
  are checked by the engine, and no guard is emitted for reads.
- **`SELECT … FOR UPDATE`** emits `bc_guard_unchanged` on every row key it
  returns, entered in the log like any effect. The commit fails if one of
  those rows changed since the snapshot. This is how TPROC-C's
  read-then-decide steps avoid write skew where they need to. A row the
  transaction also writes is already covered, and the extra guard is
  harmless.

**Later:**
- **serializable:** `bc_guard_unchanged` on every key read and not
  written, and `bc_guard_range` on every range scanned. These are the
  guards the engine's Elle check proves strict-serializable
  (the engine's `docs/isolation_checking_design.md`);
- **read committed:** a snapshot per statement.

Both are generated by the compiler from the isolation level. A plan's
guards are the only text that differs between levels.

## DDL during an open transaction — MVP

**Recommendation: the transaction keeps its catalog pin and conflicts at
commit. DDL is not refused.**

- The catalog version is pinned with the snapshot at the transaction's
  first statement. Every statement of the transaction is verified against
  that version, and its index maintenance follows that version's indexes.
- A DDL that commits meanwhile changes the catalog records of the tables it
  touches. A transaction that wrote one of those tables conflicts at commit
  through the existing catalog-record guard, and gets 40001. A transaction
  that only read them was consistent with its pin and commits.
- A statement whose plan doesn't verify against the pinned version (cached
  against a newer catalog, naming a column the pin lacks) fails with 40001
  and closes the transaction, rather than recompiling against a catalog
  older than the cache. This is rare and loses nothing a retry wouldn't
  recover.
- A DDL statement given to `Transaction::execute` is refused (SQLSTATE
  0A000) in the MVP. DDL stays its own atomic operation. Transactional DDL
  is later.

Refusing DDL while any transaction is open is the alternative. It needs
global tracking of open transactions, and it lets one idle handle hold off
every DDL indefinitely. Conflicting at commit uses the guard that already
exists.

## Limits — MVP

- **Buffer size.** A transaction's log and overlay are bounded by
  `TransactionOptions::max_bytes`, default 64 MiB, well below the engine's
  1 GiB plan limit. A statement that would take the transaction past it
  fails with `BC_E_LIMIT` (SQLSTATE 54000). Its effects are discarded by
  the statement savepoint, and the transaction continues. The bound counts
  key and value bytes plus a fixed cost per entry, so a transaction of
  small writes can't exhaust memory through overhead.
- **Memory.** The overlay allocates from the heap. A failed allocation is
  `BC_E_MEMORY` for the statement, never an abort and never an exception
  into a plan (`CONTRACT.md`, *Plan safety*). An allocation-failure sweep
  over a transaction's statements proves it, as `plan_alloc_faults_test`
  does for executions.
- **Budget.** Each statement keeps its own `bc_budget`, as each execution
  does. A transaction-wide budget, or a request deadline turned into one,
  is later (#491's *Budgets and deadlines*). Nothing here blocks it: the
  budget is passed per statement.
- **Duration.** An open transaction holds a snapshot, which keeps the data
  files it references from vacuum. An idle handle therefore defers vacuum.
  The in-process handle is the caller's to close. The token form's TTL
  (later) bounds this for remote clients.

## Changes to the plan API and the runtime

- **Plan-visible calls: none.** `bc_get`, `bc_exists`, the cursors,
  `bc_count_range` and the writes keep their signatures and contracts. The
  *Model* section's "a pure function of its parameters and one snapshot"
  becomes "… and the transaction's view".
- **Runtime calls, called by the embedded API and never by a plan:**
  - a transaction object holding the snapshot, the pinned catalog, the log
    and the overlay;
  - `bc_ctx_begin` given a transaction instead of taking its own snapshot;
  - `bc_ctx_commit` split into appending a statement's effects to the
    transaction and committing the transaction.

  Their exact C names are settled in the implementation PR, with
  `bc_plan.h`'s runtime section. `bc_execute` keeps its signature for
  autocommit.
- **`written_rows` and `deleted_ranges`** move from per execution to per
  statement.
- **Keys and values in the transaction's own C++ API** are
  `std::span<const std::uint8_t>`, or `BytesView`, as #522 drafted. They
  are never a pointer and a length.

## Tests

Each requirement gets a named Catch2 test in `sql_layer_tests`. The plugin's
`mariadb_txn_test.cpp` and #522's `transaction_test.cpp` are the reference
for their shape.
- **The view:**
  - read-your-own-writes for `bc_get`, `bc_exists` and `bc_scan_row`;
  - forward and reverse scans, over tables and indexes, with buffered
    puts, deletes and range deletes merged in, and keys of a neighbouring
    table or index never surfacing;
  - `bc_count_range` with buffered puts, deletes and ranges;
  - unique checks that see a key freed or taken by an earlier statement.
- **Statements:**
  - a statement doesn't see its own writes;
  - a later statement updates and deletes a row an earlier one wrote, and
    every index stays exact;
  - a failed statement rolls back alone.
- **Commit:**
  - first-committer-wins across transactions (lost-update, delete against
    update);
  - a conflict is 40001 with nothing written;
  - `FOR UPDATE` conflicts on a changed row;
  - a DDL racing a writing transaction conflicts;
  - rollback and the destructor write nothing.
- **Faults:**
  - read failure injected under each call through the view;
  - an allocation-failure sweep over a multi-statement transaction;
  - `BC_E_LIMIT` at the buffer bound.
- **Against PostgreSQL:** the proof machine (`docs/testing_design.md`) gains
  `BEGIN` / statements / `COMMIT` / `ROLLBACK` rules, compared with
  PostgreSQL's REPEATABLE READ on one connection, where the two agree.
- **Concurrency:** Elle through SQL, at the MVP gate (#487, 2.6): the
  harness's transactions as histories, checked as snapshot isolation, and
  as serializable where `FOR UPDATE` guards the reads that decide a write.

## Proposed `CONTRACT.md` changes

Not yet in `CONTRACT.md`. They go in with the implementation, named in its
PR.

**Added (Transactions):**
| Guarantee | Proved by |
|---|---|
| A transaction from `Database::begin()` commits all its statements' writes atomically, or none: `commit()` returns, or throws having written nothing. Rollback, and destroying an open handle, write nothing. | to add |
| A statement in a transaction reads the snapshot taken at the transaction's first statement plus the writes of its earlier statements, never its own: point reads, existence, counts, scans in both directions over tables and indexes, and the unique checks. | to add |
| A statement that fails inside a transaction writes nothing, and the transaction continues with its earlier statements' writes. | to add |
| A later statement may update or delete a row an earlier statement wrote; every index stays exact. | to add |
| A transaction that conflicts at commit with a concurrent commit (a written key changed since its snapshot, a `FOR UPDATE` row changed, a written table's DDL) is a serialization failure, SQLSTATE 40001, with nothing written. The layer does not retry it. | to add |
| A read that fails with I/O, CRC or memory errors fails the statement; it is never taken as absent. | to add |
| A statement that takes the transaction past its buffer bound is `BC_E_LIMIT`, and the transaction continues. | to add |

**Widened:**
- "Autocommit only" becomes: autocommit through `Database::execute`, and
  transactions through `Database::begin()`.
- "Snapshot isolation" covers transactions: one snapshot per transaction.

**Narrowed:**
- "A second write of one row within an execution is refused" becomes
  *within a statement*.

**Removed:**
- The exclusion "Multi-statement transactions, and a statement reading what
  an earlier statement of its transaction wrote" (#491).

## Open questions, each with a recommendation

1. **SQL `BEGIN` / `COMMIT` text, or the handle only?** *Recommend:* the
   handle only in the MVP. `Database::execute("BEGIN")` is refused (0A000).
   Statement text that opens a transaction needs per-session state that
   principle 3 keeps off the server, so it comes with the token form.
2. **A failed statement: continue, or abort the transaction as PostgreSQL
   does (25P02)?** *Recommend:* continue. It's what the statement savepoint
   gives for free and what TPROC-C's harness wants: New-Order's rollback
   on an invalid item is the caller's choice. If PostgreSQL's behaviour is
   wanted, it's a flag on the handle that closes it on the first error, and
   the proof machine compares under that flag.
3. **When is the snapshot taken: at `begin()` or at the first statement?**
   *Recommend:* the first statement, as `MariaDBTxn` does. An idle handle
   then pins nothing, and the two can't be told apart by any statement's
   result.
4. **DDL during a transaction.** *Recommend:* keep the pin and conflict at
   commit, as in *DDL* above. The alternative is refusing DDL while
   transactions are open.
5. **Buffer bound: what default, and per transaction or per database?**
   *Recommend:* 64 MiB per transaction, set in `TransactionOptions`. A
   database-wide memory bound across all open transactions comes with the
   server.
6. **Commit: replay the log, or emit each key's final state?** *Recommend:*
   replay the log. It's the order the engine already guarantees, it handles
   range deletes without extra rules, and TPROC-C's transactions write each
   key once or twice. Collapsing to final states is an optimisation behind
   the same commit, taken if a benchmark shows the log's cost.

## Later, and why none of it blocks the MVP

| Later | Why the MVP doesn't block it |
|---|---|
| Token transactions across requests (#494) | The handle is the token's in-process half; a token names a handle held with a TTL. |
| Procedures retried in the runtime (#495) | A procedure is a transaction whose statements are all the runtime's, so its retry is a loop around `begin` … `commit`. |
| Serializable and read committed | Guards are emitted per isolation level by the compiler, and the log already carries guards. |
| User `SAVEPOINT` / `ROLLBACK TO` | A log mark and an overlay copy, as statement rollback already is. |
| Sequences and auto-increment | A runtime service at commit, outside the plan (`docs/plan_api_design.md`, *Deferred*). TPROC-C's ids come from rows it reads and updates. |
| Deadlines turned into budgets | The budget is per statement already; a transaction deadline divides into it. |
| Bulk load | A transaction mode that flushes its log in batches, as `MariaDBTxn`'s bulk-copy mode does, into a keyspace no one else reads yet. |
| Transactional DDL | DDL statements writing into the transaction's log, with the catalog read through the view. |
