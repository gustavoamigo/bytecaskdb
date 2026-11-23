# The PostgreSQL wire-protocol server and the HammerDB gate

Status: **built** (owner's approval, 2026-10-22). This covers
`sql_layer_server`, the server HammerDB connects to; the HammerDB driver
script that sends the layer's statements; and the gate that runs them,
HammerDB's TPROC-C over the wire at 1, 16 and 48 virtual users (*Decisions*,
11). The in-process harness of `testing_design.md` (*The TPROC-C harness*)
checks the consistency conditions, runs Elle and the CI smoke test, and gives
the no-network baseline.

The server is a thin, stateful front over the embedded API. The thread
model, the transport and the driver format are to be redesigned
(#487, *Next*). What follows is what is built.

Names: the benchmark is HammerDB's TPROC-C, and the layer's files and
binaries use that name (the harness is `sql_layer_tprocc`,
`harness/tprocc.cppm`, since #547).
HammerDB's own identifiers, `dbset bm TPC-C` and `diset tpcc …`, are the
tool's and stay.

## Scope

In:
- startup with trust authentication;
- the simple query protocol (`Query`), and the extended one (`Parse`,
  `Bind`, `Describe`, `Execute`, `Sync`, `Close`, `Flush`);
- text-format parameters and results, with each type's PostgreSQL OID;
- `BEGIN`, `COMMIT` and `ROLLBACK` as SQL statements, and `SAVEPOINT`,
  `ROLLBACK TO` and `RELEASE`;
- one session per connection;
- errors as `ErrorResponse`, with PostgreSQL's SQLSTATE for every error the
  layer raises;
- `hammerdb/`: the driver script and the script that runs the gate.

Out, each refused with a stated error rather than half-supported: SSL and
GSSAPI encryption, every authentication method but trust, binary format,
`COPY`, the function-call protocol, `SET`, `SHOW` and `RESET` of any
parameter but the transaction modes' and `client_encoding`'s (*Run-time
parameters*), SQL
`PREPARE`/`EXECUTE`/`DEALLOCATE`, `Execute` with a row limit, several
statements in one `Query`, and the system catalogs (`pg_catalog`,
`information_schema`). `pg_stat_bytecask` is no catalog: the session answers
one query of it (*Reading the counters*).

## What the client sends

The gate's client is HammerDB running our driver script. The script calls
pgtcl, HammerDB's bundled Pgtcl (2.1.1 in HammerDB 5.0), which calls libpq. Its sources
(Pgtcl `generic/pgtclCmds.c`, libpq `fe-connect.c`/`fe-exec.c`, REL_17)
give the messages.

**At connect.** `pg_connect -conninfo {host … port … sslmode disable
gssencmode disable user … dbname …}` is `PQconnectdb`.
- `sslmode=disable` and `gssencmode=disable` mean no `SSLRequest` and no
  `GSSENCRequest`. libpq's defaults are `prefer` for both, and other
  clients keep them, so the server still answers either request with `N`,
  which libpq takes as "fall back to plaintext on this connection".
- The `StartupMessage`, protocol 3.0, carries `user`, `database`, and
  `application_name` or `client_encoding` only if the conninfo sets them.
  The conninfo's password is sent only if the server asks for one, and
  trust never asks.
- libpq accepts `AuthenticationOk`, then any `ParameterStatus`,
  `BackendKeyData` and `NoticeResponse`, up to `ReadyForQuery`.
  `BackendKeyData` is optional; libpq needs it only to cancel.
- Then pgtcl's `pg_connect` calls `PQsetClientEncoding`, which sends `set
  client_encoding to 'UTF8'` as a `Query`. Found running the driver under
  HammerDB 5.0, not in the sources read first. The server takes that one
  `SET` (*Transaction control*).

**Per statement.** The driver uses two calls.
- `pg_exec $conn "BEGIN"`, with no parameters, is `PQexec`: one `Query`
  message.
- `pg_exec $conn $sql $p1 … $pn` is `PQexecParams` with no types, no
  lengths and text formats. libpq writes `Parse` (unnamed, n OIDs of 0),
  `Bind` (unnamed portal, text parameters, text results), `Describe`
  (portal), `Execute` (no row limit) and `Sync` in one write. It reads
  `ParseComplete`, `BindComplete`, `RowDescription` or `NoData`, the
  `DataRow`s, `CommandComplete` and `ReadyForQuery`.
- The driver reads each result with `pg_result -status`, `-numTuples`,
  `-getTuple` and `-cmdTuples`, and each error with `pg_result -error sqlstate`
  (the SQLSTATE; pgtcl matches the suboption exactly, in lower case) and
  `-error` (the message).

A Payment is 13 round trips: `BEGIN`, its 11 statements and `COMMIT`.
HammerDB's MySQL client-side mode (`mysql_no_stored_procs`) has the same
shape; its PostgreSQL driver runs a transaction in one round trip as a
stored procedure.

pgtcl's `pg_exec_prepared` needs a statement made by SQL `PREPARE`, and
its `pg_prepare` does not call `PQprepare`. The driver uses neither. Each
call therefore sends its statement's text again. The server's `Parse` then
costs one lookup in `Database::prepare`'s cache, keyed by the exact text.

## The server

**Binary and modules.** `sql_layer_server --dir DIR [--listen
127.0.0.1:5432] [--database NAME] [--max-connections 100] [--workers N]
[--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION] [--sync-interval-ms 1000]
[--io-backend pread|mmap|buffer_pool] [--buffer-pool-size BYTES]
[--buffer-pool-direct-io on|off] [--statement-memory-bytes 67108864] [--max-memory-bytes BYTES]
[--idle-in-transaction-timeout-ms 60000] [--statement-timeout-ms 0]
[--transaction-timeout-ms 0] [--idle-session-timeout-ms 0] [--startup-timeout-ms 60000]
[--vacuum on|off] [--vacuum-fragmentation-threshold 0.7] [--vacuum-busy-interval-ms 500]
[--vacuum-idle-interval-ms 30000]` serves one
database directory under one database name, by default the directory's
last component. It opens the directory with `Database::open`, so it holds
the engine's directory lock. The code is in `bytecaskdb-sql-layer/server/`:
- `pg_wire.cppm` (`bytecask.sql.pg_wire`): message framing (`Framer`),
  reading a message from a buffer (`Body`) and writing one into a buffer
  (`Out`); it does no I/O and has no policy.
- `pg_text.cppm` (`bytecask.sql.pg_text`): each SQL type's OID, and its
  text input and output, which are the layer's own (`sql/text.cppm`,
  `bytecask.sql.text`): the binder reads a string literal with the same
  input functions (`frontend_design.md`, *Literals*).
- `pg_session.cppm` (`bytecask.sql.pg_session`): one session's protocol and
  SQL state machine (`Session`), and the worker pool (`Pool`).
- `vacuum_thread.cppm` (`bytecask.sql.vacuum_thread`): the background vacuum
  (`session_design.md`, *Background vacuum*).
- `main.cpp`: arguments, the libuv event loop, connections and signals.

The layer is POSIX-only here: no WASM or Windows build of the server.

**Durability.** `--sync` and `--sync-interval-ms` are `DatabaseOptions::sync`
and `sync_interval_ms`, with the names and meanings of the MariaDB plugin's
`bytecaskdb_sync` and `bytecaskdb_sync_interval_ms`
(`bytecaskdb-mariadb-plugin/docs/mariadb_engine_design.md`, *Durability
modes*). They are fixed at startup: unlike the plugin, the server has no
`SET GLOBAL`.

A commit goes to the data file before it returns, in every mode. The mode
decides only whether it waits for `fdatasync`:
- `AT_EVERY_COMMIT` (default): autocommit statements and transaction commits
  pass `sync = true` to the engine, and concurrent commits share one
  `fdatasync` through its group commit.
- `AT_INTERVAL`: they pass `sync = false`. A flusher thread, started by
  `Database::open`, waits `sync_interval_ms`, runs
  `apply_batch({.sync = true}, WritePlan{})`, and repeats: a commit is
  durable once the next sync ends, up to an interval plus a sync's own time
  after it returns.
  The engine returns from that empty synced plan once every earlier write is
  durable, and does no `fdatasync` when none is waiting. `close()` stops and
  joins the flusher before it takes the exclusive lock, so a last sync in
  flight holds up no other call, and a concurrent `close()` waits for the
  join before it closes the engine.

  A failed sync is logged to stderr. A failed `fdatasync` degrades the
  engine, as the engine's `CONTRACT.md` states (*Degraded*): every later
  write fails, and `close()` throws `DbDegraded`, naming the commits that are
  not durable (the server then exits non-zero). No sync can succeed after
  that, so the flusher stops. Any other failure is logged once and retried
  at the next interval.
- `AT_FILE_ROTATION`: they pass `sync = false` and no flusher runs. Data
  reaches the disk when the active file rotates, and at `close()`.

DDL always passes `sync = true`, as the plugin's does. The engine counter
`bytecask.fsyncs` shows the difference.

**Reads.** `--io-backend`, `--buffer-pool-size` and `--buffer-pool-direct-io`
are `DatabaseOptions::io_backend` and `buffer_pool`, with the names, values
and defaults of the plugin's `bytecaskdb_io_backend`,
`bytecaskdb_buffer_pool_size` and `bytecaskdb_buffer_pool_direct_io`: `pread`
by default, and a pool of `--buffer-pool-size` bytes, total footprint, filled
with `O_DIRECT` unless `--buffer-pool-direct-io off`. They change where
sealed-file reads are served from, not what is read
(`docs/buffer_pool_design.md`). The pool must hold at least twice the
engine's file size (default 64 MiB); a smaller one is refused at startup.

**Memory.** `--statement-memory-bytes` is `DatabaseOptions::
statement_memory_bytes`: the memory budget of every statement the server
runs, 64 MiB by default (`plan_api_design.md`, *Memory*). A statement past
it fails with 54000. 0 is refused at startup.

`--max-memory-bytes` is `DatabaseOptions::max_memory_bytes`, by default half
the machine's RAM: what every running statement and open transaction hold
together, as their budgets count it. A statement that would pass it fails at
once with `53200` (`out of memory: the statements and transactions in
progress would hold more than the memory cap of N bytes`), and in a block
fails the block; nothing waits for memory. Each statement reserves it in
steps of 1 MiB, so it can refuse up to 1 MiB per running statement early.
It counts neither the connections' I/O buffers nor the snapshots
transactions hold (`session_design.md`, *Bounds*). 0 is refused at startup.

**Threading, sessions and the worker pool** (`docs/session_design.md`). Three
things, each with one owner:
- **Connection**: a non-blocking socket, its input and output buffers, and
  message framing (`Framer`), owned by the libuv event loop.
- **Session**: a plain data structure holding the protocol's and SQL's state,
  with no thread and no lock, owned by whichever worker is running it.
- **Worker**: a thread in a fixed pool (`--workers`, by default four times the
  cores, minimum 4).

The loop hands the bytes it reads to the connection's session and schedules an
idle session onto the worker pool; the worker cuts them into messages
(`Framer`). A worker runs one session's queued messages up to the next
`ReadyForQuery` (after startup, a `Query` or a `Sync`), then puts the session
behind the others if more messages are queued. Each session has a lock for
its handoff with the loop, and each worker a queue with its own lock.

A connection past `--max-connections` gets `FATAL 53300`
(`too_many_connections`) and is closed, as PostgreSQL does. The default,
100, is PostgreSQL's, and covers the gate's 48 virtual users and its
monitor. The next 16 past the limit are taken in only for a `CancelRequest`
(`session_design.md`, *Bounds*): their startup packet is read, and a
startup is then refused with `53300`. Out of file descriptors (`EMFILE`,
`ENFILE`), the listener pauses for 100 ms rather than retry the backlog in a
loop.

**I/O.** Non-blocking sockets on libuv with `TCP_NODELAY`. A worker appends
reply bytes to the connection's output buffer at `ReadyForQuery`, at `Flush`,
or when its buffer passes 64 KiB, and wakes the loop (`uv_async_send`), which
writes them in 64 KiB chunks. A slow reader never holds a worker.

A message longer than 64 MiB is `FATAL 08P01` and closes the connection.
That bound sits well above the engine's 4 MiB default value limit plus a
statement's text.

Both directions are bounded, and neither bound is an error
(`session_design.md`, *Bounds*):
- **Input.** The loop reads a connection only while its session is idle, and
  a session is idle only once it has run every complete message it was
  given. So a session holds at most one read (64 KiB) and one partial
  message of unrun input; past that the socket is not read, and TCP holds
  the client back.
- **Output.** Once a connection's client has not taken 64 MiB of replies, its
  session runs no further statement, and so reads no further input, until
  the client takes some. The check is between statements, so one statement's
  reply can pass it; a reply is bounded too, by its rows counting against
  the statement's memory budget before they are sent.

**Shutdown.** `SIGINT` or `SIGTERM` closes the listener and stops reading on
every connection. A turn in flight on a worker finishes first, and its reply
still goes out. Queued messages and idle sessions are answered with `FATAL
57P01` (`admin_shutdown`), and open transactions are rolled back. Once the
stop has begun, a connection whose write makes no progress for ten seconds is
dropped; one whose client keeps reading, however slowly and however large its
result, gets its reply and then the `57P01`, so it learns whether its last
statement committed. Another `SIGINT` or `SIGTERM` during the stop is
ignored. When every connection and session has ended, the server shuts down
the worker pool and calls `Database::close()`.

**Cancellation and timeouts** (`session_design.md`, *Lifetime,
cancellation and shutdown*). A `CancelRequest` naming a session's
`BackendKeyData` key fails its running statement with `57014`
(`canceling statement due to user request`); one naming a session that runs
nothing, or a wrong key, does nothing. A cancel is checked where the runtime
charges a statement's reads, every 64; a `FETCH` and an `UPDATE` or `DELETE
... WHERE CURRENT OF` are statements here like any other. A session is idle from
`ReadyForQuery` until it takes its next message, which clears any cancel
sent meanwhile; one sent after, while the command compiles or runs, cancels
it. The timeouts, in milliseconds, `0`
turning one off:
- `--statement-timeout-ms` (off): a statement past it fails with `57014`
  (`canceling statement due to statement timeout`), and the session goes on;
- `--idle-in-transaction-timeout-ms` (60000): a session idle that long in a
  transaction, explicit, failed or an extended block's implicit one, gets
  `FATAL 25P03`, and its transaction rolls back;
- `--transaction-timeout-ms` (off): a transaction open that long in total,
  busy or idle, gets `FATAL 25P04` and rolls back. A statement outside a
  block is its own transaction;
- `--idle-session-timeout-ms` (off): a session idle that long outside a
  transaction gets `FATAL 57P05`;
- `--startup-timeout-ms` (60000): a connection that has not finished its
  startup that long after it was accepted is closed with no reply, as
  PostgreSQL's `authentication_timeout` does. It frees the connection's
  place under `--max-connections`, or among the 16 kept for a cancel.

**After a `FATAL`** the session ends: it reads no more messages and runs
nothing more. After startup, an invalid message length or type is `FATAL`.

## Startup

1. **The request.**
   - An `SSLRequest` or `GSSENCRequest` is answered `N`, and the next
     message is read on the same connection.
   - A `CancelRequest` cancels the running statement of the session its key
     names (*Cancellation and timeouts*), and its connection is closed with
     no reply.
   - A `StartupMessage` of protocol 3.0 is read.
   - Protocol 3.x, x > 0, gets `NegotiateProtocolVersion` (3.0, naming any
     `_pq_.` options), as PostgreSQL answers it, and startup goes on at 3.0.
   - Any other version is `FATAL 0A000`.
2. **The parameters.**
   - `user` is required, and any name is accepted: every session is the
     bootstrap superuser, whatever it names (`frontend_design.md`, *Roles
     and privileges*). It is what `CURRENT_USER`, `CURRENT_ROLE`, `USER`
     and `SESSION_USER` return, cut to 63 bytes (`frontend_design.md`,
     *Session functions*): the server passes it as the `user` of each
     statement's `ExecutionOptions` and each block's `TransactionOptions`.
   - The served name is `CURRENT_CATALOG` (`DatabaseOptions::name`).
   - `database` must be the served name, else `FATAL 3D000`
     (`invalid_catalog_name`).
   - `application_name` is kept and reported back.
   - `client_encoding` must be `UTF8`, its alias `UNICODE`, or `SQL_ASCII`,
     matched as PostgreSQL matches a name (any case, punctuation ignored),
     else `FATAL 22023`. There is no conversion: PostgreSQL converts
     nothing for `SQL_ASCII` either.
   - `options` must be empty.
   - Any other parameter is `FATAL 42704` (`undefined_object`), as
     PostgreSQL refuses an unknown setting.
3. **Authentication: trust only.**
   `AuthenticationOk` is sent at once. Anyone who can reach the port is in,
   so the server listens on 127.0.0.1 unless `--listen` names another
   address, and it prints a warning when that address is not a loopback
   one. Password and SCRAM authentication come with the transport design
   (#487, *Next*).
4. **`ParameterStatus`**:
   - `server_version` `17.0`: the dialect the statements are proven
     against, which libpq and psycopg parse;
   - `server_encoding` `UTF8`, and `client_encoding` the encoding asked
     for (`UTF8` for `UNICODE`), `UTF8` when none was;
   - `DateStyle` `ISO, MDY`, `IntervalStyle` `postgres`, `TimeZone` `UTC`;
   - `integer_datetimes` `on`, `standard_conforming_strings` `on`;
   - `is_superuser` `off`, `session_authorization` the user;
   - `application_name`.
5. **`BackendKeyData`**: the session's process id, unique among the
   server's sessions, and a random secret, which a `CancelRequest` names.
6. **`ReadyForQuery`**, status `I`.

## A session

A session holds:
- the block status (*The block status*, below);
- the open `Transaction`, if any;
- the named statements, each its text and its record (parameter types,
  result columns, command, and the catalog version that fixed them), not a
  `Statement`, so nothing a statement holds dies with a block. Each use
  resolves the text where it runs: through the open block's transaction,
  or in autocommit, which takes the cache's plan;
- the named portals;
- the unnamed statement and the unnamed portal;
- the block's named cursors (*Cursor statements*).

A portal lasts until its transaction ends, as in PostgreSQL: at `COMMIT` or
`ROLLBACK` inside `BEGIN`, and at `Sync` or the end of a `Query` outside it.
An `Execute` of a portal that ended is `34000`, and its name can be bound
again.

Closing the connection, with `Terminate` or without, rolls back an open
transaction.

**The block status** is the session's own: `I`, `T` or `E`. It drives
`ReadyForQuery`, `25P02` and `COMMIT`'s answer. It is not read off the
`Transaction`, because errors the layer never sees, or sees differently,
must still change it:
- an error in `prepare` or in a parameter's text (`42601`, `22P02`,
  `22003`, `08P01`) leaves the `Transaction` open and not aborted;
- a `40001` from a catalog race ends the `Transaction` outright.

The rules:
- `BEGIN` makes the block explicit: `T`.
- Any `ErrorResponse` inside an explicit block makes it `E`, whatever
  raised it. The server aborts the `Transaction` if it is still open
  (`Transaction::fail`), which keeps its savepoints.
- `ROLLBACK TO` a savepoint in `E` returns the block to `T`.
- In `E`, every statement but `COMMIT`, `ROLLBACK` and `ROLLBACK TO` is `25P02`, checked
  by the session before anything reaches the layer. As in PostgreSQL, a
  statement that does not parse reports its syntax error instead: it is
  parsed first, and refused before it is bound.
- `COMMIT` in `E` answers `ROLLBACK`. `ROLLBACK` ends the block in any
  state. Either way the status is `I`.
- `COMMIT` in `T` that fails (`40001`, `08007`) has ended the block: `I`.
- Outside `BEGIN`, `Sync` always returns the status to `I`, after an error
  too.

**Transaction control** is recognised before the layer's parser. The
recognised words are `BEGIN`, `START TRANSACTION`, `COMMIT`, `END`,
`ROLLBACK` and `ABORT`, each with optional `WORK` or `TRANSACTION`, any
case, a trailing `;`. These are session operations that the embedded API
exposes as calls, not as statements.
- **`BEGIN`** calls `Database::begin()`; the tag is `BEGIN`. Inside an open
  transaction it is `WARNING 25001` ("there is already a transaction in
  progress") and changes nothing. Inside an aborted one it is `25P02`.
  `BEGIN` and `START TRANSACTION` take modes, and `SET TRANSACTION` sets
  them for the open block (*Transaction modes*). The isolation is the
  layer's (`transaction_design.md`).
- **Transaction modes** are `ISOLATION LEVEL SERIALIZABLE | REPEATABLE
  READ | READ COMMITTED | READ UNCOMMITTED`, `READ ONLY`, `READ WRITE` and
  `[NOT] DEFERRABLE`, in any order, a comma allowed between two modes and
  nowhere else (else `42601`, as PostgreSQL's grammar gives it), each
  occurrence with its own value checked in the order written, as
  PostgreSQL 17 checks it
  (`transaction_design.md`, *Choosing the level: SET, SHOW, BEGIN and the
  embedded API*). A block that names none takes the session's defaults
  (*Run-time parameters*).
  - **The level.** `SERIALIZABLE` runs the block's `Transaction` at
    `Isolation::Serializable`: its reads are checked at `COMMIT`, which is
    `40001` when one changed (`transaction_design.md`, *Serializable
    isolation*). The other three run at snapshot isolation, which is what
    `REPEATABLE READ` asks and more than the other two do; `SHOW
    transaction_isolation` reports the level named, not the mechanism.
    `BEGIN` passes the level to `Database::begin`, and `SET TRANSACTION`
    to `Transaction::set_isolation`. A change of level after the block's
    first statement is `25001` "SET TRANSACTION ISOLATION LEVEL must be
    called before any query", and with a savepoint open "… must not be
    called in a subtransaction"; the block's own level is taken anywhere.
    On a follower `SERIALIZABLE` is the API's `0A000`, "cannot use
    serializable mode in a hot standby", hint "You can use REPEATABLE READ
    instead.": at `BEGIN`, which fails the block (`E`), as PostgreSQL's
    does, at `SET TRANSACTION`, and for a statement outside a block.
  - **`READ ONLY`.** A block that is read-only refuses any statement but a
    `SELECT` with `25006`, naming the command (`SELECT INTO` for a
    `SELECT … INTO`), which aborts the block, as in PostgreSQL. Set by
    `BEGIN`, or by `SET TRANSACTION` before the block's first statement with
    no savepoint open, it reaches the transaction
    (`TransactionOptions::read_only`, `Transaction::set_read_only`): its
    runtime then refuses a `SELECT … FOR UPDATE` too, and a procedure's
    statement that writes, and a `SERIALIZABLE` one records nothing. Set
    later, the session keeps it (a `SELECT … FOR UPDATE` is not told apart
    and is taken; a `CALL` is refused); `RELEASE` and `ROLLBACK TO`
    restore the mode the savepoint was taken in, and the transaction keeps
    recording what it reads. `READ WRITE` in a read-only block is `25001`,
    with a savepoint open first, then after a statement.
  - **`DEFERRABLE`** is kept for `SHOW` and never waits: a read-only
    `SERIALIZABLE` transaction records nothing and never fails. In a
    savepoint it is `25001` "SET TRANSACTION [NOT] DEFERRABLE cannot be
    called within a subtransaction", and after a statement "… must be
    called before any query", the same value too.
  - `SET TRANSACTION` outside a block is `WARNING 25P01` and changes
    nothing; in an extended pipeline's implicit block it warns and sets the
    modes, as PostgreSQL's unnamed transaction does. `SET LOCAL
    TRANSACTION` and `SET SESSION TRANSACTION` are `SET TRANSACTION`, as in
    PostgreSQL. `BEGIN` inside a block warns `25001` and then sets its
    modes under the same rules; `BEGIN` after a statement of an implicit
    block checks them before the block becomes explicit, so a refusal
    rolls the implicit block back.
- **`COMMIT`** calls `commit()`. `Committed` gives the tag `COMMIT`. A
  transaction that was aborted is rolled back instead (`RolledBack`), with
  the tag `ROLLBACK`, as PostgreSQL's `COMMIT` does. Errors:
  - `SerializationFailure` is `40001`;
  - `TransactionResolutionUnknown` is `08007`.

  Either way the block has ended, and the status is `I` (*The block
  status*). With no transaction open, `COMMIT` is `WARNING 25P01` and the tag `COMMIT`.
- **`ROLLBACK`** calls `rollback()`; the tag is `ROLLBACK`. With no
  transaction open it is `WARNING 25P01`.
- **`SAVEPOINT name`, `ROLLBACK [WORK | TRANSACTION] TO [SAVEPOINT] name`
  and `RELEASE [SAVEPOINT] name`** call `savepoint()`, `rollback_to()` and
  `release()`, with the tags `SAVEPOINT`, `ROLLBACK` and `RELEASE`. They are
  read by PostgreSQL's grammar (`ast::parse_savepoint`), which folds an
  unquoted name. Outside an explicit block each is the error `25P01`, as in
  PostgreSQL; an unknown name is `3B001`. The block's savepoints keep the
  read-only mode they were taken in, and `ROLLBACK TO` and `RELEASE` restore
  it; while one is open, an isolation level other than the block's own is
  `25001`, and so is `READ WRITE` in a read-only block
  (`transaction_design.md`, *Savepoints*). A syntax error in these statements reports the end
  of input where PostgreSQL, given the trailing `;`, names the `;`.

- **`SET [SESSION] client_encoding {TO | =} 'UTF8'`**, or another encoding
  startup takes: the tag is `SET`, with a `ParameterStatus` for
  `client_encoding`. Another encoding is `22023`.
- **`SET`, `RESET` and `SHOW` of the transaction modes and their
  defaults** (*Run-time parameters*), and `SET SESSION CHARACTERISTICS AS
  TRANSACTION`. Every other `SET`, `SHOW` and `RESET` is `0A000`.

### Run-time parameters

The session keeps six of PostgreSQL's parameters, as PostgreSQL 17 does
(`transaction_design.md`, *Choosing the level*): `transaction_isolation`,
`transaction_read_only` and `transaction_deferrable`, the open block's
modes, and `default_transaction_isolation`, `default_transaction_read_only`
and `default_transaction_deferrable`, what a later block, an implicit block
and a statement outside a block start with. `SET`, `SHOW` and `RESET` are
read by PostgreSQL's grammar (`ast::parse_variable`), so a value is quoted
or not as PostgreSQL takes it: a two-word level unquoted is `42601`. A
parameter's name matches in any case, a quoted `"Transaction_Isolation"`
too, as PostgreSQL's lookup does; an error names it as written, and `SHOW`'s
column by its lower-case name.

- **Values.** A level is matched in any case against `serializable`,
  `repeatable read`, `read committed` and `read uncommitted`; anything else
  is `22023` `invalid value for parameter "…": "…"`, hint "Available
  values: serializable, repeatable read, read committed, read
  uncommitted.". A Boolean is read as PostgreSQL's `parse_bool` reads it (a
  prefix of `true`, `false`, `yes` or `no`, `on`, `off` from `of`, `1`,
  `0`); anything else is `22023` `parameter "…" requires a Boolean
  value`. Two values are `22023` "SET … takes only one argument".
- **The defaults.** `SET [SESSION] name {= | TO} value`, `SET name TO
  DEFAULT`, `RESET name`, `RESET ALL` (which resets the three) and `SET
  SESSION CHARACTERISTICS AS TRANSACTION modes` (each mode its default)
  set them. They are transactional, as PostgreSQL's parameters are: a
  `SET` inside a block changes later blocks only; `ROLLBACK`, the
  `ROLLBACK` or `COMMIT` of a failed block, a `COMMIT` that fails, and
  `ROLLBACK TO` restore the values from before; `COMMIT` and `RELEASE`
  keep them. `SET LOCAL name …` lasts to the block's end; a later `SET`
  of the same name in the block replaces it. Outside a block `SET LOCAL`
  is `WARNING 25P01` "SET LOCAL can only be used in transaction blocks",
  then its value is read, and nothing changes. With
  `default_transaction_read_only` on, a statement outside a block runs in
  a read-only transaction of its own (`TransactionOptions::read_only`), so
  a write is `25006` from the runtime, as PostgreSQL refuses it; a `CALL`
  too, whose body's first write is refused. A `DECLARE` outside a block
  opens its hold cursor in a read-only transaction unless it is `FOR
  UPDATE`, so a hold cursor taken at `SERIALIZABLE` is not refused.
- **The block's modes.** `SET [LOCAL | SESSION] transaction_isolation`,
  `transaction_read_only` or `transaction_deferrable` inside a block is
  `SET TRANSACTION` of that mode, under its rules. Outside a block the
  value is read and nothing changes (PostgreSQL answers `SET`). `RESET` or
  `SET … TO DEFAULT` of one is `0A000` `parameter "…" cannot be reset`.
- **`SHOW`** of the six returns one text row named by the parameter, the
  tag `SHOW`; over the extended protocol `Describe` gives its
  `RowDescription`. In a block the three modes are the block's; outside one,
  the defaults. `SHOW client_encoding` returns the session's encoding. `SHOW
  ALL` and every other name are `0A000`. In a failed block `SHOW` is
  `25P02`.

The extended protocol accepts these words too: `Parse` of `BEGIN` makes a
statement the session runs at `Execute`.

**Every other statement** goes to `Database::prepare`.
- Inside a transaction it runs with `Transaction::execute`. Outside one,
  a `Query` runs it with `Database::execute`, which commits it alone. An
  `Execute` runs it in the block's implicit transaction (*Extended
  query*).
- DDL inside a block, `BEGIN`'s or an extended pipeline's implicit one,
  runs through the block's `Transaction`, as a statement of the block
  (`transaction_design.md`, *Transactional DDL*, stages 3 and 4): later
  statements see it, `ROLLBACK` undoes it and `COMMIT` publishes it with
  the block's writes. So do `CREATE TABLE AS` and `SELECT … INTO`, whose
  query reads the block's writes.
- Results: `RowDescription` (for a row-returning statement), one `DataRow`
  per row, then `CommandComplete`. The tag comes from the statement's
  command (*What it uses from the embedded API*, 3):
  - `SELECT n`, where n is the row count;
  - `INSERT 0 n`, `UPDATE n` and `DELETE n`, where n is `Rows::affected`;
  - for DDL, its command words: `CREATE TABLE`, `CREATE INDEX`,
    `DROP TABLE` and so on;
  - for `CREATE TABLE AS` and `SELECT … INTO`, PostgreSQL's: `SELECT n`
    for the n rows written, `CREATE TABLE AS` when its query did not run
    (`WITH NO DATA`, or an `IF NOT EXISTS` skip). No `RowDescription`.

## Simple query

A `Query` holds one statement, with a trailing `;` and white space
allowed. `CALL` and HammerDB's `pg_stat_database` query are read by the
session (`procedures_design.md`, *Wire protocol for `CALL`*), and
`pg_stat_bytecask`'s by the session too (*Reading the counters*). An empty one gets `EmptyQueryResponse`. Several statements are
`0A000`; no client the gate uses sends them. A `$n` in a `Query` is
`42P02` (`undefined_parameter`), PostgreSQL's "there is no parameter
$1". A
failed statement gets `ErrorResponse`; then, as after every `Query`, the
server sends `ReadyForQuery`.

A `Query` takes the unnamed portal for itself, as PostgreSQL's
`exec_simple_query` does: an unnamed portal bound before it is gone, and an
`Execute` of it is `34000`.

A `Query` runs its statement by text (`ResultCheck::none`): through the
block's `Transaction` in a block, `Database::execute(sql)` outside one. It
returns the columns of its catalog, the block's or the live one, and is
never 0A000 for a change of result type (docs/transaction_design.md, *A
transaction binds only against its own catalog*).

A `Query` that arrives after `Execute`s outside `BEGIN`, before their
`Sync`, joins their implicit transaction, as in PostgreSQL: it runs in it,
and the transaction commits when the `Query` ends. An error in the `Query`
rolls the whole block back. A DDL `Query` there is a statement of the
block too (stage 4 of transactional DDL).

## Reading the counters

A client reads the engine's operational counters with one query:

```sql
select * from pg_stat_bytecask;
```

The reply is one row per entry of `DB::stats()`, `name text, value bigint`,
in order of name, tagged `SELECT n`. A sampler needs nothing else: the same
query covers the buffer pool (`bytecask.pool_*`), the write path and the key
directory, and so a new group of counters needs no new surface. The counters
are described in the engine's `docs/bytecask_design.md`, *Operational Counters*.

- **Names are the engine's, passed through.** The server does not translate
  them, so the embedded API, the Python binding, the MariaDB plugin and the
  wire share one vocabulary. A client looks a row up by name and tolerates new
  ones. Adding a counter is routine; a PR that renames or removes one says so.
- **The session answers it.** `classify` takes this text alone, in any case,
  with any white space between its words and a trailing `;`. There is no
  catalog behind it: no plan, no statement cache entry, no transaction. Any
  other query of the name, a column list or a `WHERE` among them, reaches the
  binder and is `42P01`; a user table of that name is reachable only by such a
  query. Real system views would keep this query working.
- **Protocols and blocks.** Simple and extended query both take it. In a
  block it starts nothing and reads live values, which move with other
  sessions' work: not the block's snapshot. In a failed block it is `25P02`, as
  any statement is.
- **No pool.** With `pread` or `mmap` the `bytecask.pool_*` rows are there and
  all zero, so a sampler sees the same rows on every backend.
  `bytecask.pool_frames_total = 0` says there is no pool.
- **Values.** One `stats()` call, not one atomic snapshot: a counter read late
  in it can include work a counter read early does not.
- **Cost.** `Database::stats()` holds the shared lock every call holds, so no
  statement waits on it. The engine reads its counters' atomics, walks the file
  list, and briefly takes the change index's mutex. Measured on 16 cores, with 8
  clients at about 93,000 statements a second on a 256 MiB pool and 7 open files:
  a read's round trip from a Python client has a p50 of about 200 µs and a p99
  under 300 µs at 1 Hz. Throughput at 1 Hz was 92,000 to 93,000 statements a
  second, against 93,400 to 94,700 with no sampling, in 20-second phases. One
  client reading as fast as it can, about 7,600 reads a second, took it to 87,000.

The layer's own counters (plans compiled, retries, conflicts) are not built
(#496). They would be more rows here, under their own prefix.

## Cursor statements

`DECLARE`, `FETCH`, `MOVE` and `CLOSE` are classified by the session, as
`BEGIN` is, and parsed by the dialect's `ast::parse_cursor`
(`frontend_design.md`, *Cursor statements*). The session keeps the block's
cursors in a map of `Cursor`s from `Transaction::open_cursor`; the design is
`cursor_design.md` and what phase 3 built is its *As built (phase 3)*. What
the server adds:
- A cursor ends with the block, at `COMMIT`, `ROLLBACK` or an error, unless it
  is a hold cursor: `WITH HOLD`, or any `DECLARE` outside `BEGIN` (PostgreSQL
  says `25P01`: a known difference). A hold cursor survives the `COMMIT` of
  the block that declared it, is dropped by its `ROLLBACK`, and ends at
  `CLOSE`, a disconnect or the server's shutdown. It pins an engine snapshot
  meanwhile and has no timer or cap here (`cursor_design.md`, *As built
  (phase 4)*). A cursor shares the portals' namespace (a used name is
  `42P03`).
- `UPDATE` and `DELETE … WHERE CURRENT OF name` are ordinary statements to
  the classifier; the session finds the cursor by name (`34000`) and runs the
  statement with the cursor's key (`Transaction::execute_current_of`;
  `cursor_design.md`, *As built (phase 6)*). A refusal is `24000`. The key is
  not a parameter: `Describe` of the statement lists only its own.
- A failed block refuses them all with `25P02`; an error in one fails the
  block.
- Tags: `DECLARE CURSOR`, `FETCH n`, `MOVE n`, `CLOSE CURSOR`,
  `CLOSE CURSOR ALL`. A `FETCH` sends a `RowDescription` in a simple `Query`
  even when no row is left.
- Through the extended protocol: `Parse` of a `DECLARE` prepares its query,
  which may take `$n` parameters bound by `Bind`; `Describe` of a `FETCH`
  gives the cursor's columns when the cursor exists and `NoData` before;
  `Execute` of a `FETCH` sends `DataRow`s and the tag, never a
  `RowDescription`. `Execute` with a row limit is still `0A000`.
- Every `FETCH` and `MOVE` direction runs (`cursor_design.md`, *As built
  (phase 5)*); a backward or re-reading move on a `NO SCROLL` cursor is
  `55000` and fails the block.

## Extended query

- **`Parse`** (name, text, parameter OIDs). (name, text, parameter OIDs).
  - The name must be free: a used name is `42P05`, and the unnamed
    statement is replaced.
  - The text is described where it runs: by the block's
    `Transaction::refit(text, none)` in a block, and by
    `Database::refit(text, none)` outside one. Its record (parameter types,
    columns, command, version) is the named statement's. In a block it is
    bound against the block's catalog: with the pin behind the live catalog
    any binder error at Parse is 40001, and with the pin live a typo is
    42P01, as before. A DDL is only parsed: its binder errors are its
    Execute's, as PostgreSQL's are (`frontend_design.md`, *Binding*).
  - The OIDs the client gives are not used: each parameter's text is read
    as the type the layer inferred, so text that type does not accept is
    `22P02`. libpq sends 0s, and psycopg sends an OID per Python type
    (`int2` for a small `int`); in both cases the inferred type is the one
    the text has to fit.
  - The reply is `ParseComplete`.
- **`Bind`** (portal, statement, formats, values, result formats).
  - The statement's text is bound again where it runs with the record's
    parameter types (`refit(text, ResultCheck::given(record))`): a type
    that no longer fits is the binder's error (40001 at a stale pin), and
    result columns other than the record's are 0A000, "cached plan must not
    change result type", or 40001 in a block pinned before the record's
    version. `ParameterDescription`, the parameter count and each value's
    text input come from the record, as PostgreSQL keeps the types it
    fixed at Parse.
  - Every format must be text, else `0A000`.
  - The value count must be the statement's parameter count, else `08P01`.
  - A length of -1 is NULL; any other value is read by `pg_text` as its
    parameter's type.
  - The reply is `BindComplete`. The unnamed portal is replaced, and a used
    portal name is `42P03`.
- **`Describe 'S'`** replies with `ParameterDescription` (each parameter's
  OID), then `RowDescription` or `NoData`. **`Describe 'P'`** replies with
  `RowDescription` or `NoData`. A `RowDescription` field carries:
  - the column's name;
  - table and column 0;
  - the type's OID, length and modifier;
  - format 0.

  The column types are known before the statement runs (*What it uses from the embedded API*, 2): both
  Describes check the record where they run, as Bind does, and describe
  the record.
- **`Execute`** runs the portal and sends its rows and `CommandComplete`. A
  row limit above 0 is `0A000`; libpq sends 0. It runs the text with the
  record as `ResultCheck::given`: in the block's transaction in a block,
  else in autocommit.
- **In a block**, BEGIN's, or an extended pipeline's once its first
  `Execute` has begun it, every Parse, Bind, Describe, Execute, DECLARE
  (by text) and CALL (the block's `Transaction::procedure`) goes through
  the block's transaction, and so binds only against its catalog. A
  record fixed in a block outlives it: run after COMMIT in autocommit, it
  is 0A000 when its columns differ at the live catalog.
- **`Sync`** replies with `ReadyForQuery`.
- **`Close`** drops a statement or portal and replies with `CloseComplete`.
  **`Flush`** writes the buffer.
- **An error.** It gets `ErrorResponse`. The server then reads and discards
  messages until `Sync`, which gets `ReadyForQuery`.

**The implicit transaction, outside `BEGIN`.** This follows PostgreSQL.
Before it runs an `Execute` outside `BEGIN`, the session reads the next
message.
- **The next message is `Sync`.** The block is a single statement, which is
  what `PQexecParams` sends. It runs through `Database::execute`'s
  autocommit path, with its conflict retry, exactly as a simple `Query`
  does and as `CONTRACT.md` states.
- **Anything else.** The block has two or more statements, and it runs as
  PostgreSQL's implicit transaction:
  - this `Execute` opens a `Transaction`, and every `Execute` up to `Sync`
    runs in it;
  - `Sync` commits it, and a commit error (`40001`, `08007`) is reported
    there, before `ReadyForQuery`;
  - an error aborts it, and the messages after it are discarded up to
    `Sync`, as now. Nothing in the block is committed.

Reading ahead never waits on a client that keeps to the protocol: a client
only expects output after a `Sync` or a `Flush`, so it has sent one. The
server looks past a `Flush` when the message after it has already arrived:
`Execute`, `Flush`, `Sync` sent together is one statement. When nothing
follows the `Flush` yet, the client may be waiting for this `Execute`'s
reply, so the server cannot wait to see whether `Sync` comes next: the
statement runs in the implicit transaction and its reply is sent at once.
libpq's one statement per `Sync` gets autocommit's retry, and a pipelining
client (psycopg's pipeline mode, JDBC's batches) gets PostgreSQL's
semantics. If the stream ends instead, the statement does not run: no
`Sync` would have committed it.

A `BEGIN` inside the block makes it explicit, as PostgreSQL does: the open
`Transaction` carries on past `Sync` until `COMMIT` or `ROLLBACK`.

A control statement that reads or changes what the block runs with (a
`SET`, `SET LOCAL` or `RESET` of a transaction mode or default, `RESET
ALL`, `SET TRANSACTION`, `SET SESSION CHARACTERISTICS`) is read ahead of
too: first of two or more statements, it opens the block's `Transaction`
before it runs, with the defaults as they were, so a `SET` of a default
reaches only later blocks and the block's failure undoes it. Alone before
`Sync`, it runs by itself. Other control statements (`BEGIN`, `COMMIT`,
`ROLLBACK`, the savepoints, `SET client_encoding`) open nothing.

Outside `BEGIN` a DDL runs as any other statement does (stage 4 of
transactional DDL, transaction_design.md):
- A DDL that is its block's only statement (the next message is `Sync`)
  runs alone through `Database::execute`, as a simple `Query` does, with
  autocommit DDL's retry.
- A DDL in a block of two or more runs in the block's implicit
  `Transaction`, and commits with the block's other statements at `Sync`,
  or nothing of the block does.

**DDL in a `BEGIN` block.** A DDL `Execute` or `Query` in a `BEGIN` block
runs through the block's `Transaction` (stage 3), and a `Parse` after it
fixes its record from the transaction's private plan: at the block's base
version, marked as fixed at a private catalog (`StatementRecord::private_catalog`).
Such a record keeps its columns after the block ends: run where they differ,
it is `0A000`, as PostgreSQL's revalidation is, or `40001` in a block pinned
at that base after the live catalog moved (transaction_design.md, *Savepoints,
cursors, portals and prepared statements across DDL*).

**Portals and 55006.** A `Bind` in a block binds through
`Transaction::bind_portal`, and a `SELECT` portal holds what its plan reads
(`PortalReads`) until it is closed, replaced, or dropped with its
transaction, as PostgreSQL's `PortalStart` opens a `PORTAL_ONE_SELECT`
portal's relations at `Bind`. An `INSERT`, `UPDATE` or `DELETE` portal,
with `RETURNING` or not, holds nothing: PostgreSQL opens its relations only
while it runs. A `DROP
TABLE`, `CREATE INDEX` or `ALTER TABLE` of a table a `SELECT` portal of the block
reads, or a `DROP INDEX` of an index one scans, is `55006` "cannot … because
it is being used by active queries in this session", as for an open cursor,
whether the portal ran or not. As in PostgreSQL:
- a `ROLLBACK TO` drops the portals bound since its savepoint
  (`AtSubAbort_Portals`), and a `RELEASE` hands them to the enclosing level;
- a simple `Query` drops the unnamed portal first, so the unnamed portal
  never refuses a `Query`'s DDL, and a `Bind` into it replaces the portal
  it held.

The simple-query path keeps `Database::execute`: one `Query` is one
statement, committed alone, which is PostgreSQL's implicit transaction for a
single statement.

## Types and text

Each `SqlType` has an OID, a length and a modifier, as `pg_type` and
`format_type` give them:

| Type | OID | Length | Modifier |
|---|---:|---:|---|
| `SMALLINT` | 21 | 2 | -1 |
| `INTEGER` | 23 | 4 | -1 |
| `BIGINT` | 20 | 8 | -1 |
| `REAL` | 700 | 4 | -1 |
| `DOUBLE PRECISION` | 701 | 8 | -1 |
| `BOOLEAN` | 16 | 1 | -1 |
| `BYTEA` | 17 | -1 | -1 |
| `TEXT` | 25 | -1 | -1 |
| `VARCHAR(n)` | 1043 | -1 | n + 4 |
| `CHAR(n)` | 1042 | -1 | n + 4 |
| `NAME` | 19 | 64 | -1 |
| `DECIMAL(p, s)` | 1700 | -1 | ((p << 16) \| s) + 4 |
| `numeric`, unconstrained (a `SUM`) | 1700 | -1 | -1 |
| `TIMESTAMP` | 1114 | 8 | -1 |
| `TIMESTAMPTZ` | 1184 | 8 | -1 |
| `DATE` | 1082 | 4 | -1 |
| `TIME` | 1083 | 8 | -1 |
| `TIME WITH TIME ZONE` | 1266 | 12 | -1 |
| `jsonb` | 3802 | -1 | -1 |

A parameter a jsonb path takes (`j #> $1`, `jsonb_design.md`, *Path
arguments*) is described as `text[]`, OID 1009; it is no layer type, and
its value is its text, read by `array_in`'s rules at bind (22P02 for a
malformed one, whatever rows the plan reaches) and again by the plan.

A result column's type is the one the compiler gives it
(`Compiled::column_types`), so the server only reports it:
- a column, its declared type;
- `COUNT`, `BIGINT`;
- `SUM` of a `SMALLINT` or `INTEGER`, `BIGINT`;
- `SUM` of a `BIGINT` or `DECIMAL`, PostgreSQL's unconstrained `numeric`,
  at the input's scale (`frontend_design.md`, *The dialect*).

Unconstrained `numeric` is OID 1700 with modifier -1, and its text has the
input's scale: `123.45` for a sum of `DECIMAL(6, 2)`. `SqlType` writes it
as a `DECIMAL` of precision 0 at that scale, the type the compiler gives
`SUM`.

**Output** is PostgreSQL's text form, with the `ParameterStatus` settings
above:
- integers in decimal;
- a `DECIMAL` with exactly s fraction digits (`5.00` in a `DECIMAL(6, 2)`);
- floats as the shortest text that reads back to the same value, as
  `float8out` and `float4out` write it with `extra_float_digits` 1:
  exponent form below 1e-4 and from 1e15 (1e6 for `REAL`), `NaN`,
  `Infinity` and `-Infinity`. Like PostgreSQL's Ryu, the digits must be
  strictly closer to the value than to either neighbour: text that reads
  back only by rounding a tie to even takes one more digit (`1e23` is
  `9.999999999999999e+22`);
- `t` and `f`;
- `jsonb` as `jsonb_out` writes it (`jsonb_design.md`, *jsonb output
  text*); a parameter the layer types jsonb is read as `jsonb_in` reads
  it, whatever OID the client gave, its 22P02 with PostgreSQL's DETAIL and
  CONTEXT (D and W) at `Bind`, where PostgreSQL refuses OIDs 25 and 1043
  with 42804;
- `bytea` as `\x` and lower-case hex;
- text as its bytes;
- `TIMESTAMP` as `YYYY-MM-DD HH:MM:SS`, then `.` and the fraction digits
  with trailing zeros dropped, then ` BC` for a year before 1;
- `DATE` and `TIME` as the same parts.

**Input** reads each parameter as its inferred type. The forms accepted
are:
- integers: an optional sign and decimal digits, with white space around.
  PostgreSQL 16's `0x`, `0o` and `0b` forms and `_` separators are `22P02`
  here (*Known differences*). Out of range is `22003`, from the layer's
  range check, as for any parameter.
- `DECIMAL`: decimal with an optional exponent.
  - An assigned parameter with more fraction digits than s is rounded
    half away from zero, as PostgreSQL rounds on assignment.
  - A compared or added parameter (`w_ytd + $1`) with more fraction digits
    than s is `22P02`.
    - Compared: PostgreSQL compares the unrounded value, which a scaled
      `i64` cannot hold.
    - Added: PostgreSQL rounds the sum, and half away from zero depends on
      the sum's sign, so rounding the operand first can differ. For
      example, -1.00 + 0.995 rounds to -0.01, but -1.00 + 1.00 is 0.00.

    The server refuses rather than give either value. `ParamInfo` says
    which use a parameter has (*What it uses from the embedded API*, 1).
  - `NaN` is `22P02`.
- floats: what `strtod` reads, plus `NaN`, `Infinity`, `-Infinity`
  (`inf` too), any case. Rounding a `REAL` is the layer's.
- `BOOLEAN`: PostgreSQL's `boolin`: `t`, `true`, `y`, `yes`, `on`, `1` and
  their negatives, any case, any unique prefix (`on` and `off` need two
  letters), white space around.
- `BYTEA`: hex (`\x…`) and escape format (`\\`, `\nnn`).
- text: the bytes; the layer checks UTF-8 (`22021`).
- `TIMESTAMP`: `YYYY-MM-DD`, then optionally a space or `T` and
  `HH:MM[:SS[.f…f]]`, then optionally ` BC`.
- `DATE`: `YYYY-MM-DD`, optionally ` BC`.
- `TIME`: `HH:MM[:SS[.f…f]]`.
- In both, as PostgreSQL's `DecodeTime` reads them: a fraction of any
  number of digits, rounded to the microsecond as `rint(strtod(".f…f") *
  10^6)` rounds it (to even, so `.1234565` is `.123456`), carrying into the
  seconds; second 60, a leap second, as the next minute's first; hour 24,
  minute 59 and second 60 at most, and at most `24:00:00` in all, which a
  timestamp reads as the next day's midnight (22008 past it).

Other datetime forms (`now`, `epoch`, `infinity`, a time zone, names of
months) are `22007`. That is a known
difference: the driver writes ISO.

## Errors

`ErrorResponse` carries:
- `S` and `V`: `ERROR`, or `FATAL` before the connection closes;
- `C`: the SQLSTATE;
- `M`: the exception's message;
- `D` and `H`: PostgreSQL's DETAIL and HINT, where the error has them
  (`error_detail`, `error_hint`): an `ast::SqlError`'s or a `DdlRefused`'s
  own (an `INSERT` through a view PostgreSQL does not write through is the
  first to carry them), and a runtime error's (`ExecError::note`, `hint`,
  from `bc_error_note` and `bc_error_hint`). Other errors send neither,
  where PostgreSQL may;
- `P`: for an `ast::SqlError`, its position, one-based and in characters,
  converted from the layer's byte offset;
- `W`: PostgreSQL's CONTEXT, where a runtime error has one
  (`error_context`, `ExecError::context`, from `bc_error_context`). No
  runtime call sets one before jsonb (`jsonb_design.md`, *jsonb runtime
  errors and their SQLSTATEs*).

The embedded API builds a runtime error's `ExecError` with its message and
lines under a `std::bad_alloc` guard, dropping all the text if it cannot be
allocated, and throws it by move, which allocates nothing: memory that runs
out once the runtime raised the error never turns its SQLSTATE into 53200.
Until jsonb, the tests reach `H` and `W` end to end through
`Database::before_commit_for_testing`, a hook that fails an autocommit
write's context with `bc_ctx_fail_for_testing` before its commit.

Each field is sent only where it is not empty, in PostgreSQL's order: `S`,
`V`, `C`, `M`, `D`, `H`, `P`, `W` (`pg::put_error_fields`).

Warnings are `NoticeResponse` with `S` `WARNING`. A statement's notices
(`Rows::notices`: a DDL that `IF EXISTS` or `IF NOT EXISTS` skipped, and
what `DROP SCHEMA … CASCADE` dropped) are `NoticeResponse` with `S`
`NOTICE`, PostgreSQL's SQLSTATE and text, and `D` when the notice has a
DETAIL (`Notice::detail`), sent
before the statement's `CommandComplete` in both protocols.

The SQLSTATE comes from one function in the API, `sqlstate(const
std::exception&)` (*What it uses from the embedded API*, 4). The server, the Python binding and
the proof oracle all use it, so the oracle's check of SQLSTATEs against
PostgreSQL also checks the server's mapping. The mapping:

| Raised | SQLSTATE |
|---|---|
| `ExecError`, `BC_E_CONSTRAINT`, `BC_C_UNIQUE` | `23505` |
| `BC_C_NOT_NULL` | `23502` |
| `BC_C_RANGE`, by the check that failed | `22003` (`smallint`/`integer`/`numeric field overflow`/`REAL`), `22001` (`VARCHAR` too long), `22021` (encoding), `22008` (datetime) |
| `BC_E_LIMIT` | `54000` |
| `BC_E_ARITH` | the SQLSTATE `bc_error_detail` packs (`ExecError::state`): `22012` for a zero divisor (`bc_div`, `bc_mod`), `22011` for a negative substring length (`bc_substr`), `22023` for random's bounds (`bc_random`), `22003` for an overflow or a range the helpers check; a detail of 0 is `22003`. No message is matched |
| `BC_E_DATA` | the SQLSTATE `bc_error_detail` packs; `22000` for one that packs none, which the runtime never raises |
| `SerializationFailure`, a conflict on every retry, `DdlInProgress` | `40001` |
| `TransactionResolutionUnknown` | `08007` |
| `TransactionAborted` | `25P02` |
| `FeatureNotSupported` | `0A000` |
| `ParamError` | `08P01` (the server's own count check makes this unreachable over the wire) |
| `ast::SqlError`, `DdlRefused` | the code it carries, PostgreSQL's for the same fault; `42601` by default (below) |
| `BC_E_IO`, `BC_E_CORRUPT`, `BC_E_DEGRADED` | `58030` |
| `BC_E_MEMORY`, `std::bad_alloc` | `53200` |
| `DbClosed` | `57P01` |
| anything else | `XX000` |

`SqlError` carries its SQLSTATE the way `DdlRefused` does, set where it is
thrown. Both take it from one flat set of constants (`sql/sqlstate.cppm`),
so a throw site cannot name a code that is not in it. The layer claims
PostgreSQL's code only for a statement PostgreSQL also refuses with it:

| Fault | SQLSTATE |
|---|---|
| unknown table, or a qualifier naming no table of the statement | `42P01` |
| unknown column | `42703` |
| a column assigned from a column of a type PostgreSQL has no assignment cast from (a number to or from a boolean, a string to a number, ...) | `42804` |
| one `$n` compared with two columns no operator relates (a string and a number, a boolean and anything else, ...) | `42883` |
| `SUM` or `MIN` of a type PostgreSQL has no such aggregate for | `42883` |
| an arithmetic operator PostgreSQL has none of for its operands' types (`date + date`, `integer - date`) | `42883` |
| an arithmetic operator PostgreSQL has none of for a date or time type beside an operand of type unknown (`date * unknown`, `unknown / time`) | `42883` |
| `time + time`, and `date + unknown`, which PostgreSQL finds ambiguous | `42725` |
| a `$n` no use gives a type | `42P18` |
| a column reference both joined tables have | `42702` |
| a column listed twice in an `INSERT` | `42701` |
| an aggregate beside a bare column or in `ORDER BY`, or in an `UPDATE` | `42803` |
| `FOR UPDATE` with an aggregate | `0A000` |
| an operand that overflows a `DECIMAL` (PostgreSQL raises it when a row is updated; the layer, before the plan runs) | `22003` |

Every other `SqlError` is `42601`: a syntax error, and a form the layer
does not support where PostgreSQL would run the statement. Those include a
column assigned from a number of another width or any type into a string
(`SET dbl = bigint_col`, `SET text_col = int_col`), a number compared with
another number of a different physical type (`d = $1 AND dbl = $1`), `SUM` of a
`REAL`, `DOUBLE PRECISION` or `TIME` column, `MIN` of a `REAL`, `DOUBLE PRECISION`
or string column, a join on columns of two types, and an unsupported
`DEFAULT` or type. So are a `$n` used with two types when either use
assigns it: PostgreSQL types a `$n` by its first use, which for `UPDATE` is
its `WHERE` before the `SET` the layer binds first, and refuses an untyped `$n`
assigned to two columns of different types with `42P08` whatever the types.
The planner plans every statement the binder takes; its "no plan"
refusal, `FeatureNotSupported`, `0A000`, is for a logical plan the binder
never builds (`planner_design.md`, *What the search refuses*). HammerDB's driver sends
no statement that fails to bind.

## What it uses from the embedded API

Five calls of `api/database.cppm` exist for the server. All read what the
binder and compiler already keep:
1. `Statement::params() -> std::vector<ParamInfo>`, with `ParamInfo
   {SqlType type; bool assigned_only;}`.
   - `type` is the declared type of the column of the parameter's first
     use, which the binder keeps (`ParamUse::first`).
   - `assigned_only` is true when every use is an assignment, which is the
     binder's `param_sql` being empty. A compared or added parameter is not
     assigned-only.

   The server needs it for `ParameterDescription` and for text input.
   `param_types()`, the physical types, stays for its callers.
2. `Statement::columns() -> std::vector<std::pair<std::string, SqlType>>`,
   from `Compiled::columns` and `column_types`. These are of the plan the
   statement runs now, as `param_types()` is. The server needs it for
   `Describe` before the statement runs.
3. `Statement::command() -> Command`. `Command` is an enum of `Select`,
   `Insert`, `Update`, `Delete` and each DDL statement, from the AST, for the
   command tag.
4. `sqlstate(const std::exception&) noexcept -> std::string_view`: the
   table above. The range split moves into the catalog beside
   `out_of_range`, which writes every range message, so the split lives
   where those messages are written. The binding exposes it as
   `ExecError.sqlstate`, which the oracle reads.
   `test_every_range_message_maps_to_its_sqlstate` proves that every
   message maps.

5. `Database::stats()`: the engine's `DB::stats()`, under the shared lock
   every call takes (*Reading the counters*).
6. `Database::refit(text, check)` and `Transaction::refit(text, check)`:
   the `StatementRecord` of the text's run, under a `ResultCheck`, against
   the live catalog or the transaction's; they run nothing. The server
   takes the record at `Parse` with `none`, and checks it at `Bind` and
   `Describe` with `given`, which binds the text with the record's
   parameter types and is 0A000 for other columns. A DDL between `Bind`
   and `Execute` is caught by the run, which checks the record again. In a
   block, `Bind` takes `Transaction::bind_portal(text, check)`: the same
   record, and the `PortalReads` its portal holds for 55006.
   `Transaction::procedure(name)` gives `CALL` in a block the procedure of
   the block's catalog.

`DdlRefused` carries the SQLSTATE PostgreSQL gives the same refusal, for (4).

## The HammerDB driver

The gate's driver is `hammerdb/bcdboltp.tcl`, a HammerDB 6.0 custom script
(`customscript`) whose logic is HammerDB 6.0's PostgreSQL stored-procedure
driver's (`src/postgresql/pgoltp.tcl`, `pg_storedprocs=true`, the timed
driver):
- **Each virtual user** draws from the same deck (`RandomNumber 1 23`) with
  the same inputs: the district, the customer by `NURand` id or, 60% of the
  time, by `NURand` last name, the remote warehouse 15% of Payments, the
  amount, the line count, the threshold. Each transaction is one `CALL` of
  a procedure of `hammerdb/tprocc_procs.sql`, which runs it on the server
  in one transaction (`procedures_design.md`, *Running a procedure*),
  retried there on 40001 as an autocommit statement is.
- **The procedures** draw New-Order's lines with `random(lo, hi)`, as
  HammerDB's do with its `DBMS_RANDOM`, and pick a customer by last name
  with `FOR … IN query`. Their SQL is the harness's statements
  (`harness/tprocc.cppm`), which the proof suite checks against PostgreSQL
  17; the file runs on PostgreSQL 17 unchanged.
- **Only the call syntax is the layer's:** arguments in the procedures'
  order, NULL for the `INOUT` ones, the time as a timestamp literal, the
  item count, and Payment's history key, `(run << 48) | (position << 40) |
  n`, which the driver draws as the harness does, since `history.h_id` is
  the table's key.
- **A failed call** is counted by its SQLSTATE and not retried, as
  HammerDB's drivers do not retry. New-Order's 1% unused item fails its
  call with P0002 (`SELECT … INTO STRICT`), which rolls it back, as the
  benchmark asks: not counted as an error.
- **The monitor** is HammerDB's: TPM from `select sum(xact_commit +
  xact_rollback) from pg_stat_database`, NOPM from `select sum(d_next_o_id)
  from district`, then `testresult`.
- **Its home warehouse** is fixed by position, as below.

`run-gate.sh` creates the procedures in the load (`sql_layer_tprocc procs`),
and, with `--smoke`, on PostgreSQL with `psql`. `--driver=custom` runs the
older script, below, until it is removed (#758).

`bytecaskdb-sql-layer/hammerdb/bytecask_tprocc.tcl` is a HammerDB custom
script (`customscript`). It is a port of the client-side TPROC-C logic in
HammerDB's MySQL driver, `src/mysql/mysqloltp.tcl`: the `neword`,
`payment`, `ostat`, `delivery` and `slev` procedures of
`insert_mysql_no_stored_procs`, and the timed driver's monitor. The ported
script uses pgtcl and runs the statements of `testing_design.md` (*The
TPROC-C statements*), with `$n` parameters.

It was written when the layer had no procedures, which HammerDB's
PostgreSQL driver needs.

**A virtual user** connects as above.
- **Its run.** The run script takes one run per cell before the server
  starts: `sql_layer_tprocc next-run --dir` makes the `harness_run` bump
  offline and prints the run. It writes the run into the script's header.
  A virtual user's `h_id`s are `(run << 48) | (position << 40) | n`, the
  harness's scheme with the position as the thread, so users never race on
  `harness_run`. The harness's limits hold: 2^15 runs, 256 positions.
- **Its warehouse.** Its home warehouse is fixed by position:
  `((position - 2) mod W) + 1`, position 1 being the monitor. HammerDB's
  default, a random home warehouse, draws correlated numbers across
  virtual users (`run-hammerdb.sh`, `--warehouses`), so users would share
  rows whatever W is. The run script requires W ≥ the user count.
- **Its loop.** It runs HammerDB's mix, `RandomNumber 1 23`, with no keying
  or thinking time. Each transaction is `BEGIN`, the statements, then
  `COMMIT`, or `ROLLBACK` for New-Order's 1%.
- **A failed statement.** The user rolls back, counts the error by its
  SQLSTATE, and moves on; it does not retry, as HammerDB's drivers do not.
  A `40001` costs the throughput it costs. With `raiseerror` set, the user
  stops instead.
- **Its counts.** Each transaction adds exactly one outcome to a `tsv`
  counter:
  - committed;
  - rolled back, for New-Order's 1%;
  - failed, for an error, whose `ROLLBACK` is not counted again.

  These stand in for `Com_commit` and `Com_rollback`.

**The monitor**, position 1, is HammerDB's timed monitor.
1. It waits out the ramp-up.
2. It reads both counters at the start of the measured window. It sums
   `d_next_o_id` with S1 (`SELECT d_next_o_id FROM district WHERE d_w_id =
   $1 AND d_id = $2`) over every district, in one transaction, so the sum
   is one snapshot; the layer has no full-table `SUM`.
3. It sleeps for the duration, then reads both again.
4. It prints `testresult $nopm $tpm` with HammerDB's own procedure:
   - NOPM is the `d_next_o_id` delta per minute, the benchmark specification's definition and
     HammerDB's: committed New-Orders only, as `sql_layer_tprocc` counts them;
   - TPM is transactions per minute, one per outcome, which
     `xact_commit + xact_rollback` would count.
5. It sets HammerDB's `abort` flag, and the users stop.

The output is HammerDB's line, `System achieved N NOPM from M … TPM`.

**The statements.** They are those of `harness/tprocc.cppm`, which the proof
suite checks against PostgreSQL 17, and the script must not drift from
them. `sql_layer_tprocc statements` prints the harness's, and
`scripts/check_driver_statements.py` (in the layer) fails CI if the
script's set differs.

The script runs all five transaction types; the run script's `--mix` can
limit it to some. The gate is the full mix.

**The same script against PostgreSQL 17.** The statements are
PostgreSQL's, so the script runs unchanged on PostgreSQL loaded with the
same population.
- `sql_layer_tprocc load --sql-out FILE` writes that population, from the
  same seed, as a `psql` script:
  - the schema fixture's DDL, which is PostgreSQL's dialect;
  - `harness_run (1, 0)`;
  - one `COPY … FROM STDIN` per table.
- `psql` loads it.
- `hammerdb/pg_consistency.sql` holds the nine conditions as the harness
  runs them. That is how the driver itself is checked: the smoke test runs
it against PostgreSQL as well, and the consistency conditions must hold
there too. The gate reports only ByteCaskDB's numbers
(*Decisions*, 10): PostgreSQL's throughput from these
runs is not part of its output.

## Running the gate

`bytecaskdb-sql-layer/hammerdb/run-gate.sh [--warehouses=48] [--vus=1,16,48]
[--rampup=2] [--duration=5] [--mix="neword payment ostat delivery slev"]
[--driver=bcdboltp|custom] [--hammerdb-home=PATH] [--data-root=PATH]
[--server-flags="--io-backend buffer_pool"]
[--smoke --pg=CONNINFO]`. `--server-flags` goes to the server in every cell. It
follows `run-hammerdb.sh` in the MariaDB plugin. `--smoke` is the short run
of *Tests*, and `--pg` names the PostgreSQL it checks the driver on (its
password in `PGPASSWORD`). `XMAKE_FLAGS` adds `xmake f` flags, as CI sets
its toolchain.

**The two sync modes.** Without `--sync` in `--server-flags`, each user count
runs twice, on a fresh copy each: `AT_EVERY_COMMIT` and `AT_INTERVAL` at 1000 ms
(or at the flags' `--sync-interval-ms`). They test different things:
`AT_EVERY_COMMIT` on a SATA SSD mostly measures `fdatasync`, and `AT_INTERVAL`
measures the layer. So a default gate takes twice as long as one mode. With
`--sync`, only that mode runs. Compare only rows of the same sync mode.
`--smoke` runs `AT_EVERY_COMMIT` alone: it checks the driver and the conditions,
never the numbers.

**The data outgrows its load.** On 244ae692, a 48-warehouse load of 6 GB grew
to 23–30 GB over a 2+5 minute cell at 16 and 48 users, so a buffer pool sized
from the load does not keep the data in memory. Each cell reports its final
size. Size a pool from that, at the same user count and length.

TinyCC is built with `--with-selinux` whenever SELinux is enabled on the
build machine (`xmake.lua`): in enforcing mode its default way of making
plan code executable, `mprotect`, is refused, and no plan links. It maps
the code through a file in `/tmp` instead, which works in every SELinux
mode but needs `/tmp` to allow executable mappings.

1. **Build.** It builds `sql_layer_tprocc` and `sql_layer_server` in
   `releasedbg`; a debug engine walks the key directory on every commit.
   Each `xmake` call passes `-P` with the script's own tree, so a worktree
   under `.claude/worktrees` builds itself and not the checkout around it.
   The verbose output is `build.log` in the data root.
2. **Load.** It loads W warehouses once with `sql_layer_tprocc load`, not
   HammerDB's `buildschema`, and keeps the directory as the pristine copy.
   With `--smoke`, `--sql-out` also writes the population for PostgreSQL.
3. **Per user count and sync mode:**
   - restore the copy (a reflink where the filesystem has them);
   - take a run with `sql_layer_tprocc next-run`, written into the driver's
     header;
   - start the server on a free port, and wait for its `ready` line, up to
     5 minutes because a large pool touches its arena at open; a server that
     exits first, or does not make it, fails the script with its log, and
     is not left running;
   - run `hammerdbcli auto` on a generated file, which sets
     `dbset db pg`, `dbset bm TPC-C`, `diset connection pg_host`,
     `pg_port` and `pg_sslmode disable`, `diset tpcc pg_user` and
     `pg_dbase`, then `customscript` (the script, with the run's settings
     written into its header, as HammerDB's editor does), `custommonitor
     timed`, `vuset vu N`, `vucreate`, `vurun`, `vucomplete` and
     `vudestroy`;
   - parse the result line;
   - stop the server with `SIGTERM`;
   - measure the data directory's apparent size;
   - run `sql_layer_tprocc check --dir`, the nine conditions, on what the
     run left.

   A cell whose check fails, or whose run printed no result, fails the
   script, whatever its NOPM.
4. **With `--smoke`, PostgreSQL.** A database of its own, the population
   loaded with `psql`, the same driver with its engine named PostgreSQL,
   then `pg_consistency.sql`: the conditions must hold there too. Its
   numbers are printed by HammerDB and not recorded.
5. **Results.** One row per cell goes to
   `bytecaskdb-sql-layer/benchmarks/hammerdb_results.csv`, which the
   layer's `.gitignore` ignores: date, commit, W, users, ramp-up,
   duration, NOPM, TPM, errors by SQLSTATE, the check's result, the driver,
   the server's flags, the final data size in bytes and the sync mode, all
   of our server; the report prints the same. No PostgreSQL row is written.
   A file with an earlier header of this one is rewritten to it once, its
   rows with the columns they lack empty, which means not recorded. Any other
   header stops the script before it builds.
   Each cell also appends `pg_stat_bytecask`, read when the driver ends and
   before the server stops, to `benchmarks/hammerdb_counters.csv`, ignored
   too: date, commit, W, users, sync mode, name, value, one line per counter, so a new
   counter changes no column. `hammerdb/read_counters.py` reads it with the
   standard library, because the gate's machines need not have `psql`. A failed
   read is reported and loses that cell's counters, not its result.

The harness gains three commands for this:
- `check`, over its existing `consistency()`;
- `next-run`, the `harness_run` bump it already makes per phase;
- `load --sql-out FILE`: after the load, what was loaded as a psql script
  (the schema's DDL with `COLLATE "C"` on each `VARCHAR`, then one `COPY`
  per table, values written by `pg_text`), so PostgreSQL holds exactly the
  layer's rows.

The run script requires W ≥ the user count; the monitor is not a user.

## Tests

- **The proof machine over the wire.** `tests/proof/`'s state machine gains
  a third target. It already checks the embedded layer against PostgreSQL
  17; now every statement also goes through the server with psycopg, with
  every OID loaded as `str`. psycopg is set to send only what the server
  takes:
  - text placeholders (`%(pn)t`), so parameters go in text format; psycopg's
    default dumpers are binary for several types, which would be `0A000`.
    They are named by their `$n`, so a `$n` the statement uses twice is one
    parameter, as written;
  - `prepare_threshold=None`, so it never prepares server-side or sends
    `DEALLOCATE ALL`;
  - no `SET` of any kind, `search_path` included. The wire results must match PostgreSQL's text,
  byte for byte, and its SQLSTATEs. That proves `pg_text` (output and
  input), the tags, the status letters and the error mapping, on the
  statements and values the machine generates.
- **`tests/server/test_wire.py`**: protocol flows psycopg does not produce,
  over a raw socket:
  - `SSLRequest` and `GSSENCRequest` answered `N`;
  - protocol 3.2 negotiated down;
  - each startup refusal;
  - an error in an extended sequence discarding to `Sync`;
  - two `Execute`s before `Sync` outside `BEGIN`, the second failing:
    neither is committed;
  - two that succeed, committed together at `Sync`;
  - a lone DDL `Execute` run, and one beside DML `Execute`s, by `Execute`
    or by a `Query` inside the pipeline, committed with them at `Sync`, or
    nothing committed if one of them fails;
  - DDL in a `BEGIN` block, by `Query` and by `Execute`: seen by the
    block, undone by `ROLLBACK` (a statement Parsed on it is `42P01` at its
    next `Bind`) and by `ROLLBACK TO`, published by `COMMIT`; a `Parse`
    after the block's `ADD COLUMN` is `0A000` after `ROLLBACK`; `55006` for
    a DDL on what a portal of the block reads;
  - `Terminate` and a dropped connection rolling back an open
    transaction;
  - `COMMIT` of an aborted transaction tagged `ROLLBACK`;
  - the block status: inside `BEGIN`, a statement that fails to bind, or a
    parameter that fails to parse, makes the block `E`; later statements
    are `25P02`; `COMMIT` answers `ROLLBACK`, and nothing is committed;
  - after an error in an implicit block, `Sync` reports `I`;
  - the warnings;
  - `--max-connections` and `--workers`;
  - a slow reader on `--workers 1` holding no worker, and a long pipeline
    yielding at `ReadyForQuery`;
  - `--sync`, `--sync-interval-ms` and the I/O flags: each mode starts and
    stops cleanly, and a bad value exits before the server listens;
  - a pipelining client that takes no replies: the session runs nothing past
    64 MiB unsent, the server reads no more and holds far less than the
    300 MB of replies, and every reply arrives once the client reads;
  - `--max-memory-bytes`: a statement past it is `53200` and fails its block;
  - each thread the server starts, failed in turn by an `LD_PRELOAD` shim
    (`tests/server/fail_thread.c`): an error and exit 1, never an abort;
  - `--vacuum` and its pauses and threshold: on by default the data directory
    shrinks under an overwrite load and SIGTERM still exits 0, off it keeps
    what was written, a sealed file about 63% dead stays at the default
    threshold and goes at 0.5, and a bad value exits before the server listens.
- **`tests/vacuum_test.cpp`** (`sql_layer_tests "[sql][server]"`, `"[sql][api]"`):
  `VacuumThread` with a scripted pass (its pauses, a stop during one, a failure
  retried, `DbDegraded` ending it), and over a real database, reclaiming disk,
  and a transaction and a hold cursor across a vacuum.
- **`tests/pg_session_test.cpp`** (`sql_layer_tests "[sql][server]"`):
  sessions moving across worker threads while keeping message order (under
  ThreadSanitizer in CI), `Execute` lookahead parking, pipeline fairness, a
  session held back past `max_unsent_bytes` until its replies are taken,
  write skew between two `SERIALIZABLE` sessions (40001), `SERIALIZABLE` on
  a follower (0A000), and the transaction defaults across `COMMIT`,
  `ROLLBACK`, `RELEASE` and `ROLLBACK TO`.
- **`SERIALIZABLE` and the run-time parameters over the wire**
  (`test_wire.py`): each SQLSTATE, message and value of *Transaction
  modes* and *Run-time parameters* over simple and extended query; write
  skew and the range phantom over two connections (40001), disjoint ranges
  committing; a read under a savepoint's `READ ONLY` failing the commit;
  and, with `BYTECASK_SQL_ORACLE` set, the forms statement by statement
  against PostgreSQL 17.
- **HammerDB smoke, nightly.** A scheduled workflow,
  `.github/workflows/sql-layer-hammerdb-nightly.yml`, runs `run-gate.sh --smoke`: two warehouses, two
  users, no ramp-up and a one-minute window, against the server and against
  PostgreSQL 17. It then runs `sql_layer_tprocc check`, and
  `pg_consistency.sql` on PostgreSQL. It needs HammerDB's Linux release, cached
  between runs. It can also be started by hand (`workflow_dispatch`).
  The workflow file has to live in `.github/workflows/`, outside the layer.
  It goes in `ci.yml`'s `paths-ignore` lists, for push and for pull
  request, next to `sql-layer.yml`, so a change to it does not run the
  engine pipeline. Per PR, CI runs the wire proof target and
  `test_wire.py`.
- **`scripts/check_driver_statements.py`** in CI.

No microbenchmarks: the gate's numbers are HammerDB's.

## Known differences from PostgreSQL

- Parameter OIDs from the client are not used.
- Binary format, for parameters and results, is `0A000`.
- A compared or added `DECIMAL` parameter finer than its column's scale, and `NaN`,
  are `22P02`.
- The integer and datetime input forms listed above only.
- A form the layer does not support is `42601`, not PostgreSQL's code for running it.
- `pg_stat_bytecask` is not a relation: only `select * from pg_stat_bytecask` reads it, and PostgreSQL has no such view.
- `Execute` with a row limit, several statements in one `Query`, SQL
  `PREPARE`, `COPY`, and `SET`, `SHOW` and `RESET` of parameters other than
  the transaction modes, their defaults and `client_encoding` are refused.
- An extended-query pipeline outside `BEGIN` becomes a block at its second
  statement (*Extended query*); PostgreSQL's unnamed transaction starts
  with its first message. The two agree for every first statement whose
  effect depends on the block: a `SET`, `SET LOCAL` or `RESET` of a
  transaction mode or default, `RESET ALL`, `SET TRANSACTION` and `SET
  SESSION CHARACTERISTICS` first of several statements start the block
  before they run, so the block runs with the defaults it found, a `SET
  LOCAL` or `SET TRANSACTION` lasts until `Sync`, and a failure of the
  block undoes the `SET`. A `SET client_encoding` there runs by itself.
- A `SERIALIZABLE` commit's `40001` carries PostgreSQL's message and hint,
  not its DETAIL (a reason code such as "Canceled on identification as a
  pivot, during commit attempt."). Which transactions fail differs from
  PostgreSQL's where its predicate locks are coarser or finer than the
  layer's read set (`transaction_design.md`, *Differences from PostgreSQL
  17 for serializable isolation*): two transactions that read and write
  disjoint ranges of a small table both commit here, where PostgreSQL may
  fail one.
- With `default_transaction_isolation` set to `serializable` on a follower,
  `BEGIN` is `0A000`; PostgreSQL refuses the block's first query, with
  another hint.
- `CALL` is taken as a simple `Query` only, its arguments constants, and is
  refused in a `READ ONLY` block (`procedures_design.md`, *Wire protocol
  for `CALL`*). `pg_stat_database` answers one query, the transaction
  count HammerDB's monitor reads.
- The idle-in-transaction timeout is on by default (60 s); PostgreSQL's
  `idle_in_transaction_session_timeout` is off by default. An open
  transaction holds a snapshot (`session_design.md`, *Bounds*).
- A cancel or statement timeout is seen where a statement charges its reads,
  every 64: a statement that reads fewer runs to its end.
- Cursors: `DECLARE` outside `BEGIN` is a hold cursor, where PostgreSQL says
  `25P01`; `WITH HOLD ... FOR UPDATE` and `BINARY` are `0A000`
  (`cursor_design.md`, *Known differences from PostgreSQL*).
- A result is built whole by the embedded API before its first `DataRow`
  is sent. Streaming rows waits on the API streaming them, part of the
  transport design (#487, *Next*).
- `Database::prepare`'s cache keeps every distinct text until close, so a
  client sending ever-new literal text grows it. The driver sends a fixed
  set (#549).
- PostgreSQL has no server-wide memory cap: past `--max-memory-bytes` a
  statement is `53200` where PostgreSQL would run it, or be ended by the
  operating system.
- The output bound is per connection and outside the memory cap: in the
  worst case, every connection pipelining and none reading, the server holds
  up to `--max-connections` × 64 MiB of replies besides the cap. Folding them
  into the cap would let one client's unread replies fail another's
  statements.

## Decisions

1. The PostgreSQL protocol before gRPC, as a front over the embedded API.
2. Originally one thread per connection; replaced by `session_design.md`
   (sessions as plain data structures scheduled onto a worker pool by a
   libuv loop).
3. Trust only, loopback by default.
4. A ported pgtcl driver script, with `PQexecParams` per statement and no
   SQL `PREPARE`.
5. NOPM from the `d_next_o_id` sum, and TPM from client-side counts.
6. One SQLSTATE mapping, in the API, shared by the server and the oracle.
7. The five API additions.
8. Outside `BEGIN`, a block of one `Execute` autocommits through
   `Database::execute`, with its retry. A block of two or more is one
   implicit `Transaction`, committed at `Sync`, as in PostgreSQL. A DDL
   there was `0A000` beside others until stage 4 of transactional DDL,
   which runs it in the block's `Transaction`. The session owns the block status (coordinator review).
9. The HammerDB smoke runs nightly, as a scheduled workflow, not per PR
   (owner, 2026-10-22).
10. The gate reports ByteCaskDB's NOPM and TPM only. PostgreSQL 17 runs the
    driver to check it (the nightly smoke), and its throughput is not
    reported (owner, 2026-10-22).
11. The gate is HammerDB's TPROC-C, with the layer's statements, through
    this server at 1, 16 and 48 virtual users. It replaced the in-process
    harness as the gate (owner, 2026-10-21).
