# Transactions in the SQL layer

Status: **built** (owner's approval, 2026-10-20, #491). How the layer runs
several statements as one transaction: TPROC-C's New-Order is about thirty
statements that read their own writes. The parts not built are listed under
*Later*.

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

## Autocommit statements

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

## Shape

An in-process transaction handle on the embedded API:

```cpp
class Transaction {                    // from Database::begin(); move-only
public:
  auto execute(const Statement&, const std::vector<Value>& params) -> Rows;
  auto commit() -> CommitOutcome;      // Committed, or RolledBack after a failed statement;
                                       // throws SerializationFailure (40001), or
                                       // TransactionResolutionUnknown (08007)
  void rollback() noexcept;
  ~Transaction();                      // rolls back if still open
};
auto Database::begin(TransactionOptions = {}) -> Transaction;
```

- **One handle, one thread at a time.** Each handle owns its own `bc_ctx`,
  so transactions run concurrently on different threads. This is what the
  harness needs at 16 and 48 threads. Prepared statements are shared
  read-only across handles; compiling one is serialised.
- **`Database::execute` is autocommit,** a one-statement transaction
  retried as *Autocommit statements* says.
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
- **After a failed statement the transaction is aborted,** and lets go at
  once of its snapshot, its pinned catalog and its writes: an aborted handle
  holds back no vacuum, and keeps only its state. As in PostgreSQL: every later `execute()` is refused with SQLSTATE 25P02
  ("current transaction is aborted, commands ignored until end of
  transaction block"), and `commit()` rolls it back without an error, as
  PostgreSQL's COMMIT does (owner's decision, 2026-10-20).
- **After `commit()`, `rollback()` or a serialization failure,** the handle
  is closed; any further call throws. So does a moved-from handle.
- **Threads.** Handles run on different threads at once, alongside
  prepares and autocommit statements. No statement, commit or prepare waits
  on another's engine I/O: the only lock any call holds across I/O is
  shared, and only `close()` takes it exclusively, to wait for the calls in
  flight. A DDL that finds another in progress fails at once (*DDL*), and a
  statement's pin may wait only for the instant a DDL publishes. A
  compiled plan is never changed while a statement runs it; a recompile
  swaps in a new one. `close()` ends every open handle, and a handle may
  outlive its `Database`: it then touches nothing of it.

**`BEGIN`, `COMMIT` and `ROLLBACK` as SQL text** are the server's: it keeps
the session's block status and opens a handle at `BEGIN`
(`server_design.md`, *A session*). Through the embedded API,
`Database::execute("BEGIN")` is refused (0A000), since the handle is how a
transaction is opened there.

**Later:**
- a token form, where `BEGIN` returns a token naming a snapshot and a
  pending log held with a TTL, and statements carry it across requests on
  any thread. It belongs to the transport design (#487, *Next*);
- procedures that run a whole transaction inside the runtime and retry it
  there (#495).

Both reuse this engine. The handle is the token's in-process half.

## The transaction's view

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
| `bc_scan_open` / `bc_scan_next`, forward and reverse, tables and indexes | A two-pointer merge of the snapshot cursor and the overlay, in the scan's direction and inside the scan's bounds. On a tie the overlay wins and a tombstone hides the snapshot's key. Snapshot keys inside a range tombstone are passed over by one seek past the range, so a scan after `DELETE FROM t` costs what the rows left cost, not what was deleted. `MariaDBTxn::MergeIterator`'s algorithm, with ranges added. `bc_del_range`'s walk of its rows and the unique checks read the same way. |
| `bc_scan_row` on an index | The row is fetched through the view, so an index entry an earlier statement wrote finds the row it wrote. |
| `bc_count_range` | The snapshot's `count_keys`, then corrected: one for each overlay point entry in range whose presence differs from the snapshot's, and minus `count_keys` over each range tombstone's part of the range. Still bounded, at two record reads plus one existence check per overlay entry in range, all charged. |
| Unique checks in `bc_insert` and `bc_update` | Against the view: a key or prefix an earlier statement freed is free, and one it took is taken. The range guard over the prefix stays, for concurrent transactions. |

- **Budget.** A read the overlay answers is charged as if it had read a
  record, so a plan's budget doesn't depend on what the transaction has
  buffered. Every record read is charged, the one that finds the end of a
  range too, and nothing past a read's bounds is read.
- **Lifetime.** The overlay lives in the transaction, outside the context's
  arena, which is still reset after every statement.
- **Errors propagate. No read error is ever taken as "absent".** An I/O,
  CRC or memory error in any read through the view, overlay or snapshot,
  sets the context's sticky error (`BC_E_IO`, `BC_E_CORRUPT`,
  `BC_E_MEMORY`), and the statement fails. The plugin's version of this bug
  is #524. Here it is a requirement with a test: inject a read failure under
  each call in the table above.

## Statements inside a transaction

**A statement sees the snapshot plus the writes of earlier statements,
never its own.** A statement's effects go to its own pending list, as an
execution's do. The view it reads is the overlay as it stood when the
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
works as in autocommit: `BC_E_HANDLE`, or `BC_C_UNIQUE` for a second insert of one
key. A later statement may write a row an earlier one wrote, and the
cross-statement refusal that `docs/plan_api_design.md` describes is removed.

**A failed statement aborts the transaction, as in PostgreSQL.** A
statement that fails — a constraint, the budget, `BC_E_LIMIT`, a read error
— discards its pending effects, so the overlay and the guards are untouched:
each statement is its own savepoint. Then every later `execute()` is
SQLSTATE 25P02 until the transaction ends: `rollback()`, or `commit()`,
which rolls back and returns `RolledBack`. The statement's own error is
what `execute()` raises; 25P02 is for the statements after it.

User `SAVEPOINT` / `ROLLBACK TO` are **later**: TPROC-C doesn't use them.
When they come they are a mark in the log plus a copy of the overlay's
state at the mark, as `MariaDBTxn` keeps them; nothing here blocks that.

## Commit and conflicts

**Mechanism: commit builds one `WritePlan` on the transaction's snapshot
from the overlay.** The overlay's range tombstones go first, then each
key's final state, then the statements' guards. The engine applies a
plan's operations in order (engine `CONTRACT.md`, *Ordered*), and every
point entry is newer than any range over it, so a range delete followed by
a put inside it keeps the put. The writes are held once, in the overlay:
there is no separate log. The commit also adds the catalog-record guard of
every table the transaction wrote, as `bc_ctx_commit` does.

The engine checks every written key against the snapshot, so the whole
transaction is first-committer-wins. A row read and then written, in any
statements, needs no guard.

| Outcome | Autocommit (`Database::execute`) | Transaction (`commit()`) |
|---|---|---|
| Committed | Rows returned | Returns |
| Conflict on a key the transaction inserted, now taken (below) | The retry's own unique check reports it: `ExecError`, `BC_C_UNIQUE`, **23505** | `ExecError`, `BC_C_UNIQUE`, SQLSTATE **23505**. Nothing written; the handle is closed. |
| Any other conflict | Retried on a fresh snapshot, up to three times; then `ExecError` with `conflict()` | `SerializationFailure`, SQLSTATE **40001**. Nothing written; the handle is closed. The caller reruns the transaction. The layer never retries it: it can't rerun the caller's logic. |
| Constraint or other error in a statement | `ExecError`, never retried | Raised by `execute()` for the failing statement; the transaction is then aborted (25P02) |
| The commit failed and degraded the engine: its batch may have reached the data file | `TransactionResolutionUnknown`, SQLSTATE **08007**, never retried | The same, and the handle is closed. Never "nothing written": a reopen finds the writes whole or not at all. |

A DDL statement's commit follows the same rule. The rule is decided by
whether the engine was degraded before the commit and is after: another
writer's failure in between makes a commit that wrote nothing report 08007,
the safe direction. Reporting it exactly needs the engine to say whether
this batch was appended (#531).

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
  conflict is 40001, and so it is when the check cannot read: the conflict
  is the fact, and the claim check only names it.
- **The first conflict in statement order decides** (#552). PostgreSQL's
  second inserter only reaches its `INSERT` if nothing before it stopped
  it. A `SELECT … FOR UPDATE`, `UPDATE` or `DELETE` of a row a concurrent
  transaction changed waits for it and then fails with 40001 at that
  statement. TPROC-C's New-Order is the case: both transactions lock the
  district row, bump `d_next_o_id` and insert the same order, and
  PostgreSQL's second one gets 40001 at the lock, never 23505. So each
  statement in a transaction records the rows it locks or writes, as its
  snapshot holds them: the key and a 64-bit fingerprint of the stored
  bytes, taken from the row the statement already read. Rows the overlay
  holds are not recorded: they are an earlier statement's write, already
  recorded, or its insert, a claim. Each claim notes how many locks came
  from statements up to and including its own. Before a claim is checked,
  those locks are read in the latest state, in order, and one whose row is
  gone or whose fingerprint differs makes the conflict 40001. The claim's
  own statement counts because an `UPDATE` that moves a row to a unique
  value locks the old row first: PostgreSQL fails it there with 40001 when
  a concurrent commit changed the row. An `INSERT` locks nothing, so its
  claim decides.
  Comparing bytes, not versions, a row a concurrent commit changed and
  another changed back counts as unchanged, and the commit reports the
  duplicate; the engine gives a snapshot no per-key version to compare.
- **Where claims are kept.** A statement's claims are its own until
  `bc_txn_add`, which appends them to the transaction's with its writes, so
  a failed statement leaves none. They count towards the transaction's
  buffer bound. At the check, a claim counts only while the transaction's
  final writes still put its key or its index entry: a value a later
  statement moved or deleted away claims nothing. An index claim is taken
  only by another row's entry: two updates of one row to one value conflict
  on the row, 40001, as in PostgreSQL. (That statement's unique
  check still guards the value it moved away, so such a commit can
  conflict where PostgreSQL's would not: #537.)
- **What it costs.** No read on a commit that succeeds: each claim is a
  copy of its key, kept until the commit, and each lock a copy of its key
  and a fingerprint, computed from bytes the statement already holds. On a
  conflict, one record read per claim, at most one per inserted row plus
  one per unique index it maintains, and one per lock of a statement up to
  the last claim checked. These reads come after every statement has run, so
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
  - New-Order's race: a row an earlier statement locked with `FOR UPDATE`,
    or updated, changed by the commit that took the inserted key, is 40001;
    an earlier locked row nobody changed leaves it 23505, and so does one
    rewritten with the same bytes, and an insert that comes before the
    changed row's statement;
  - an `UPDATE` moving a row to a unique value, raced by a commit that
    changed the row and took the value: 40001;
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

## Isolation

- **Snapshot isolation by default.** One snapshot per transaction. Writes
  are checked by the engine, and no guard is emitted for reads.
- **`SELECT … FOR UPDATE`** emits `bc_guard_unchanged` on every row key it
  returns, through `bc_guard_row`, which encodes the row's key into the
  guard without the arena, kept with the transaction's guards. The
  commit fails if one of those rows changed since the snapshot, in a
  transaction that wrote nothing too: the engine checks a guard-only plan
  and writes nothing. An autocommit execution that writes nothing commits
  nothing, guards and all: what its one statement read is its snapshot's
  either way, so the engine, which may be refusing writes, is not asked. A
  range an index would cover reads each row instead, for its key. Only
  `FOR UPDATE` itself is accepted: `FOR SHARE` and `FOR NO KEY UPDATE`,
  `NOWAIT` and `SKIP LOCKED`, which only mean something to a locker, and
  `FOR UPDATE OF` are refused at parse. This is how TPROC-C's
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

## DDL during an open transaction

**DDL never waits.** It commits at once, whatever transactions are open.
The transaction keeps its pinned catalog and conflicts at commit if it
wrote a table the DDL changed.

- The catalog version is pinned with the snapshot at the transaction's
  first statement. Every statement of the transaction is verified against
  that version, and its index maintenance follows that version's indexes.
- A DDL that commits meanwhile changes the catalog records of the tables it
  touches. A transaction that wrote one of those tables conflicts at commit
  through the existing catalog-record guard, and gets 40001. A transaction
  that only read them was consistent with its pin and commits, unless it
  took `FOR UPDATE` rows the DDL rewrote (an `ADD COLUMN … DEFAULT`): its
  guards see the change, and it gets 40001. PostgreSQL would make the DDL
  wait for those row locks; never waiting, the reader fails instead.
- A statement whose plan doesn't verify against the pinned version (cached
  against a newer catalog, naming a column the pin lacks) fails with 40001
  and closes the transaction, rather than recompiling against a catalog
  older than the cache. This is rare and loses nothing a retry wouldn't
  recover. On the transaction's first statement, which has read nothing
  yet, a DDL that raced the pin is not a failure: the transaction is
  pinned again, to the live catalog and a new snapshot, as PostgreSQL takes
  the snapshot when the first statement starts.
- **A DDL never waits for another DDL either.** One that finds another DDL
  of its catalog in progress fails at once, writing nothing, with 40001,
  and may be retried. The one wait left is a statement's pin: it waits for
  the instant between a DDL's batch reaching the engine and the new catalog
  taking effect, never while the DDL writes or syncs.
- A DDL statement given to `Transaction::execute` is refused with
  `FeatureNotSupported` (SQLSTATE 0A000; Python's
  `bytecask_sql.FeatureNotSupported`, a `SqlError`) before anything runs, and the transaction aborts, as any error aborts one in
  PostgreSQL. DDL stays its own atomic operation. Transactional DDL
  is later.

PostgreSQL makes the DDL wait for the open transaction's locks; see *Known
differences*.

## Limits

- **Buffer size.** What a transaction holds, its overlay and its guards,
  is bounded by `TransactionOptions::max_bytes`, default 64 MiB, well below
  the engine's 1 GiB plan limit. A statement's write set is counted as it
  grows, at the call that adds to it, against the room the transaction has
  left (`plan_api_design.md`, *The write set is bounded as it grows*), and
  the bound is checked again before a statement's writes are taken in,
  from the statement's own pending effects: one that would take the
  transaction past it fails with `BC_E_LIMIT` (SQLSTATE 54000) without the
  transaction allocating for it, the transaction is aborted like any failed
  statement's, and what it held is freed with the abort.
  The count is a conservative bound, never less than the memory held: each
  buffer's capacity plus an allocator overhead, each map node's full size,
  and the guards' and written tables' vectors at the size they could grow
  to. So a transaction of small writes can't exhaust memory through
  overhead.
- **Memory.** The overlay allocates from the heap. A failed allocation is
  `BC_E_MEMORY` for the statement, never an abort and never an exception
  into a plan (`CONTRACT.md`, *Plan safety*). An allocation-failure sweep
  over a transaction's statements proves it, as `plan_alloc_faults_test`
  does for executions.
- **Budget.** Each statement keeps its own `bc_budget`, its work and its
  memory, as each execution does. A transaction-wide budget, or a request deadline turned into one,
  is later (*Later*). Nothing here blocks it: the
  budget is passed per statement.
- **Duration.** An open transaction holds a snapshot, which keeps the data
  files it references from vacuum. An idle handle therefore defers vacuum.
  The in-process handle is the caller's to close. The token form's TTL
  (*Later*) would bound this for remote clients.

## The plan API and the runtime

- **Plan-visible calls: none.** `bc_get`, `bc_exists`, the cursors,
  `bc_count_range` and the writes are the same in a transaction. A plan is
  a pure function of its parameters and the transaction's view
  (`plan_api_design.md`, *Model*).
- **Runtime calls, called by the embedded API and never by a plan:**
  - a transaction object holding the snapshot, the pinned catalog, the
    overlay and the guards;
  - `bc_ctx_begin` given a transaction instead of taking its own snapshot;
  - `bc_ctx_commit` split into appending a statement's effects to the
    transaction and committing the transaction.
  - `bc_txn_commit(bc_txn*, sync)` takes no context and reports through
    the transaction (`bc_txn_error`); an abort and a free detach the
    context, keeping its finished statement's results readable.

  These calls are internal: the embedded API's `Transaction` is their only
  caller, and it keeps their protocol by construction. One context per
  transaction; each statement is `bc_ctx_begin_txn`, `bc_ctx_run`,
  `bc_txn_add`, then `bc_ctx_reset`, except that a statement refused with
  `BC_L_INCOMPATIBLE` before it ran may be reset and begun again; after
  any other failed statement the transaction is only freed; and one
  `bc_txn_commit`, with no statement open. The runtime does not defend
  against use out of this order, and the contract does not cover it. The
  checks it does make (a second context or a commit with a statement open
  refused as `BC_E_HANDLE`) are a debugging aid, not a guarantee.

  They are declared in `bc_plan.h`'s runtime section. `bc_execute` keeps
  its signature for autocommit.
- **`written_rows` and `deleted_ranges`** are per statement, not per
  execution.
- **Keys and values in the transaction's own C++ API** are
  `std::span<const std::uint8_t>`, or `BytesView`, never a pointer and a
  length.

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
- **Against PostgreSQL:** the proof machine (`testing_design.md`) has
  `BEGIN` / statements / `COMMIT` / `ROLLBACK` rules, compared with
  PostgreSQL's REPEATABLE READ on one connection, aborted transactions and
  25P02 included.
- **Concurrency:** Elle through SQL (`testing_design.md`, *The TPROC-C
  harness*): the harness's transactions as histories, checked as snapshot isolation, and
  as serializable where `FOR UPDATE` guards the reads that decide a write.

## Known differences from PostgreSQL

Each comes from the rule *never wait*.

| PostgreSQL (REPEATABLE READ) | This layer | Why |
|---|---|---|
| A second updater of a row waits for the first transaction, then gets 40001 at its statement if the first committed. | Both update. The second to commit gets 40001 at `commit()`. | No row locks: conflicts are found at commit. |
| A second inserter of a unique key waits, then gets 23505 at its `INSERT`. | Both insert. The second to commit gets 23505 at `commit()` (*Commit and conflicts*). | The same outcome, reported at commit instead of after a wait. |
| `SELECT … FOR UPDATE` locks the rows: other writers wait. | It guards them: the guarding transaction gets 40001 at commit if a writer of those rows committed first. A writer is never held back. | A guard, not a lock. |
| DDL waits for the locks of open transactions on its table. | DDL commits at once. An open transaction that wrote the table gets 40001 at commit. | DDL never waits. |
| On a standby, `SELECT … FOR UPDATE` is refused at the statement (25006). | On a follower it runs, and a transaction that used it fails at commit with the engine's refusal of writes, as its writes would; in autocommit it reads as the `SELECT` does. | Its guards are checked by the engine at commit, which a follower refuses. |

## Later

Not built. Each gets an issue when it is picked up.

| Later | Why nothing built blocks it |
|---|---|
| Token transactions across requests (the transport design, #487) | The handle is the token's in-process half; a token names a handle held with a TTL. |
| Procedures retried in the runtime (#495) | A procedure is a transaction whose statements are all the runtime's, so its retry is a loop around `begin` … `commit`. |
| Serializable and read committed | Guards are emitted per isolation level by the compiler, and the transaction already keeps guards. |
| User `SAVEPOINT` / `ROLLBACK TO` | A log mark and an overlay copy, as statement rollback already is. |
| Sequences and auto-increment | A runtime service at commit, outside the plan (`docs/plan_api_design.md`, *Deferred*). TPROC-C's ids come from rows it reads and updates. |
| Deadlines turned into budgets | The budget is per statement already; a transaction deadline divides into it. |
| Bulk load | A transaction mode that flushes its log in batches, as `MariaDBTxn`'s bulk-copy mode does, into a keyspace no one else reads yet. |
| Transactional DDL | DDL statements writing into the transaction's log, with the catalog read through the view. |
