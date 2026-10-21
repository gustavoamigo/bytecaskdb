# The PostgreSQL wire-protocol server: slice 1, for the MVP gate

Status: design, for review (#494's first slice).

The MVP gate is HammerDB's TPROC-C driving the layer over PostgreSQL's
wire protocol (`frontend_design.md`, *The MVP gate*). This slice is the
server that HammerDB connects to and the HammerDB driver script that sends
our statements. The harness of `testing_design.md` (*The TPC-C harness*) stays: it checks the consistency conditions, runs Elle and the CI smoke
test, and gives the no-network baseline. It is not the gate.

#494 planned a gRPC server first, with the PostgreSQL protocol as a shim in
front of it. The gate needs the PostgreSQL protocol and nothing else, so the
protocol comes first, as a thin stateful front over the embedded API. gRPC
stays in #494, after this slice.

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
`COPY`, the function-call protocol, `CancelRequest`, `SET` and `SHOW`, SQL
`PREPARE`/`EXECUTE`/`DEALLOCATE`, `Execute` with a row limit, several
statements in one `Query`, and the system catalogs (`pg_catalog`,
`information_schema`).

## What the client sends

The gate's client is HammerDB running our driver script. The script calls
pgtcl, HammerDB's bundled Pgtcl (2.2.x), which calls libpq. Its sources
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
  `-getTuple` and `-cmdTuples`, and each error with `pg_result -error C`
  (the SQLSTATE) and `-error` (the message).

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
127.0.0.1:5432] [--database NAME] [--max-connections 100]` serves one
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

**Shutdown.** `SIGINT` or `SIGTERM` closes the listener. Each connection
then gets `FATAL 57P01` (`admin_shutdown`) between messages: a statement in
flight finishes first, and an open transaction is rolled back. When every
thread has joined, the server calls `Database::close()`.

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
   - `client_encoding` must be `UTF8` (any case, `-` or `_` allowed), else
     `FATAL 22023`. There is no conversion.
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
   - `server_encoding` and `client_encoding` `UTF8`;
   - `DateStyle` `ISO, MDY`, `IntervalStyle` `postgres`, `TimeZone` `UTC`;
   - `integer_datetimes` `on`, `standard_conforming_strings` `on`;
   - `is_superuser` `off`, `session_authorization` the user;
   - `application_name`.
5. **`ReadyForQuery`**, status `I`.

## A session

A session holds:
- the open `Transaction`, if any;
- the named statements, each a `const Statement&` from `Database::prepare`,
  which is valid until close;
- the named portals;
- the unnamed statement and the unnamed portal.

Closing the connection, with `Terminate` or without, rolls back an open
transaction.

**The transaction status** in `ReadyForQuery` comes from the
`Transaction`:
- `I` when there is none, or it has ended;
- `T` when it is open;
- `E` when it is aborted (`is_aborted()`).

The layer aborts a transaction when one of its statements fails, as
PostgreSQL does, so the server keeps no status of its own.

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

  Either way the transaction has ended, and the status is `I`. With no
  transaction open, `COMMIT` is `WARNING 25P01` and the tag `COMMIT`.
- **`ROLLBACK`** calls `rollback()`; the tag is `ROLLBACK`. With no
  transaction open it is `WARNING 25P01`.

The extended protocol accepts these words too: `Parse` of `BEGIN` makes a
statement the session runs at `Execute`.

**Every other statement** goes to `Database::prepare`.
- Inside a transaction it runs with `Transaction::execute`, and outside
  one with `Database::execute`, which commits it alone.
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
`08P01`, as PostgreSQL's "there is no parameter $1" is reported here. A
failed statement gets `ErrorResponse`; then, as after every `Query`, the
server sends `ReadyForQuery`.

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

**Several `Execute`s before one `Sync`, outside `BEGIN`.** PostgreSQL
runs them as one implicit transaction, committed at `Sync`. Here the
first `Execute` after a `Sync` commits alone, as libpq's one statement per
`Sync` needs. A second one before the next `Sync` is `0A000`, and the
messages after it are discarded up to `Sync`, as after any error. Where we
don't match PostgreSQL we refuse, rather than answer with other semantics.
A pipelining client (psycopg's pipeline mode, JDBC's batches) gets that
refusal. The first statement has committed by then, as it would have
alone. Inside `BEGIN` any number of `Execute`s per `Sync` run in the
transaction, as in PostgreSQL.

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
| `TIMESTAMP` | 1114 | 8 | -1 |
| `DATE` | 1082 | 4 | -1 |
| `TIME` | 1083 | 8 | -1 |

A result column the catalog does not declare (an aggregate, `COUNT`) has
its `plain_sql_type`: `BIGINT`, `DOUBLE PRECISION`, `BOOLEAN` or `BYTEA`.
PostgreSQL reports `COUNT` as `bigint` too, and `SUM` of a `DECIMAL` as
`numeric`. The front end's result types already follow PostgreSQL's, and
the server only reports them.

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
  - An assigned or added parameter with more fraction digits than s is
    rounded half away from zero, as PostgreSQL rounds on assignment. For an
    added one, `w_ytd + $1`, rounding the operand gives what PostgreSQL
    gets by rounding the sum, since the column's value is exact at s.
  - A compared parameter with more fraction digits than s is `22P02`.
    PostgreSQL compares the unrounded value, which a scaled `i64` cannot
    hold, so the server refuses it rather than round to a value that could
    compare equal.
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
| `BC_E_ARITH` | `22003` |
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
them needs a code on `SqlError`, and is a follow-up issue. HammerDB's
driver sends no statement that fails to bind.

## API changes

The server needs four things that `api/database.cppm` does not expose. All
are read from what the binder and compiler already keep:
1. `Statement::params() -> std::vector<ParamInfo>`, with `ParamInfo
   {SqlType type; bool compared;}`. `type` is the declared type of the
   column of the parameter's first use, which the binder keeps
   (`ParamUse::first`). `compared` is true when one of its uses is a
   comparison. The server needs it for `ParameterDescription` and for text
   input. `param_types()`, the physical types, stays for its callers.
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

`DdlRefused` gains a kind for (4).

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
- **Its run.** At start it runs the `harness_run` bump in its own
  transaction (`testing_design.md`), so its `h_id`s are `(run << 48) | n`.
  Each virtual user takes a run of its own, which spends 49 of the 2^15
  runs per 48-user cell; the harness's limits otherwise hold.
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
- **Its counts.** It adds each commit, rollback and failure to `tsv`
  counters, which `Com_commit` and `Com_rollback` stand in for.

**The monitor**, position 1, is HammerDB's timed monitor.
1. It waits out the ramp-up.
2. It reads both counters at the start of the measured window. It sums
   `d_next_o_id` with S1 (`SELECT d_next_o_id FROM district WHERE d_w_id =
   $1 AND d_id = $2`) over every district, in one transaction, so the sum
   is one snapshot; the layer has no full-table `SUM`.
3. It sleeps for the duration, then reads both again.
4. It prints `testresult $nopm $tpm` with HammerDB's own procedure:
   - NOPM is the `d_next_o_id` delta per minute, TPC-C's definition and
     HammerDB's;
   - TPM is commits plus rollbacks plus failures per minute, which
     `xact_commit + xact_rollback` would count.
5. It sets HammerDB's `abort` flag, and the users stop.

The output is HammerDB's line, `System achieved N NOPM from M … TPM`.

**The statements.** They are those of `harness/tpcc.cppm`, which the proof
suite checks against PostgreSQL 17, and the script must not drift from
them. `sql_layer_tpcc statements` prints the harness's, and
`scripts/check_driver_statements.py` (in the layer) fails CI if the
script's set differs.

The script runs every transaction type. Delivery and Stock-Level wait on
the front end's slices 4 and 5 (`frontend_design.md`, *Slices*). Until
they land, the run script's `--mix` limits the script to the types the
layer runs, and the gate is the full mix.

**The same script against PostgreSQL 17.** The statements are
PostgreSQL's, so the script runs unchanged on PostgreSQL loaded with the
same schema. That is how the driver itself is checked: the smoke test runs
it against PostgreSQL as well, and the consistency conditions must hold
there too. Whether the gate also reports PostgreSQL's numbers is a
decision for the owner (`frontend_design.md`, *Decisions*, 7); this slice
only makes it possible.

## Running the gate

`bytecaskdb-sql-layer/hammerdb/run-gate.sh --warehouses=W --vus=1,16,48
[--rampup=2] [--duration=5] [--mix=full] [--hammerdb-home=PATH]
[--data-root=PATH] [--smoke]`. It follows `run-hammerdb.sh` in the MariaDB
plugin. `--smoke` is the short run of *Tests*.

1. **Build.** It builds `sql_layer_tpcc` and `sql_layer_server` in
   `releasedbg`; a debug engine walks the key directory on every commit.
2. **Load.** It loads W warehouses once with `sql_layer_tpcc load`, not
   HammerDB's `buildschema`, and keeps the directory as the pristine copy.
3. **Per user count:**
   - restore the copy (a reflink where the filesystem has them);
   - start the server, and wait for its `ready` line;
   - run `hammerdbcli auto` on a generated file, which sets
     `dbset db pg`, `dbset bm TPC-C`, `diset connection pg_host`,
     `pg_port` and `pg_sslmode disable`, `diset tpcc pg_user` and
     `pg_dbase`, then `customscript` (the script, with the run's settings
     written into its header, as HammerDB's editor does), `custommonitor
     timed`, `vuset vu N`, `vucreate`, `vurun`, `vucomplete` and
     `vudestroy`;
   - parse the result line;
   - stop the server with `SIGTERM`;
   - run `sql_layer_tpcc check --dir`, conditions 1, 8 and 9, on what the
     run left.

   A cell whose check fails is reported as failed, whatever its NOPM.
4. **Results.** One row per cell goes to
   `bytecaskdb-sql-layer/benchmarks/hammerdb_results.csv`, which the
   layer's `.gitignore` ignores: date, commit, W, users, ramp-up,
   duration, NOPM, TPM, errors by SQLSTATE, and the check's result.

`sql_layer_tpcc check` is a new command over the harness's existing
`consistency()`.

## Tests

- **The proof machine over the wire.** `tests/proof/`'s state machine gains
  a third target. It already checks the embedded layer against PostgreSQL
  17; now every statement also goes through the server with psycopg, with
  every OID loaded as `str`. The wire results must match PostgreSQL's text,
  byte for byte, and its SQLSTATEs. That proves `pg_text` (output and
  input), the tags, the status letters and the error mapping, on the
  statements and values the machine generates.
- **`tests/server/test_wire.py`**: protocol flows psycopg does not produce,
  over a raw socket:
  - `SSLRequest` and `GSSENCRequest` answered `N`;
  - protocol 3.2 negotiated down;
  - each startup refusal;
  - an error in an extended sequence discarding to `Sync`;
  - a second `Execute` before `Sync`, outside `BEGIN`, refused;
  - `Terminate` and a dropped connection rolling back an open
    transaction;
  - `COMMIT` of an aborted transaction tagged `ROLLBACK`;
  - the warnings;
  - `--max-connections`.
- **HammerDB smoke, manual or nightly, not per PR.** It runs
  `run-gate.sh --smoke`: one warehouse, two users, no ramp-up and a
  one-minute window, against the server and against PostgreSQL 17. It then
  runs `sql_layer_tpcc check` and the same conditions on PostgreSQL. It
  needs HammerDB's Linux release. Per PR, CI runs the wire proof target and
  `test_wire.py`.
- **`scripts/check_driver_statements.py`** in CI.

No microbenchmarks: the gate's numbers are HammerDB's.

## Known differences from PostgreSQL

- Outside `BEGIN`, a second `Execute` before `Sync` is `0A000` rather than
  part of an implicit transaction.
- Parameter OIDs from the client are not used.
- A compared `DECIMAL` parameter finer than its column's scale, and `NaN`,
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
  set. This is a follow-up issue.

## `CONTRACT.md` changes

**Added (SQL):**
- the server's startup: what it accepts and what it refuses, with trust
  only;
- the SQLSTATE each error is reported with, over the wire and through
  `sqlstate()`;
- the transaction-control statements' outcomes and tags;
- the text forms, as PostgreSQL's for the types and forms listed.

Each row names its tests: the wire proof, `test_wire.py` and the smoke
test.

## Decisions for review

1. The PostgreSQL protocol before gRPC, as a front over the embedded API.
2. One thread per connection, no pool.
3. Trust only, loopback by default.
4. A ported pgtcl driver script, with `PQexecParams` per statement and no
   SQL `PREPARE`.
5. NOPM from the `d_next_o_id` sum, and TPM from client-side counts.
6. One SQLSTATE mapping, in the API, shared by the server and the oracle.
7. The four API additions.
8. Outside `BEGIN`, one `Execute` per `Sync`: a second is `0A000`, not an
   implicit transaction (coordinator review: refuse where we don't match).
9. The HammerDB smoke runs manually or nightly, not per PR (pending the
   owner).
