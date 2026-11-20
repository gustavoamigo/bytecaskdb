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
  keeps no disk blocks of files vacuum reclaimed, and keeps only its state. As in PostgreSQL: every later `execute()` is refused with SQLSTATE 25P02
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
- **Sharing.** The transaction holds the overlay by `shared_ptr`.
  `bc_txn_overlay_pin` (a cursor's `Transaction::open_cursor`) takes a
  second reference, and `bc_txn_add` writes through `own_overlay()`, the one
  place that mutates it: if a pin shares the overlay it is cloned first and
  the statement writes to the copy, so a pinned overlay never changes. A
  transaction with no pin, or a statement that adds no put or delete, never
  clones. The clone is one copy of the overlay, made by the first writing
  statement after a pin; later statements find it unshared. A statement that
  reads through a pin (`bc_ctx_use_overlay`) reads the frozen copy and writes
  nothing. A clone that fails allocating is `BC_E_MEMORY` at `bc_txn_add`,
  which aborts the transaction, and changes neither overlay.
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

User savepoints build on the same rule: *Savepoints* below.

## Savepoints

`SAVEPOINT`, `ROLLBACK TO SAVEPOINT` and `RELEASE SAVEPOINT` behave as in
PostgreSQL 17. The embedded API has `Transaction::savepoint`, `rollback_to`
and `release`; the server runs the statements on an explicit block's
transaction.

**A savepoint is a mark.** `bc_txn_savepoint` pushes a `TxnMark`: a
`shared_ptr` to the transaction's overlay, the lengths of its guards,
claims, locks and written tables, and `held`. Those vectors only grow
between marks, so their lengths are enough to cut them back. A mark taken
before the first statement pins nothing: a savepoint takes no snapshot, as
in PostgreSQL.

**The overlay is shared copy-on-write, as a cursor's pin is.** The mark
copies nothing. The first statement after it that writes clones the
overlay in `bc_txn_add` (`own_overlay`), and the mark keeps the old one.
That copy is charged to `max_bytes` from the clone on (`marked_bytes`,
counted by `txn_holds`; marks that share an overlay count it once). Before
the clone, each call of the statement counts the copy it would force
(`mark_pending`), so a statement that would pass the bound because of a
mark fails at its call with `BC_E_LIMIT`, as any other. So `SAVEPOINT`
itself never fails for room, and a block of savepoints with no write
between them costs nothing.

**`ROLLBACK TO` restores the mark.** `bc_txn_rollback_to` drops the marks
above it, points the overlay back at the mark's, cuts the vectors back,
and restores `held`; the mark stays, as PostgreSQL's savepoint stays after
`ROLLBACK TO`. The snapshot, the pinned catalog and the cursors' pins stay.
It takes an aborted transaction back to open, and a healthy one too: an
error the API raised before a statement reached the runtime (a bind error,
a DDL refused, `Transaction::fail`) leaves the runtime's transaction
healthy. The reservation on the memory cap shrinks to what is left.

**`RELEASE` drops marks.** `bc_txn_release` drops the mark and those above
it; the writes since stay. An overlay only a dropped mark held goes with it.

**An abort with marks keeps the snapshot.** Without marks an abort frees
everything at once (*Statements inside a transaction*). With marks
`txn_abort` keeps the snapshot, the catalog and the marks, and goes back to
the newest mark, so what the failed statements wrote is freed all the same.
The API's `TxnHandle::abort` keeps the context, the cursors and the
savepoints. Every call but `rollback_to`, `commit` and `rollback` is then
25P02, as before.

**Names.** The API matches names exactly, newest first, so a name taken
again hides the older until released or rolled back past. An unknown name
is `ast::SqlError` 3B001, "savepoint "x" does not exist", and aborts the
transaction, as any error does in PostgreSQL; in an aborted transaction it
stays aborted. The server parses the statements with PostgreSQL's grammar
(libpg_query), which folds an unquoted name to lower case.

**Cursors (#727): PostgreSQL's rule.** Each cursor records how many cursors
its transaction had opened before it, and each savepoint the count at its
mark. `rollback_to` closes the cursors opened since the savepoint, hold
cursors included. Those opened before stay open where they stand: a fetch
is not undone. A cursor whose own fetch failed is closed and *failed*
(`Cursor::is_failed`): it cannot be run, which the server reports as
PostgreSQL does (55000, "portal "c" cannot be run"), until it is closed. If
the rollback goes to a savepoint taken before it was declared, it is
forgotten instead (34000 when named). While the transaction is aborted, a
fetch is 25P02.

**The server.** Outside an explicit block, an implicit one included, each
statement is 25P01, "SAVEPOINT can only be used in transaction blocks" (or
`RELEASE SAVEPOINT`, `ROLLBACK TO SAVEPOINT`). In a failed block,
`SAVEPOINT` and `RELEASE` are 25P02 and `ROLLBACK TO` ends the failure. A
failed block's transaction is aborted (`Transaction::fail`), not ended, so
the savepoints survive the error. Each savepoint records the block's
read-only mode, which `ROLLBACK TO` and `RELEASE` restore, as PostgreSQL's
end of a subtransaction does. While a savepoint is open, `SET TRANSACTION
ISOLATION LEVEL` naming a level other than the block's own is 25001, "must
not be called in a subtransaction" (the block's own level is taken), and
`SET TRANSACTION READ WRITE` in a read-only block is 25001, "cannot set
transaction read-write mode inside a read-only transaction", as PostgreSQL
17 reports them. The tags are `SAVEPOINT`, `ROLLBACK` and `RELEASE`.

**Cost.** The first write after each savepoint copies the whole overlay:
O(overlay) time and memory per savepoint followed by a write, charged to
`max_bytes`. A client that wraps every statement in a savepoint (psql's
`ON_ERROR_ROLLBACK`, a driver's autosave) pays it per statement, which is
quadratic in the transaction's writes. A per-mark undo log would make it
O(statement) (#850).

**What does not change.** A statement that fails with 40001 under the rule's
exceptions 1 and 2 (*A transaction binds only against its own catalog*)
ends a transaction that has run no DDL, savepoints or not. A ROLLBACK TO
keeps the transaction's private plans for the catalog it returns to: a
mark does not record them.

**With DDL (stage 2).** A mark carries the transaction's catalog, so ROLLBACK
TO undoes the DDL since the savepoint as it undoes the writes, and an
abort with marks goes back to the newest mark's catalog. A transaction
that has run DDL is 40001 at its next statement once another DDL commits
(*Catalog snapshots and isolation levels with DDL*); that 40001 aborts it,
as a failed statement does, and ROLLBACK TO a mark taken before its first
DDL lets it go on.

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
  PostgreSQL reports as 40001 too. Nor is a key under the arbiter of an
  `INSERT … ON CONFLICT DO NOTHING` (`bc_insert_on_conflict`): a
  concurrent commit of it is 40001, as in PostgreSQL's REPEATABLE READ,
  where the inserter cannot see the row it would skip on. `DO UPDATE`
  (`bc_insert_or_existing`) is the same where it inserts; where it
  updates, the row it updates is a written key the commit checks.
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
  final writes still put its key, or for an index some entry under its
  value: a value a later statement moved or deleted away claims nothing. An index claim is taken
  only by another row's entry: two updates of one row to one value conflict
  on the row, 40001, as in PostgreSQL.
  The same rule prunes the guards: a unique check's range guard covers the
  value's prefix, and the commit submits it only while the final writes put
  some entry under that prefix (`Overlay::puts_under`, which decides the
  index claims too). The test is on the value, not the entry the check
  recorded: an UPDATE that moves the row's primary key rewrites the entry
  without checking the value again, and the guard must survive it. A value
  a later statement moved or deleted away, with no row of the transaction
  holding it, is free to a concurrent commit, as in PostgreSQL (#537); one
  moved away and back, or a row kept at its value, keeps its guard. A row
  re-inserted under the value has its own statement's guard. Dropping a
  guard admits no lost update or duplicate: it protects only values this
  transaction writes, and the primary-key and row-lock guards are untouched.
  The pruning depends on `Overlay::puts_under` and the entry-key layout (the
  index prefix first).
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
  first statement. Every statement of the transaction is bound against
  that catalog, and only that one (*A transaction binds only against its
  own catalog*), and its index maintenance follows that version's indexes.
- A DDL that commits meanwhile changes the catalog records of the tables it
  touches. A transaction that wrote one of those tables conflicts at commit
  through the existing catalog-record guard, and gets 40001. A transaction
  that only read them was consistent with its pin and commits, unless it
  took `FOR UPDATE` rows the DDL rewrote (an `ADD COLUMN … DEFAULT`): its
  guards see the change, and it gets 40001. PostgreSQL would make the DDL
  wait for those row locks; never waiting, the reader fails instead.
- A statement is compiled against the pinned catalog, and is 40001 only by
  the rule's exceptions 1 and 2: a binder error while the live catalog is
  past the pin, or result columns fixed at a later catalog that differ at
  the pin. Either closes the transaction, and loses nothing a retry
  wouldn't recover. On the transaction's first statement, which has read
  nothing yet, a DDL that raced the pin is not a failure: the transaction
  is pinned again, to the live catalog and a new snapshot, as PostgreSQL
  takes the snapshot when the first statement starts.
- **A DDL never waits for another DDL either.** One that finds another DDL
  of its catalog in progress fails at once, writing nothing, with 40001,
  and may be retried. The one wait left is a statement's pin: it waits for
  the instant between a DDL's batch reaching the engine and the new catalog
  taking effect, never while the DDL writes or syncs.
- A DDL statement given to `Transaction::execute`, CREATE TABLE AS and
  SELECT … INTO included, is a statement of the transaction, and so is one
  in a server block, explicit or implicit: *Transactional DDL*.

PostgreSQL makes the DDL wait for the open transaction's locks; see *Known
differences*.

## Limits

- **Buffer size.** What a transaction holds, its overlay, its guards,
  claims and locks, is bounded by `TransactionOptions::max_bytes`, default
  64 MiB, well below the engine's 1 GiB plan limit. A statement's write set
  is statement memory, charged to its statement budget as it grows
  (`plan_api_design.md`, *The write set is statement memory*). `bc_txn_add`
  moves it to the transaction: it releases it from the statement budget
  and counts it against `max_bytes` by the same size functions
  (`effect_bytes`, `table_bytes`, `claim_bytes`, `lock_bytes`). Each of the
  statement's calls checks the transaction's room for the write set it
  would reach, so `bc_txn_add` merges only what fits: a statement that
  would take the transaction past it fails with `BC_E_LIMIT` (SQLSTATE
  54000) without the transaction allocating for it, the transaction is
  aborted like any failed statement's, and what it held is freed with the
  abort. The commit copies the overlay and the guards into the engine's
  `WritePlan` while they are still held, so each write, guard and table is
  counted with its entry in that copy from the statement that adds it
  (`effect_bytes`, `table_bytes`): a transaction never fails at COMMIT for
  memory its statements did not reserve, and the statement that would
  take it past `max_bytes` fails instead. A unique check's range guard
  that the commit prunes stays counted: the count is reserved when the
  statement adds the guard, and the prune only leaves it above what the
  commit copies. The rows, unique values and ranges the statement
  wrote stay statement memory, and go with the statement.
  The count is a conservative bound, never less than the memory held: each
  buffer's capacity plus an allocator overhead, each overlay node's full size,
  and each guard, claim, lock and table's slot at twice its size, the most
  its vector grows to. So a transaction of small writes can't exhaust
  memory through overhead.
- **Pins.** A pin reserves the overlay's bound as of the pin (`Overlay::bytes`)
  against `max_bytes`, counted with what the transaction holds until the pin
  is released: the copy a later write forces is then already paid for.
  A pin that does not fit is `BC_E_LIMIT` (54000) at the pin and aborts the
  transaction; a pin of an empty overlay reserves nothing. A write that would
  pass the bound because a pin holds a copy fails at the write, as any other.
  `bc_txn_repin` is refused while a pin is live: the snapshot a cursor reads
  does not move. The pin also pins the snapshot and catalog if no statement
  has.
- **Savepoints.** A savepoint charges nothing; the first write after it
  copies the overlay for it, charged as a pin's copy is, and O(overlay) in
  time: a savepoint around every statement is quadratic in the
  transaction's writes (*Savepoints*, #850).
- **Memory.** The overlay allocates from the heap. A failed allocation is
  `BC_E_MEMORY` for the statement, never an abort and never an exception
  into a plan (`CONTRACT.md`, *Plan safety*). An allocation-failure sweep
  over a transaction's statements proves it, as `plan_alloc_faults_test`
  does for executions.
- **Budget.** Each statement keeps its own `bc_budget`, its work and its
  memory, as each execution does. A transaction-wide budget, or a request deadline turned into one,
  is later (*Later*). Nothing here blocks it: the
  budget is passed per statement.
- **Duration.** An open transaction holds a snapshot, which keeps the disk
  blocks of the data files it references, even once vacuum has unlinked them
  (`session_design.md`, *Background vacuum*). An idle handle therefore defers
  what vacuum gives back. The in-process handle is the caller's to close. The token form's TTL
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
  `bc_txn_add`, then `bc_ctx_reset`; after a failed statement the
  transaction is only freed, or rolled back to a mark; and one
  `bc_txn_commit`, with no statement open. A statement's plan is bound at
  the pin before it begins (`bc_txn_pinned_catalog` pins first), so the
  first statement's re-pin (`bc_txn_repin`) comes before its begin, and a
  plan refused with `BC_L_INCOMPATIBLE` is a bug, reported as 40001. The
  private plans are charged with `bc_txn_charge_plans` and returned with
  `bc_txn_release_plans`, a term of their own beside `held`; a cursor's
  pin reserves its private plan with `bc_txn_overlay_pin_reserving`. The runtime does not defend
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
- **Savepoints:** `transaction_test.cpp` (a rollback to a mark and the
  mark staying, an abort with a mark keeping the snapshot, release, the
  mark's copy charged at the first write), `api_transaction_test.cpp`,
  `api_cursor_test.cpp` (#727's cursor rule) and the wire tests
  `test_savepoints_*` and `test_rollback_to_closes_the_cursors_declared_after_the_savepoint`,
  over simple and extended Query.
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
  - an allocation-failure sweep over a pin and over the clone the next write forces;
  - an allocation-failure sweep over a multi-statement transaction;
  - `BC_E_LIMIT` at the buffer bound.
- **Against PostgreSQL:** the proof machine (`testing_design.md`) has
  `BEGIN` / statements / `COMMIT` / `ROLLBACK` rules, compared with
  PostgreSQL's REPEATABLE READ on one connection, aborted transactions and
  25P02 included.
- **Transactional DDL** (*Stages of transactional DDL*): stage 0's
  `api_rebind_test.cpp` ("rebind: …"); stages 1, 2 and 4's
  `api_txn_ddl_test.cpp` ("txn DDL: …", the allocation-fault sweep
  included) and `transaction_test.cpp`; stages 3 and 4's wire tests in
  `tests/server/test_wire.py`, the proof machine's
  `test_ddl_inside_begin_agrees_with_postgresql`, and the functional cases
  under `pg:transactional-ddl`.
- **Concurrency:** Elle through SQL (`testing_design.md`, *The TPROC-C
  harness*): the harness's transactions as histories, checked as snapshot isolation, and
  as serializable where `FOR UPDATE` guards the reads that decide a write.

## Known differences from PostgreSQL

Each comes from the rule *never wait*, except where its row gives another
reason.

| PostgreSQL (REPEATABLE READ) | This layer | Why |
|---|---|---|
| A second updater of a row waits for the first transaction, then gets 40001 at its statement if the first committed. | Both update. The second to commit gets 40001 at `commit()`. | No row locks: conflicts are found at commit. |
| A second inserter of a unique key waits, then gets 23505 at its `INSERT`. | Both insert. The second to commit gets 23505 at `commit()` (*Commit and conflicts*). | The same outcome, reported at commit instead of after a wait. |
| `INSERT … ON CONFLICT DO NOTHING` whose arbiter key a concurrent transaction inserts waits, then gets 40001. | Both insert. The second to commit gets 40001 at `commit()`; in autocommit the statement is retried and skips the row. | The same outcome, reported at commit. |
| `INSERT … ON CONFLICT DO UPDATE` whose conflicting row a concurrent transaction updates or deletes waits, then gets 40001. | Both write. The second to commit gets 40001 at `commit()`; in autocommit the statement is retried and updates the row as committed, or inserts where it was deleted. | The same outcome, reported at commit; no retry inside the statement (owner decision, as for `DO NOTHING`). |
| `INSERT … ON CONFLICT DO NOTHING` whose conflicting row a concurrent transaction deletes and commits waits for it, then inserts the row. | The row is skipped, as the statement's snapshot holds its key, and the commit succeeds. | Owner decision (2026-11-19), not the rule *never wait*: a guard per skipped row would make the commit a conflict and an autocommit retry insert the row; it was not taken. |
| `SELECT … FOR UPDATE` locks the rows: other writers wait. | It guards them: the guarding transaction gets 40001 at commit if a writer of those rows committed first. A writer is never held back. | A guard, not a lock. |
| A DDL in a transaction waits for open transactions' locks on its table; readers and writers queue behind an uncommitted DDL's lock. | Nothing waits. The second to commit fails: at commit, or at its next statement for a transaction that ran DDL. | Never wait. |
| Two DDL transactions on unrelated objects both commit, and their other statements run. | Once one commits, the other's next statement, whatever it is, is 40001, and so is its commit. GRANT and CREATE ROLE count as DDL. | One catalog version key. |
| A transaction that began before another session's DDL committed runs its own DDL, on any object, in READ COMMITTED and REPEATABLE READ. | Its DDL is 40001 unless it is the transaction's first statement. | A catalog consistent with the snapshot. |
| T2's CREATE of a name T1 created waits for T1's lock, then is 23505 once T1 commits. | No wait. The second to commit is 40001, at its next statement or at commit; its retry's CREATE is 42P07. | Every conflict between two DDL is 40001; owner's decision 1. |
| DDL in a transaction is bounded only by disk. | Its backfill and writes are bounded by `TransactionOptions::max_bytes` (default 64 MiB), and never past 1 GiB: 54000. Autocommit DDL keeps its 1 GiB bound. | The transaction is held in memory until commit. |
| REPEATABLE READ: catalog lookups read the latest committed catalog, so a later CREATE TABLE is visible and empty, a later ADD COLUMN shows its default on old rows, a later TRUNCATE or rewrite shows 0 rows. READ COMMITTED: each statement takes the latest catalog and a new snapshot. | Every level reads the catalog of its first statement (*A transaction binds only against its own catalog*). A statement whose binding fails at the pin after the catalog moved, such as one naming an object created later, is 40001. A table dropped later is still read; a column added later is not seen; a view or table dropped and created again is read as at the pin. | A catalog consistent with the snapshot; one snapshot per transaction (*Isolation*). |
| A 40001 in a transaction is recoverable: ROLLBACK TO a savepoint, then COMMIT. | A doomed DDL transaction's 40001 is recoverable the same way. A no-DDL transaction's 40001, the rule's exceptions 1 and 2 included, ends the transaction, savepoints or not (CONTRACT.md, the 40001 row). | A no-DDL transaction's 40001 ends it. |
| A statement Parsed in a transaction fixes the latest catalog's result columns, and runs after COMMIT with them. | One Parsed in a transaction whose pin was behind fixes the pin's columns; run after COMMIT in autocommit, it is 0A000 when they differ at the live catalog (exception 3). So is a `TxnStatement`. | A transaction binds only against its own catalog. |
| On a standby, `SELECT … FOR UPDATE` is refused at the statement (25006). | On a follower it runs, and a transaction that used it fails at commit with the engine's refusal of writes, as its writes would; in autocommit it reads as the `SELECT` does. | Its guards are checked by the engine at commit, which a follower refuses. |

## Transactional DDL

Status: **built** (design approved by the owner on 2026-11-19; stages 0 to 4). Every DDL statement, CREATE TABLE AS included, runs in an embedded `Transaction`, in a server BEGIN block and in an extended pipeline's implicit block. Where building a stage found the design incomplete against the code, the stage's entry says what was done instead (*Stage 0 as built* to *Stage 4 as built*). The owner's answers are recorded in *Owner's decisions on transactional DDL*. Stage 0 comes first, and edits *DDL during an open transaction* (stage 0's *Docs*). Once transactional DDL is built, this section replaces *DDL during an open transaction*, and the edits listed under *Edits to this document once transactional DDL is built* are made.

This section applies the two standing rules: do what PostgreSQL does, and never wait. Behaviour observed on PostgreSQL 17.12 is cited as "PG 17".

### Statements that become transactional

Every statement in the `Ddl` variant runs inside a transaction:

- CREATE / DROP TABLE, CREATE / DROP INDEX, ALTER TABLE … ADD COLUMN;
- CREATE / DROP VIEW, CREATE / DROP PROCEDURE, CREATE / DROP ROLE, GRANT / REVOKE;
- CREATE DOMAIN, CREATE TYPE, DROP TYPE, CREATE / DROP SEQUENCE;
- CREATE TABLE AS (stage 4).

Each one becomes part of the transaction:

- **Later statements see it.** A table created earlier in the transaction can be inserted into, indexed, altered and queried. A dropped one is 42P01.
- **ROLLBACK, an abort, or dropping the handle undoes all of it.** Nothing reaches the data file.
- **COMMIT applies it together with the transaction's row writes, or applies nothing.**

These are PostgreSQL's errors, unchanged inside a block:

- a duplicate name is 42P07 and aborts the transaction;
- `IF NOT EXISTS` on a taken name skips it with the notice;
- DDL in a failed block is 25P02;
- DDL in a READ ONLY block is 25006;
- DDL on a follower is 25006 at the statement, as PostgreSQL refuses DDL on a standby. A transaction's DDL touches the engine only at commit, so without this check a follower would run the whole block and fail only at COMMIT. It is one check of the database's mode.

**A DDL that does nothing is not a DDL of the transaction.** An `IF [NOT] EXISTS` that skips, as `LiveCatalog::prepare` reports with no `next`, creates no private catalog, adds no version guard, and does not make the transaction one that has run DDL. In PostgreSQL a skipped DDL changes nothing, so it must not add a 40001 PostgreSQL never gives.

The layer has no `CONCURRENTLY` form, no `CREATE DATABASE`, no RENAME and no TRUNCATE, so PostgreSQL's 25001 refusals inside a block have nothing to apply to. If one of those statements is added later, it is refused inside a block as PostgreSQL refuses it.

**Autocommit DDL does not change.** A DDL outside a block still commits at once, never waits, carries the guards it carries today, and is retried up to `kDdlAttempts` times on a write conflict with a backfill.

### What other sessions see of an uncommitted DDL

Nothing, and they never wait for it. This is PG 17's visibility: another session sees `to_regclass` NULL and gets 42P01 at once for an uncommitted CREATE TABLE. It is not PG 17's blocking. PG 17 makes a SELECT wait behind an uncommitted ALTER or DROP, and an INSERT wait behind an uncommitted CREATE INDEX. Here those statements run against the published catalog and their own snapshot. Whichever side commits second may fail; see *Conflicts between DDL and other writers*.

At commit, the new catalog is published exactly as an autocommit DDL publishes it. Other sessions pick it up the same way: at their next pin, their next prepare of a new text, or the next `fitting_plan` or `refit` of a cached plan (catalog_design.md, *Compatibility*). Nothing is invalidated eagerly.

### A transaction binds only against its own catalog

**The rule** (owner's decision 5). A transaction's catalog is the published catalog it pinned at its first statement, plus, from stage 2, its own DDL. Every statement of the transaction is bound against that catalog, and only that one. Nothing in a transaction is bound against, or run as a plan bound against, any other catalog. The rest of this section refers to it as *the rule*.

- **Where a statement's plan comes from.** A statement runs its shared plan only when that plan's bind version (`plan->catalog_version`, the version in its `ref`) equals the transaction's pinned version, the transaction has run no DDL, and, for a held or Parsed statement (`ResultCheck` **own** or **given**), the plan's parameter types equal the record's: the *exact-version test*. Every other statement is compiled privately against the transaction's catalog, the catalog object the transaction holds, never against `live_->current()`. That plan is a `TxnPlan` in the transaction's plan map, and never enters the shared cache (*Mechanism: plans compiled against the transaction's catalog*). A transaction stores nothing in the shared cache.
- **Shared plans are bound at the live catalog.** From stage 0, autocommit's `fitting_plan` binds a cached plan again when its bind version is behind the live catalog, where today it checks the plan and marks it `checked` (database.cppm:1929-1954). This rebind after any DDL is how decision 4's "a plan binds again when a table it read has other columns" is built: the version comparison is the trigger, and decision 4 states the outcome. A transaction whose pin is live therefore finds a shared plan at its version once an autocommit run of the text has followed the last DDL.
- **The first statement re-pins, as today.** A transaction that starts after a DDL sees it: on its first statement, which has read nothing, a pin behind the live catalog is taken again from the live one (`bc_txn_repin`, plan_api.cpp:3311). The rule alone would let the first statement run at its pin, as PG 17 takes its snapshot when the statement starts. The re-pin of a racing DDL is kept for CONTRACT.md:185's re-pin clause: once the first statement's compile at the pin has ended, whether it gave a plan or failed with a binder error or exception 2, the transaction compares the pin with the live catalog, and on a difference re-pins and compiles again, up to `kTxnPlanAttempts` times (database.cppm:1182), as `Database::run` does today. A DDL that creates a name the first statement uses therefore re-pins it, not 40001. The pin is taken lazily, as the first statement starts, so compiling at the pin and then re-pinning on a difference ends where re-pinning first would: at the live catalog. Past `kTxnPlanAttempts` the statement runs at its last pin, which the rule allows, and an exception there is 40001 as for any later statement; today it is 40001 (`throw_serialization_failure`, database.cppm:2067).
- **Isolation is a catalog snapshot.** A transaction sees exactly its catalog. A table dropped after the pin stays readable: the snapshot holds its rows. A table dropped and created again, a view dropped and created again, a column added, an index created or dropped after the pin are all seen as at the pin.
- **The result shape follows PG 17** (owner's decision 4), with the transaction's catalog as "its catalog":
  - text parsed and run in one step binds against its catalog and returns those columns. That is the embedded `execute(sql)` and `Transaction::execute(sql)`, and the server's Simple Query, which always runs by text, in every mode;
  - a prepared statement whose plan for its catalog has other result columns than the ones it fixed is 0A000 "cached plan must not change result type". That is an embedded `Statement` held across calls, any statement prepared through Parse, named or unnamed, described or not, and any portal. In autocommit its catalog is the live one.
- **Parameter types follow PG 17.** A run by text (`ResultCheck` **none**: `execute(sql)`, `Transaction::execute(sql)`, the server's Simple Query) binds with inferred parameter types, as PG 17's unnamed Parse does, and binds afresh when they differ from the cached `Statement`'s. Only a held or Parsed statement (**own** or **given**) keeps the types it fixed: it binds again with them, and a fixed type that no longer fits gives the binder's error, which item 1 below makes 40001 in a transaction whose pin is behind. Only a change of result columns is 0A000.
- **A cached or shared plan serves a fixed-type check only at the record's types.** For **own** and **given**, a cached plan in autocommit, or a shared plan under the exact-version test in a transaction, is used only when its parameter types equal the record's. On a mismatch the statement is bound at the record's types: in a transaction privately, into the plan map; in autocommit, for **given**, as a one-shot plan that is not entered in the cache, and for **own**, a held `Statement` the cache no longer holds, as a plan swapped into it in place when its result columns are unchanged (stage 0). A held `Statement` the cache still holds always has the cache's types. The result is that plan, the binder's error, or 40001 at a stale pin (item 1).
- **The intended exceptions.** The rule's errors are these:
  1. **40001, "the catalog changed since the transaction's first statement", for a binder error at a stale pin.** In a transaction that has run no DDL, when binding a statement at the pin fails with any binder error and the live catalog is past the pin, the error is 40001. One rule covers every case where the pinned catalog lacks what the statement needs: a name created after the pin (42P01, 42704, 42883, `Transaction::procedure(name)` included), a column added after it (42703, already 40001 today), an `ON CONFLICT (cols)` whose unique index was created after it (42P10), an INSERT of the new arity after an ADD COLUMN (42601), a type mismatch after a table was dropped and created again. Today such a statement is bound at the live catalog and runs when that plan passes the runtime's check against the pin. When the pin is live, binder errors are reported as they are: a typo is 42P01. A transaction that has run DDL is already doomed once the live catalog moves (*Catalog snapshots and isolation levels with DDL*), so its own DDL's effects reach no 40001 here: `DROP PROCEDURE p; CALL p` is 42883, as in PG 17.
  2. **40001 for a statement whose record's version is later than the pin** and whose result columns differ at the pin: they were fixed at a catalog the transaction does not see. The record's version is the version of the plan that last fixed it, so an autocommit rebind that swaps a held `Statement`'s plan in place after the pin raises it too (`ResultCheck`, **own**). A record fixed after a transaction's own DDL counts as later than its base while the live catalog is past that base (*Savepoints, cursors, portals and prepared statements across DDL*).
  3. **0A000 by owner's decision 4**: a prepared statement or portal whose result columns were fixed at its catalog or an earlier one, not under item 2, and whose plan for its catalog has other result columns. In autocommit every such change is 0A000. This includes a record fixed inside a transaction and run after COMMIT in autocommit: a server Parse in a block, or stage 2's `TxnStatement`, fixed at a pin behind the live catalog is 0A000 when its result columns differ at the live catalog, since its catalog moved forward under it. PG 17 reads the latest catalog at Parse, so it fixes the live columns and gives no error.
  4. **54000** when the transaction's private plans take it past `max_bytes`.

  A retry of items 1 and 2 runs the statement against the new catalog and reports its real result there. These are errors where today the statement may run, never wrong results. A 40001 here ends a transaction that has run no DDL, savepoints or not, as today (CONTRACT.md:185). Stage 0 names CONTRACT.md:185 as narrowed by items 1 and 2.

  One more error is not an exception of the rule: a held or Parsed statement keeps the parameter types it fixed, so a fixed type that no longer fits is the binder's error (40001 at a stale pin, item 1), where today the statement binds again with inferred types and runs. This matches PG 17 for a type PG 17 refuses too and for integers of another width; a conversion PG 17 makes and the layer does not (an integer to a float, a DECIMAL or a string) is refused (42601, *Stage 0 as built*), and narrows today: stage 0 names CONTRACT.md:171, :256 and the server row as narrowed by it.

### Catalog snapshots and isolation levels with DDL

The rule gives each transaction one catalog, consistent with its snapshot. PostgreSQL does not work this way. It reads the catalog as last committed, whatever the snapshot. In REPEATABLE READ this shows rows that never existed together (PG 17):

- a table created after the snapshot is visible, and empty;
- a column added after the snapshot shows its default on old rows;
- after another session's committed TRUNCATE or table rewrite, the reader counts 0 rows that its snapshot held.

In READ COMMITTED, PostgreSQL takes a new snapshot and the latest catalog for each statement, so it has no such anomaly. The layer runs every level as one snapshot per transaction (*Isolation*). With its own DDL (stage 2):

- **"Has run DDL" means `txn->catalog != txn->pinned`**, evaluated after any ROLLBACK TO has restored a mark's catalog. A transaction that rolled back to a mark taken before its first DDL has run no DDL again.
- **A transaction that has run DDL is doomed once another DDL commits**, on any object: its commit can only conflict (*Mechanism: committing and publishing a transaction's catalog changes*). Its next statement, whatever it is, even `SELECT 1`, is therefore 40001, "the catalog changed since the transaction's first statement". The check is one comparison of the live catalog with the transaction's base. It is a failed statement: it aborts the transaction, and ROLLBACK TO a savepoint reopens it. ROLLBACK and ROLLBACK TO are exempt from the check. A ROLLBACK TO a mark taken before the first DDL lifts the doom, since the transaction then has nothing to conflict; one taken after it leaves the transaction doomed, and its next statement is 40001 again.
- **The first DDL of a transaction whose pin is already behind** is 40001 by the same check, even when the DDL needs nothing from the later catalog. PG 17 runs it in both READ COMMITTED and REPEATABLE READ. If the DDL is the transaction's first statement, the transaction re-pins and runs it.

When a snapshot per statement for READ COMMITTED is built (*Later*), it must also take a new catalog per statement. A transaction that has run DDL then keeps its own catalog, as PostgreSQL's does.

### Conflicts between DDL and other writers

PostgreSQL orders DDL against everything else with locks. Most DDL takes AccessExclusiveLock, CREATE INDEX ShareLock, and holds it to the end of its transaction, so writers and readers wait (PG 17). CREATE VIEW takes only AccessShareLock on the tables it reads, and blocks no writer. The layer never waits. A conflict is found at commit, or at the next statement as above, and **the second to commit fails**. Nothing is ever half applied: every interleaving below ends with one side committed and the other with nothing written.

| Interleaving | PG 17 | This layer |
|---|---|---|
| T1 (in a block) runs DDL on table t; T2 writes t and commits first | CREATE INDEX, ADD COLUMN and DROP make T2 wait for T1's lock. CREATE VIEW does not: T2's INSERT returns at once. | T1's commit is 40001 if its DDL depends on t's rows. CREATE INDEX and ADD COLUMN … DEFAULT guard the rows they backfilled. DROP TABLE and DROP INDEX range-delete t's rows or the index's entries; the engine checks a range delete in a plan with a snapshot against every write to that range since the snapshot, as it checks a range guard. ADD COLUMN with no default and CREATE VIEW do not depend on the rows, so T1 commits; for CREATE VIEW this is PG 17's outcome. |
| T1 commits DDL on t; T2 (in a transaction) wrote t from an older pin | T2 waited earlier, then saw the new catalog | T2's commit is 40001 through the catalog-record guard (unchanged, CONTRACT.md:188). An autocommit T2 is retried, recompiles against the new catalog and succeeds. |
| T1 commits DDL on t; T2 only read t | T1 waited for T2's AccessShareLock | T2 commits. Its snapshot and pinned catalog stay consistent. Its FOR UPDATE rows that T1 rewrote make T2 40001 (unchanged). |
| T1 and T2 both create table `x` in blocks | T2 waits; on T1's commit T2 gets 23505 (`pg_type_typname_nsp_index`); on T1's rollback T2 succeeds | The first to commit wins. The other gets 40001, at its next statement once T1 has committed, or at commit. Its retry's CREATE TABLE then gets 42P07. |
| T1 (block) creates `x`; T2 runs autocommit `CREATE TABLE x` | T2 waits, then 23505 or succeeds | T2 commits at once, since it cannot see T1's table. T1 then gets 40001, and its retry 42P07. |
| T1 (block) runs any DDL, GRANT and CREATE ROLE included; T2 runs any autocommit DDL or a DDL transaction | T2 waits only if they touch one object | Any two DDL commits conflict: each guards the catalog version. The second is 40001 at its next statement, whatever it is, or at commit. |
| An autocommit DDL is in flight at the moment T1 commits | — | T1's commit finds `ddl_mu_` held: 40001 at once (`DdlInProgress`). |

**Any two DDL transactions conflict, even on unrelated objects.** This is coarser than PostgreSQL, which serialises only DDL that touches one object. It is the cost of the single catalog version key, which gives the layer its cheap pin: `pin()` relies on that key moving with every DDL. Per-object catalog conflicts are a follow-up issue (*Follow-up issues*), opened if a workload needs concurrent DDL.

### Savepoints, cursors, portals and prepared statements across DDL

**Savepoints.** ROLLBACK TO undoes the DDL since the savepoint, as PG 17 does (CREATE TABLE, CREATE INDEX and ADD COLUMN after a savepoint are all undone, and rows inserted before it are kept). RELEASE keeps the DDL. A DDL that fails aborts the transaction as any statement does, and ROLLBACK TO reopens it. An abort with savepoints goes back to the newest mark's catalog.

DROP TABLE is RESTRICT: a table a view reads is 2BP01, and there is no CASCADE. A transaction that runs `DROP VIEW v; DROP TABLE t` and rolls back gets both back.

**Cursors and portals.** PostgreSQL refuses a DDL on a relation that an active query of the session has open: 55006. A cursor counts, even after FETCH ALL, and so does a bound SELECT portal, since PortalStart opens its relations; an INSERT, UPDATE or DELETE portal does not, since PostgreSQL opens its relations only while it runs. The layer does the same:

- A DROP TABLE, CREATE INDEX or ALTER TABLE on a table that an open cursor or portal of the transaction reads is 55006, and aborts the transaction. This uses the objects the plan read. A DROP TABLE of a table a view depends on is 2BP01 instead, as in PG 17, which checks dependencies before `CheckTableNotInUse`; ALTER TABLE and CREATE INDEX check use first, so a duplicate column or index name with a cursor open is 55006.
- A DROP INDEX is 55006 only when an open cursor's or portal's plan scans that index. A plan that reads the table without the index does not count, as in PG 17, where `index_drop` checks only the index's own references. The plan's schema ref already lists the indexes it scans.
- CREATE VIEW, DROP VIEW (of a view a cursor reads too) and any DDL that touches no open plan's table run, as in PG 17.
- The server's open SELECT portals in the block, named or unnamed, count with the transaction's cursors.
- A WITH HOLD cursor that outlived its own transaction is not counted. It reads its frozen view, as PostgreSQL's materialised one does.
- ROLLBACK TO keeps PostgreSQL's cursor rule (#727): cursors declared since the savepoint close, whatever DDL they followed.

The messages name the object as PG 17 does, each ending "because it is being used by active queries in this session":

- `cannot DROP TABLE "t"`, `cannot CREATE INDEX "t"` and `cannot ALTER TABLE "t"` name the table;
- `cannot DROP INDEX "i"` names the index.

**Prepared statements** follow the rule. After the transaction's own DDL, a statement is compiled against the transaction's catalog: it sees the transaction's tables, columns and indexes, and its writes maintain the indexes created in the transaction. A statement compiled before that DDL is compiled again at its next run. A private compile never changes the plan other sessions use. Outside the transaction, a statement fits the live catalog as in autocommit. A named statement prepared inside a transaction that was then rolled back, naming a table the rollback removed, is 42P01 at its next Bind, Describe or Execute, as PostgreSQL's revalidation reports it.

The result shape needs nothing more. `SELECT *` run by text after an in-transaction ADD COLUMN on its table returns the new column; a held or Parsed statement whose columns change is 0A000 and aborts the transaction. A plan compiled after the transaction's own DDL carries the transaction's base version, and a record fixed from it is marked as fixed at a private catalog. Its columns may come from a catalog later than the base: the one its transaction published at COMMIT. So in another transaction pinned at that base, while the live catalog is past it, a record so marked whose columns differ at the pin is 40001 (exception 2); the retry pins at the live catalog and runs, or meets exception 3. Everywhere else a change is 0A000: in its own transaction while it has run DDL, after a ROLLBACK at a live base (the record's columns came from no published catalog), and in any transaction pinned past the base. In its own transaction after a ROLLBACK TO a mark taken before its first DDL, it has run no DDL again, so exception 2 applies: 40001 when the live catalog is past the base, 0A000 otherwise. A 40001 here is repeated at most once, since its retry's pin is past the base.

### Crash atomicity of a transaction with DDL

A transaction's commit is already one `WritePlan` and one `apply_batch` (*Commit and conflicts*). A transaction with DDL puts its catalog records, its counters, its name entries, its range deletes and its backfill into that same plan, beside its row writes. The engine applies a plan whole or not at all, at run time and at recovery (engine CONTRACT.md:138, :249). After a crash, the catalog and the rows are therefore both committed or both absent. A recovered database never holds a table record without its index entries, or rows of a table whose record is missing.

The overlay emits its range deletes before its points, and every point is newer than any range over it, so a drop and re-create of one name in a transaction lands as the re-created table. The counters merge to the last DDL's values.

Such a commit is always `sync = true`, as autocommit DDL is: a DDL is durable before it is visible. A commit that may have reached the engine and then failed is 08007 (`TransactionResolutionUnknown`), as today.

No engine code changes. There is no new durability site: no new fdatasync, rename or unlink, and no new ordering, because the commit goes through the existing `LiveCatalog::commit`. Nothing is stored outside the data file. The private catalog lives only in the transaction's memory, and the overlay holds its keys.

### Mechanism: the transaction's private catalog

- **`bc_txn` gains a `catalog`** (`shared_ptr<const Catalog>`):
  - It equals `pinned` until the transaction's first DDL that changes something.
  - `bc_ctx_begin_txn` sets `cx->cat` from `catalog`, not from `pinned`. Every check the runtime makes (`verify`, layouts, index maintenance) then reads the transaction's catalog.
  - `pinned` stays the base: the published catalog the snapshot holds.
- **A DDL in a transaction is a step of the transaction, not a plan.** `Transaction` binds it against `txn->catalog`, then calls `Catalog::next_version` on that catalog, reading through the transaction's view (below). This yields the next catalog and its `DdlOp`s, exactly as autocommit gets them. A new runtime call, `bc_txn_ddl(txn, next, ops)`, then:
  1. sets `txn->aborted = true`, as `bc_txn_add` does before merging a statement, so a step that fails part way can never be committed;
  2. moves the `Put`, `Del` and `DelRange` ops into the overlay through `own_overlay()`, so they share the savepoint and cursor copy-on-write. Each op's bytes are freed once the overlay holds its copy, as `LiveCatalog::prepare` frees them into the plan, so a backfill is in memory about once, not twice;
  3. appends each `EnsureRangeUnchanged` (the backfill's `guard_rows`) to the transaction's guards, and drops the `EnsureUnchanged` on the version key and the `EnsureAbsent` guards on name, procedure, role and type entries (below);
  4. charges the overlay growth, the guards and the new catalog's estimate to `max_bytes`;
  5. last, with nothrow moves, swaps in `txn->catalog = next` and its charge, clears the private plan map, and clears `aborted`.

  A failure at any step is a failed statement: the transaction stays aborted, and ROLLBACK TO restores the mark's overlay, catalog and charge. The allocation-fault sweep of stage 2 asserts each step.
- **The backfill reads through the transaction's view.** `next_version` reads rows today from a `Snapshot`. It takes a `RowSource` instead: an ordered scan of one key prefix. A `Snapshot` provides it for autocommit DDL; the transaction's two-pointer merge of snapshot and overlay provides it in a transaction. A CREATE INDEX after an INSERT in the same transaction therefore indexes the inserted row (PG 17 counts it). An ADD COLUMN … DEFAULT rewrites the rows the transaction wrote earlier too.
- **The bound on a backfill in a transaction** is `max_bytes`, and never more than `BackfillBudget`'s 1 GiB, the engine's batch limit. Past either, the statement is 54000, and its message names running the DDL outside a transaction. A `max_bytes` above 1 GiB therefore never lets a backfill reach the engine's refusal at commit.
- **Catalog-key guards inside a transaction.** `next_version` emits one `EnsureUnchanged`, on the version key (`guard_catalog`), and `EnsureAbsent` on each name, procedure, role and type entry it creates. Autocommit keeps both. Inside a transaction, `bc_txn_ddl` drops them, and the commit adds one version guard instead:
  - The `EnsureAbsent` on a name entry would be wrong: it is checked against the head at commit, so a transaction that drops `t` and creates `t` again would always fail it.
  - It is also not needed. The commit's plan writes the name entry, and the engine checks every key a plan with a snapshot writes against writes since the snapshot. Any other DDL that took the name since the snapshot also moved the version key, which the commit guards.
  - **Invariant:** every write to catalog namespace `01` goes through `next_version`, and every `next_version` that changes something bumps the version key. Stage 2 states it in catalog_design.md, *DDL*, and `bc_txn_ddl` asserts that the ops it is given carry the version `EnsureUnchanged` and a `Put` of the version key before it drops the guards. Exceptions: the test seam `conflict_on_every_commit_for_testing` rewrites table records unchanged with `db_->put`; it creates no name and is test-only. A future sequence counter (`nextval`, not built) would write namespace `01` without bumping the version; when it is built, its writes go around the transaction's overlay (below), and the invariant names it.
- **Object ids.** The transaction allocates ids from its private `next_id`. Two open transactions may hand out the same id. Only one of them can commit: both write the id counter, so the second conflicts. An id never published never reached the data file, so "ids are never reused" still holds of every id the file holds.
- **Hidden row ids.** `LiveCatalog::row_ids_` may be seeded for a private table's id. Its seed reads the engine, which holds no rows under an unpublished id, so it starts at 1. Two transactions holding the same private id draw from one counter, which only rises, so they never get the same row id. The entry stays after a rollback. Erasing it would be wrong: another open transaction may hold the same id, and a reseed would hand out its row ids again. The entries do not grow without bound: a rolled-back id is handed out again by the next transaction, since the published `next_id` did not move.
- **The private catalog's cost is charged.** A `Catalog` clone is a deep copy of its maps, so each DDL costs O(catalog). Every distinct catalog the transaction holds that is not `pinned` (its current one and those its savepoint marks keep) is charged to `max_bytes` by `Catalog::estimated_bytes()`: a named constant per table, per column, per index and per other object, plus the bytes of the names. A transaction of many DDL statements with a savepoint before each fails with 54000 rather than growing unbounded.
- **Sequences, when built.** CREATE SEQUENCE is transactional in PG 17, but `nextval` is not: a value drawn in a rolled-back transaction stays consumed. When `nextval` is built, its counter writes bypass the transaction's overlay and private catalog, except for a sequence created in the same transaction.

### Mechanism: plans compiled against the transaction's catalog

Stage 0 builds all of this for the pinned catalog; stage 2 points it at `txn->catalog`.

- **A transaction compiles from text.** `Transaction` turns a text into the plan for its catalog in one place. It takes the parse from the shared cache when the text is cached, since a parse depends on no catalog, and parses it otherwise. It then takes the plan where the rule says (*A transaction binds only against its own catalog*, "Where a statement's plan comes from"): the shared plan when it passes the exact-version test, otherwise a compile at the transaction's catalog, kept in the plan map. A held or Parsed statement is compiled with the parameter types it fixed. Every bind on this path is against the transaction's catalog; nothing calls `prepare_locked`'s bind, which binds at `live_->current()` (`Database::build`, database.cppm:1558-1559). Today `Transaction::execute(sql)` (database.cppm:2853) and a cursor's keyed twin (:2552) bind at the live catalog through `prepare_locked`, and `Database::run` runs that plan when the runtime's check against the pin passes (:2032-2107); the "recompiled, pinned again, or 40001" loop there becomes the rule's choice of plan and the first statement's re-pin.
- **DDL is classified from the parse.** In stage 0, a DDL text given to a transaction is not bound: it is refused at execute with 0A000, as today (`run_locked`, database.cppm:2477). A DDL Parsed in a block and never executed does not abort it. A CREATE TABLE AS in a transaction is 0A000 at execute, whatever its query names.
- **`TxnPlan`.** `Database::build_private` compiles a statement against a given catalog and returns a `TxnPlan`: it owns its `Compiled`, the catalog it was built for and the statement's parse, gives the runner only `const Compiled&`, and never hands out a `shared_ptr<const Compiled>`. A run holds the `shared_ptr<const TxnPlan>` it took from the map until the statement ends, so a clear of the map during the run, such as the first statement's re-pin, leaves no reference dangling. Its `Compiled` is a private member that no accessor returns by pointer, so no code outside `TxnPlan` can build an aliasing `shared_ptr<const Compiled>` from one. `Compiled` is move-only, and `Statement::set_compiled` takes `shared_ptr<const Compiled>`, so putting a private plan into the shared cache does not compile (root CLAUDE.md, *Safety through abstraction*). A `TxnPlan`'s version is the version of the published catalog it was compiled at: the pin's, also after the transaction's own DDL. A private procedure plan is a `TxnPlan` too: it owns a `CompiledProcedure` in place of a `Compiled`, whose statement plans stay inside it as they do in `procedures_` today, and it carries the same catalog pointer, the same check before use and its own charge.
- **The plan map.** `TxnHandle` holds a map from a cache key to `shared_ptr<const TxnPlan>`. The key is the statement's text, `"\x01"`+text for a cursor's keyed twin (the prefix the shared cache already uses), or `"\x02"`+name for a procedure. No SQL text starts with either byte. Keying by text, not by `const Statement*`, means a freed `Statement`'s address can never match a stale plan, also once #549 evicts. A second run of a text compiles nothing.
  - **Its charge.** Each `TxnPlan` in the map is charged to `max_bytes` by an estimate (a named constant plus its code's bytes), released when the map drops it. The charge is its own term in `txn_holds` (plan_api.cpp:1766), not part of `held`, so a savepoint mark neither records nor restores it (`txn_restore`, :1807). Past `max_bytes` the statement is 54000.
  - **When an entry is used.** Each `TxnPlan` owns the catalog it was built for. A map entry is used only when that catalog is the transaction's current one (`plan->catalog == txn->catalog`, by pointer) and its parameter types are the ones the run fixed; otherwise the text is compiled again and the entry replaced. This one check keeps every path correct that changes the catalog: a re-pin, and from stage 2 a DDL, a ROLLBACK TO, and an abort with marks, which restores the newest mark's catalog (`txn_abort` calls `txn_restore`, plan_api.cpp:1824-1830). A ROLLBACK TO in a transaction with no DDL keeps the catalog, so it keeps the entries in use.
  - **When it is cleared.** Clearing only frees memory. The handle clears the map when it sees the transaction's catalog changed since the map was filled, at an abort without marks, which drops the pin (`txn_abort`, :1824-1840), and at the end of the transaction. `txn_abort` zeroes the term beside `held`, and the handle frees the plans when it next sees the pin gone; at the other clears, the handle's clear releases the term.
- **Procedures.** `Transaction::procedure(name)` returns the definition in the transaction's catalog. When the name is absent there, it is a binder error under exception 1: 40001 when the transaction has run no DDL and the live catalog is past the pin, null for the caller's 42883 otherwise. A transaction with no pin pins first. `Transaction::call` runs that definition. It uses `procedures_`' entry only when the entry's `catalog_version` equals the pinned version and the transaction has run no DDL, the test `procedure_plan` (database.cppm:2226) already makes against the live catalog; otherwise it compiles the procedure privately at the transaction's catalog into the plan map under `"\x02"`+name, and never stores into `procedures_`. Autocommit's `procedure_plan` is unchanged. Today `Transaction::call` looks the procedure up in the live catalog and compiles it there (:2822-2831).
- **Cursors hold either plan.** `CursorState::compiled` becomes a plan reference that holds either the shared `Compiled` or a `shared_ptr<const TxnPlan>`, so a cursor keeps its private plan, and that plan's catalog, alive after the map drops it or the transaction ends. A cursor that holds a `TxnPlan` adds the plan's estimate to its overlay pin's `reserved`, so to `pinned_bytes`. A WITH HOLD cursor's hold moves the estimate into `held_share` with the overlay's share, so `pinned_by_holds` carries it, and `bc_txn_overlay_release` subtracts both. The estimate therefore leaves `pinned_bytes` and `pinned_by_holds` together, and `cap_holds` (plan_api.cpp:1770) cannot underflow when the map drops the plan. While the map also holds the plan, it is counted twice, which errs only toward 54000.
  - A cursor's keyed twin follows the rule under `"\x01"`+text. Today its lookup runs inside a catch-all that falls back to the plain plan (database.cppm:2551-2554). From stage 0 only the planner's "not plannable with the key" falls back; any other failure, such as a 54000, fails the open as a statement's failure does.
  - A positioned statement's key table is checked against the cursor's on the plan that runs, not on the shared plan (`*stmt.compiled()` in `with_cursor_key`, database.cppm:2458-2476). `execute_window`'s check that the statement returns rows reads the plan that runs too.
  - The overlay pin a cursor takes at DECLARE records the transaction's catalog of that moment, and `bc_txn_hold` hands that catalog to the hold, not `txn->pinned`. A WITH HOLD cursor over a table the transaction created (stage 2) reads it after COMMIT with the catalog that holds the table.
- **Result columns.** Every run and describe of a statement takes a `ResultCheck`. `Database::execute` and `Database::refit`, and `Transaction::execute`, `execute_window`, `open_cursor` and `execute_current_of`, gain a form that takes one; `Transaction::refit` (below) takes one. It has three forms:
  - **own** (today's forms): the `Statement`'s fixed parameter types and result columns, and the version of the plan that fixed them. An embedded `Statement` held across calls. The three are one record, read and replaced as a whole under the cache's lock, so a check never pairs one plan's columns with another's version. A rebind that swaps a plan in place, its result columns and parameter types unchanged, replaces the record with one carrying the new plan's bind version: the columns are confirmed there too, and a version only rises to a published one.
  - **given**: parameter types, columns, a version and the private mark (*Savepoints, cursors, portals and prepared statements across DDL*) the caller fixed. The server's record from Parse, carried by its portals, and stage 2's `TxnStatement`.
  - **none**: by text. The parameter types are inferred, the plan's columns are returned and nothing is compared. `execute(sql)` and `execute(sql, params)` (which runs `execute(prepare(sql), …)`, database.cppm:1672-1674), `Transaction::execute(sql)`, the server's Simple Query, a Simple Query's WHERE CURRENT OF, and every DECLARE, which returns no rows, so it has no result type to keep.

  The fixed parameter types are inputs to the bind; a type that no longer fits gives the binder's error. When the plan's result columns differ from the check's, the check's version and private mark decide: later than the transaction's pin, 40001 (exception 2); otherwise 0A000 (exception 3). In autocommit no version is later than the live catalog, so it is 0A000.
- **`Transaction::refit(sql or stmt, check)`** gives `params()`, `columns()`, `command()`, the version of the plan the transaction will run and its private mark, under the check. Before the transaction's first statement it pins first, as that statement would.
- **`Transaction::prepare(sql)` (stage 2)** returns a `TxnStatement`, a value that holds the text and its record (result columns, parameter types, version and private mark) from `refit(sql, none)`, as the server's named statement does. It holds no plan and no `Statement`. A parse the transaction makes for a text not in the shared cache stays with the transaction, so `Transaction::prepare` puts nothing in the shared cache. A run passes the text and the record as `ResultCheck::given` to the `execute` forms that take a check, so the text resolves by the rule wherever it runs. A `TxnStatement` outlives its transaction: run in another transaction or in autocommit, it resolves its text there. A text naming a table the transaction created and did not commit is therefore 42P01 there, as it is for another session's `prepare` or `execute` of the text.
- **Published and private catalogs are distinct types (stage 2).** A transaction's catalog is a `TxnCatalog`: the published catalog it pinned and its own. A version number identifies only a published catalog: two open transactions can both hold a private base+1 with different contents. A `Published` is made only from a `TxnCatalog` whose catalog is still its pin, and the transaction's sites keyed by version (the rule's exact-version test, the shared procedure map) take a `Published` only. Autocommit's (`fitting_plan`'s comparison with the live catalog, `procedure_plan`) read the live catalog, which is published. The shared `Compiled` is built only there; a transaction's private compile returns a `TxnPlan` (*Stage 2 as built*). Under the rule a transaction that has run DDL uses no shared plan, so no version-keyed site ever sees a private catalog.

### Mechanism: committing and publishing a transaction's catalog changes

`bc_txn_commit` builds the plan as today: range deletes, then points, then the guards that still count. A transaction with DDL differs in its guards:

- it carries an `ensure_unchanged` on the catalog version key, which it also writes with its new value. Any DDL committed since the snapshot is then a conflict; one that committed after the pin and before this transaction's DDL was already caught by the doomed check;
- it carries its backfills' row guards;
- it carries no catalog-record guards. The version guard subsumes them: by the invariant above, any record changed since the snapshot moved the version.

A transaction with no DDL commits exactly as today.

**Publication reuses `LiveCatalog::commit`.** `commit(Prepared)` already try-locks `ddl_mu_` and throws `DdlInProgress` (40001), returns false if `current() != base`, runs the odd-`gen_` window around `apply_batch_or_unknown({.sync = true})`, and stores `current_` on success. The transaction builds a `Prepared{base = pinned, next = catalog, plan, backfill}` from its plan and calls it. `pin()`'s invariant therefore still holds: the version a snapshot holds equals the published catalog's, except inside that window. Nothing is factored out, and the durability path is the autocommit one.

`commit` compares catalogs by pointer, and the doomed check compares versions. `pin()`'s reload path can return a fresh catalog after losing its `compare_exchange` (catalog.cppm:3170-3175), so a transaction pinned on it gets a 40001 at publish that the doomed check did not predict. It is safe, and happens only with two `LiveCatalog`s on one database, which only tests open.

The published catalog's version is the private catalog's: the base version plus one per DDL in the transaction. It is above every version published before, because the version guard proved that none was published since the base. Versions are not contiguous, and nothing needs them to be. Outside tests, the only arithmetic on a version is `next_version`'s `version_ + 1`; every other use compares for equality or keys a cache.

**Classifying a conflict.** `conflict_or_duplicate` cannot see which guard failed: it reports 23505 when any claim's key is now taken. With private object ids, a row key under a private table id can equal a committed table's row key, so for a transaction with DDL the commit classifies first when `apply_batch` reports a conflict:

1. Read the version key at the head. If it has not moved, run `conflict_or_duplicate` as today: no DDL was published since, so no row key under a private id can be taken, and a taken key is a true duplicate (23505) as for any transaction.
2. If it has moved, the result is 40001, whatever key is taken. A DDL published since made the commit fail on the version guard anyway, and a taken key may be a private id's collision, not a duplicate. Two CREATEs of one name land here: the loser gets 40001, not PG 17's 23505 (*Owner's decisions on transactional DDL*).

A false return from `commit` means `current() != base`, a DDL published since: it is 40001 without reading the version key.

The doomed check at the next statement is step 2's 40001.

### Mechanism: savepoint marks carry the catalog

`TxnMark` gains `shared_ptr<const Catalog> catalog` and the catalog charge that goes with it:

- `bc_txn_savepoint` records the transaction's current catalog and its charge. That costs nothing until a DDL replaces it.
- `bc_txn_rollback_to` restores them with the overlay. The catalog keys, rows and index entries the DDL wrote are in the overlay, so restoring the overlay undoes them, and restoring the catalog undoes the in-memory form. The plan map's entries for another catalog are no longer used (*When an entry is used*). "Has run DDL" is then evaluated afresh.
- `bc_txn_release` drops them with the mark.
- An abort with marks goes back to the newest mark's catalog, through the same restore, so the plan map's rule covers it too. An abort without marks drops the private catalog, its charge and the plan map with everything else.

The cursor rule (#727) and the read-only-mode rule are unchanged.

### Server routing of Parse, Bind, Describe and Query

From stage 0, inside a block every message goes through the block's transaction, and so binds only against its catalog. A block is the explicit one (`txn_`) or, once its first Execute has begun it, the implicit one of an extended pipeline (`implicit_`, pg_session.cppm:1723). Parse, Bind and Describe that come before that first Execute are outside a block.

| Message | Today | From stage 0 |
|---|---|---|
| Simple Query | `db_.prepare`, then a held run (pg_session.cppm:1446-1455) | by text, `ResultCheck::none`: through the block's transaction in a block, `db_.execute(sql)` outside one |
| Parse, DECLARE | `db_.prepare` + `db_.refit` (:1265, :1524-1531) | the record (parameter types, columns, version) from the block's `refit(text, none)` in a block, from `db_.refit` outside one |
| Bind, Describe 'S', Describe 'P', Execute | `db_.refit` against the live catalog (:1557, :1622), `p.stmt->columns()` (:1626), `txn_->execute` (:1705) | the block's `refit` and `execute` in a block, `db_`'s outside one, by the statement's text with its record as `ResultCheck::given` |
| CALL | `db_.catalog()->procedure`, then `txn_->call` (:1762-1771) | the block's `procedure(name)`, arguments bound to it, and its `call` |

- **A named statement holds its text and its record**, not a `Statement*`, so nothing it holds dies with the block. Each use resolves the text where it runs: through the open block, or through `db_.prepare` in autocommit, which hits the cache.
- RowDescription for Describe 'S' and 'P' comes from the record. ParameterDescription and Bind take the parameter count and types from the record too, and each rebind binds with them, as PG 17 does.
- A Parse in a block binds against the block's catalog. With the pin behind the live catalog, any binder error at Parse is 40001 (exception 1); with the pin live, a typo is 42P01 at Parse, as today.
- A record fixed in a block outlives it. Run after COMMIT in autocommit, it is 0A000 when its result columns differ at the live catalog (exception 3).
- Stage 3 changes no routing: stage 2 makes the transaction's `refit`, `execute` and `procedure` read `txn->catalog`.

From stage 4, a DDL in an implicit block, by an Execute before one Sync or by a Simple Query sent inside that pipeline, runs in the block's transaction as in an explicit one. A Simple Query of several statements is still refused (0A000), so it forms no block.

### Differences from PostgreSQL for transactional DDL

Stage 0 adds the catalog-snapshot row (the fourth), the Parse-in-a-transaction row, and the part of the fifth row about the rule's exceptions 1 and 2 to *Known differences from PostgreSQL*, since stage 0 ships the rule. The other rows join it once transactional DDL is built, and the row "DDL waits for the locks of open transactions on its table" is replaced.

| PG 17 | This layer | Why |
|---|---|---|
| A DDL waits for open transactions' locks on its table; readers and writers queue behind an uncommitted DDL's lock. | Nothing waits. The second to commit fails: at commit, or at its next statement for a transaction that ran DDL. | Never wait. |
| Two DDL transactions on unrelated objects both commit, and their other statements run. | Once one commits, the other's next statement, whatever it is, is 40001, and so is its commit. GRANT and CREATE ROLE count as DDL. | One catalog version key. |
| A transaction that began before another session's DDL committed runs its own DDL, on any object, in READ COMMITTED and REPEATABLE READ. | Its DDL is 40001 unless it is the transaction's first statement. | A catalog consistent with the snapshot. |
| REPEATABLE READ: catalog lookups read the latest committed catalog, so a later CREATE TABLE is visible and empty, a later ADD COLUMN shows its default on old rows, a later TRUNCATE or rewrite shows 0 rows. READ COMMITTED: each statement takes the latest catalog and a new snapshot. | Every level reads the catalog of its first statement, plus its own DDL (*A transaction binds only against its own catalog*). A statement whose binding fails at the pin after the catalog moved, such as one naming an object created later, is 40001. A table dropped later is still read; a column added later is not seen; a view or table dropped and created again is read as at the pin. | A catalog consistent with the snapshot; one snapshot per transaction (*Isolation*). |
| A 40001 in a transaction is recoverable: ROLLBACK TO a savepoint, then COMMIT. | A doomed DDL transaction's 40001 is recoverable the same way. A no-DDL transaction's 40001 ends the transaction, savepoints or not (CONTRACT.md:185), including the rule's exceptions 1 and 2. | The no-DDL rule is CONTRACT.md:185. |
| A statement Parsed in a transaction fixes the latest catalog's result columns, and runs after COMMIT with them. | One Parsed in a transaction whose pin was behind fixes the pin's columns; run after COMMIT in autocommit, it is 0A000 when they differ at the live catalog (exception 3). So is stage 2's `TxnStatement`. | A transaction binds only against its own catalog. |
| T2's CREATE of a name T1 created waits for T1's lock, then is 23505 once T1 commits. | No wait. The second to commit is 40001, at its next statement or at commit; its retry's CREATE is 42P07. | Every conflict between two DDL is 40001; owner's decision 1. |
| DDL in a transaction is bounded only by disk. | Its backfill and writes are bounded by `TransactionOptions::max_bytes` (default 64 MiB), and never past 1 GiB: 54000. Autocommit DDL keeps its 1 GiB bound. | The transaction is held in memory until commit. |

CREATE VIEW's conflict outcome matches PG 17 and needs no row. So does a statement's result type, in autocommit and across the transaction's own DDL, and a held or Parsed statement's fixed parameter types.

Privileges are not checked today (README.md). When #653 checks them, the rule applies: a transaction binds with the privileges of its catalog, so one pinned before a REVOKE keeps the privilege, where PG 17 checks the latest catalog. #653 adds that row to *Known differences from PostgreSQL*.

### Stages of transactional DDL

Each stage is one PR that ships on its own. Each says what it still refuses, updates `bytecaskdb-sql-layer/README.md`, and names its contract changes. None changes the engine, so `engine_bench` does not apply. No performance claim is made. Stage 0's and stage 2's PRs show `sql_layer_bench` before and after: stage 0 adds a recompile per cached text after every DDL, and the exact-version test and private compiles in transactions; stage 2 takes `cx->cat` from `txn->catalog` on every transaction. The numbers show what statements without DDL pay.

**Stage 0: plans rebind on a column change, and a transaction binds only against its own catalog.**
- **Autocommit: a cached plan behind the live catalog is bound again.** `fitting_plan` compares the plan's bind version with the live catalog's version, and binds a plan that is behind again at the live catalog, inferring its parameter types, as the first prepare did and as today's recompile does (`build(stmt)`, database.cppm:1608-1612). Today it checks such a plan against the catalog (`bc_ctx_check`) and marks it `checked` (database.cppm:1929-1954); the check, `Compiled::checked` and its plan-check counter are removed. The runtime's check of a plan against the catalog it pinned stays, for a DDL that races the run. Bound again, `*`, `t.*` and NATURAL re-expand, USING re-resolves, and a name a new column makes ambiguous is 42702. An INSERT without a column list binds the same way after an ADD COLUMN, since it fills the leading columns (catalog_design.md, *Compatibility*).
  - This replaces `recompile_view_plan` (database.cppm:2017-2021), which rebinds every plan read through a view after any DDL; it is removed.
  - A `*` over a view gives the view's recorded columns. A view whose own `*` would now give other columns is refused when read (0A000), as today; PG 17 freezes a view's `*` at CREATE VIEW, and that difference stays.
  - A cached plan therefore always holds the parameter types inference gives at its version, so a run by text (**none**) can use a cached plan at the live version as it is.
  - A rebind whose parameter types and result columns equal the `Statement`'s swaps the plan in place, as today's recompile does, and replaces the `Statement`'s record with one at the new plan's version (`ResultCheck`, **own**). Otherwise the rebind's plan goes into a new `Statement`, which replaces the old one in the cache; a run by text then runs it. A binder error is the run's error. A `prepare` of the text again returns the new one, as PG 17's DEALLOCATE and PREPARE do.
  - A run that fixed its types (**own** or **given**) uses the cache's plan only when its parameter types equal the record's. Otherwise a **given** run binds a one-shot plan at the record's types, not entered in the cache, and the result is that plan or the binder's error. A held `Statement` that the cache no longer holds is seen by no run by text, so it binds at its own fixed types and swaps that plan in place when its result columns are unchanged. Result columns that differ are 0A000.
  - A replaced `Statement` stays valid for its holders until `close()`. Until #549 bounds the cache, nothing bounds the replaced ones: each DDL that changes a table's columns, or the types inferred for a text that reads it, retains one per cached text that reads it.
  - Cost: one recompile per cached text after each DDL, any DDL, GRANT included, where today a plan the DDL left compatible is checked and reused. Two runs pay per call instead, and this is stated rather than cached, since a per-version failure record would be more state on a shared `Statement` for an error path: a held `Statement` whose rebind keeps failing, with 0A000 or the binder's error, binds once per call until it is prepared again; a server record whose types differ from the cache's binds a one-shot plan per call until it is Parsed again. The PR shows `sql_layer_bench` before and after.
- **Transactions: the rule** (*A transaction binds only against its own catalog*), with the machinery it needs (*Mechanism: plans compiled against the transaction's catalog*): compiling from text, `build_private`, `TxnPlan`, the plan map and its charge, procedures at the transaction's catalog, cursors holding either plan, `ResultCheck`, `Transaction::refit` and `Transaction::procedure`.
  - Cost: a transaction compiles privately, once per distinct text, every text whose shared plan is not at its pin: all of them when its pin is behind, and, when its pin is live, those no autocommit run has bound again since the last DDL. A text run only inside transactions therefore compiles privately in every transaction after a DDL, until an autocommit run of it rebinds the shared plan.
- **The server** routes every message in a block through the block's transaction, and runs every Simple Query by text (*Server routing of Parse, Bind, Describe and Query*).
- Contract:
  - :255 is narrowed: "runs without a recompile" goes, since any DDL now rebinds each cached plan once; a statement over a table whose column list changed binds again; by text it returns the new columns or the binder's error; a prepared statement whose result columns change is 0A000. Its writes still give the new column its default and maintain the new index. Of its tests, "embedded API: compatibility decides which cached plans recompile" (api_test.cpp:401) is rewritten to expect one recompile per cached plan after ADD COLUMN and after CREATE INDEX, and none on a second run; "plan API: bc_ctx_run checks what a plan depends on, not a catalog version" is kept, since the runtime's check stays. "embedded API: a cached plan is checked once per catalog version" (api_test.cpp:328) is removed with `Database::plan_checks_for_testing` and `Database::plan_checks_`; stage 0's "rebinds once after each DDL … and not again" test replaces it;
  - :256 is narrowed: a rebind through a held statement that changes its result columns is 0A000, not run; and a held statement keeps the parameter types it fixed, so a fixed type that no longer fits is the binder's error (or 40001 at a stale pin), where today it is recompiled with inferred types and run;
  - :171 is narrowed (`refit` of such a statement is 0A000; `params()` keeps the types the statement fixed and no longer changes on a rebind, and a fixed type that no longer fits is the binder's error, or 40001 at a stale pin, where today `refit` rebinds with inferred types and `params()` reports them) and widened (`ResultCheck` and the forms that take it, `Transaction::refit`, `Transaction::procedure`);
  - :139 is fixed: it says a column added to a table a `SELECT *` view reads shows in the view; the view is refused when read (0A000) until it is created again;
  - :185 is narrowed by the rule's exceptions 1 and 2, listed in the PR: in a transaction with no DDL, a statement whose binding at the pin fails with any binder error while the live catalog is past the pin is 40001, where today it may run (a name created after the pin, an `ON CONFLICT` arbiter index created after it, an INSERT of the new arity after an ADD COLUMN, a type changed by a drop and re-create; a column added after the pin is 40001 today, unchanged); a statement whose record's version is later than the pin and whose result columns differ there is 40001. It is widened too: a statement over a table dropped or changed after the pin runs against the pin, where today a text not cached is 42P01 and a table dropped and created again is 40001; `Transaction::call` runs the procedure as the pinned catalog has it; and a first statement that a DDL raced more than `kTxnPlanAttempts` times runs at its last pin, where today it is 40001. Its sentence "a statement whose plan does not fit the catalog the transaction's first statement saw" becomes the rule's exceptions 1 and 2. Its re-pin clause stays: a DDL racing the first statement still re-pins it, also when the first statement names an object that DDL created. Of its tests, "a DDL racing a transaction's first statement re-pins it, not 40001" is kept unchanged, and "a statement that no longer fits the catalog a transaction pinned is 40001" changes: T reads the old row (100) and commits, and is renamed "a transaction reads a table dropped and created again after its pin as at the pin";
  - a server row is added: Simple Query runs by text; a statement prepared through Parse, named or unnamed, and a portal are 0A000 when their result columns change; in a block every message binds against the block's catalog. It names one narrowing: a statement Parsed in a block whose pin was behind fixes the pin's columns, and run after COMMIT in autocommit it is 0A000 when they differ at the live catalog, where today Parse fixes the live catalog's columns (pg_session.cppm:1265, :1524-1531) and the statement runs. It names a second: a Parsed statement and its portals keep the parameter types fixed at Parse, so a fixed type that no longer fits is the binder's error at Bind (or 40001 at a stale pin), where today Bind takes `params()` from a rebind with inferred types (pg_session.cppm:1557-1558) and runs.
- Docs: catalog_design.md, *Compatibility* (a cached plan bound again after each DDL, its cost, the result-type rule, the rule for transactions); frontend_design.md, *Binding* (`*`, NATURAL and USING bound again after a column change); server_design.md, *Simple query*, *Extended query* and *A session* (routing in a block, records); session_design.md, *Prepared statements and portals* (a named statement holds its text and record); cursor_design.md, *Cursors and savepoints* (cursors holding a `TxnPlan`, the keyed twin, DECLARE by text); this document's *The plan API and the runtime* (the reset and new begin after `BC_L_INCOMPATIBLE` remains only for the first statement's re-pin), *Savepoints*, "What does not change" (the no-DDL 40001 is the rule's exceptions 1 and 2), *DDL during an open transaction*, whose bullet "A statement whose plan doesn't verify against the pinned version … fails with 40001 … rather than recompiling against a catalog older than the cache" becomes: a statement is compiled against the pinned catalog, and is 40001 only by the rule's exceptions 1 and 2; a DDL that raced the first statement's pin re-pins it, as today; and *Known differences from PostgreSQL*, which gains the rows stage 0 ships (*Differences from PostgreSQL for transactional DDL*).
- Tests. Each interleaving below names T, a transaction pinned at v1, and B, another session's DDL published as v2; each runs embedded and over the wire, in autocommit where it applies and in an explicit and an implicit block:
  - autocommit: `SELECT *`, `SELECT t.*` and `INSERT … RETURNING *` by text return the new column after ADD COLUMN; a held `Statement`, a named and an unnamed Parse never described, and a bound portal are 0A000 at execute, `refit`, `open_cursor`, Bind, Describe and Execute; `prepare` again returns the new columns; `SELECT a FROM t, u` after u gains `a` is 42702; USING and NATURAL re-resolve; a table dropped and created again with another column type gives the new type by text and 0A000 held; a Simple Query is never 0A000; every cached text, `SELECT a FROM t` included, rebinds once after each DDL, a GRANT included, and not again (the plan-compile counter); a view over `SELECT a FROM t` keeps its columns, one over `SELECT * FROM t` is 0A000 when read;
  - B adds a column to t: T's `SELECT * FROM t` by text, held from before v1 and Parsed in the block returns (a); a statement prepared after B, or Parsed outside the block after B, is 40001 in T; T's INSERT without a column list runs and its commit is 40001 (CONTRACT.md:188);
  - B drops t, and B drops t and creates it again: T reads the old t by text, held and through a cursor; `UPDATE … WHERE CURRENT OF` runs, and T's commit is 40001 with nothing written (the catalog-record guard, CONTRACT.md:188); a T that only read t commits;
  - B creates table x, view w, a type, a domain, a procedure: T's statement naming each is 40001, at Parse over the wire, and its retry runs; a typo in T is 40001 and its retry 42P01; a view dropped and created again over the same tables is read with its old definition;
  - T starts after B: its first statement re-pins and sees B; a statement held from before B whose columns changed is 0A000;
  - B races T's first statement (`after_txn_pin_for_testing`): T re-pins and reads B's table ("a DDL racing a transaction's first statement re-pins it, not 40001", unchanged); B creates table x there and T's first statement names x: T re-pins and reads x, not 40001; B races it more than `kTxnPlanAttempts` times: T runs at its last pin;
  - portals in T: a portal bound in the block before B and executed after B returns the pin's rows and columns; one bound after B over a text Parsed before B whose columns differ at the pin is 0A000; a Bind after B of a statement Parsed outside the block after B is 40001;
  - parameter types, integer to bigint: B drops t, whose `a` was integer, and creates it with `a` bigint. By text, `execute("SELECT 1 FROM t WHERE a = $1", {5000000000})` infers bigint and runs, in autocommit and in a transaction pinned after B, and the cache then holds a `Statement` with `$1` bigint; a held `SELECT 1 FROM t WHERE a = $1` prepared before B runs with `$1` integer: in autocommit its plan is bound at integer and swapped into it; in a transaction pinned after B it runs at integer from a private `TxnPlan` in the plan map, and nothing is swapped into the `Statement`; a held statement whose result columns change is 0A000 there, and 40001 in T if its record's version is later than T's pin;
  - parameter types, integer to date: B drops t, whose `a` was integer, and creates it with `a` date, and an autocommit run of the text puts a `Statement` with `$1` date in the cache. A server record of `SELECT 1 FROM t WHERE a = $1` Parsed before B, `$1` int4: its Bind is 42883 in autocommit; in a transaction pinned after B, with another DDL published since, it is 40001 and its retry 42883; the cache's `Statement` is never run with the record's types (a test hook counts its runs);
  - a CALL in T of a procedure created after the pin is 40001, and of one that exists nowhere is 40001 and its retry 42883;
  - binder errors at a stale pin: after B's CREATE UNIQUE INDEX on t(b), T's `INSERT … ON CONFLICT (b)` is 40001 and its retry runs; after B's ADD COLUMN, T's INSERT VALUES of the new arity is 40001 and its retry runs; a column only another table gained is 40001 and its retry 42703; in a transaction whose pin is live, a typo'd table name is 42P01;
  - a record fixed in T: a `SELECT * FROM t` Parsed in T's block after B added a column fixes (a); after COMMIT, its Bind, Describe and Execute in autocommit are 0A000;
  - cursors: one opened in T after B from a statement held from before B opens with (a); a WITH HOLD cursor from a private plan reads the pin's rows and columns after COMMIT, keeps the `TxnPlan`'s estimate reserved until it closes, and `cap_holds` returns to its value before it; a 54000 from charging the keyed twin's private plan fails the open;
  - CALL after B drops the procedure, creates it again with another body and argument types, or adds a column to a table its body reads: T runs it as the pin has it; a second CALL compiles nothing; a CALL that is T's first statement pins first;
  - savepoints: SAVEPOINT, a private compile, ROLLBACK TO, the same text again (no compile), COMMIT; the map's charge stays consistent and returns to zero; an abort without marks and a re-pin zero it once;
  - the shared cache: a second run of a text in T compiles nothing; a private plan never reaches the shared cache (another session's `columns()` is unchanged); a transaction stores nothing in the shared cache; after a GRANT, a transaction with a live pin compiles each text privately until an autocommit run rebinds it, and then uses the shared plan and compiles nothing (the plan-compile counter); a shared plan bound at v0 is not run in a transaction pinned at v1; `max_bytes` past by private plans is 54000;
  - a DDL Parsed in a block and never executed leaves it open; CREATE TABLE AS in T is 0A000 at execute;
  - the proof machine runs `SELECT *` across ADD COLUMN by Simple Query and by a prepared statement, and compares with PostgreSQL.
- Faults: an allocation-failure sweep, in `plan_alloc_faults_test`'s shape, over a private compile (the build of the `TxnPlan`, its charge, its insert in the map), a private procedure compile, a cursor's private twin, and an autocommit rebind that puts a new `Statement` in the cache. A failure fails the statement as any allocation failure in a transaction does, and leaves the shared cache and the map as they were.
- Still refused: everything refused today.

**Stage 0 as built.** These are the smallest changes to the design above that the code needed; the guarantees are the ones stated.
- **A text no one has prepared, at a live pin.** A transaction whose pin is the live catalog prepares a text not in the shared cache there, as `Database::prepare` would, and runs that shared plan; a plan bound at the live published catalog is no transaction's own. Without it, every transaction would compile each text it runs by text privately, which the stage's cost does not state ("a text run only inside transactions compiles privately in every transaction after a DDL"). A transaction whose pin is behind, or whose text's shared plan is behind, adds nothing to the cache, as the tests require.
- **A pin a refit took is not re-pinned.** The first statement re-pins only a pin it took itself. A pin taken earlier by `Transaction::refit` (a Parse or Bind in a block) is older than a DDL since, not raced by it: a portal bound in the block before the DDL runs at that pin, as the stage's portal test requires.
- **The private mark** of a record is stage 2's: no catalog is private in stage 0, so `StatementRecord` carries none yet.
- **Fixed parameter types** need the binder: `bind` takes the types a statement fixed, and each `$n` starts typed. A fixed integer type compared with, or assigned to, an integer column of another width is taken, as PostgreSQL's cross-type operators and assignment casts are, and its value is range-checked as its own type (22003), as PostgreSQL's Bind reads it. Any other mismatch is the binder's refusal. Where PostgreSQL has no operator or assignment cast either, it is PostgreSQL's SQLSTATE: 42883 for a comparison, 42804 for an assignment. Where PostgreSQL converts the value (an integer `$n` compared with or assigned to a float or DECIMAL column, any value assigned to a string column), the layer does not yet, and refuses with 42601 (frontend_design.md, *Fixed parameter types*). So keeping the fixed types matches PG 17 only for the integer widths and the refusals; for those conversions it is a refusal where PG 17 runs the statement, and where the layer before stage 0 bound it again with inferred types and ran it.
- **`Database::call`** compiles a procedure into the shared map (`procedure_plan`) when its transaction's pin is the live catalog; `Transaction::call` never does.
- **The fault sweep** asserts that a failed statement leaves the shared cache as it was and the plan map's charge equal to its plans: a plan compiled before a later failure of the same statement (a cursor's pin, a procedure's run) stays in the map, valid at the pin.
- **Tests not built as listed:** a Bind in a block of a statement Parsed outside it after the block's pin cannot happen on one connection (the block is open), so exception 2 is tested embedded ("rebind: B adds a column; T reads t with the columns of its pin"); the run-counting hook on the cache's `Statement` is replaced by the compile counter; a DDL Parsed in a block and never executed is tested on the wire (`test_a_ddl_parsed_in_a_block_and_never_executed_leaves_it_open`); types, domains and procedures created after the pin are tested through the procedure (CALL, `Transaction::procedure`), tables and views, since DDL naming them is refused in a transaction (0A000).
- **`sql_layer_bench`, before (base) and after (stage 0), releasedbg, one run each:** `bc_ctx_run` of an empty plan 18.1 / 16.9 ns; point read, verify and map ids 45.5 / 44.4 ns; New-Order, verify and map ids 974.7 / 906.3 ns; `bc_col_i64` + `bc_add` 27.5 / 29.1 ns; nullable VARCHAR column 44.1 / 40.6 ns; Payment's `bc_update` 832.4 / 969.5 ns. Run to run these vary by about 15% (a second base run gave 1124.9 and 903.8 ns for the last two), so stage 0 shows no change on the bench. The bench measures the runtime below the cache: it runs no DDL, so the one recompile per cached plan after a DDL and the private compiles in a transaction are not on it.
- `TxnPlan` gives its `Compiled` by reference, to its friends only; `Compiled` is neither copied nor moved, since its schema ref points into it.

**Stage 1: DDL bound where it runs.**
- `prepare_locked` classifies DDL from its syntax tree, as a transaction does from stage 0, and binds it at execute, against the catalog it runs in. A cached DDL `Statement` holds its parse and no plan, as today.
- `next_version` reads through a `RowSource`; `Snapshot` implements it.
- Autocommit DDL's guards and publication are unchanged.
- Contract: CONTRACT.md:171 is narrowed (a DDL's binder error is thrown at execute, not prepare).
- Tests:
  - the whole suite passes, with the tests that expected a DDL's binder error at `prepare` moved to `execute`;
  - a DDL with a binder error prepares and throws at execute; a non-DDL statement's binder error is still thrown at prepare;
  - a `RowSource` test backfills from a source that is not a snapshot.
- Still refused: everything refused today.

**Stage 1 as built.**
- **`SnapshotRows` adapts a `Snapshot`.** `Snapshot` is the engine's type, and no stage changes the engine, so it cannot implement `RowSource` itself. `RowSource` is an interface of the catalog module with one call, `scan(prefix, visit)`; `SnapshotRows` implements it over a snapshot, and `LiveCatalog::prepare` passes one. Stage 2's view of the transaction implements it too.
- **`Catalog::next_version` is public**, as stage 2's `Transaction` calls it outside `LiveCatalog`; the `RowSource` test calls it with a source that is not a snapshot.
- **A DDL with a binder error is cached** like any other DDL text, as its parse: before, `prepare` threw and cached nothing.
- **Over the wire**, Parse and Describe of a DDL with a binder error are no error, and its Execute is: PostgreSQL 17 does the same (`test_a_ddl_is_bound_at_execute_and_a_query_at_parse`).
- `CREATE TABLE AS` and `SELECT … INTO` bind their query at prepare, as before; stage 4 makes them transactional.

**Stage 2: DDL in an embedded `Transaction`.**
- All `Ddl` variants except CREATE TABLE AS run in a transaction, with `bc_txn::catalog`, `bc_txn_ddl`, `Published` and `TxnCatalog`, marks that carry the catalog, the 55006 rule, the doomed check, the commit classification, and the commit through `LiveCatalog::commit`. Stage 0's private plans are compiled against `txn->catalog`; `Transaction::prepare` is added.
- The invariant on namespace `01` is stated in catalog_design.md, *DDL*, and asserted in `bc_txn_ddl`.
- The server keeps refusing DDL in a block with 0A000, now checked in the session before `txn_->execute`, until stage 3.
- Contract:
  - CONTRACT.md:188: the embedded API's 0A000 is removed, and a row is added for DDL inside a `Transaction`; the server's 0A000 in a block stays, stated in wire row :208, until stage 3;
  - :184 is widened (ROLLBACK TO undoes DDL);
  - :185 is widened (the doomed check, recoverable by ROLLBACK TO);
  - :171 is widened: `Transaction::prepare` is added. A `TxnStatement` run after its transaction is 0A000 when its result columns differ where it runs, and 40001 under exception 2 when it was fixed after its transaction's own DDL and runs in a transaction pinned at that base after the live catalog moved;
  - :179 is widened (catalog and rows atomic, at run time and after a crash).
- Docs: catalog_design.md, *DDL* (the invariant, and that a version number identifies only a published catalog).
- Tests (api_transaction_test.cpp, transaction_test.cpp, api_cursor_test.cpp):
  - CREATE TABLE, INSERT, CREATE INDEX, SELECT in one transaction, then rollback leaves nothing (42P01) and commit leaves all of it;
  - `DROP VIEW v; DROP TABLE t; ROLLBACK` brings back both, with t's rows;
  - ADD COLUMN … DEFAULT backfills rows the transaction wrote earlier;
  - CREATE INDEX after INSERT in the same transaction indexes the row;
  - drop then re-create one name in one transaction commits;
  - ROLLBACK TO undoes DDL after the savepoint and keeps rows before it; RELEASE keeps it; an aborted DDL reopened by ROLLBACK TO; an abort with marks returns to the newest mark's catalog;
  - an abort that restores the catalog: SAVEPOINT s; CREATE INDEX i; an INSERT (its private plan maintains i); a statement that fails; ROLLBACK TO s; the INSERT again writes no entries for i, and the commit holds none;
  - in a transaction that has run DDL, with the pin live, binder errors are reported as they are: `DROP PROCEDURE p; CALL p` is 42883, and `DROP TABLE t; CREATE TABLE t` without c, then `SELECT c FROM t`, is 42703, not 40001;
  - ROLLBACK TO across CREATE INDEX: an INSERT prepared after the CREATE INDEX, run again after a ROLLBACK TO that undid the index, writes no entries for it, and the commit holds none;
  - a duplicate name is 42P07 and aborts; a skipped `IF NOT EXISTS` does not make the transaction one that ran DDL (another DDL committing does not doom it);
  - DDL in a READ ONLY transaction and on a follower is 25006 at the statement;
  - 55006, with each of the four messages, for DDL on a table an open cursor reads; none for an unrelated table, for CREATE VIEW, or for DROP INDEX of an index the cursor does not scan; 55006 for DROP INDEX of one it scans; 2BP01 for DROP TABLE of a table a view depends on; a WITH HOLD cursor from an earlier transaction is not counted;
  - a WITH HOLD cursor over a table created in the transaction, then COMMIT, then FETCH; a cursor over a name dropped and re-created in the transaction reads the new table;
  - a statement compiled before an in-transaction CREATE INDEX maintains the new index after it;
  - `SELECT *` run by text before and after an in-transaction ADD COLUMN returns the new column the second time, and after COMMIT in another session; a statement held from before is 0A000 and aborts the transaction;
  - another session sees nothing before commit and all of it after; another session executing or preparing a text the transaction compiled for its private table gets 42P01, and so does a `TxnStatement` from `Transaction::prepare` over that table, run after ROLLBACK; a `TxnStatement` over `SELECT * FROM t` from a transaction pinned before another session's ADD COLUMN on t, run after COMMIT in autocommit, is 0A000; T1 pins at base b, T2 runs ADD COLUMN on t, prepares `SELECT * FROM t` and commits, and the `TxnStatement` run in T1 is 40001, its retry returns the new column; the same after T2's ROLLBACK, in a transaction at b with the live catalog still b, is 0A000;
  - each row of the conflict table in *Conflicts between DDL and other writers* has its own test, with nothing written by the loser, DROP INDEX against a concurrent insert included;
  - the doomed check: T1 runs DDL, an autocommit DDL on an unrelated object commits, and T1's next `SELECT 1` is 40001 and aborts; ROLLBACK TO a mark before T1's first DDL then lets T1 commit; ROLLBACK TO a mark after it leaves the next statement 40001;
  - re-pin: a transaction whose first statement is a DDL and whose pin is behind re-pins and runs it; a DDL that is not first, with a stale pin, is 40001;
  - the commit classification: two transactions that allocate the same private id and insert rows with the same key, then commit in turn, give 40001, not 23505, with two `LiveCatalog`s on one database as well as one; two CREATEs of one name give the loser 40001, at its next statement and at its commit, and its retry's CREATE 42P07; a transaction with DDL whose commit finds a unique key taken by a concurrent insert, with no DDL published since, gives 23505;
  - hidden row ids: two transactions each create a table with hidden row ids under the same private id and insert; one commits and the other fails; a later table that reuses the rolled-back id draws unique row ids;
  - `max_bytes`: a backfill past it is 54000; a `max_bytes` above 1 GiB with a backfill past 1 GiB is 54000, not an engine error at commit; many DDL statements, each behind a SAVEPOINT, fail with 54000; `Catalog::estimated_bytes` against its constants.
- Faults:
  - "a transaction with DDL whose commit may have reached the engine is 08007", in the shape of the existing test at api_transaction_test.cpp:907, followed by a reopen that loads a catalog consistent with its rows: both or neither;
  - a reopen after the commit hook (`set_commit_hook_for_testing`) of a DDL transaction's commit, for a commit that applied and one that failed: the reopened catalog and rows match each other;
  - an allocation-failure sweep over a DDL step in a transaction (the clone, each overlay write, the backfill, the charge), over a private compile after it, over ROLLBACK TO across a DDL, and over the commit, in `plan_alloc_faults_test`'s shape. It asserts that a failed step leaves the transaction aborted, its catalog and charge unchanged, and that ROLLBACK TO restores them.

  The commit goes through `LiveCatalog::commit` unchanged, so no durability site changes and tests/CLAUDE.md asks for no mutation patch. If review finds a new durability site after all, it gets a row in `docs/correctness_validation.md` and a mutation.

**Stage 2 as built.** These are the smallest changes to the design above that the code needed; the guarantees are the ones stated.
- **`Published` and `TxnCatalog`** are types of the database module, not of the catalog module: `Catalog` stays one type. `Database::TxnCatalog` holds a transaction's base (the published catalog it pinned) and its catalog. Its `published()` makes a `Database::Published`, whose constructor only `TxnCatalog` can call, and only while the two catalogs are one. The transaction's version-keyed sites, `shared_txn_plan` (the exact-version test against the shared cache and a held statement) and `shared_procedure` (the shared procedure map), take a `Published`, so passing them a private catalog's version does not compile, and a transaction that has run DDL never reaches them. Autocommit's version-keyed sites (`fitting_plan`, `procedure_plan`) read `live_->current()`, which is always published, so they take no new type. The tests that another session's description and plans are unchanged after a transaction's private catalog (*Tests* above) cover the guard.
- **The runtime's calls:** `bc_txn_catalog` (the transaction's catalog, pinned first), `bc_txn_has_ddl`, `bc_txn_current_catalog` (without pinning, for ROLLBACK TO), `bc_txn_rows` (the backfill's `RowSource`: a two-pointer merge of the snapshot and the overlay, one prefix at a time) and `bc_txn_ddl`. A DDL counts as a statement of the transaction, so the statement after it is not a first statement and never re-pins.
- **A mark taken before the transaction's first DDL records no catalog**, and stands for the pinned one: a mark may be taken before the first statement pins, and the first statement may re-pin after it. Only a private catalog, which exists only after a DDL, is recorded.
- **The plan map across ROLLBACK TO:** the handle drops the private plans compiled for another catalog than the one restored (`drop_plans_not_for`), returning their charge; a DDL clears the map.
- **The backfill's bound** is thrown as `BackfillTooLarge` (a `std::length_error`), so the transaction can tell it from the catalog's other length errors; in a transaction it is `ExecError` `BC_E_LIMIT`, "… does not fit the transaction's bound of N bytes: run the DDL outside a transaction", N the smaller of `max_bytes` and the engine's batch limit. What the DDL adds past `max_bytes` (its writes, its guards, the catalog's charge) is `BC_E_LIMIT` from `bc_txn_ddl`, with the same advice.
- **The commit** (`commit_ddl`) builds the plan as any transaction's, without the catalog-record guards and with one `ensure_unchanged` on the version key, and calls `LiveCatalog::commit` with `Prepared{base = pinned, next = catalog}`. A false return is classified by one read of the version at the head (`LiveCatalog::head_version`), which covers both of its causes: a DDL published since (`current() != base`) moved the version, and an engine conflict with an unmoved version is classified as any transaction's (`conflict_or_duplicate`). `DdlInProgress` is 40001.
- **The doomed check** runs at the start of every statement (`as_statement`), SAVEPOINT and RELEASE included. A `SerializationFailure` in a transaction that had run DDL when the statement started aborts it; one that had run none ends it, as before.
- **The 55006 rule** reads each open cursor's schema ref (`objects_read`, `tables_written`); a table's own indexes count as the table. It skips a DROP TABLE that a view depends on, which the catalog refuses with 2BP01. The server's portals count from stage 3, which runs DDL in a block.
- **The private mark** is `StatementRecord::private_catalog`, and a private plan's record carries its transaction's base version (`TxnPlan::version_`). `Transaction::prepare` puts nothing in the shared cache: it opts out of stage 0's prepare of an unknown text at a live pin.
- **25006** is tested on a follower; the embedded API has no READ ONLY transaction, and a READ ONLY block's 25006 is the server's, checked before the block's DDL refusal (`wire: test_ddl_in_a_begin_block_is_refused_and_fails_the_block`).
- **Tests** are in `tests/api_txn_ddl_test.cpp` ("txn DDL: …"), and the runtime's in `tests/transaction_test.cpp`: the commit classification with two `LiveCatalog`s on one database, and the transaction's rows source. The allocation-failure sweep covers CREATE INDEX and ADD COLUMN … DEFAULT (the clone, the overlay writes, the backfill, the charge), a private compile after a DDL, ROLLBACK TO across a DDL (it allocates nothing) and the commit (whole or nothing).
- **`sql_layer_bench`, before (stage 1) and after (stage 2), releasedbg, one run each:** `bc_ctx_run` of an empty plan 18.6 / 18.8 ns; point read, verify and map ids 39.8 / 39.4 ns; New-Order, verify and map ids 865.0 / 895.8 ns; `bc_col_i64` + `bc_add` 25.1 / 24.9 ns; nullable VARCHAR column 36.2 / 38.1 ns; Payment's `bc_update` 869.1 / 865.0 ns. Within the run-to-run variation stage 0 measured (about 15%): taking `cx->cat` from `txn->catalog` costs statements without DDL nothing the bench shows.

**Stage 3: DDL in a server BEGIN block.**
- Stage 3 lifts stage 2's 0A000 for explicit blocks. Routing is stage 0's; a Parse after the block's DDL fixes its record from the `TxnPlan`, at the block's base version, with the private mark.
- The block's open portals count with its cursors for 55006.
- Contract: wire row :208 is widened (DDL runs in an explicit block).
- Docs: server_design.md, *Extended query* (DDL in a block, portals and 55006).
- Tests:
  - wire tests over simple and extended Query: BEGIN; CREATE TABLE; Parse/Bind/Describe/Execute a statement on it; ROLLBACK; then 42P01 at the next Bind;
  - a named statement prepared in a rolled-back block;
  - Parse after ADD COLUMN in a block, ROLLBACK, then Bind in autocommit, in a new block at the same base, and in one pinned at another session's base+1 with other columns: 0A000 each time, and on retry;
  - Parse after ADD COLUMN in a block, COMMIT, then Bind in a block that pinned the base before the COMMIT: 40001, and its retry returns the new column;
  - DROP TABLE on a table a bound portal of the block reads: 55006;
  - SAVEPOINT across DDL;
  - stage 0's block tests pass unchanged;
  - the functional case "DDL inside a transaction" (transactions.yaml:147) loses `unimplemented`, and more cases are added under `pg:transactional-ddl` (DROP rollback, ROLLBACK TO across DDL, 42P07 in a block, 25P02 after a failed DDL);
  - the proof machine gets DDL rules inside BEGIN and compares them with PostgreSQL on one connection;
  - the conformance table in README.md is refreshed from a run.

**Stage 3 as built.** These are the smallest changes to the design above that the code needed; the guarantees are the ones stated.
- **A portal holds what its plan reads, not its plan.** The server's portal holds a text and its record, and runs by text at Execute, so it has no plan for the 55006 check to read. `Transaction::bind_portal(text, check)` is `refit` that also returns a `PortalReads`, the tables and indexes the plan it would run names (its schema ref's objects read and tables written). The portal holds it; the transaction keeps a weak reference, and `check_in_use` reads the live ones beside its cursors. It holds no plan and is charged nothing. A Bind outside a block takes `Database::refit` as before: there is no transaction for a DDL to run in.
- **Which portals count, as in PG 17.** A SELECT portal counts from its Bind until it is closed, replaced by another Bind of its name, or dropped with its transaction, whether it ran or not: PostgreSQL's PortalStart starts the executor, which opens the portal's relations, at Bind only for a PORTAL_ONE_SELECT portal. An INSERT, UPDATE or DELETE portal, with RETURNING or not, never counts: PostgreSQL opens its relations only while it runs and closes them before Execute returns, so a DROP TABLE, ALTER TABLE or CREATE INDEX on its table runs (review of this stage found the first build counting them, a refusal PostgreSQL does not make). `Transaction::bind_portal` returns no `PortalReads` for a plan that writes a table. Two PostgreSQL rules the server did not follow before decide the rest, and are built here: a ROLLBACK TO drops the portals bound since its savepoint (`AtSubAbort_Portals`; an Execute of one is then 34000), and a RELEASE hands them to the enclosing level; a Simple Query drops the unnamed portal first (`exec_simple_query`), so the unnamed portal of an earlier extended statement never makes a Simple Query's DDL 55006. A Bind of a DDL text into the unnamed portal replaces the one it held, as in PostgreSQL. PG 17 chooses a sequential scan on a small table where the layer may scan an index, so a DROP INDEX's 55006 can differ by plan, as for a cursor.
- **An aborted transaction reports only a syntax error before 25P02.** The proof machine's DDL rules in a block found that the layer reported a CREATE TABLE with two primary keys (42P16 in PostgreSQL, which raises it in parse analysis) in an aborted transaction, where PG 17 says 25P02. Review found the layer gave that statement 42601, in a transaction or not; the parser now raises it as 42P16. `Transaction::execute(sql)` in an aborted transaction, and the server in a failed block, now check only the raw parse (`ast::check_syntax`, PostgreSQL's grammar through libpg_query) before 25P02, not the layer's own reading of the tree. A form the layer refuses but PostgreSQL's grammar takes is therefore 25P02 there, as in PostgreSQL.
- **Not testable on one connection:** "Parse after ADD COLUMN in a block, COMMIT, then Bind in a block that pinned the base before the COMMIT" needs a block open on the connection while the DDL block commits, which one connection cannot have. Exception 2 for a record fixed after the transaction's own DDL is tested embedded, through `TxnStatement` (stage 2's tests).
- **Tests:** `tests/server/test_wire.py` (`test_ddl_in_a_begin_block_runs_in_the_block_and_rollback_undoes_it`, `test_a_statement_parsed_after_the_block_s_ddl_keeps_its_columns_after_rollback`, `test_a_ddl_on_what_a_portal_of_the_block_reads_is_55006`, `test_savepoints_across_ddl_in_a_block`, `test_a_block_that_ran_ddl_is_40001_once_another_ddl_commits`; all but the last also pass against PG 17 unchanged), "txn DDL: a portal bound in the transaction counts for 55006 while it is held" and the aborted-transaction checks in "txn DDL: a duplicate name is 42P07 …" (`tests/api_txn_ddl_test.cpp`), the functional cases under `pg:transactional-ddl`, and the proof machine, whose CREATE TABLE, DROP TABLE and CREATE INDEX rules now run inside its transactions too (`test_ddl_inside_begin_agrees_with_postgresql` is its fixed case).

**Stage 4: CREATE TABLE AS in a transaction, and DDL in implicit blocks.**
- CREATE TABLE AS runs its query through the transaction's view and puts its rows into the overlay.
- `refuse_ddl_beside_others` is removed: Executes before one Sync, and a Simple Query sent inside that pipeline, form one transaction with their DDL, as PostgreSQL's implicit block does. A Simple Query of several statements stays refused (0A000) until it is built.
- Contract: wire row :208 is widened again.
- Tests: CREATE TABLE AS in a block, rolled back and committed; an extended pipeline with DDL and DML that fails half way writes nothing, with the DDL sent by Execute and by a Simple Query inside the pipeline.

**Stage 4 as built.** The smallest changes to the design above that the code needed; the guarantees are the ones stated.
- **The query is compiled privately, under a key of its own.** `Transaction` compiles CREATE TABLE AS's query against the transaction's catalog through the rule's path (`plan_for`, with its re-pin and exception 1), never from the shared cache: the cached statement's query plan may be the stand-in of a constant that raises an error, which must never run. It is kept in the plan map under `"\x02"` and the text, since the text's own key holds its parse, a CREATE TABLE AS. It runs as a statement of the transaction (`run_txn`), so it reads the transaction's view and its own tables; its rows then go through `Catalog::next_version` and `bc_txn_ddl` as any DDL's backfill, bounded by `max_bytes` and the 1 GiB batch.
- **Its order of errors is autocommit's:** a taken name (42P07, or the notice of IF NOT EXISTS, with no DDL of the transaction) before the query is planned; then the query's errors, a constant's folding error at run, and 0A000 for WITH NO DATA of such a query.
- **Its parameters are the statement's in a block too.** PG 17 analyses CREATE TABLE AS's query at Parse, in a block as outside one, and takes `$n` there. A block's `refit` of the text binds the query against the block's catalog for its parameters, so a binder error is the Parse's, as outside a block; a constant's folding error is left to Execute, and its record lists the parameters of a stand-in with that constant NULL, compiled at the block's catalog and never kept or run, as the autocommit statement's are.
- **Row ids past the rows it writes.** CREATE TABLE AS writes its rows under row ids 1 to n itself. `LiveCatalog::next_row_id` keeps a counter per table id that outlives a rolled-back table of that id (*Mechanism: the transaction's private catalog*, "Hidden row ids"), and a counter seeded from the engine for an unpublished table starts at 1: an insert after a CREATE TABLE AS in its transaction, or after an autocommit one under an id a rolled-back transaction had drawn row ids for, drew an id the table already held, and was 23505. The design missed this, and the autocommit case reached it from stage 2. `LiveCatalog::reserve_row_ids` moves the table's counter past n once the DDL's next catalog is built, in `LiveCatalog::prepare` and in the transaction's step; ids are drawn, never returned, so a DDL that then fails leaves a gap.
- **Tests:** "txn DDL: CREATE TABLE AS …" and "txn DDL: rows inserted into a CREATE TABLE AS table take row ids past its own" (`tests/api_txn_ddl_test.cpp`), `test_ddl_in_an_implicit_block_is_one_transaction_with_its_statements` and `test_create_table_as_in_a_begin_block_is_a_statement_of_the_block` (`tests/server/test_wire.py`; both also pass against PG 17), and the functional case "CREATE TABLE AS in a transaction reads its writes, and ROLLBACK undoes it". The stage lists no allocation-fault sweep: its step is stage 2's, which the sweep covers.

### Decisions the standing rules settle

These follow from "never wait", "do what PostgreSQL does" and CONTRACT.md:185, and are not open:

- **A transaction that ran DDL learns it is doomed at its next statement**, not only at commit. The no-DDL transaction already gets its 40001 at the statement, and it saves the work of a transaction that cannot commit.
- **No priority or reservation protects a long DDL transaction** from autocommit DDL that keeps dooming it. Either would be a wait in disguise. First committer wins, and the client retries.
- **Open cursors and portals refuse DDL on what they read (55006)**, as PostgreSQL does.
- **DDL on a follower is 25006 at the statement**, as PostgreSQL's standby refuses it.

### Owner's decisions on transactional DDL

The owner decided these on 2026-11-19:

1. **Every conflict between two DDL is 40001, two CREATEs of one name included.** PG 17 makes the second CREATE wait, then gives it 23505; here the loser gets 40001 without waiting, and its retry's CREATE gets 42P07. The case is rare, and a 40001 is retried. So the commit reads no name entries to tell this case apart (*Classifying a conflict*, under *Mechanism: committing and publishing a transaction's catalog changes*).
2. **A backfill in a transaction is bounded by `max_bytes`, never past the engine's 1 GiB.** Past either, the statement is 54000, and its message names running the DDL outside a transaction.
3. **A DDL's binder errors are thrown at execute** in the embedded API, not at prepare: one rule, in PostgreSQL's order. CONTRACT.md:171 narrows for DDL in stage 1.
4. **A statement's result type follows PG 17 everywhere, fixed first in stage 0.** A plan binds again when a table it read has other columns. Parsed and run in one step, a statement returns its catalog's columns; a prepared one whose result columns change is 0A000. In a transaction its catalog is the transaction's (decision 5).
5. **A transaction binds only against its own catalog** (rule A): the published catalog it pinned at its first statement, plus, from stage 2, its own DDL. Nothing in a transaction is bound against, or run as a plan bound against, any other catalog. The shared plan cache serves a transaction only with a plan compiled at exactly that catalog. Every other statement is compiled privately against the transaction's catalog and never enters the shared cache. The narrowed 40001s, the 0A000 of decision 4, 54000 for private plans, and a held or Parsed statement keeping the parameter types it fixed (PG 17's rule; today it binds again with inferred types) are the only exceptions (*A transaction binds only against its own catalog*).

### Follow-up issues

Opened when the design is approved, as the repository's rules ask for deferred work:

- **Per-object catalog conflicts**, so DDL on unrelated objects commits concurrently. `pin()` needs a version that moves with every DDL, so this needs a design of its own. Opened if a workload needs concurrent DDL.
- **Row counts for a table created in the transaction** (#857): the planner's row counts come from the engine, not the transaction's view (*Risks and scope of transactional DDL*).
- **catalog_design.md, *The in-memory form*** says DDL is "serialized by a mutex … held from the prepare to the publication". The code has only try-locked since `DdlInProgress` existed. This is wrong today and is fixed in a small separate change, not in a stage (#858).

### Risks and scope of transactional DDL

**Risks**
- **A private plan leaking into the shared cache** would run one transaction's catalog for everyone. A move-only `Compiled`, `TxnPlan` and `set_compiled`'s signature make this a compile error, and stage 0 tests that another session's description is unchanged; stage 2 tests it again for a private catalog.
- **A private catalog reaching a version-keyed cache** would accept a plan built for other contents. `Published` and `TxnCatalog` are distinct types, and the transaction's version-keyed sites take `Published` (as built in the database module: *Stage 2 as built*).
- **The namespace `01` invariant** lets a transaction drop the `EnsureAbsent` guards. A future catalog write that does not bump the version would break it; `nextval` is the known candidate. It is stated in catalog_design.md and asserted in `bc_txn_ddl`. Autocommit DDL keeps its guards, so it does not rely on the invariant.
- **Compile cost.** In autocommit, one recompile per cached text after each DDL, any DDL, GRANT included, and one bind per call for a held `Statement` whose rebind keeps failing and for a server record whose types differ from the cache's; stage 0's PR shows `sql_layer_bench` before and after. In a transaction, a private compile per distinct text whose shared plan is not at its pin: every text when the pin is behind, and, with a live pin, each text no autocommit run has bound again since the last DDL. A text run only in transactions compiles privately in each of them after a DDL until an autocommit run rebinds it. After its own DDL, a transaction compiles each distinct statement privately, again after each DDL or ROLLBACK TO that changes its catalog; one that alternates DDL and DML across many texts pays a TCC compile per pair. All of it is bounded by the statements run, and private plans are charged to `max_bytes`.
- **Cost of DDL with savepoints.** A savepoint before each DDL keeps one catalog clone per mark, O(catalog) each. This is charged to `max_bytes`, so it fails with 54000, not out of memory.
- **The plan cache's row counts for a table created in the transaction** come from the engine, not the transaction's view, so a fresh table looks empty to the search. Plans are still correct, though possibly not the best choice (follow-up issue).
- **Statements replaced in the cache (stage 0)** stay alive until `close()`: one per cached text per DDL that changed its result columns or inferred parameter types. They are unbounded until #549 bounds the cache; #549 then bounds them with it.
- **The server's routing (stage 0)** touches every message inside a block. Stage 0's wire tests, stage 3's, and the proof machine are the check.

**Out of scope**
- Lock-based waiting of any kind, and PostgreSQL's read-latest catalog semantics.
- DDL forms the layer does not have: CONCURRENTLY, RENAME, TRUNCATE, ALTER beyond ADD COLUMN, CREATE DATABASE, DROP … CASCADE. Each states its transactional behaviour when it is added.
- DDL inside procedure bodies.
- Per-object catalog conflicts, and the other follow-up issues above.
- A snapshot per statement for READ COMMITTED, serializable guards, and token transactions. These stay under *Later*.
- Engine changes: none are needed or made.

### Edits to this document once transactional DDL is built

Made with stage 4. *DDL during an open transaction* keeps what still holds of autocommit DDL against open transactions, and points here for DDL inside one. Stages 0 to 4 are PRs stacked on the design's, #856.

- The status line names transactional DDL as built and links the PRs.
- *DDL during an open transaction* is replaced by this section's semantics; the paragraph "A DDL statement given to `Transaction::execute` is refused …" goes.
- *Savepoints*, "What does not change" (reworded by stage 0): add that a transaction that ran DDL is 40001 at its next statement once another DDL commits, recoverable by ROLLBACK TO, and that a mark carries the catalog.
- *Known differences from PostgreSQL*: the DDL row is replaced by the rows in *Differences from PostgreSQL for transactional DDL* that stage 0 did not already add.
- *Later*: the "Transactional DDL" row is removed.
- *Tests*: a "Transactional DDL" item lists stage 0's to stage 4's tests.

Other documents: catalog_design.md (*DDL*: the namespace `01` invariant and its exceptions, and that a version number identifies only a published catalog; *Compatibility*: the rebind after each DDL and the rule, from stage 0), server_design.md (*A session*, *Simple query*, *Extended query*; item 6 of *What it uses from the embedded API*), session_design.md (*Prepared statements and portals*), cursor_design.md (*Cursors and savepoints*; the 55006 rule with portals and DROP INDEX; cursors holding a `TxnPlan`), CONTRACT.md as listed per stage, and README.md per stage.

## Later

Not built. Each gets an issue when it is picked up.

| Later | Why nothing built blocks it |
|---|---|
| Token transactions across requests (the transport design, #487) | The handle is the token's in-process half; a token names a handle held with a TTL. |
| Procedures retried in the runtime (#495) | A procedure is a transaction whose statements are all the runtime's, so its retry is a loop around `begin` … `commit`. |
| Serializable and read committed | Guards are emitted per isolation level by the compiler, and the transaction already keeps guards. |
| Sequences and auto-increment | A runtime service at commit, outside the plan (`docs/plan_api_design.md`, *Deferred*). TPROC-C's ids come from rows it reads and updates. |
| Deadlines turned into budgets | The budget is per statement already; a transaction deadline divides into it. |
| Bulk load | A transaction mode that flushes its log in batches, as `MariaDBTxn`'s bulk-copy mode does, into a keyspace no one else reads yet. |
