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

**`ROLLBACK TO` keeps a SERIALIZABLE transaction's reads.** The read set
is not part of a mark: the client saw what the rolled-back statements
read, as PostgreSQL keeps a rolled-back subtransaction's reads (*Read sets
across savepoints, failed statements and cursors*).

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

At SERIALIZABLE a commit that writes also submits its read set, every
range as `ensure_range_unchanged` and every point it does not write as
`ensure_unchanged`, and a guard on the catalog version (*Mechanism: the
serializable commit*). Its 40001 is "could not serialize access due to
concurrent update" when a key it wrote changed since its snapshot, else
"could not serialize access due to read/write dependencies among
transactions" with the hint "The transaction might succeed if retried.",
as PostgreSQL 17 words them. The unique-check prune below is unchanged.

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

- **SERIALIZABLE** (`Isolation::Serializable` in the embedded API;
  `ISOLATION LEVEL SERIALIZABLE` and `default_transaction_isolation` over
  the wire, `server_design.md`, *Transaction modes*): the runtime records every key and range
  a statement reads, and a commit that writes submits them as guards, so it
  is strict serializable against every other write. See *Serializable
  isolation*.

**Later:**
- **read committed:** a snapshot per statement.

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
  claims and locks, and at SERIALIZABLE its read set, is bounded by
  `TransactionOptions::max_bytes`, default
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
  A SERIALIZABLE read is charged to `max_bytes` as it is made, with its
  guard's entry in the commit's plan, and ROLLBACK TO does not release it
  (*Read-set memory and the 54000 bound*); an autocommit statement's read
  set is charged to its statement budget.
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
- **Serializable** (*Stages of serializable isolation*, stage 1):
  `api_serializable_test.cpp` ("serializable: …"): the probes' cases
  through the embedded API, the read set's model test, its fault sweeps
  and 54000 bound; the mutation set
  `tests/serializable_mutations/` (`Guarded-by:` headers checked by
  `scripts/check_guarded_by.py --scope sql`, each patch run by
  `scripts/serializable_mutation_check.sh` nightly, and checked to still
  apply by `ci.yml` on every pull request); `transaction_test.cpp`'s read
  failure test, at both levels. Stage 2: `pg_session_test.cpp` (write skew
  between two sessions, a follower's 0A000, the transactional defaults),
  `test_wire.py`'s serializable and run-time-parameter tests (each
  SQLSTATE of *Choosing the level* over simple and extended query, write
  skew and phantoms over two connections, a read under a savepoint's READ
  ONLY, and the forms compared with PostgreSQL 17), and the proof
  machine's `begin_serializable_transaction` rule.
- **Concurrency:** Elle through SQL (`testing_design.md`, *The TPROC-C
  harness*): the harness's transactions as histories, checked as snapshot isolation, and
  as serializable where `FOR UPDATE` guards the reads that decide a write;
  the point and the range workloads; and the `serializable` configuration,
  with and without a third of its transactions read-only, checked as
  strict-serializable and against the range invariant.

## Known differences from PostgreSQL

Each comes from the rule *never wait*, except where its row gives another
reason.

| PostgreSQL (REPEATABLE READ) | This layer | Why |
|---|---|---|
| A second updater of a row waits for the first transaction, then gets 40001 at its statement if the first committed. | Both update. The second to commit gets 40001 at `commit()`. | No row locks: conflicts are found at commit. |
| A second inserter of a unique key waits, then gets 23505 at its `INSERT`. | Both insert. The second to commit gets 23505 at `commit()` (*Commit and conflicts*). | The same outcome, reported at commit instead of after a wait. |
| `INSERT … ON CONFLICT DO NOTHING` whose arbiter key a concurrent transaction inserts waits, then gets 40001. | Both insert. The second to commit gets 40001 at `commit()`; in autocommit the statement is retried and skips the row. | The same outcome, reported at commit. |
| `INSERT … ON CONFLICT DO UPDATE` whose conflicting row a concurrent transaction updates or deletes waits, then gets 40001. | Both write. The second to commit gets 40001 at `commit()`; in autocommit the statement is retried and updates the row as committed, or inserts where it was deleted. | The same outcome, reported at commit; no retry inside the statement (owner decision, as for `DO NOTHING`). |
| `INSERT … ON CONFLICT DO NOTHING` whose conflicting row a concurrent transaction deletes and commits waits for it, then inserts the row. | Below SERIALIZABLE, the row is skipped, as the statement's snapshot holds its key, and the commit succeeds. At SERIALIZABLE the skip is a read: a transaction's commit is 40001, and an autocommit statement that writes is retried and inserts the row. | Owner decision (2026-11-19), not the rule *never wait*: a guard per skipped row would make the commit a conflict and an autocommit retry insert the row; it was not taken below SERIALIZABLE (owner's question 1 of *Serializable isolation*). |
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

## Serializable isolation

Status: **built.** The design was approved by the owner on 2026-11-20 with every recommendation under *Owner's questions on serializable isolation*: question 1 (a), 2 (a), 3 (a), 4 (a), 5 (a); question 6 was decided (a) by stage 1's writer-p99 numbers. Stage 0 (branch `gustavoamigo/sql-serializable-isolation-check`): the SQL isolation check runs nightly over the point and range workloads. Stage 1 (`gustavoamigo/sql-serializable-embedded`): SERIALIZABLE transactions and autocommit statements run in the embedded API (`Isolation::Serializable`), checked nightly by the `serializable` Elle configuration. Stage 3 (`gustavoamigo/engine-prefix-range-guard-prepare`): a range guard inside one registered prefix is prepared outside the engine's writer section. Stage 2 (`gustavoamigo/sql-serializable-server`): the server runs SERIALIZABLE blocks and keeps the transaction defaults. *Stage 1 as built*, *Stage 2 as built* and *Stage 3 as built* say where the code differed from this design. Every edit of *Edits to this document once serializable isolation is built* is made.

The owner's decision (2026-11-20): SERIALIZABLE is real serializability. Snapshot isolation is never run under the SERIALIZABLE name. The two standing rules apply: do what PostgreSQL does, and never wait. Behaviour observed on PostgreSQL 17.12 is cited as "PG 17"; the probes are cases 1 to 15 and A to D of the fact-gathering run, and the review probes (p1.sql, s1.py to s4.py), kept with the design PR. Code citations are to branch `gustavoamigo/sql-txn-ddl-ctas-implicit`; they are re-checked before the design PR.

The design in one paragraph: a SERIALIZABLE transaction records what it reads, as point keys and key ranges, in a *read set*. The runtime's read calls record it, so every access path is covered without the compiler's help. At commit, a transaction that writes submits its read set as `ensure_unchanged` and `ensure_range_unchanged` guards in the same `WritePlan` as its writes, with one guard on the catalog version. The engine then proves the transaction serializable against every other write (engine `CONTRACT.md`, *Isolation is what the plan guards*). A transaction that writes nothing submits no read guards. It is serialized at its snapshot, which is already consistent. A failed check is 40001 at COMMIT, with nothing written. Nothing new is stored: the read set lives in memory and ends with the transaction.

### Serializable semantics: what a SERIALIZABLE transaction guarantees

- **One serial order.** Take the committed SERIALIZABLE transactions. Each one that wrote is placed at its commit's sequence; each one that wrote nothing is placed at its snapshot's sequence, after the writes it saw. Every such transaction returned the rows, errors and counts it would have returned had all its statements run alone at that point, against every write committed before it. Its writes are the ones that run would have made. The point lies inside the transaction's lifetime (the snapshot is taken at the first statement, and the commit's sequence is assigned before `commit()` returns), so the order respects real time: strict serializability, as the engine states it for guarded plans. SERIALIZABLE runs only on the engine that orders the writes, never on a follower (*Followers*), so this holds wherever SERIALIZABLE runs.
- **Against every other write.** A SERIALIZABLE transaction is checked against every concurrent commit, whatever that commit's level, autocommit statements and DDL included. PG 17 protects a SERIALIZABLE transaction only against other SERIALIZABLE ones (case C: write skew with a READ COMMITTED transaction commits both). Here the SERIALIZABLE side fails. Transactions at other levels keep snapshot isolation, so a history of them alone can still show write skew, as in PG 17.
- **A conflict is 40001 at COMMIT.** The commit throws `SerializationFailure`, SQLSTATE 40001, with nothing written, and the transaction ends (`CONTRACT.md`, the 40001 row). The message is chosen on the failure path only (commit step 5):
  - a key the transaction wrote that changed since its snapshot: "could not serialize access due to concurrent update", no DETAIL and no hint, as PG 17 reports it at SERIALIZABLE (case 11; review probe s4);
  - otherwise: "could not serialize access due to read/write dependencies among transactions", with PG 17's hint "The transaction might succeed if retried.";
  - a key the transaction inserted that a concurrent commit took: today's 23505 (*Commit and conflicts*).
- **An autocommit statement at SERIALIZABLE** is a one-statement SERIALIZABLE transaction. On a conflict it is retried on a fresh snapshot up to three times, as today, each attempt with an empty read set. Nothing between its attempts is the caller's logic, so the retry is invisible. PG 17 does not retry (case 8). Here the statement fails only after its last retry.
- **What is not promised.** A transaction is not told about a conflict before COMMIT (PG 17 often reports it at the statement: cases 2, 4, 8, 12). A SERIALIZABLE writer that PG 17 would commit may fail here, and a reader PG 17 would cancel commits here (*Differences from PostgreSQL 17 for serializable isolation*). A history PG 17 would refuse is never committed here: one of its transactions fails, though not always the one PG 17 picks.

### Choosing the level: SET, SHOW, BEGIN and the embedded API

**Embedded API.**
- `TransactionOptions::isolation`, of `enum class Isolation { Snapshot, Serializable }`, defaults to `Snapshot`. `Database::begin` takes it.
- `TransactionOptions::read_only`, public, defaults to false. A read-only transaction refuses every write, `FOR UPDATE` guard, DDL and `CALL` that writes with 25006, "cannot execute … in a read-only transaction", in the runtime itself, not only in the server. A read-only SERIALIZABLE transaction records nothing (*Read-only serializable transactions*).
- `ExecutionOptions::isolation`, with the same default, applies to the autocommit statement it is given to.
- `Transaction::set_isolation(Isolation)` and `Transaction::set_read_only(bool)` are accepted until the transaction's first statement, with no savepoint open. After that they throw with PG 17's 25001 texts, so the server's checks are made before them, never in their place. `set_read_only(true)` later, or under a savepoint, is the server's own business (*The server*): the transaction keeps recording.
- `Snapshot` keeps its name. It is what READ UNCOMMITTED, READ COMMITTED and REPEATABLE READ run today, and it is not called one of them. A real READ COMMITTED would add a value later.
- On a follower, `Database::begin` with `Serializable`, and an autocommit statement with it, are 0A000 (*Followers*).

**The server** (`server_design.md`, *Transaction modes*). Each item below matches PG 17 as observed:
- `BEGIN` / `START TRANSACTION … ISOLATION LEVEL SERIALIZABLE` and `SET [LOCAL | SESSION] TRANSACTION ISOLATION LEVEL SERIALIZABLE` are accepted. The 0A000 at pg_session.cppm:324-325 goes, along with its comment at :304-309.
- **25001, the isolation level.** A *change* of level is refused after a query ("SET TRANSACTION ISOLATION LEVEL must be called before any query") and inside a savepoint ("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction"); an unchanged level is accepted in both places. These rules exist today in `SetTransaction` (pg_session.cppm:1180-1186). **They are added to `BEGIN` of an implicit block** (`Control::Begin`, :1138-1150, which today moves `implicit_` into `txn_` and sets the level unchecked): `SELECT 1; BEGIN ISOLATION LEVEL SERIALIZABLE; …` in one simple Query is 25001 "…must be called before any query", as PG 17 reports it.
- **25001, DEFERRABLE.** `SET TRANSACTION [NOT] DEFERRABLE` inside a savepoint is 25001 "SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction", even before any query. After a query it is 25001 "SET TRANSACTION [NOT] DEFERRABLE must be called before any query" **even when the value is unchanged**, unlike the isolation level. Today both are accepted and ignored.
- Outside a block, `SET TRANSACTION …` is WARNING 25P01, "SET TRANSACTION can only be used in transaction blocks", and changes nothing.
- `READ ONLY`, `READ WRITE` and `[NOT] DEFERRABLE` combine with it. DEFERRABLE never waits (*Read-only serializable transactions*).
- **The session defaults.** `default_transaction_isolation`, `default_transaction_read_only` and `default_transaction_deferrable` are kept by the session:
  - set by `SET [SESSION | LOCAL] <name> {= | TO} <value>`, by `SET SESSION CHARACTERISTICS AS TRANSACTION <modes>` (ISOLATION LEVEL, READ ONLY | READ WRITE, [NOT] DEFERRABLE, each setting its default), and by `RESET <name>` and `SET <name> TO DEFAULT`, which restore `read committed`, `off` and `off`;
  - `RESET ALL` resets the three (and nothing else changes: `client_encoding` is always UTF8);
  - applied to each later `BEGIN` without that mode, to implicit blocks, and to autocommit statements. `default_transaction_read_only = on` makes a later block and an autocommit write 25006, as in PG 17;
  - **transactional, as PG 17's GUCs are.** A SET of a default inside a block changes later blocks only; `SHOW transaction_isolation` in the open block is unchanged. The pending value is kept with the block and with each savepoint: ROLLBACK, the ROLLBACK of a failed block, and ROLLBACK TO restore the value from before; COMMIT and RELEASE keep it. Without this, a rolled-back `SET default_transaction_isolation = 'read committed'` would drop the session from SERIALIZABLE to snapshot isolation;
  - `SET LOCAL <name> …` outside a block is WARNING 25P01 "SET LOCAL can only be used in transaction blocks" and changes nothing; inside one it lasts to the block's end.
- **Values.** Matched case-insensitively against `serializable`, `repeatable read`, `read committed` and `read uncommitted` (booleans for the other two). A two-word value must be quoted: `SET default_transaction_isolation = repeatable read`, unquoted, is 42601, syntax error at "read". A one-word value may be an identifier. The SET classifier today strips quotes before it splits words (pg_session.cppm:411-422); the new parameters are parsed quote-aware. Any other value is 22023, `invalid value for parameter "default_transaction_isolation": "bogus"`, hint "Available values: serializable, repeatable read, read committed, read uncommitted."
- **`transaction_isolation`.** `SET [SESSION | LOCAL] transaction_isolation = '…'` inside a block sets the block's level, under the 25001 rules above. Outside a block it is taken and changes nothing (PG 17 prints `SET`). `RESET transaction_isolation` and `SET transaction_isolation TO DEFAULT` are 0A000, `parameter "transaction_isolation" cannot be reset`; inside a block that error aborts it, as any error does.
- **SHOW.** `SHOW transaction_isolation`, `transaction_read_only`, `transaction_deferrable`, `default_transaction_isolation`, `default_transaction_read_only` and `default_transaction_deferrable` return PG 17's values. Outside a block `SHOW transaction_isolation` reports the session default, not `read committed`. The levels below SERIALIZABLE still run snapshot isolation; `SHOW` reports the level the client chose, not the mechanism.
- `SET`, `SHOW` and `RESET` of any other parameter stay 0A000, except `client_encoding`. The session field `isolation_` (pg_session.cppm:1848) gains a `Serializable` value. The server passes it to `begin` and to autocommit runs.
- **Read-only at top level.** `BEGIN … READ ONLY`, or `SET TRANSACTION READ ONLY` before the block's first query with no savepoint open, reaches the transaction as `read_only` (`TransactionOptions::read_only` or `Transaction::set_read_only`). A `READ ONLY` set any other way (after a query, or inside a savepoint, where RELEASE and ROLLBACK TO restore READ WRITE: pg_session.cppm:1106-1124) is enforced by the server's 25006 as today, and the transaction keeps recording.

### Mechanism: the transaction's read set

**Recorded by the runtime, not emitted by the compiler.** The *Isolation* sketch had codegen emit `bc_guard_unchanged` and `bc_guard_range` at every read site. This design moves the recording into the runtime's read calls instead:
- **No read can be missed.** A plan reads data only through `bc_get`, `bc_exists`, the cursor calls and `bc_count_range` (`plan_api_design.md`, *Model*). If those calls record, no access path and no future operator can forget to.
- **Only the runtime knows where a cursor stopped.** A `LIMIT`, `EXISTS` or `MIN`/`MAX` closes its cursor early, and codegen does not know the last key it read (*What a read records for phantoms*). Guarding whole ranges from the compiler would add false positives.
- **Plans stay one per text.** The plan cache, the golden files and the transaction's plan map need no isolation key, and a prepared statement runs at whatever level its transaction has.
- **The recording is runtime-internal.** Its calls are not plan-visible, and a hand-written plan gets it too.

The sketch's other claim, "a plan's guards are the only text that differs between levels", goes with it. No plan text changes except the codegen header's fixed `isolation: snapshot` (codegen.cppm:3410), which goes since a plan has no level. The choice point's count (codegen.cppm:1085-1113) keeps calling `bc_count_range`, and records like any count. Its range is the first branch's own scan range, which that branch records anyway when it runs, so it adds few false positives.

**One recording reader.** Every data read in the runtime goes through one internal reader that records: it alone holds the context's snapshot and overlay view for reading, and the commit takes the snapshot from it by one release call. A read path that bypasses it does not compile. Each recording site is also protected by a committed mutation (*Stages*, stage 1).

**The switch.** `bc_txn_new` takes the level and the read-only flag. An autocommit context is given the level by `bc_ctx_set_isolation` before `bc_ctx_run`. With `Snapshot`, or a read-only transaction, each read call pays one predictable branch and records nothing. With `Serializable`, each read call records into the read set. A read is recorded whether or not the statement later succeeds (*Read sets across savepoints, failed statements and cursors*).

**Where the read set lives.**
- In a transaction, on `bc_txn`. It is never cut back by a mark (below).
- In autocommit, on the context. `bc_ctx_begin` clears it, so each attempt of `bc_execute`'s retry (plan_api.cpp:3976-3987) starts empty and submits only its own reads.
- An autocommit run whose plan writes no table (`bc_schema_ref::n_written == 0`, bc_plan.h:132) records nothing. The runtime refuses a write to a table the plan's reference does not list as written (`declared_object`, plan_api.cpp:1387-1397), and `bc_ctx_commit` submits nothing for a run without writes (:3253-3261), so its reads could never be submitted: it is serialized at its snapshot.
- `bc_txn_repin` (plan_api.cpp:3366-3373) returns false when the read set is not empty, so a read made at the old snapshot is never guarded against the new one. Its callers (database.cppm:3270, 3405, 4073) repin only before the first statement has run, when it is always empty; a test asserts that.

**What each read records.**

| Read | Records |
|---|---|
| `bc_get`, `bc_exists` (PointGet, COUNT(*) of a point, IndexJoin and NestedLoop inner probes, correlated subqueries) | the key, as a point. A key found absent is recorded the same way: `ensure_unchanged` holds for a key absent at both times and fails for one inserted since. A probe that reads nothing (a NULL key part, a key that does not fit: codegen.cppm:1236-1239) records nothing, since its result depends on no data. |
| `bc_scan_open` … `bc_scan_next` … close, table or index, either direction | the range the cursor covered (below) on the scanned object's keyspace |
| `bc_scan_row` on an index (fetching the row) | the row's primary key, as a point. An index range does not see an `UPDATE` of a column the index does not hold, since `bc_update` rewrites only the entries whose columns changed (plan_api.cpp:6486 on). A covering read (codegen.cppm:1590-1606) fetches nothing and records nothing more. |
| `bc_count_range` (CountRange, KeyAggregates' COUNT(*), the choice point's count) | its `[lo, hi)`. A count stopped at its limit records the whole range as well: a superset is safe, and the engine's count does not name the key it stopped at. |
| `bc_insert_on_conflict` that skips a row | the arbiter key it found taken, as a point. At SERIALIZABLE the skip is a read, which PG 17 also treats as one: "DO NOTHING skip, then skew" commits at REPEATABLE READ and is 40001 at SERIALIZABLE. This overrides, at SERIALIZABLE only, the owner's 2026-11-19 decision not to guard skipped rows (owner's question 1). |
| Runtime-internal reads: `pk_free`, `unique_free` (plan_api.cpp:2848, 2919), the old row of `bc_update`, `bc_delete` and `bc_insert_or_existing`, the walk of `bc_del_range` | the key as a point, or the unique prefix or deleted range as a range. Most are written keys that the engine's write-write check already covers. They are recorded anyway: an insert later deleted by the same transaction writes nothing at commit, but its `pk_free` still observed the key free, and the transaction acted on that. The commit drops the points it writes (*Mechanism: the serializable commit*). |

Other paths that read data, outside a plan's read calls:
- `bc_txn_rows`, the backfill's `RowSource` (plan_api.cpp:3716; database.cppm:3430), reads a table's rows for CREATE INDEX and CTAS in a transaction. It is covered by the backfill's `GuardRange` (plan_api.cpp:3745-3758), which every transaction keeps, and by the catalog-version guard of `commit_ddl`.
- `Database::row_counts` (database.cppm:1991-1999) counts rows on its own snapshot for the planner. It changes only the plan chosen, never a result, so it records nothing.

Foreign keys are not enforced (`CONTRACT.md`, :243), so there are no foreign-key reads. Procedures, subqueries, joins, aggregates, cursors and CTAS reach the data only through these calls, so they need nothing of their own.

**What a read records for phantoms.** A range covers every key a scan's answer depends on, the absent ones included.
- A cursor that ran to its end records its whole `[lo, hi)` (or its prefix range for `BC_SCAN_PREFIX`). A range that held no key is recorded all the same, and that is the empty-range phantom: PG 17 case 6, where each transaction counts an empty range and inserts into the other's, is 40001 at the second commit.
- A forward cursor closed early, after key `k`, records `[lo, k⁺)`. A reverse cursor records `[k, hi)`. `k⁺` is `k` followed by a zero byte. If that would pass `max_key_bytes` (engine `CONTRACT.md`, :73), `k⁺` is `key_successor(k)` (key_codec.cppm:387): the successor of `k`'s longest prefix that does not end in 0xFF, the shortest key above every key that starts with `k`. It is never empty and never past the object's keyspace end, because every key starts with the object's storage-id prefix, which `k` cannot exceed. A point read has no `hi` and needs none. Each choice is a superset.
- A cursor that returned no key before it closed records nothing.
- MIN and MAX read past NULL entries to their first value, and those entries are inside the recorded range.

What a range does not cover is any key outside it. The guard is a key range, never the whole table, so scans of disjoint ranges do not conflict. PG 17 does conflict on them (case 6b, a false positive from coarse locks).

**Recording never fails after data was read.** A read that is lost from the read set while the transaction goes on is a silent anomaly: under a savepoint, ROLLBACK TO takes a failed transaction back to open, and its commit would miss the guard. So each recording is reserved before the read returns anything:
- A point, and a count's range, are recorded (charged and inserted) before the read is made. A charge or allocation that fails fails the call with 54000 or `BC_E_MEMORY` before it has read anything, so nothing depends on the missing entry.
- A cursor's range is reserved at `bc_scan_open`: its interval node (a node handle, allocated then), and buffers for both bounds, at most `max_key_bytes` each, charged then. When the cursor closes, runs out, or its statement ends, whichever comes first (an error path included), the reserved entry is filled in and merged, which allocates and charges nothing, and the unused part of the charge is released. A failed reservation fails `bc_scan_open` before any key is read.
- A merge only frees: it erases swallowed points and joined intervals.

**The read set's shape.** The transaction keeps two sorted structures:
- **points**: a set of keys;
- **ranges**: an interval set, keyed by `lo`, that merges overlapping and adjacent intervals as they are added. A merge covers exactly the union, never a gap between ranges.

A point inside a recorded range is not kept, and a range that swallows points drops them. Two effects follow:
- Repeated reads, such as a correlated subquery run per row, nested-loop probes of one key, or a cursor's FETCH replaying its plan from the start (`cursor_design.md`, *Replay*), add nothing after the first time.
- A transaction's read set grows only with the distinct keys and ranges it read, not with the number of times it read them.

The read set is not part of a statement's pending effects, and the savepoint marks do not record its length or its charge (*Read-set memory and the 54000 bound*).

### Mechanism: the serializable commit

`bc_txn_commit` (plan_api.cpp:3892-3961) builds the `WritePlan` as today: range tombstones, then point writes, then `txn->guards`, then the record guards of written tables. A SERIALIZABLE transaction adds the following. Autocommit's `bc_ctx_commit` (:3246-3293) applies the same steps 1, 2, 4 and 5, with the context's Put, Del and DelRange effects as the final writes and the context's read set.

1. **A transaction whose final writes are empty and that ran no DDL drops its read set** and commits as today: "final writes" is the final overlay (`ov.empty()`), not what its statements did, so a transaction that inserted a key and deleted it again wrote nothing and is a reader placed at its snapshot. With no `FOR UPDATE` guards either it commits nothing; with them it submits a guard-only plan for them alone (*Read-only serializable transactions*). The emptiness test stays `ov.empty() && none_of(guards, counts)` (:3917-3921).
2. **Otherwise every read range becomes `ensure_range_unchanged`, and every read point becomes `ensure_unchanged`.** The exceptions:
   - A point the final writes put or delete is skipped, since the engine checks every written key against the snapshot.
   - A point on which a hand-written `bc_guard_present` or `bc_guard_absent` (plan_api.cpp:6639-6650) already sits is submitted as the one-key range `[k, k⁺)` instead. The engine refuses two different preconditions on one key with `std::logic_error` (engine `docs/transaction_design.md`, :131), and a one-key range guard has `ensure_unchanged`'s meaning. Generated plans never call these two; only hand-written plans need this. `FOR UPDATE`'s `ensure_unchanged` on a read point needs nothing: the engine allows repeating the same guard.
   - Empty ranges are dropped, as `bc_guard_range` drops them (plan_api.cpp:6630-6637).
3. **The unique-check prune is unchanged.** `guard_counts` (plan_api.cpp:3569-3571) still drops a unique check's `GuardUniqueRange` whose value the final writes no longer hold, at every level (#537). At SERIALIZABLE the `unique_free` lookup is also in the read set as a range, which step 2 never drops, so a concurrent commit that took the value is a conflict whenever the transaction writes anything.
4. **The catalog version is guarded.** Every SERIALIZABLE writer adds `ensure_unchanged(catalog_version_key())`, as `commit_ddl` already does for a transaction with DDL (plan_api.cpp:3861). Every published DDL moves it, so a writer placed at its commit never read a table, index, view, procedure, type or sequence whose definition changed since its pin. This covers what a list of objects would miss: `objects_read` names only the tables and indexes a plan touches (codegen.cppm:261; bc_plan.h:131), not the views, procedures and types it was bound through, and is empty for a plan folded to no read (`WHERE false`). A concurrent `CREATE OR REPLACE VIEW v`, a replaced procedure, or a DROP of a table a folded plan named, all commit before this writer, and the writer had used the old definition. It is one point guard per commit. The cost is that any concurrent DDL fails every overlapping SERIALIZABLE writer (owner's question 2; *Differences*).
5. **On `BC_CONFLICT`** the claim and lock check of *Commit and conflicts* runs unchanged.
   - A taken insert claim with no changed lock before it is 23505. Anything else is 40001.
   - A read is not a lock. A transaction that read a key absent and then inserted it, raced by a commit that took the key, gets 23505. PG 17 gives 23505 when the other transaction inserted the key without reading it first, at any level, and 40001 only when both read it absent before inserting (review probes s2, s3). The layer's rule therefore matches PG 17 when the other inserted blindly, and differs when both read first; there PG 17 reports 40001 at the INSERT and the layer 23505 at COMMIT. Owner's question 3 offers the alternative.
   - When the claims and locks decide nothing, the failure path probes the final overlay's point writes against the snapshot, which the engine leaves usable on a conflict (engine `CONTRACT.md`, :141). A written key changed since the snapshot gives "concurrent update" with no hint; otherwise the read/write message with its hint. This runs only after a conflict, and costs nothing on the success path. The engine does not report which guard failed, so the message cannot name a key, and PG 17 names none either.
   - The outcome's code is fixed before the message is built, as today.

The read set is copied into the `WritePlan` while it is still held, and each entry, plan slot included, was already counted against the bound from the call that recorded it (*Read-set memory and the 54000 bound*). The commit therefore never fails for memory its statements did not reserve, as *Limits* already promises.

### Read-only serializable transactions

A SERIALIZABLE transaction that wrote nothing is serialized at its snapshot, and needs no guard:
- Every SERIALIZABLE writer validates its whole read set, and the catalog version, at its commit's sequence. So at any sequence `s`, the committed SERIALIZABLE writers up to `s` have run as if one at a time, in sequence order.
- A snapshot at `s` holds exactly their effects. A transaction that only reads it is consistent with running after the last of them and before the next.

**PG 17's read-only anomaly** (Fekete et al.; case 4) is caught on the writer's side:
- T2 reads `ctl`. T1 bumps `ctl` and commits. T3 reads the new `ctl` and finds no receipts. T2 then inserts a receipt for the old `ctl`.
- In that order (T3 reads the receipts before T2 commits), PG 17 and this design agree: T3 commits, and T2 fails. PG 17 refuses T2's INSERT ("…pivot, during write") or its COMMIT ("…pivot, during commit attempt"); here T2 fails at COMMIT on its guard of `ctl`.
- If T2 commits before T3 reads the receipts, PG 17 commits T2 and cancels T3 at its SELECT, 40001 "Canceled on conflict out to pivot …, during read", even when T3 is declared READ ONLY (review probe s1). Here T2 fails at COMMIT and T3 commits. Both outcomes are serializable; a different transaction is refused, at a different point (*Differences*).

Consequences:
- **Readers never fail here.** A read-only SERIALIZABLE transaction commits whatever was committed since its snapshot (cases 3 and 14 commit in PG 17 too). PG 17 sometimes cancels a reader, as above.
- **Read-only costs only memory, unless declared.** The read set of a transaction that turns out not to write is still recorded until COMMIT, because a later statement could write. A transaction declared read-only before its first statement (`TransactionOptions::read_only`; the server's top-level READ ONLY, *Choosing the level*) records nothing: the runtime refuses its writes with 25006, so its reads can never be submitted. A READ ONLY that can be lifted again (inside a savepoint), or one set after reads were made, does not stop recording: a write before it, or after RELEASE, would commit with reads unguarded.
- **DEFERRABLE never waits.** PG 17's `SERIALIZABLE READ ONLY DEFERRABLE` waits for a snapshot that no concurrent transaction can invalidate. Every snapshot here is such a snapshot, so DEFERRABLE is taken and nothing waits.

**Followers.** SERIALIZABLE is refused on a follower with 0A000 "cannot use serializable mode in a hot standby", hint "You can use REPEATABLE READ instead.", as PG 17 refuses it on a hot standby (from PG's source and documentation; not run, as no standby is available). The refusal is made at `Database::begin`, at a server `BEGIN` or `SET` naming the level, and again at the statement that takes the snapshot, since the mode can change in between. Reason: a follower's snapshot lags the leader, so a read-only transaction there that began after a leader commit returned may not see it. That is serializable but not strict, and the row promises strict. Owner's question 5 keeps the alternative.

The claim that read-only transactions need no guard is not covered by any check today. Stage 1 adds an Elle run that mixes read-only transactions into the serializable run (*Stages of serializable isolation*).

### Read sets across savepoints, failed statements and cursors

**ROLLBACK TO keeps the reads.** PG 17 keeps the reads of a rolled-back subtransaction (cases 5 and D: a read inside a savepoint, ROLLBACK TO, then skew, is still 40001 at commit). The client saw those rows and may have decided on them. The read set is therefore not cut back at a mark: `bc_txn_rollback_to` truncates `txn->guards`, claims and locks as today, and leaves the read set and its charge alone. RELEASE changes nothing in it.

**A failed statement's reads are kept.** A failed statement discards its pending effects (*Statements inside a transaction*), but its reads are already in the read set, since they are recorded before they are made. The error itself, and any rows sent before it, depended on them. After an abort without marks the transaction can only roll back, so this matters only with savepoints.

**A no-DDL 40001 still ends the transaction.** The serializable check runs at COMMIT, so there is no statement-level 40001 to recover with ROLLBACK TO. PG 17's statement-level 40001 is recoverable that way ("40001 at a statement, then ROLLBACK TO s; SELECT 1; COMMIT" commits). This is listed under the differences.

**Cursors inside the transaction.** A FETCH replays the plan on the pinned snapshot and overlay (`cursor_design.md`, *Replay*). Its reads are recorded like any statement's, while the transaction is open, so a replay that reads other keys than the first run (a plan calling `random()`, below) records those too. The read set's merging keeps identical replays from growing it. A cursor's reads go to the transaction that declared it. A cursor reading through a pin reads the pinned overlay; its recorded keys are still keys of the snapshot's keyspace, and the engine checks them against the transaction's one snapshot.

**WITH HOLD is refused in a SERIALIZABLE transaction that is not read-only**, with 0A000. A hold cursor is replayed lazily after COMMIT on a clone of the snapshot (`cursor_design.md`, *WITH HOLD*), and fetches after COMMIT belong to no transaction, so their reads could not join the commit's check. PG 17 accepts it and runs the query to completion at COMMIT (case 9). In a read-only SERIALIZABLE transaction, or a declared READ ONLY one, WITH HOLD is accepted: the transaction is placed at its snapshot, and the cursor reads that snapshot. Lifting the refusal is a follow-up issue (owner's question 4). Running the query once at DECLARE to record its reads is not enough on its own: a plan that calls `bc_random` (codegen.cppm:1677-1680; `random(lo, hi)` is drawn anew at each evaluation, frontend_design.md:438-440) can read other keys at a later replay. `cursor_design.md`:205's "Randomness … None in a plan" is stale for the same reason, which affects ordinary cursor replay too; that is its own follow-up issue.

### Serializable transactions with DDL

- **Rule A stands.** A SERIALIZABLE transaction binds against its pin and its own DDL, like any other.
- **A transaction with DDL commits through `commit_ddl`,** which guards `catalog_version_key` (plan_api.cpp:3853-3861). Its read set is added the same way. A backfill's `GuardRange` (plan_api.cpp:3745-3758) is kept as it is: it is a read of the rows the backfill copied. Both DDL row scans emit it (catalog.cppm:1162-1164).
- **The doom rule** (*Catalog snapshots and isolation levels with DDL*) is unchanged.
- **CREATE TABLE AS in a transaction** reads through the same calls, so its source query's reads are recorded. PG 17 commits case 10 (read a count, concurrent insert, CREATE TABLE, INSERT…SELECT, COMMIT). Here, if the concurrent insert lands inside a range the transaction read, its commit is 40001: the single-edge false positive below.
- **Autocommit CREATE TABLE AS at SERIALIZABLE** runs as a one-statement SERIALIZABLE transaction through the transactional CTAS path, so its query's reads are guarded by `commit_ddl`, and it is retried on 40001 as autocommit statements are. Today's autocommit CTAS (database.cppm:2525-2575) reads on one snapshot (`run_auto`) and commits on a later pin (`run_ddl`), with nothing tying the two. At `Snapshot` that path stays.
- **Other autocommit DDL** is not affected by a session's level: it reads no data except through a backfill, whose guard it already has.

### Read-set memory and the 54000 bound

**Charging.** The read set is held to a bound of its own kind, by a conservative size function in the style of `effect_bytes` (algebra/runtime.cppm):
- each point: its buffer, the set's node and the plan slot it will take at commit;
- each range: both bounds, the node and the slot;
- an open cursor's reservation (*Recording never fails after data was read*) until its close.

**Which bound.**
- In a transaction, `TransactionOptions::max_bytes`, beside the transaction's other holdings, in a counter of its own, `read_bytes`, that `txn_holds` and the memory cap's reservation include. `read_bytes` is not saved in `TxnMark` and not restored by `txn_restore` or `txn_abort` (plan_api.cpp:1597-1610, 1852-1877), since the reads it counts are not removed. Charged to `held`, a loop of SAVEPOINT, read, ROLLBACK TO would grow the read set while its charge dropped back each time, and the commit's copy could pass both `max_bytes` and the memory cap.
- In autocommit, the statement's budget, `bc_budget::max_bytes` (`Options::statement_memory_bytes`), as the statement's write set is (*Plan safety*, CONTRACT.md:63).

This is a memory charge only. Recording does no record read and charges no work to the statement's read budget.

**Size.** A point on a 20-byte key is charged 349 B (`read_point_bytes`: its buffer, its node, and the larger of its two guard entries), so about 192 000 distinct point reads fill the default 64 MiB. A scan costs one range however many rows it reads. Index scans that fetch rows, and join probes, cost one point per distinct row.

**The bound.** The read call that would take the read set past its bound fails its statement with `BC_E_LIMIT`, SQLSTATE 54000, before it reads. Its message names SERIALIZABLE and the bound. As with any failed statement, the transaction aborts. The reads recorded before it stay, so a ROLLBACK TO does not make room among them; it frees only writes. A declared read-only transaction, and an autocommit plan that writes no table, record nothing and never reach it.

**No coarsening.** The design does not turn many point reads on one table into a whole-table range when they grow. PG 17 promotes predicate locks to the relation (`max_pred_locks_per_relation`), but here a whole-table guard costs O(table) at every commit that carries it, possibly inside the engine's writer section (*Read-guard cost at commit and the writer critical section*). That would make one transaction's memory saving every writer's latency. A transaction whose reads do not fit fails with 54000, which is predictable. Coarsening when the bound would be reached is a follow-up issue if a workload hits 54000.

**Range merging is a superset only by construction.** Two ranges merge only when they overlap or touch, so merging never adds a key no scan covered, and never adds a false positive.

### Read-guard cost at commit and the writer critical section

What the engine does with the guards (engine_state.cpp:105-219, 256-321):
- **A point guard** costs two key-directory lookups: one on the head, one on the snapshot.
- **A range guard** costs a walk of the head's keys in the range and a walk of the snapshot's, one lookup per key: linear in the keys it covers (bytecask_design.md:150).
- **Outside the lock.** `prepare_validation` answers both on the caller's thread, outside `write_mu_`, for a plan with writes and no range delete whose range guards each lie inside one prefix. Before stage 3 that was only exact prefix ranges (`is_prefix_range`); since stage 3, any range whose keys all share a prefix: an index range's `[lo, hi)` lies inside its index's 5-byte prefix `0x03 index_id`, and a row range inside its table's.
- **Inside the lock.** Any other plan is checked inside the serial section, as is any plan whose `RecentWrites` probe says a key under a prefix it read may have been written since. Before stage 3 every SERIALIZABLE writer that scanned an index range was checked in the section, at O(keys in its ranges) while every other writer waited; since then that walk is made in the section only when the index (or table) its range lies in was written since the writer's preparation.

**What this means:**
- Point reads are cheap.
- The read set bounds the *number* of ranges, not the keys they cover. Merged ranges can cover a whole index, and statement budgets bound each statement, not the transaction. Before stage 3, one SERIALIZABLE writer that scanned index ranges could hold every writer, snapshot-isolation writers that never opted in included, for a walk of O(keys covered) at its commit. That ran against predictable latency (principle 3). Since stage 3 it does so only while the index it read is being written between its preparation and its turn in the section.
- A full scan's range is a table prefix, and it is checked outside the lock unless that table was written since the snapshot.

**Measured before the server exposes it.** Stage 1 adds a writer-latency benchmark: snapshot-isolation writers' p99 alone, and alongside concurrent SERIALIZABLE writers that scanned index ranges of 100, 10 000 and 1 000 000 keys. Owner's question 6 decides, with those numbers, what lands before stage 2 lets any client turn SERIALIZABLE on with `SET`:
- **(a) A range guard inside one registered prefix**, prepared outside the lock by probing that prefix's hash, as exact prefix ranges are today. The section then walks the range only when the table was written since. This is stage 3.
- **(b) A bound on what a transaction's in-section ranges may cover:** the keys its cursors stepped over, which they already count, capped per transaction, with 54000 past it.
- **(c) Sublinear range checks.** A range is unchanged since `s` exactly when no key in it carries a sequence above `s` and its key count equals the snapshot's. With a per-node maximum sequence in the key directory, the check is O(log n). This needs a key-directory design of its own, with `map_bench` and the tests in `tests/CLAUDE.md`. It is a follow-up issue, not a stage.

**Read-guard cost measured** (stage 1, before stage 3; stage 3's numbers are in *Stage 3 as built*; `sql_layer_bench` in releasedbg, a 4-core machine, kv of 1M rows with an index on `g = k`; one run each, so the numbers show size, not small differences):

| Transaction, µs | Snapshot | Serializable |
|---|---|---|
| ten point reads and one write | 208 | 254 |
| an index range of 100 rows fetched, one write | 338 | 786 |
| an index range of 10 000 rows fetched, one write | 12 685 | 43 289 |
| a read-only full scan of 1M rows | 456 333 | 448 424 |

| Snapshot-isolation writers (4 threads, autocommit UPDATE of a row each), µs | p50 | p99 | p99.9 | writes in 6 s |
|---|---|---|---|---|
| alone | 320 | 1 264 | 3 571 | 63 503 |
| with 2 Serializable writers reading covering index ranges of 100 keys | 617 | 3 735 | 5 561 | 31 931 |
| … of 10 000 keys | 32 541 | 46 217 | 48 432 | 811 |
| … of 1 000 000 keys | 382 | 4 261 | 6 464 | 7 651 |
| alone, again | 312 | 1 451 | 3 947 | 62 554 |

The Serializable writers committed 10 876, 352 and 4 transactions in the three runs, none 40001. The effect on snapshot-isolation writers is measurable: at ranges of 10 000 keys their p50 rises a hundredfold, as each Serializable commit's range is walked in the engine's writer section while they wait. Some of the 100-key run's doubling is the two extra threads on four cores. At 1 000 000 keys each Serializable transaction spends most of its time reading, so its few commits stall writers rarely; the p99 of a 6 s run does not catch them. Question 6's recommendation for a measurable effect is (a): stage 3 lands before stage 2. The owner decides.

TPROC-C (`sql_layer_tprocc run`, 4 warehouses, 30 s, releasedbg): REPEATABLE READ with `FOR UPDATE` 99 356 TPM at 1 thread and 219 648 at 4, with 14 retries on 40001; SERIALIZABLE with New-Order's district read without `FOR UPDATE` 85 038 and 203 044, with 10 retries. Every run kept the consistency conditions.

**Per-read cost in the layer:**
- At snapshot isolation, one branch per read call. Stage 1's PR shows `sql_layer_bench` before and after, to show that this costs nothing measurable.
- At SERIALIZABLE, one ordered-set insert per distinct point read: a key copy and O(log n) comparisons. One interval insert per cursor close, into a node reserved at its open.

### Engine guarantees serializable isolation rests on

These are quoted from the engine's `CONTRACT.md`. No engine row changes.
- **Isolation (:142):** "A key it read and does not write takes part in the check only under `ensure_unchanged`, a range only under `ensure_range_unchanged`: guarded, the plan is strict serializable against every other write: there is one order of all writes that every outcome agrees with, and a write that returned before another began comes first in it; unguarded, it has snapshot isolation." Proved by `scripts/run_isolation_check.py`, nightly, including the range workload, under vacuum, `fdatasync` faults and SIGKILL.
- **No trace on conflict (:141):** "When a guard, a range guard or the check on the plan's own write set fails, the call returns `nullopt`: nothing is appended, nothing becomes visible, and the plan's snapshot stays usable." So a failed SERIALIZABLE commit writes nothing, and commit step 5's probe can read the snapshot.
- **Guard-only plans (:144):** "A guard-only plan commits nothing. Its guards are checked like any plan's." This is used only for `FOR UPDATE` guards, since read-only transactions submit no read guards.
- **Range bounds (:59)** must satisfy `from < to`, or the call throws `std::invalid_argument`. The commit drops empty ranges. **Key size (:73):** `max_key_bytes` applies to every guard bound, which is why `k⁺` falls back as described above.
- **Followers (:224):** `apply_batch` throws `DbFollowerMode`. SERIALIZABLE is refused on a follower before it gets there.
- **Semantics** (engine `docs/transaction_design.md`, :230; bytecask_design.md:143, :377):
  - guards compare sequences, so ABA is caught;
  - a key absent at both times is unchanged;
  - a key vacuum relocated is unchanged;
  - a key inserted and deleted since the snapshot is not a change for a range. That is safe: the transaction's answer for that range is the same at both points.
- **One precondition per key; repeating the same guard is allowed** (engine `docs/transaction_design.md`, :131): handled by step 2 of the commit.

The engine's `docs/transaction_design.md`:242 still says `ensure_range_unchanged` and phantoms are not part of the isolation check, which contradicts :240 and `CONTRACT.md`:142. It is fixed in a separate small change (follow-up issue).

### Differences from PostgreSQL 17 for serializable isolation

Each row was observed on PG 17.12 except where it says otherwise.

| PG 17 (SERIALIZABLE, SSI) | This layer | Why |
|---|---|---|
| A single read-write anti-dependency commits: T reads k1, another commits k1, T writes k2 (cases A, B). Likewise T inserts a unique value and deletes it, a concurrent insert of it waits for T, and both commit (review probe s4). | T's commit is 40001 when T writes anything (in the insert-then-delete case, anything else); nothing waits. | A changed read set is a conflict: no dependency graph is kept. Guard-at-commit refuses every writer whose reads changed: a superset of PG's dangerous structures, never a subset. |
| SERIALIZABLE is protected only against other SERIALIZABLE transactions; write skew against a READ COMMITTED one commits both (case C). | The SERIALIZABLE side is 40001. | The engine checks guards against every write. |
| Conflicts surface at the statement where PG detects them ("…during write", cases 2, 4, 8; ON CONFLICT case 12) and are recoverable by ROLLBACK TO a savepoint. | At COMMIT only, and a no-DDL 40001 ends the transaction. | Never wait, and no shared conflict state: the check is the commit's. |
| A reader, READ ONLY included, can be cancelled at a SELECT once a pivot has committed: "Canceled on conflict out to pivot …, during read" (review probe s1). | Readers never fail. The pivot writer fails at its COMMIT instead, so PG commits it and cancels the reader where this layer does the reverse. | Read-only transactions are placed at their snapshot; only writers are validated. |
| A failed transaction's DETAIL names a reason code ("Canceled on identification as a pivot, …", "…during read"). | No DETAIL. The message and the hint are PG's. | The engine reports that a guard failed, not which. |
| Concurrent DDL: CREATE INDEX on a table a SERIALIZABLE writer read runs, and both commit; ACCESS EXCLUSIVE DDL on it (ADD COLUMN, DROP, CREATE OR REPLACE VIEW of a view it read) waits for the writer; DDL on objects it did not use runs, and both commit (review probe s2). | Any DDL committed between a SERIALIZABLE writer's pin and its commit makes the writer's COMMIT 40001; the DDL never waits. | One guard on the catalog version (commit step 4) covers every catalog object; a per-object list can miss one. Owner's question 2. |
| Disjoint range reads on a small table conflict (case 6b): page- and relation-level predicate locks. | They commit. | Guards are exact key ranges. |
| Many fine-grained predicate locks are promoted to a relation lock (`max_pred_locks_per_relation`, `max_pred_locks_per_transaction` 64). | No promotion. A read set past its bound is 54000. | A whole-table guard costs O(table) at commit. |
| `READ ONLY DEFERRABLE` may wait for a safe snapshot. | Never waits; every snapshot is safe. | Writers validate at their commit sequence. |
| Concurrent insert of a key both transactions read absent first: the second INSERT is 40001 ("pivot, during write"). When the other inserted without reading it, at any level: 23505 (review probes s2, s3). | 23505 at the second commit in both cases. | *Commit and conflicts*' rule; it matches PG when the other inserted blindly. Owner's question 3. |
| An autocommit statement at SERIALIZABLE that conflicts is 40001 (case 8). | Retried up to three times, then reported. | Autocommit's existing retry; nothing of the caller's runs between attempts. |
| `DECLARE … WITH HOLD` in a SERIALIZABLE transaction is accepted; its query runs to completion at COMMIT (case 9). | 0A000 in a SERIALIZABLE transaction that is not read-only; accepted in a read-only one. | Fetches after COMMIT belong to no transaction, so their reads could not join the commit's check. Owner's question 4. |
| `ON CONFLICT DO NOTHING` over a row a concurrent transaction deleted inserts the row. | At SERIALIZABLE, a transaction's commit is 40001, and an autocommit statement's retry inserts the row. At other levels the row is skipped, as today. | The skip is a read at SERIALIZABLE. Owner's question 1. |

### Guarantees and CONTRACT rows serializable isolation changes

`bytecaskdb-sql-layer/CONTRACT.md`, *Transaction isolation* and neighbouring sections. Each change is named in its stage's PR. Every row names tests that `scripts/check_contract_tests.py` can find; the Elle runs are named by the backticked path `bytecaskdb-sql-layer/scripts/sql_isolation_check.py`.

- **Added** (stage 1): a SERIALIZABLE row.
  - It states the serial order of *Serializable semantics*: a writer at its commit, a reader at its snapshot, real-time order kept, checked against every concurrent write and DDL.
  - It states that a conflict is 40001 at COMMIT with nothing written, and the two messages.
  - It states that SERIALIZABLE is 0A000 on a follower.
  - Proved by the stage 1 tests and the Elle configurations of stages 0 and 1.
- **Added** (stage 1): "read absent, then insert, raced" is 23505 at commit, at SERIALIZABLE too; PG 17's 40001 when both read first is listed as a known difference.
- **Added** (stage 1): `TransactionOptions::read_only`: 25006 for each write, guard and DDL, in the runtime.
- **Extended** (*Plan safety*, :63): the memory row's list of what is charged gains an autocommit statement's read set; the transaction bound gains the read set, which ROLLBACK TO does not release. A read set past its bound is 54000 at the read, never at COMMIT.
- **Unchanged** (:269): "Statements the compiler generates run under snapshot isolation" stays true at SERIALIZABLE, which is stronger: one snapshot per statement or transaction, write-write conflicts caught.
- **Removed** (:270): "Serializable conflict detection only with hand-written guards …; generated statements emit none beyond the unique checks'". The SERIALIZABLE row replaces it. Hand-written guards stay valid, and the FOR UPDATE row is unchanged.
- **Fixed** (:273, stage 0): "Isolation against concurrent histories of many statements is not checked by any tool yet: none" is stale. `bytecaskdb-sql-layer/scripts/sql_isolation_check.py` exists. The row becomes what the tool checks, naming the nightly job.
- **Narrowed** (the 23505 row, :186): "A unique value a later statement moved or deleted away … guards nothing, and a transaction whose final writes hold no entry under that value commits" holds at `Isolation::Snapshot`. At SERIALIZABLE the unique check's lookup is a read like any other, so a transaction that writes something else gets 40001 when a racing commit took the value.
- **Narrowed** (the ON CONFLICT DO NOTHING row, :238): at SERIALIZABLE a skipped row's arbiter key is a read, so a concurrent change to it is 40001 at a transaction's commit, and an autocommit retry. The owner's 2026-11-19 decision holds below SERIALIZABLE (owner's question 1).
- **Extended** (the savepoint row, :184): at SERIALIZABLE, ROLLBACK TO does not undo reads, which still take part in the commit's check, as in PG 17.
- **Narrowed** (the cursor row, :189): `DECLARE … WITH HOLD` in a SERIALIZABLE transaction that is not read-only is 0A000.
- **Added** (stage 2, server rows): BEGIN, START TRANSACTION and SET TRANSACTION take ISOLATION LEVEL SERIALIZABLE; the 25001 rules, including BEGIN of an implicit block and DEFERRABLE's two; SET, RESET, SET TO DEFAULT, RESET ALL and SHOW of the three defaults and `transaction_isolation`, `transaction_read_only`, `transaction_deferrable`, and SET SESSION CHARACTERISTICS, with their SQLSTATEs; the defaults are transactional (undone by ROLLBACK and ROLLBACK TO).
- **Unchanged:** the engine's `CONTRACT.md`. No engine row changes unless stage 3 is taken. Stage 3 changes mechanism only: the guarantee stays *Isolation is what the plan guards*, and `bytecask_design.md` describes the new preparation.

### Stages of serializable isolation

Each stage is one PR that ships on its own. Each updates `bytecaskdb-sql-layer/README.md` (:15 "Snapshot isolation by default" stays true; :64's refusal list loses SERIALIZABLE in stage 2), names its contract changes, and says what it still refuses. No stage changes durability: nothing new is written to disk, and no sync, rename or unlink is added. So no fault sweep over a durability site and no crash test are owed. The commit with guards under `fdatasync` faults and SIGKILL is the engine's, proved by its nightly Elle run. Stages 0 to 2 leave the engine untouched, so `engine_bench` does not apply. Stage 1's PR shows `sql_layer_bench` before and after.

**Stage 0: the SQL isolation check runs nightly, and covers ranges.** Built. One adaptation: the range workload's ranges are fixed, drawn once from the seed, not per transaction as in the engine's workload. A SQL commit reports no sequence to replay the history against, and with fixed ranges the invariant is global, so the check needs no commit order (`testing_design.md`, *The TPROC-C harness*). Blind must break the invariant at least once over the run, as unguarded must. The check runs in its own workflow, `.github/workflows/sql-layer-isolation-nightly.yml`, not as a job of the engine's `isolation-nightly.yml`, so a pull request to one side does not run the other's check.
- Add `--workload range` to `sql_layer_tprocc elle` (harness/main.cpp:171-183, tprocc.cppm:1088-1250): the engine range workload's shape, through SQL, with the same invariant: a range that held a key keeps one (engine `docs/isolation_checking_design.md`, *Range workload*). A transaction runs `SELECT k FROM elle_r WHERE k BETWEEN $1 AND $2`, then deletes one row when it saw two or more, and sometimes inserts first. Under the existing `unguarded` configuration it must break the invariant at least once over the run.
- The existing `unguarded` configuration (`ElleConfig::Unguarded`, tprocc.cppm:160) already requires write skew as its only anomaly, at least once over the run (`bytecaskdb-sql-layer/scripts/sql_isolation_check.py`:16-18, 52-118), so it shows the checker can see what SERIALIZABLE must prevent. No new configuration.
- Run `bytecaskdb-sql-layer/scripts/sql_isolation_check.py` in `.github/workflows/isolation-nightly.yml` with a short budget: guarded, unguarded and blind, each over the point and range workloads.
- Contract: :273 fixed.
- Docs: `testing_design.md`, *The TPROC-C harness*.
- No behaviour change.

**Stage 1: SERIALIZABLE transactions and autocommit statements in the embedded API.**
- **Code:**
  - `Isolation`, `TransactionOptions::isolation` and `::read_only`, `ExecutionOptions::isolation`, `Transaction::set_isolation` and `::set_read_only`; 25006 in the runtime for a read-only transaction;
  - the runtime's level switch (`bc_txn_new`, `bc_ctx_set_isolation`), the recording reader, and the read set with its reservations and its `read_bytes` charge;
  - recording in every read call of the table above;
  - the commit's steps 1, 2, 4 and 5 in `bc_txn_commit`, `commit_ddl` and `bc_ctx_commit`;
  - ROLLBACK TO and failed statements keeping reads; `bc_ctx_begin` clearing an autocommit read set; `bc_txn_repin` refusing a non-empty one;
  - autocommit CTAS at SERIALIZABLE through the transactional path;
  - WITH HOLD refused with 0A000 in a SERIALIZABLE transaction that is not read-only;
  - SERIALIZABLE refused on a follower.
  - The codegen header's fixed `isolation: snapshot` goes; the golden files are regenerated for it.
- **Tests** (`api_serializable_test.cpp`, "serializable: …"). The interleavings are forced with two handles on one thread, which never wait, and with `bc_ctx_before_commit_for_testing`:
  - write skew (case 1): the second commit is 40001 with nothing written, and the same at `Snapshot` commits both (case 1b);
  - the empty-range phantom (case 6) is 40001; disjoint ranges (case 6b) commit;
  - equality reads on an index, cross updates (case 15); MIN and MAX then cross inserts; COUNT(*) of a point and of a range; EXISTS; LIMIT stopping early, where an insert past the stop point commits and one before it is 40001, forward and reverse;
  - `k⁺` at `max_key_bytes`, and a key ending in 0xFF bytes, falling back to `key_successor`;
  - an index scan that fetches rows, raced by an update of a column the index does not hold: 40001; the same through a covering read commits;
  - a join probe, and a correlated subquery, whose probed key a concurrent commit inserts: 40001; the probe repeated a thousand times charges one point;
  - a procedure whose body reads, called in a SERIALIZABLE transaction, then skew: 40001;
  - the read-only anomaly in both orders (case 4; review probe s1): T2 is 40001, T3 commits; read-only transactions whose reads changed commit (cases 3, 14);
  - a single anti-dependency (cases A, B) is 40001, the documented difference;
  - a read inside a savepoint, ROLLBACK TO, then skew is 40001 (cases 5, D); a failed statement's reads stay;
  - SAVEPOINT, read, ROLLBACK TO in a loop: 54000 at a read, never at COMMIT, with `read_bytes` never restored;
  - ON CONFLICT DO NOTHING: skip then skew is 40001; a concurrently deleted arbiter row makes an autocommit retry insert the row;
  - a unique value inserted then deleted by a transaction that also writes another key, raced by a commit that took the value: 40001 at SERIALIZABLE, commits at `Snapshot`; the same transaction with no other write commits (a reader);
  - insert-then-delete of one key and nothing else: no read guard submitted, commits;
  - concurrent DDL: ADD COLUMN on a table only read, CREATE OR REPLACE VIEW of a view read, a replaced procedure called, a DROP of a table a `WHERE false` plan named, then a write elsewhere: 40001 each; the same transactions read-only commit;
  - a transaction with DDL and write skew, committing through `commit_ddl`: 40001;
  - CREATE TABLE AS in a transaction and in autocommit, raced by an insert into its source range (case 10): 40001 in a transaction, a retry in autocommit;
  - a hand-written `bc_guard_present` on a read key commits without `std::logic_error`;
  - an autocommit statement that conflicts once commits on retry, submitting exactly one attempt's guards; one that always conflicts reports 40001 after its last attempt;
  - an autocommit SELECT reading more than the statement budget's worth of points succeeds (it records nothing); a declared read-only transaction doing the same succeeds, and its INSERT is 25006;
  - `bc_txn_repin` with a non-empty read set returns false;
  - SERIALIZABLE on a follower: 0A000 at `begin`, at the snapshot-taking statement after a demotion, and for an autocommit statement;
  - 54000 at the read that passes the bound, and COMMIT never fails for memory;
  - a cursor's FETCH replays add nothing to the read set;
  - the two 40001 messages (a written key changed: "concurrent update", no hint; otherwise read/write with hint) and 23505 for a taken insert claim;
  - a randomized model test of the read set (interval merge, adjacency, points swallowed by ranges, dedupe) against a naive list of keys and ranges.
- **Mutations:** one patch per recording site, and per reservation, under `bytecaskdb-sql-layer/tests/serializable_mutations/`, each with a `Guarded-by:` header naming the test it must fail, checked by `scripts/check_guarded_by.py` and run in CI (`tests/CLAUDE.md`, *Mutation sets*). Also: a mutation that saves `read_bytes` in `TxnMark`, one that drops the catalog-version guard, and one that skips `bc_ctx_begin`'s clear.
- **Faults:**
  - an allocation-failure sweep over a SERIALIZABLE transaction's statements and commit, reaching each read-set transition (new point, new range, merge, swallow, reservation fill), under a savepoint: inject `BC_E_MEMORY` and 54000 at each recording site, ROLLBACK TO, then cause skew; the transaction must never commit;
  - a read failure injected under each recording call: the statement fails and nothing is recorded as "absent".
- **Proof:** `sql_layer_tprocc elle --config serializable`, the stage 0 workloads at `Isolation::Serializable` with no `FOR UPDATE`, must be strict-serializable. Its run is in the PR, and it joins the nightly job. It has three runs:
  - the point workload;
  - the range workload, which must keep its invariant;
  - a run with a third of the transactions read-only, which must stay strict-serializable. This is the check of *Read-only serializable transactions*.
- **Bench:**
  - `sql_layer_bench` before and after at the default level: the cost of the branch;
  - new SERIALIZABLE rows: a point-read transaction of ten reads and one write; an index-range transaction reading 100 and 10 000 rows, then writing one; a read-only full scan;
  - TPROC-C at SERIALIZABLE against REPEATABLE READ with `FOR UPDATE`: throughput and the 40001 rate;
  - writer p99 at snapshot isolation, alone and alongside SERIALIZABLE index-range writers (*Read-guard cost at commit*). Its numbers decide owner's question 6.
  None of these is claimed as an improvement.
- **Contract:** the SERIALIZABLE, 23505-race and `read_only` rows added; :63 extended; :270 removed; :186, :238 narrowed; :184 extended; :189 narrowed.
- **Docs:** this section's *Edits*; `plan_api_design.md`:1151-1158 and the example at :1222-1250 (recording, not emitted guards); `CONTRACT.md`.

**Stage 1 as built.** Where the code differed from the design above, the design's intent was kept:
- **The final writes.** The overlay keeps a tombstone for a key a transaction inserted and deleted again, so `ov.empty()` is false for it. Commit step 1 therefore asks whether the final writes only delete keys the snapshot does not hold (one lookup per tombstone, none once a put or a range is found): such a transaction writes nothing, and is a reader, its tombstones dropped with its reads. Below SERIALIZABLE the commit is as before.
- **One recording reader.** A read reaches the snapshot only through `bc_ctx::view(Recorded)`, and a `Recorded` token is made only by the recording calls (`record_point`, `record_range`, `record_scan`), so a read that skips them does not compile. The commit still moves the snapshot from the context into its `WritePlan`, as before.
- **A cursor's reservation** keeps its range's bounds and the last key it handed out in buffers of its own, the last key with room for k⁺'s zero byte, and reserves the charge of the largest range it can record (a bound of `kMaxKeyBytes` + 1 bytes for the key). Filling it in at close swaps those buffers into the reserved node: nothing is allocated or charged then.
- **The old row of `bc_update` and `bc_delete`** is not recorded again: the plan's read that returned it recorded its key, and the key is written.
- **Read-only refusal.** `Transaction::execute` refuses a statement that writes, a `SELECT … FOR UPDATE` and DDL in a read-only transaction before it runs, and `open_cursor` a `FOR UPDATE` cursor as it opens (as PostgreSQL refuses it at DECLARE, over an empty table too), with PostgreSQL's 25006 text naming the command; a procedure's writing statement is refused as it starts. The runtime refuses the same on its own (`bc_ctx_run` of a plan that writes, each guard call, `bc_txn_ddl`) with `BC_E_READ_ONLY`.
- **The 25001 checks' order** is PostgreSQL 17's. `set_isolation`: after a query (`bc_txn_queried`: a statement run, a cursor pinned, or a read made) first, then an open savepoint, then a follower's 0A000. `set_read_only(false)` in a read-only transaction: an open savepoint first ("cannot set transaction read-write mode inside a read-only transaction"), then a query.
- **Fault tests.** The read-failure test (`transaction_test.cpp`, "a read that fails is the statement's error, never absent") runs at both levels and covers ON CONFLICT's arbiters, by primary key and by unique index, for DO NOTHING and DO UPDATE. A sweep of the transaction's bound (54000) over every recording site (`bc_get`, `bc_exists`, a scan with `bc_scan_row`, `bc_count_range`, `bc_del_range`, an insert's primary-key and unique checks, both ON CONFLICT forms by both arbiters) fails each statement at its read or write, keeps no read past the bound, and leaves the read set charged what it holds.
- **Mutations** cover every recording site, ON CONFLICT DO UPDATE's two probes and `bc_del_range`'s range included. Three are "Expected: NOT caught", each with its reason: the primary-key check of an insert and DO UPDATE's primary-key probe, whose key is always written, and `bc_del_range`'s range, which the engine checks as a planned range delete. No SQL statement plans `bc_del_range` today; its test is hand-written.
- **`set_read_only(true)` after the first statement** is 25001 "transaction read-only mode must be set before any query"; PostgreSQL accepts it, and the server, which keeps READ ONLY itself, enforces it there (stage 2).
- **CREATE OR REPLACE VIEW** is not in the dialect: its test drops the view and creates it again.
- **Measured size.** A point on a 20-byte key is charged 349 B (`read_point_bytes`), so about 192 000 distinct point reads fill the default 64 MiB; a range on two 20-byte bounds 416 B.
- **The bench rows and the writer-p99 numbers** are in *Read-guard cost measured* (*Read-guard cost at commit and the writer critical section*).

**Stage 2: the server.** Lands after whatever owner's question 6 decides must precede it.
- **Code:**
  - ISOLATION LEVEL SERIALIZABLE in BEGIN, START TRANSACTION and SET TRANSACTION;
  - the 25001 check in BEGIN of an implicit block; DEFERRABLE's two 25001s;
  - the three transactional session defaults, SET SESSION CHARACTERISTICS, SET/RESET/SET TO DEFAULT/RESET ALL/SHOW as listed, with quote-aware value parsing;
  - top-level READ ONLY passed to the transaction;
  - SERIALIZABLE refused on a follower.
- **Tests:**
  - wire tests over simple and extended Query for each SQLSTATE and value under *Choosing the level*, including: `SELECT 1; BEGIN ISOLATION LEVEL SERIALIZABLE` in one simple Query (25001); DEFERRABLE in a savepoint and after a query with an unchanged value (25001 each); a SET of a default undone by ROLLBACK, by the ROLLBACK of a failed block and by ROLLBACK TO, and kept by COMMIT and RELEASE; `SET LOCAL` outside a block (25P01); an unquoted two-word value (42601); `RESET transaction_isolation` (0A000); `RESET ALL`; SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY then a write (25006);
  - `BEGIN ISOLATION LEVEL SERIALIZABLE; SAVEPOINT s; SET TRANSACTION READ ONLY; SELECT …; RELEASE s; INSERT …` raced by skew: 40001 at COMMIT (the read inside the window was recorded);
  - the write-skew and phantom cases over two connections, comparing outcomes with PG 17 where they agree, and asserting the documented differences;
  - the proof machine gains `BEGIN ISOLATION LEVEL SERIALIZABLE` rules, compared with PG 17 SERIALIZABLE on one connection, where results agree;
  - the functional suite: transactions.yaml:107-124 loses `unimplemented`; not_implemented/sqltest.txt:70-71 loses e152_01_01_01 and _02; the conformance table is refreshed from a `--conformance` run.
- **Contract:** server rows added.
- **Docs:**
  - `server_design.md`: the transaction-modes text at :371-389, and SET/SHOW;
  - `README.md`:64.

**Stage 2 as built.** Where the code differed from the design above, the design's intent was kept:
- **Question 6 first.** Stage 3 landed before this stage, as question 6 (a) asks. Its numbers (*Stage 3 as built*) show the snapshot writers' remaining p99 beside two range-reading writers matching a control whose range reads submit no guard, so the range checks no longer measurably affect other writers at the sizes benchmarked; (b), a per-transaction cap on in-section range keys, is not built.
- **`SELECT 1; BEGIN ISOLATION LEVEL SERIALIZABLE` in one simple Query** cannot be sent: the server refuses several statements in one Query (0A000). The 25001 of BEGIN after a query is tested in its other form, the same statements as an extended-query pipeline before one Sync, where PostgreSQL 17 gives the same 25001 and leaves the session idle (I): the modes are checked before the implicit block becomes explicit, so a refusal rolls it back.
- **BEGIN inside a block** warns 25001, as before, and then sets its modes as SET TRANSACTION does, as PostgreSQL's BEGIN does: a change of level after a query fails the block.
- **SET, SHOW and RESET** are read by libpg_query (`ast::parse_variable`), so values are quote-aware and a syntax error is PostgreSQL's 42601 with its position. `SHOW TRANSACTION ISOLATION LEVEL` is `SHOW transaction_isolation`, as in PostgreSQL. `SHOW client_encoding` is answered too.
- **transaction_read_only and transaction_deferrable** take SET in a block as transaction_isolation does (each the SET TRANSACTION of its mode), and their RESET is 0A000, as PostgreSQL's.
- **An extended pipeline's implicit block** is no transaction block to PostgreSQL: SET TRANSACTION and SET LOCAL in it warn 25P01 and still apply until Sync, as here. PostgreSQL's unnamed transaction starts with the pipeline's first message, the layer's implicit block at its second statement; so that the two agree, a SET, SET LOCAL or RESET of a mode or default, RESET ALL, SET TRANSACTION or SET SESSION CHARACTERISTICS sent first of several statements opens the implicit block before it runs. The block then runs with the defaults it found (a read-only default turned off by the pipeline's first statement still refuses its INSERT with 25006, as PostgreSQL does), and a failure of the block undoes the SET (`server_design.md`, *Extended query*).
- **The mode list** is read as PostgreSQL's transaction_mode_list: a comma only between two modes (else 42601 at the comma or at the end of input), and each occurrence of a mode keeps its own value, checked in order, so `SET TRANSACTION READ ONLY, READ WRITE` after a query is 25001 as in PostgreSQL. A parameter's name in SET, SHOW and RESET matches in any case, a quoted one too, as PostgreSQL's lookup does.
- **A BEGIN the engine refuses** (SERIALIZABLE on a follower) leaves a failed block with no `Transaction`: ROLLBACK TO in it is 3B001, as it is for any name the block has no savepoint for, and every other statement but COMMIT and ROLLBACK is 25P02.
- **default_transaction_read_only outside a block.** A statement outside a block then runs in a read-only `Transaction` of its own, so the runtime refuses its write with PostgreSQL's 25006 text, a SELECT … FOR UPDATE and a procedure's writing statement included. A DECLARE outside a block, a hold cursor in a transaction of its own, opens it read-only unless it is FOR UPDATE, so its WITH HOLD is not refused at SERIALIZABLE.
- **CALL in a read-only block.** Where READ ONLY reached the transaction, a CALL runs and its body's first write is refused, as in PostgreSQL; where only the session keeps it (set after a query or under a savepoint) a CALL is still refused, as before.
- **On a follower** SERIALIZABLE is refused by the embedded API, at BEGIN (which fails the block, as PostgreSQL's BEGIN ISOLATION LEVEL SERIALIZABLE does on a hot standby), at SET TRANSACTION, and for a statement outside a block. A default of serializable is refused at BEGIN, where PostgreSQL refuses the block's first query. The server has no follower mode yet; `pg_session_test.cpp` sets one on the engine.
- **Tests.** Wire: `test_serializable_isolation_and_the_transaction_defaults_over_simple_and_extended_query`, `test_the_transaction_defaults_are_transactional`, `test_the_25001_rules_of_isolation_read_write_and_deferrable`, `test_write_skew_between_serializable_blocks_is_40001_at_the_second_commit`, `test_a_phantom_in_a_read_range_is_40001_and_disjoint_ranges_commit`, `test_a_read_in_a_read_only_savepoint_is_recorded_and_fails_the_commit`, `test_a_set_first_in_an_extended_query_block_runs_in_that_block`, and `test_serializable_and_the_modes_match_postgresql` (with `BYTECASK_SQL_ORACLE`). `pg_session_test.cpp`: "pg_session: write skew between two SERIALIZABLE sessions …", "… on a follower is 0A000 …", "the transaction defaults are kept by COMMIT and RELEASE and undone by ROLLBACK and ROLLBACK TO". The proof machine's `begin_serializable_transaction` rule begins at SERIALIZABLE on all three sides. The functional suite's E152-01 case and sqltest's e152_01_01_01 and _02 pass: 623 of sqltest's 743 tests, every one PostgreSQL 17 accepts.
- No mutation set or fault sweep is added: the stage touches no durability site and no recording site; the session passes the level to the API, whose recording sites stage 1's mutations cover.

**Stage 3 (owner's question 6): range guards inside one registered prefix are prepared outside the engine's writer section.**
- **Code:** `WritePlan::prepare_validation` accepts a range guard whose bounds share a registered prefix length, and probes that prefix's hash, as it does for exact prefix ranges.
- **Tests:**
  - an engine test that plans checked this way commit and conflict exactly as when checked in the section, in the pattern of "checked puts: plans checked by their descents commit and conflict exactly as when checked in a pass of their own";
  - the engine's Elle range workload rerun;
  - a mutation that skips the probe fails a test (`tests/CLAUDE.md`).
- **Bench:** `python3 scripts/run_engine_bench.py` before and after, and stage 1's writer-p99 benchmark before and after, as the evidence the benchmarking rule asks for.
- **Docs:** `bytecask_design.md`, the validation section (:1764-1773).
- **Contract:** none changes.

**Stage 3 as built.** Where the code differed from the design above, the design's intent was kept:
- **Which prefix.** A range's keys all start with the bounds' common prefix, or one byte more when `to` is the successor of `from`'s byte there (engine `range_prefix_length`). Any length up to that is sound. A length is registered by the first guard that needs it (at most 8, as before), so the engine does not register one per range: a range takes its own length when it is an exact prefix range or that length is registered, else the longest registered length below it, and registers its own only when none is (`probe_prefix_length`). An index range's own length is 5 plus the bytes its bounds' values share, so the first index range registers that, and a full scan registers 5; from then on an index range whose own length is not registered is probed by the longest registered length below it, at worst its index's prefix. The engine design's *Validation prepared before the section* has the rule.
- **What is left in the section.** A range is walked twice when the table or index it lies in is written between the writer's preparation and its turn in the section: once at preparation, once in the section. A SERIALIZABLE writer that scanned an index range while other writers change that index's keys is still checked in the section at O(keys in its range). Question 6 (b) and the sublinear range checks of (c) remain for that case.
- **The bench's control.** The writer-p99 benchmark gained a row: the same two range writers at Snapshot, which submit no range guard, to tell the range writers' own work on a 4-core machine from the commit's check.
- **Tests.** Engine: `prepared validation: range guards inside one prefix commit and conflict exactly as when checked in the section` (two databases, one preparing and one not, groups whose earlier slots write into later slots' ranges), `prepared validation: a range guard's prefix is the longest every key in its range shares` (against its definition), `prepared validation: a range guard inside a prefix is checked again only when its prefix was written`, and a section of `prepared validation: a guard whose key is written between preparing and the section is checked again`. The mutation `range_guard_prefix_not_probed` (engine `tests/durability_mutations/`) leaves such a range unprobed and fails the last two. The engine's range workload (`run_isolation_check.py --workload range`, with and without `--kill`, 4 rounds each) and `sql_isolation_check.py --workloads range` (2 rounds, serializable and serializable-ro included) passed.

Writer latency, the stage 1 benchmark (`sql_layer_bench`, releasedbg, the same 4-core machine as stage 1's numbers, one run each), snapshot-isolation writers (4 threads, autocommit UPDATE of a row each), µs:

| | before: p50 | p99 | p99.9 | writes in 6 s | after: p50 | p99 | p99.9 | writes in 6 s |
|---|---|---|---|---|---|---|---|---|
| alone | 220 | 630 | 2 597 | 96 324 | 247 | 727 | 3 335 | 85 364 |
| with 2 Serializable writers reading covering index ranges of 100 keys | 532 | 3 482 | 5 569 | 37 638 | 434 | 2 747 | 5 334 | 44 018 |
| … of 10 000 keys | 30 431 | 56 899 | 64 014 | 872 | 264 | 3 823 | 6 129 | 65 851 |
| … of 1 000 000 keys | 247 | 2 989 | 5 905 | 11 037 | 271 | 3 770 | 6 221 | 66 159 |
| with 2 Snapshot writers reading ranges of 10 000 keys (control) | | | | | 288 | 4 728 | 7 990 | 44 511 |
| alone, again | 222 | 562 | 2 344 | 97 504 | 236 | 792 | 4 143 | 86 274 |

The Serializable writers committed 12 841, 375 and 5 transactions before, 13 186, 429 and 4 after, none 40001. At ranges of 10 000 keys the snapshot writers' p50 is back to their p50 alone, and their throughput rose from 872 writes to 65 851. Their p99 is still about five times its value alone, but the control, the same reads with no range guard, is as high: what is left is the two range-reading threads' work on four cores, not the commit's check. The bench measures no effect of the Serializable writers' range checks on other writers' latency at these sizes, while the index they read is not being written.

`engine_bench` (`python3 scripts/run_engine_bench.py`, 50k keys, built with `BYTECASK_NO_ROCKSDB=1`; two runs of each tree, interleaved before, after, before, after; before = 9741a5b3, after = 1a1fc890), ns, real time per op:

| | before | after |
|---|---|---|
| Put/Sync | 99 159, 131 174 | 120 998, 124 866 |
| Del/Sync | 118 181, 115 147 | 129 368, 117 949 |
| MixedBatch/Sync | 289 337, 322 906 | 278 711, 346 111 |
| Put/NoSync | 4 225, 4 271 | 4 054, 4 138 |
| Get | 220, 247 | 236, 247 |
| CasMT/Sync, 4 threads | 203 209, 267 336 | 241 488, 206 305 |
| PutMT/Sync, 4 threads | 198 893, 227 286 | 243 411, 206 896 |

No `engine_bench` workload submits a range guard, so none runs the new preparation; every difference above lies inside the spread between two runs of the same tree. No engine-level improvement or regression is claimed.

### Owner's questions on serializable isolation

1. **ON CONFLICT DO NOTHING at SERIALIZABLE.** This overrides the owner's 2026-11-19 decision not to guard skipped rows, at SERIALIZABLE only.
   - (a) A skipped row's arbiter key is a read at SERIALIZABLE (this design): PG 17's outcome ("DO NOTHING skip, then skew" is 40001; a concurrently deleted arbiter row is inserted by the autocommit retry).
   - (b) Keep the 2026-11-19 rule at every level: a SERIALIZABLE transaction would commit a skip that a serial order contradicts.

   **Recommendation: (a).** (b) relaxes SERIALIZABLE.
2. **Concurrent DDL against SERIALIZABLE writers.**
   - (a) One guard on the catalog version (this design): any DDL committed during a writer's life fails it at COMMIT. Covers every catalog object, including views, procedures, types and folded plans.
   - (b) Guard the catalog records of every object the binder resolved: tables, indexes, views, procedures, types, sequences, including those a plan folds away. Fewer false positives (a CREATE INDEX elsewhere fails nothing), but a dependency the list forgets is a silent anomaly, and CREATE INDEX would still change the read table's record unless that is redesigned.

   **Recommendation: (a)**, with (b) as a follow-up issue if a workload's DDL rate shows the aborts.
3. **"Read absent, then insert", raced.**
   - (a) 23505, the existing rule (this design): matches PG 17 when the other transaction inserted blindly; differs when both read first, where PG 17 gives 40001.
   - (b) 40001 when the taken key or unique prefix lies inside a read the transaction's statements recorded (not the insert's own `pk_free`/`unique_free`): matches PG 17 when both read first, keeps client retry loops working there, and differs when the other inserted blindly.

   **Recommendation: (a).** Neither matches PG 17 in every case, since PG's split depends on whether the *other* transaction read the key, which the layer cannot know; (a) keeps one rule at every level, and 23505 is a correct outcome either way.
4. **WITH HOLD in a SERIALIZABLE writer.**
   - (a) Refuse with 0A000 (this design), and open an issue.
   - (b) Run the query once at DECLARE, recording reads, and refuse WITH HOLD for a plan whose replay is not proven deterministic (`random()`), or check every post-commit replay's reads against the frozen read set and fail the FETCH outside it.
   - (c) Materialize at COMMIT as PG 17 does. That keeps rows in memory, and a 54000 would land at COMMIT.

   **Recommendation: (a)** until a workload asks for it. The refusal is a gap, marked in the functional suite.
5. **SERIALIZABLE on a follower.**
   - (a) Refuse with 0A000 "cannot use serializable mode in a hot standby", hint "You can use REPEATABLE READ instead.", as PG 17 does (this design).
   - (b) Allow read-only SERIALIZABLE transactions there, and narrow the SERIALIZABLE row: strict on the leader, serializable as of the applied sequence on a follower.

   **Recommendation: (a).** It is PG 17's behaviour, and it keeps one guarantee, strict serializability, wherever SERIALIZABLE runs.
6. **The writer-section cost of SERIALIZABLE range writers.** Decided with stage 1's writer-p99 numbers, before stage 2 ships:
   - (a) Stage 3 (prefix-range preparation outside the section) lands before stage 2.
   - (b) A per-transaction cap on the keys its in-section ranges may cover, 54000 past it, lands with stage 2.
   - (c) Neither, if the numbers show no measurable effect on snapshot-isolation writers at the sizes benchmarked; stage 3 becomes an issue.

   **Recommendation:** none until the numbers exist. If they show a measurable p99 effect, (a); (b) if (a) does not bound it.

   **Decided by stage 1's numbers: (a).** Stage 3 is built; its numbers (*Stage 3 as built*) show no remaining effect of the range checks on other writers at the sizes benchmarked while the index read is not written, so (b) is not scheduled.

### Risks and scope of serializable isolation

**Risks**
- **A read path that bypasses the recording** would be a silent anomaly. Mitigations:
  - one recording reader holds the snapshot, so a bypass does not compile;
  - one committed mutation per recording site, run in CI;
  - the Elle runs, point, range and read-only, run nightly.
- **The read-only argument** rests on every SERIALIZABLE writer validating at its commit sequence, catalog included. Stage 1's read-only Elle run is its check. If it fails, the fallback is to submit read-only transactions' read sets as guard-only plans.
- **False positives.** Every changed read fails a writer, and any concurrent DDL fails overlapping writers, so abort rates exceed PG 17's under contention. TPROC-C's numbers at SERIALIZABLE are reported in stage 1. A workload that needs fewer aborts uses REPEATABLE READ with `FOR UPDATE`, as today.
- **Commit latency in the writer section,** O(keys in its ranges), for every writer while a SERIALIZABLE range writer commits. Measured in stage 1; owner's question 6. Since stage 3 only when the table or index a range lies in was written between the writer's preparation and the section.
- **Memory.** About 192 000 distinct point reads on 20-byte keys fill the default `max_bytes`. Index scans that fetch many rows and large join probes reach it first. The failure is a clean 54000 at a read.
- **The successor key at `max_key_bytes`** falls back to a wider range. That is a superset, and only a false positive.

**Out of scope**
- SSI-style dependency tracking (rw-edges, pivots, SIREAD locks). It would need shared conflict state across transactions, outside the data files, to avoid the single-edge false positive. Not taken.
- Statement-time 40001 and its ROLLBACK TO recovery.
- READ COMMITTED as a snapshot per statement, which stays under *Later*.
- WITH HOLD in a SERIALIZABLE writer, coarsening, sublinear range checks, and token transactions.
- Engine changes, except stage 3 if taken.

**Follow-up issues**, opened when the design is approved:
- sublinear range guards in the key directory;
- stage 3, if question 6 does not schedule it;
- coarsening of point reads when the bound would be reached;
- WITH HOLD in a SERIALIZABLE writer (question 4), naming the `random()` replay hazard;
- the stale "Randomness … None in a plan" row at `cursor_design.md`:205, which affects ordinary cursor replay;
- per-object catalog guards (question 2), if DDL aborts show;
- the stale line at engine `docs/transaction_design.md`:242.

### Edits to this document once serializable isolation is built

Made with stage 1 unless noted; all are made, the last with stage 2.
- *Isolation*: the **Later** item "serializable" is replaced by a pointer here. The sentence "A plan's guards are the only text that differs between levels" goes. The first bullet becomes "Snapshot isolation by default".
- *Limits*, **Buffer size**: the read set joins "its overlay, its guards, claims and locks", with the note that ROLLBACK TO does not release it; an autocommit statement's read set is charged to its statement budget.
- *Savepoints*: ROLLBACK TO keeps a SERIALIZABLE transaction's reads.
- *Commit and conflicts*: at SERIALIZABLE, the read set and the catalog-version guard join the plan; the 40001 message rule; the unique-check prune is unchanged.
- *Known differences from PostgreSQL*: the ON CONFLICT DO NOTHING row (owner decision 2026-11-19) says it holds below SERIALIZABLE.
- *Tests*: a "Serializable" item names stage 1's and stage 2's tests and the mutation set. **Concurrency** names the range workload and the serializable configuration.
- *Later*: "Serializable and read committed" becomes "Read committed".
- The status line of this section becomes **built**, naming the PRs (stage 2).

## Later

Not built. Each gets an issue when it is picked up.

| Later | Why nothing built blocks it |
|---|---|
| Token transactions across requests (the transport design, #487) | The handle is the token's in-process half; a token names a handle held with a TTL. |
| Procedures retried in the runtime (#495) | A procedure is a transaction whose statements are all the runtime's, so its retry is a loop around `begin` … `commit`. |
| Read committed | A snapshot per statement; the runtime's level switch (`bc_txn_new`) is where it would be chosen. |
| Sequences and auto-increment | A runtime service at commit, outside the plan (`docs/plan_api_design.md`, *Deferred*). TPROC-C's ids come from rows it reads and updates. |
| Deadlines turned into budgets | The budget is per statement already; a transaction deadline divides into it. |
| Bulk load | A transaction mode that flushes its log in batches, as `MariaDBTxn`'s bulk-copy mode does, into a keyspace no one else reads yet. |
