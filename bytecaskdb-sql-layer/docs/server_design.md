# The PostgreSQL wire-protocol server: slice 1, for the MVP gate

Status: design, approved (owner, 2026-10-22). #494's first slice.

The MVP gate is HammerDB's TPROC-C driving the layer over PostgreSQL's
wire protocol (`frontend_design.md`, *The MVP gate*). This slice is the
server that HammerDB connects to and the HammerDB driver script that sends
our statements. The harness of `testing_design.md` (*The TPROC-C
harness*) stays: it checks the consistency conditions, runs Elle and the
CI smoke test, and gives the no-network baseline. It is not the gate.

#494 planned a gRPC server first, with the PostgreSQL protocol as a shim in
front of it. The gate needs the PostgreSQL protocol and nothing else, so the
protocol comes first, as a thin stateful front over the embedded API. gRPC
stays in #494, after this slice.

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
- `BEGIN`, `COMMIT` and `ROLLBACK` as SQL statements;
- one session per connection;
- errors as `ErrorResponse`, with PostgreSQL's SQLSTATE for every error the
  layer raises;
- `hammerdb/`: the driver script and the script that runs the gate.

Out, each refused with a stated error rather than half-supported: SSL and
GSSAPI encryption, every authentication method but trust, binary format,
`COPY`, the function-call protocol, `CancelRequest`, `SET` (but `SET
client_encoding` to UTF8) and `SHOW`, SQL
`PREPARE`/`EXECUTE`/`DEALLOCATE`, `Execute` with a row limit, several
statements in one `Query`, and the system catalogs (`pg_catalog`,
`information_schema`).

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
127.0.0.1:5432] [--database NAME] [--max-connections 100]
[--sync AT_EVERY_COMMIT|AT_INTERVAL|AT_FILE_ROTATION] [--sync-interval-ms 1000]
[--io-backend pread|mmap|buffer_pool] [--buffer-pool-size BYTES]
[--buffer-pool-direct-io on|off]` serves one
database directory under one database name, by default the directory's
last component. It opens the directory with `Database::open`, so it holds
the engine's directory lock. The code is in `bytecaskdb-sql-layer/server/`:
- `pg_wire.cppm` (`bytecask.sql.pg_wire`): message framing. It reads a
  message from a buffer and writes one into a buffer; it does no I/O and has
  no policy.
- `pg_text.cppm` (`bytecask.sql.pg_text`): each SQL type's OID, its text
  input and its text output.
- `pg_session.cppm` (`bytecask.sql.pg_session`): one connection's protocol
  state machine, over a file descriptor.
- `main.cpp`: arguments, the listener, signals.

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
  engine: every later write fails, and `close()` throws `DbDegraded`, naming
  the commits that are not durable (the server then exits non-zero). No sync
  can succeed after that, so the flusher stops. Any other failure is logged
  once and retried at the next interval. No test of the layer reaches this
  path: its tests have no way to fail an `fdatasync`.
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

**Threading.** One thread per connection, and an acceptor thread. A
connection's thread reads a message, runs it on the embedded API, which
borrows the thread as it does for any caller, and writes the reply. The
layer never makes one caller wait for another:
- a transaction takes no locks;
- `FOR UPDATE` is a guard checked at commit;
- a conflict is a `40001` at commit, not a wait.

So the server adds no waits either. A thread blocks only on its own
socket, and on its own commit's `fdatasync`, which the engine's group commit
shares with concurrent commits.

No state is shared between connections but the `Database`, which is
thread-safe, and the registry of live connections, whose mutex is held only
to add or remove one.

A worker pool is rejected. It would make a connection wait for a worker
another connection's `fdatasync` holds, and the per-statement work is a
call, not something a queue could batch.

A connection past `--max-connections` gets `FATAL 53300`
(`too_many_connections`) and is closed, as PostgreSQL does. The default,
100, is PostgreSQL's, and covers the gate's 48 virtual users and its
monitor.

**I/O.** Blocking sockets with `TCP_NODELAY`. Replies are buffered and
written when the server next has to read: at `ReadyForQuery`, at `Flush`,
or when the buffer passes 64 KiB. A libpq round trip is then one read and
one write.

A message longer than 64 MiB is `FATAL 08P01` and closes the connection.
That bound sits well above the engine's 4 MiB default value limit plus a
statement's text.

**Shutdown.** `SIGINT` or `SIGTERM` closes the listener. The server then
calls `shutdown(fd, SHUT_RD)` on every registered connection, so a thread
blocked in `read` wakes at once, idle or not. A statement in flight finishes
first: its reply still goes out, since the write side stays open. The
thread then sends `FATAL 57P01` (`admin_shutdown`), rolls back an open
transaction and exits. A send never blocks in the kernel: it waits in
`poll` a second at a time, so it sees the stop. A client that has stopped
reading takes no bytes; once the stop has begun, a send that makes no
progress for ten seconds drops the connection without the `57P01`. One that
is still reading, however slowly and however large its result, gets its
reply and then the `57P01`, so it learns whether its last statement
committed. When every thread has ended, the server calls
`Database::close()`.

**After a `FATAL`** the session ends: it reads no more messages and runs
nothing more. After startup, an invalid message length or type is `FATAL`.

## Startup

1. **The request.**
   - An `SSLRequest` or `GSSENCRequest` is answered `N`, and the next
     message is read on the same connection.
   - A `CancelRequest` closes the connection with nothing done. There is no
     cancel, so no `BackendKeyData` is sent.
   - A `StartupMessage` of protocol 3.0 is read.
   - Protocol 3.x, x > 0, gets `NegotiateProtocolVersion` (3.0, naming any
     `_pq_.` options), as PostgreSQL answers it, and startup goes on at 3.0.
   - Any other version is `FATAL 0A000`.
2. **The parameters.**
   - `user` is required, and any name is accepted: there are no roles.
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
3. **Authentication: trust only, and only for the MVP.**
   `AuthenticationOk` is sent at once. Anyone who can reach the port is in,
   so the server listens on 127.0.0.1 unless `--listen` names another
   address, and it prints a warning when that address is not a loopback
   one. Password and SCRAM authentication come with #494's authentication
   item.
4. **`ParameterStatus`**:
   - `server_version` `17.0`: the dialect the statements are proven
     against, which libpq and psycopg parse;
   - `server_encoding` `UTF8`, and `client_encoding` the encoding asked
     for (`UTF8` for `UNICODE`), `UTF8` when none was;
   - `DateStyle` `ISO, MDY`, `IntervalStyle` `postgres`, `TimeZone` `UTC`;
   - `integer_datetimes` `on`, `standard_conforming_strings` `on`;
   - `is_superuser` `off`, `session_authorization` the user;
   - `application_name`.
5. **`ReadyForQuery`**, status `I`.

## A session

A session holds:
- the block status (*The block status*, below);
- the open `Transaction`, if any;
- the named statements, each a `const Statement&` from `Database::prepare`,
  which is valid until close;
- the named portals;
- the unnamed statement and the unnamed portal.

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
  raised it. The server rolls back the `Transaction` if it is still open.
- In `E`, every statement but `COMMIT` and `ROLLBACK` is `25P02`, checked
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
  `BEGIN` with a mode (`ISOLATION LEVEL …`, `READ ONLY`, `DEFERRABLE`) is
  `0A000`. The isolation is the layer's (`transaction_design.md`).
- **`COMMIT`** calls `commit()`. `Committed` gives the tag `COMMIT`. A
  transaction that was aborted is rolled back instead (`RolledBack`), with
  the tag `ROLLBACK`, as PostgreSQL's `COMMIT` does. Errors:
  - `SerializationFailure` is `40001`;
  - `TransactionResolutionUnknown` is `08007`.

  Either way the block has ended, and the status is `I` (*The block
  status*). With no transaction open, `COMMIT` is `WARNING 25P01` and the tag `COMMIT`.
- **`ROLLBACK`** calls `rollback()`; the tag is `ROLLBACK`. With no
  transaction open it is `WARNING 25P01`.

- **`SET [SESSION] client_encoding {TO | =} 'UTF8'`**, or another encoding
  startup takes, is the one `SET` taken: the tag is `SET`, with a
  `ParameterStatus` for `client_encoding`. Another encoding is `22023`, and
  every other `SET` is `0A000`.

The extended protocol accepts these words too: `Parse` of `BEGIN` makes a
statement the session runs at `Execute`.

**Every other statement** goes to `Database::prepare`.
- Inside a transaction it runs with `Transaction::execute`. Outside one,
  a `Query` runs it with `Database::execute`, which commits it alone. An
  `Execute` runs it in the block's implicit transaction (*Extended
  query*).
- DDL inside a transaction is the layer's `0A000`.
- Results: `RowDescription` (for a row-returning statement), one `DataRow`
  per row, then `CommandComplete`. The tag comes from the statement's
  command (*API changes*, 3):
  - `SELECT n`, where n is the row count;
  - `INSERT 0 n`, `UPDATE n` and `DELETE n`, where n is `Rows::affected`;
  - for DDL, its command words: `CREATE TABLE`, `CREATE INDEX`,
    `DROP TABLE` and so on.

## Simple query

A `Query` holds one statement, with a trailing `;` and white space
allowed. An empty one gets `EmptyQueryResponse`. Several statements are
`0A000`; no client the gate uses sends them. A `$n` in a `Query` is
`42P02` (`undefined_parameter`), PostgreSQL's "there is no parameter
$1". A
failed statement gets `ErrorResponse`; then, as after every `Query`, the
server sends `ReadyForQuery`.

A `Query` that arrives after `Execute`s outside `BEGIN`, before their
`Sync`, joins their implicit transaction, as in PostgreSQL: it runs in it,
and the transaction commits when the `Query` ends. An error in the `Query`
rolls the whole block back. A DDL `Query` there is `0A000`, as a DDL beside
other statements is.

## Extended query

- **`Parse`** (name, text, parameter OIDs).
  - The name must be free: a used name is `42P05`, and the unnamed
    statement is replaced.
  - The text goes to `Database::prepare`.
  - The OIDs the client gives are not used: each parameter's text is read
    as the type the layer inferred, so text that type does not accept is
    `22P02`. libpq sends 0s, and psycopg sends an OID per Python type
    (`int2` for a small `int`); in both cases the inferred type is the one
    the text has to fit.
  - The reply is `ParseComplete`.
- **`Bind`** (portal, statement, formats, values, result formats).
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

  The column types are known before the statement runs (*API changes*, 2).
- **`Execute`** runs the portal and sends its rows and `CommandComplete`. A
  row limit above 0 is `0A000`; libpq sends 0.
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

DDL is its own atomic operation in the layer and cannot run in a
`Transaction`.
- A DDL that is its block's only statement (the next message is `Sync`)
  runs alone through `Database::execute`, as a simple `Query` does.
- A DDL in a block of two or more is `0A000`. If it opens the block,
  nothing has run. If it comes later, the block's implicit `Transaction`
  aborts. Either way nothing in the block commits.

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
| `DECIMAL(p, s)` | 1700 | -1 | ((p << 16) \| s) + 4 |
| `numeric`, unconstrained (a `SUM`) | 1700 | -1 | -1 |
| `TIMESTAMP` | 1114 | 8 | -1 |
| `DATE` | 1082 | 4 | -1 |
| `TIME` | 1083 | 8 | -1 |

A result column's type is the one the compiler gives it
(`Compiled::column_types`), so the server only reports it:
- a column, its declared type;
- `COUNT`, `BIGINT`;
- `SUM` of a `SMALLINT` or `INTEGER`, `BIGINT`;
- `SUM` of a `BIGINT` or `DECIMAL`, PostgreSQL's unconstrained `numeric`,
  at the input's scale (`frontend_design.md`, *The dialect subset*).

Unconstrained `numeric` is OID 1700 with modifier -1, and its text has the
input's scale: `123.45` for a sum of `DECIMAL(6, 2)`. `SqlType` writes it
as a `DECIMAL` of precision 0 at that scale, the type slice 4's compiler
gives `SUM`.

**Output** is PostgreSQL's text form, with the `ParameterStatus` settings
above:
- integers in decimal;
- a `DECIMAL` with exactly s fraction digits (`5.00` in a `DECIMAL(6, 2)`);
- floats as the shortest text that reads back to the same value, as
  `float8out` and `float4out` write it with `extra_float_digits` 1:
  exponent form below 1e-4 and from 1e15 (1e6 for `REAL`), `NaN`,
  `Infinity` and `-Infinity`;
- `t` and `f`;
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
    which use a parameter has (*API changes*, 1).
  - `NaN` is `22P02`.
- floats: what `strtod` reads, plus `NaN`, `Infinity`, `-Infinity`
  (`inf` too), any case. Rounding a `REAL` is the layer's.
- `BOOLEAN`: PostgreSQL's `boolin`: `t`, `true`, `y`, `yes`, `on`, `1` and
  their negatives, any case, any unique prefix (`on` and `off` need two
  letters), white space around.
- `BYTEA`: hex (`\x…`) and escape format (`\\`, `\nnn`).
- text: the bytes; the layer checks UTF-8 (`22021`).
- `TIMESTAMP`: `YYYY-MM-DD`, then optionally a space or `T` and
  `HH:MM[:SS[.f…f]]` with up to 6 fraction digits, then optionally ` BC`.
- `DATE`: `YYYY-MM-DD`, optionally ` BC`.
- `TIME`: `HH:MM[:SS[.f…f]]`.

Other datetime forms (`now`, `epoch`, `infinity`, a time zone, names of
months, more than 6 fraction digits) are `22007`. That is a known
difference: the driver writes ISO.

## Errors

`ErrorResponse` carries:
- `S` and `V`: `ERROR`, or `FATAL` before the connection closes;
- `C`: the SQLSTATE;
- `M`: the exception's message;
- `P`: for an `ast::SqlError`, its position, one-based and in characters,
  converted from the layer's byte offset.

Warnings are `NoticeResponse` with `S` `WARNING`.

The SQLSTATE comes from one function in the API, `sqlstate(const
std::exception&)` (*API changes*, 4). The server, the Python binding and
the proof oracle all use it, so the oracle's check of SQLSTATEs against
PostgreSQL also checks the server's mapping. The mapping:

| Raised | SQLSTATE |
|---|---|
| `ExecError`, `BC_E_CONSTRAINT`, `BC_C_UNIQUE` | `23505` |
| `BC_C_NOT_NULL` | `23502` |
| `BC_C_RANGE`, by the check that failed | `22003` (`smallint`/`integer`/`numeric field overflow`/`REAL`), `22001` (`VARCHAR` too long), `22021` (encoding), `22008` (datetime) |
| `BC_E_LIMIT` | `54000` |
| `BC_E_ARITH` | `22003`. The subset has no division; adding one adds `22012` for division by zero |
| `SerializationFailure`, a conflict on every retry, `DdlInProgress` | `40001` |
| `TransactionResolutionUnknown` | `08007` |
| `TransactionAborted` | `25P02` |
| `FeatureNotSupported` | `0A000` |
| `ParamError` | `08P01` (the server's own count check makes this unreachable over the wire) |
| `ast::SqlError` | `42601` |
| `DdlRefused` | `42P07` for an existing name, `42P01` for a missing table, `42601` otherwise, by its kind |
| `BC_E_IO`, `BC_E_CORRUPT`, `BC_E_DEGRADED` | `58030` |
| `BC_E_MEMORY`, `std::bad_alloc` | `53200` |
| `DbClosed` | `57P01` |
| anything else | `XX000` |

The binder's errors are all `ast::SqlError` today, so an unknown table or
column is `42601` rather than PostgreSQL's `42P01` or `42703`. Splitting
them needs a code on `SqlError` (#548). HammerDB's
driver sends no statement that fails to bind.

## API changes

The server needs five things that `api/database.cppm` did not expose. All
are read from what the binder and compiler already keep:
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
   where those messages are written. It is `RANGE_SQLSTATE` in
   `tests/proof/oracle.py` today, matched by message prefix. The binding
   exposes it as `ExecError.sqlstate`, and the oracle drops its own tables
   for it. `test_every_range_message_maps_to_its_sqlstate` keeps proving
   that every message maps.

5. `Database::refit(stmt)`: recompiles a cached plan that the live catalog
   no longer fits, as its next run would, and runs nothing. A statement
   prepared before a `DROP TABLE` and `CREATE TABLE` of other types still
   describes its old plan until it runs. The server refits at `Parse`,
   `Bind` and `Describe`, so `ParameterDescription`, the parameters' text
   and `RowDescription` follow the catalog. A DDL between `Bind` and
   `Execute` is caught by the run, which recompiles and checks the
   parameters against the new plan.

`DdlRefused` carries the SQLSTATE PostgreSQL gives the same refusal, for (4).

## The HammerDB driver

`bytecaskdb-sql-layer/hammerdb/bytecask_tprocc.tcl` is a HammerDB custom
script (`customscript`). It is a port of the client-side TPROC-C logic in
HammerDB's MySQL driver, `src/mysql/mysqloltp.tcl`: the `neword`,
`payment`, `ostat`, `delivery` and `slev` procedures of
`insert_mysql_no_stored_procs`, and the timed driver's monitor. The ported
script uses pgtcl and runs the statements of `frontend_design.md` (*The
statements*), with `$n` parameters.

HammerDB's PostgreSQL driver cannot be used, because it calls PL/pgSQL
procedures (or functions) through SQL `PREPARE`. Its monitor also reads
`pg_stat_database`.

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
(`frontend_design.md`, *Decisions*, 7): PostgreSQL's throughput from these
runs is not part of its output.

## Running the gate

`bytecaskdb-sql-layer/hammerdb/run-gate.sh [--warehouses=48] [--vus=1,16,48]
[--rampup=2] [--duration=5] [--mix="neword payment ostat delivery slev"]
[--hammerdb-home=PATH] [--data-root=PATH] [--server-flags="--sync AT_INTERVAL"]
[--smoke --pg=CONNINFO]`. `--server-flags` goes to the server in every cell. It
follows `run-hammerdb.sh` in the MariaDB plugin. `--smoke` is the short run
of *Tests*, and `--pg` names the PostgreSQL it checks the driver on (its
password in `PGPASSWORD`). `XMAKE_FLAGS` adds `xmake f` flags, as CI sets
its toolchain.

1. **Build.** It builds `sql_layer_tprocc` and `sql_layer_server` in
   `releasedbg`; a debug engine walks the key directory on every commit.
2. **Load.** It loads W warehouses once with `sql_layer_tprocc load`, not
   HammerDB's `buildschema`, and keeps the directory as the pristine copy.
   With `--smoke`, `--sql-out` also writes the population for PostgreSQL.
3. **Per user count:**
   - restore the copy (a reflink where the filesystem has them);
   - take a run with `sql_layer_tprocc next-run`, written into the driver's
     header;
   - start the server on a free port, and wait for its `ready` line;
   - run `hammerdbcli auto` on a generated file, which sets
     `dbset db pg`, `dbset bm TPC-C`, `diset connection pg_host`,
     `pg_port` and `pg_sslmode disable`, `diset tpcc pg_user` and
     `pg_dbase`, then `customscript` (the script, with the run's settings
     written into its header, as HammerDB's editor does), `custommonitor
     timed`, `vuset vu N`, `vucreate`, `vurun`, `vucomplete` and
     `vudestroy`;
   - parse the result line;
   - stop the server with `SIGTERM`;
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
   duration, NOPM, TPM, errors by SQLSTATE, and the check's result, all
   of our server; the report prints the same. No PostgreSQL row is written.

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
  - text placeholders (`%t`), so parameters go in text format; psycopg's
    default dumpers are binary for several types, which would be `0A000`;
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
  - a lone DDL `Execute` run, and one beside a DML `Execute` refused with
    nothing committed;
  - `Terminate` and a dropped connection rolling back an open
    transaction;
  - `COMMIT` of an aborted transaction tagged `ROLLBACK`;
  - the block status: inside `BEGIN`, a statement that fails to bind, or a
    parameter that fails to parse, makes the block `E`; later statements
    are `25P02`; `COMMIT` answers `ROLLBACK`, and nothing is committed;
  - after an error in an implicit block, `Sync` reports `I`;
  - the warnings;
  - `--max-connections`;
  - `--sync`, `--sync-interval-ms` and the I/O flags: each mode starts and
    stops cleanly, and a bad value exits before the server listens.
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

- A DDL in an extended-query block with other statements is `0A000`, and
  so is one followed by a `Flush` with nothing after it yet.
- Parameter OIDs from the client are not used.
- Binary format, for parameters and results, is `0A000`.
- A compared or added `DECIMAL` parameter finer than its column's scale, and `NaN`,
  are `22P02`.
- The integer and datetime input forms listed above only.
- Binder errors are `42601`.
- `Execute` with a row limit, several statements in one `Query`, SQL
  `PREPARE`, `SET`, `SHOW`, `COPY` and cancel are refused.
- A result is built whole by the embedded API before its first `DataRow`
  is sent. Streaming rows waits on the API streaming them (#494's
  "results stream as `bc_emit` produces them").
- `Database::prepare`'s cache keeps every distinct text until close, so a
  client sending ever-new literal text grows it. The driver sends a fixed
  set (#549).

## `CONTRACT.md` changes

**Added (SQL):**
- `DatabaseOptions` and the three sync modes: when a commit reaches the
  disk, `AtEveryCommit` as before and the default;
- `DatabaseOptions`' I/O backend and buffer pool, `Pread` as before and the
  default;
- the server's startup: what it accepts and what it refuses, with trust
  only;
- the SQLSTATE each error is reported with, over the wire and through
  `sqlstate()`;
- the transaction-control statements' outcomes and tags;
- the text forms, as PostgreSQL's for the types and forms listed.

Each row names its tests: the wire proof, `test_wire.py` and the smoke
test.

## Decisions

1. The PostgreSQL protocol before gRPC, as a front over the embedded API.
2. One thread per connection, no pool.
3. Trust only, loopback by default.
4. A ported pgtcl driver script, with `PQexecParams` per statement and no
   SQL `PREPARE`.
5. NOPM from the `d_next_o_id` sum, and TPM from client-side counts.
6. One SQLSTATE mapping, in the API, shared by the server and the oracle.
7. The five API additions.
8. Outside `BEGIN`, a block of one `Execute` autocommits through
   `Database::execute`, with its retry. A block of two or more is one
   implicit `Transaction`, committed at `Sync`, as in PostgreSQL. A DDL
   runs only as a block's one statement, and is `0A000` beside others. The
   session owns the block status (coordinator review).
9. The HammerDB smoke runs nightly, as a scheduled workflow, not per PR
   (owner, 2026-10-22).
10. The gate reports ByteCaskDB's NOPM and TPM only. PostgreSQL 17 runs the
    driver to check it (the nightly smoke), and its throughput is not
    reported (owner, 2026-10-22; `frontend_design.md`, *Decisions*, 7).
