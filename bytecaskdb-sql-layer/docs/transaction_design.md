# Transactions in the SQL layer

Status: **design, approved by the owner (2026-10-20, #491).** Built: the
transaction's view, statements inside a transaction, and commit with
40001 on any conflict (slice A). Not yet built: insert claims (23505 at
commit), DDL during an open transaction, and `FOR UPDATE` (slice B). It
decides how the layer runs several statements as one
transaction, which the MVP needs: TPROC-C's New-Order is about thirty
statements that read their own writes (#487, *MVP*). Each decision is
marked **MVP** or **later**.

## Two standing rules

1. **Do what PostgreSQL does** whenever a behaviour has to be chosen.
2. **Never wait.** The layer never blocks on another transaction, a lock or
   a DDL. Where PostgreSQL's choice relies on waiting, the layer takes the
   closest outcome that needs none, and the difference is listed under
   *Known differences from PostgreSQL*.

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
  PostgreSQL's REPEATABLE READ takes it: "a query in a repeatable read
  transaction sees a snapshot as of the start of the first
  non-transaction-control statement in the transaction" (PostgreSQL 17
  documentation, *Transaction Isolation*). An idle handle then pins
  nothing. The catalog version is pinned with it (see *DDL*).
- **`commit()` returns its outcome or throws.** It returns `Committed`, or
  `RolledBack` for an aborted transaction, as PostgreSQL's command tag
  reports COMMIT or ROLLBACK, so a caller never takes a rollback for a
  commit. A transaction with no writes and no guards commits nothing and
  returns `Committed`.
- **After a failed statement the transaction is aborted,** as in
  PostgreSQL: every later `execute()` is refused with SQLSTATE 25P02
  ("current transaction is aborted, commands ignored until end of
  transaction block"), and `commit()` rolls it back without an error, as
  PostgreSQL's COMMIT does (owner's decision, 2026-10-20).
- **After `commit()`, `rollback()` or a serialization failure,** the handle
  is closed; any further call throws.

**Later:**
- `BEGIN` / `COMMIT` / `ROLLBACK` as SQL text. The MVP has the handle
  only, and `Database::execute("BEGIN")` is refused (0A000). Statement text
  that opens a transaction needs per-session state, which principle 3 keeps
  off the server, so it comes with the server;
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

**A failed statement aborts the transaction, as in PostgreSQL.** A
statement that fails — a constraint, the budget, `BC_E_LIMIT`, a read error
— discards its pending effects, so the overlay and the log are untouched:
each statement is its own savepoint. Then every later `execute()` is
SQLSTATE 25P02 until the transaction ends: `rollback()`, or `commit()`,
which rolls back and returns `RolledBack`. The statement's own error is
what `execute()` raises; 25P02 is for the statements after it.

User `SAVEPOINT` / `ROLLBACK TO` are **later**: TPROC-C doesn't use them.
When they come they are a mark in the log plus a copy of the overlay's
state at the mark, as `MariaDBTxn` keeps them; nothing here blocks that.

## Commit and conflicts — MVP

**Mechanism: commit replays the log into one `WritePlan` built on the
transaction's snapshot.** The log holds every put, delete, range delete and
guard, in the order the statements made them. The engine applies a plan's
operations in order (engine `CONTRACT.md`, *Ordered*), so a range delete
followed by a put inside it keeps the put. The commit also adds the
catalog-record guard of every table the transaction wrote, as
`bc_ctx_commit` does today. Emitting each key's final state instead is an
optimisation behind the same commit, with nothing visible changed, taken
only if a benchmark shows the log's cost.

The engine checks every written key against the snapshot, so the whole
transaction is first-committer-wins. A row read and then written, in any
statements, needs no guard.

| Outcome | Autocommit (`Database::execute`) | Transaction (`commit()`) |
|---|---|---|
| Committed | Rows returned | Returns |
| Conflict on a key the transaction inserted, now taken (below) | The retry's own unique check reports it: `ExecError`, `BC_C_UNIQUE`, **23505** | `ExecError`, `BC_C_UNIQUE`, SQLSTATE **23505**. Nothing written; the handle is closed. |
| Any other conflict | Retried on a fresh snapshot, up to three times, as today; then `ExecError` with `conflict()` | `SerializationFailure`, SQLSTATE **40001**. Nothing written; the handle is closed. The caller reruns the transaction. The layer never retries it: it can't rerun the caller's logic. |
| Constraint or other error in a statement | `ExecError`, never retried | Raised by `execute()` for the failing statement; the transaction is then aborted (25P02) |

Conflicts are detected only at commit. A statement never waits and never
fails because another transaction holds a row (see *Known differences*).

**A duplicate from a concurrent transaction is 23505, not 40001.**
PostgreSQL makes the second inserter of a unique key wait for the first and
then reports 23505. The closest outcome without waiting: when a commit
conflicts and a key the transaction inserted is now taken, report the
duplicate.
- **What is recorded.** As each statement runs, the transaction records
  its *insert claims*, in log order. A claim is a primary key `bc_insert`
  wrote, or a unique-index prefix it or `bc_update` checked, that the
  transaction's snapshot itself held free, with the table's or the index's
  id. A key free only because an earlier statement deleted it is not a
  claim: there the conflict is with the row the transaction deleted, which
  PostgreSQL reports as 40001 too.
- **How the commit decides.** On `BC_CONFLICT`, take a fresh snapshot, the
  latest committed state, and check the claims in log order:
  `contains_key` for a primary key, the first key at or after a prefix for
  a unique prefix. The first claim that is taken is the error: `BC_C_UNIQUE`
  with that claim's table or index id, SQLSTATE 23505. If none is taken, the
  conflict is 40001.
- **What it costs.** Nothing on a commit that succeeds. On a conflict, one
  record read per claim: at most one per inserted row plus one per unique
  index it maintains. These reads come after every statement has run, so
  they aren't charged to a statement's budget; they are bounded by the
  transaction's own inserts.
- **Autocommit agrees.** Its retry runs the statement again on a fresh
  snapshot, and the insert's own unique check reports the duplicate from
  the same latest state. When the last retry still conflicts, the same
  claim check runs before reporting a conflict. So both report 23505
  exactly when an inserted key is taken in the latest committed state.
- **Tests:**
  - two transactions insert one primary key and commit in turn: the second
    gets 23505 naming the table;
  - the same for one unique-index value, naming the index;
  - a conflict on a row the transaction updated, not inserted, is 40001;
  - deleting and reinserting a row that a concurrent transaction updates is
    40001;
  - the autocommit race test ("tcc: a unique insert raced by another commit
    conflicts, and its retry reports the duplicate") and its transaction
    twin report the same code for the same interleaving.

  The interleavings are forced with the commit hook
  (`bc_ctx_before_commit_for_testing`).

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

**DDL never waits.** It commits at once, whatever transactions are open.
The transaction keeps its pinned catalog and conflicts at commit if it
wrote a table the DDL changed.

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

PostgreSQL makes the DDL wait for the open transaction's locks; see *Known
differences*.

## Limits — MVP

- **Buffer size.** A transaction's log and overlay are bounded by
  `TransactionOptions::max_bytes`, default 64 MiB, well below the engine's
  1 GiB plan limit. A statement that would take the transaction past it
  fails with `BC_E_LIMIT` (SQLSTATE 54000). Its effects are discarded by
  the statement savepoint, and the transaction is aborted like any failed
  statement's. The bound counts
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
  - a failed statement writes nothing, every later `execute()` is 25P02,
    and `commit()` rolls back;
  - the snapshot is taken at the first statement, not at `begin()`.
- **Commit:**
  - first-committer-wins across transactions (lost-update, delete against
    update);
  - a conflict is 40001 with nothing written;
  - a duplicate from a concurrent transaction is 23505 (the tests under
    *Commit and conflicts*);
  - `FOR UPDATE` conflicts on a changed row;
  - a DDL racing a writing transaction conflicts;
  - rollback and the destructor write nothing.
- **Faults:**
  - read failure injected under each call through the view;
  - an allocation-failure sweep over a multi-statement transaction;
  - `BC_E_LIMIT` at the buffer bound.
- **Against PostgreSQL:** the proof machine (`docs/testing_design.md`) gains
  `BEGIN` / statements / `COMMIT` / `ROLLBACK` rules, compared with
  PostgreSQL's REPEATABLE READ on one connection, aborted transactions and
  25P02 included.
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
| A statement in a transaction reads the snapshot taken at the start of the transaction's first statement, not at `begin()`, plus the writes of its earlier statements, never its own: point reads, existence, counts, scans in both directions over tables and indexes, and the unique checks. | to add |
| A statement that fails inside a transaction writes nothing, and aborts the transaction: every later `execute()` is SQLSTATE 25P02, and `commit()` rolls back. | to add |
| A later statement may update or delete a row an earlier statement wrote; every index stays exact. | to add |
| A transaction that conflicts at commit with a concurrent commit (a written key changed since its snapshot, a `FOR UPDATE` row changed, a written table's DDL) is a serialization failure, SQLSTATE 40001, with nothing written. The layer does not retry it. | to add |
| A commit that conflicts because a key it inserted (a primary key, or a unique index value) was taken by a concurrent commit is `BC_C_UNIQUE`, SQLSTATE 23505, naming the table or index, with nothing written; an autocommit statement reports the same for the same race. | to add |
| No statement, commit or DDL waits for another transaction: a conflict is reported at commit, and a DDL commits whatever transactions are open. | to add |
| A read that fails with I/O, CRC or memory errors fails the statement; it is never taken as absent. | to add |
| A statement that takes the transaction past its buffer bound (`TransactionOptions`, 64 MiB by default) is `BC_E_LIMIT`, SQLSTATE 54000, and aborts the transaction. | to add |

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

## Known differences from PostgreSQL

Each comes from the rule *never wait*.

| PostgreSQL (REPEATABLE READ) | This layer | Why |
|---|---|---|
| A second updater of a row waits for the first transaction, then gets 40001 at its statement if the first committed. | Both update. The second to commit gets 40001 at `commit()`. | No row locks: conflicts are found at commit. |
| A second inserter of a unique key waits, then gets 23505 at its `INSERT`. | Both insert. The second to commit gets 23505 at `commit()` (*Commit and conflicts*). | The same outcome, reported at commit instead of after a wait. |
| `SELECT … FOR UPDATE` locks the rows: other writers wait. | It guards them: the guarding transaction gets 40001 at commit if a writer of those rows committed first. A writer is never held back. | A guard, not a lock. |
| DDL waits for the locks of open transactions on its table. | DDL commits at once. An open transaction that wrote the table gets 40001 at commit. | DDL never waits. |

## Later, and why none of it blocks the MVP

| Later | Why the MVP doesn't block it |
|---|---|
| `BEGIN` / `COMMIT` text (with the server) | The handle is what it opens; the text is the server's spelling of `begin()`. |
| Token transactions across requests (#494) | The handle is the token's in-process half; a token names a handle held with a TTL. |
| Procedures retried in the runtime (#495) | A procedure is a transaction whose statements are all the runtime's, so its retry is a loop around `begin` … `commit`. |
| Serializable and read committed | Guards are emitted per isolation level by the compiler, and the log already carries guards. |
| User `SAVEPOINT` / `ROLLBACK TO` | A log mark and an overlay copy, as statement rollback already is. |
| Sequences and auto-increment | A runtime service at commit, outside the plan (`docs/plan_api_design.md`, *Deferred*). TPROC-C's ids come from rows it reads and updates. |
| Deadlines turned into budgets | The budget is per statement already; a transaction deadline divides into it. |
| Bulk load | A transaction mode that flushes its log in batches, as `MariaDBTxn`'s bulk-copy mode does, into a keyspace no one else reads yet. |
| Transactional DDL | DDL statements writing into the transaction's log, with the catalog read through the view. |
