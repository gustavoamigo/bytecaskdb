# Sessions, the worker pool and the transport

Status: **slice 1 built; slices 2–3 next** (design approved by the owner,
2026-10-25; tracked in #642). The second of the post-MVP designs (#487,
*Next*). Slice 1 replaced *Threading*, *I/O* and *Shutdown* in
`server_design.md`. The protocol the server speaks, and everything a client
sees, stays as `server_design.md` describes it.

## Why

Today's server couples three things: a connection has a thread, and the
thread holds the session's state on its stack and in its locals
(`server_design.md`, *Threading*). That is the usual design, and it is
there because most servers wait: on locks, on other transactions, on the
network. The layer never waits for another transaction
(`transaction_design.md`), so nothing forces the coupling, and it costs
something:
- a thread per connection, idle or not, so 1,000 mostly idle clients are
  1,000 threads;
- a thread blocked on its own socket for as long as a slow client takes;
- session state that exists only as the thread's control flow, so it
  can't outlive the connection or move between threads.

## The model

Three things, each with one owner:

| | What it is | Owned by |
|---|---|---|
| **Connection** | A socket, its input and output buffers, and message framing (`pg_wire`). | The I/O loop. |
| **Session** | A plain data structure holding the protocol's and SQL's state: the startup parameters, the block status (`I`, `T`, `E`), the open `Transaction` handle, the prepared statements and portals, and the pending implicit block. It has no thread and no lock. | Whoever is running it, one at a time. |
| **Worker** | A thread in a fixed pool. It runs one session's queued messages, then takes the next session. | The pool. |

The transport stays PostgreSQL's wire protocol (owner, 2026-10-24). While
it does, a connection maps to one session: the server keeps a
*connection-to-session map*. It is not a connection pool: clients still
open one cheap connection each, and nothing is pooled on their side. A
transport that names the session in each request, later, removes the
mapping (*Later: a session-aware transport*).

## The I/O loop

Sockets are non-blocking and owned by one event loop on libuv (owner,
2026-10-24). libuv is MIT-licensed C, compiled once, and uses epoll on
Linux and kqueue on macOS. Asio was the alternative; it is larger, and
being header-only it would cost every translation unit that includes it.

The loop's work, in plain callbacks:
1. **Readable.** Append the bytes to the connection's input buffer and cut
   out complete messages, with today's framing and the 64 MiB message
   bound. Append them to the session's queue, and schedule the session if
   it is idle.
2. **Woken by a worker** (`uv_async_send`). Write what the connection's
   output buffer holds. Write the rest when the write completes.
3. **Closed.** Free the connection in libuv's close callback, its only
   owner, once no worker holds its session.

Callbacks, not coroutines. The protocol logic never runs in the loop: it
runs on a worker, on complete messages, with no I/O wait in the middle, so
there's no long sequential logic for a coroutine to hold. A coroutine
frame destroyed while a libuv request is pending would be a use-after-free;
a connection that frees itself in its close callback has one rule in one
place. The connection is an explicit state machine (startup, ready,
closing), as `pg_session` is today.

One loop thread to start. Moving to one loop per core is mechanical, since
nothing is shared between connections but the `Database` and the session
map.

## Running a session

**One session runs on one worker at a time, in message order.** The
protocol is sequential per connection, and a client may pipeline
`Parse`, `Bind`, `Execute` and `Sync` before reading anything. So a session
is *idle*, *queued* or *running*:
- the loop appends messages to the session's queue and makes an idle
  session queued;
- a worker takes a queued session, makes it running, and runs its messages
  through today's `pg_session` logic, synchronously and to completion;
- when its queue is empty, the session goes back to idle. Otherwise it
  goes back to queued, behind the others.

**Lookahead at `Execute`.** Outside `BEGIN` and an open implicit block, an
`Execute` looks at the queued messages after it (`CONTRACT.md`,
*PostgreSQL wire-protocol server*):
- if the next message (past any `Flush` that already has another message
  queued behind it) is `Sync`, the statement runs with `Database::execute`
  (autocommit, with conflict retry);
- if the next message is `Flush` with nothing queued behind it yet, it
  opens the implicit transaction, runs there, and flushes its reply at
  once;
- if no message is queued after `Execute` yet, `Execute` stays at the head
  of the session's queue and the session returns to idle until the next
  message (or end of stream) arrives. A worker never blocks on the socket.

The queue's lock is the only lock. Taking a session from it gives the
worker exclusive access, and makes the previous worker's writes to the
session visible to it. No lock exists inside a session. The embedded API
already allows this: every `Database` call may come from any thread, and a
`Transaction` handle may move between threads as long as one thread uses
it at a time (`transaction_design.md`, *Shape*).

**Fairness.** A worker runs a session's messages up to the next point the
protocol replies at: `ReadyForQuery`, after a `Sync` or a `Query`. Then
the session goes behind the others, so one client's pipeline can't hold a
worker indefinitely. A single statement's memory is bounded by its budget
(`plan_api_design.md`, *The guarantee*), and its run time is bounded when
the statement timeout is set.

**Replies.** A worker appends reply bytes to the connection's output buffer
at the points today's server writes (`ReadyForQuery`, `Flush`, or past
64 KiB) and wakes the loop. A slow reader never holds a worker. While a
connection's unsent output is past its bound (*Bounds*), the session isn't
scheduled for its next turn: its messages wait in its queue, and the loop
stops reading its socket, so TCP pushes back on the client.

## Commits and durability

A commit runs on the worker, as today (owner, 2026-10-24). The sync mode
decides whether it waits for the disk (`server_design.md`, *Durability*):
- `AT_EVERY_COMMIT`: the worker waits for its commit's `fdatasync`, which
  the engine's group commit shares with every commit in flight.
- `AT_INTERVAL` and `AT_FILE_ROTATION`: it doesn't. The commit is visible
  and acknowledged before it is durable, which is the trade-off those modes
  name.

Durability advances in sequence order. A session that reads a commit not
yet durable, and commits something based on it, gets a later sequence:
whenever its own commit is durable, so is what it read.

**This reverses one of `server_design.md`'s decisions.** It rejected a
worker pool because, under `AT_EVERY_COMMIT`, a connection would wait for a
worker that another connection's `fdatasync` holds. That is still true. The
wait is bounded by one `fdatasync` and is never on another transaction, so
"never wait" holds. The pool's size decides how many commits can be in
flight at once (*Decisions*, 6). If profiles show workers parked on the
disk, the engine could complete a commit asynchronously, but that is a new
engine primitive, and not part of this design.

## Lifetime, cancellation and shutdown

**Lifetime.** While a connection maps to one session, the session lives as
long as its connection. A connection that closes, or ends with `Terminate`
or a `FATAL`, ends its session: an open transaction is rolled back, its
prepared statements and portals are dropped, and the session is freed once
no worker holds it.

**Cancellation.** Today a `CancelRequest` closes its connection and
cancels nothing, and no `BackendKeyData` is sent. With sessions:
- startup sends `BackendKeyData`, a session id and a random secret;
- a `CancelRequest` naming both, on a new connection as the protocol sends
  it, marks the session's running statement cancelled;
- a running statement sees the mark where the runtime already checks its
  budget's deadline (`bc_budget::cancel` beside `deadline_ns`), and fails
  with SQLSTATE 57014 (`query_canceled`), as in PostgreSQL;
- a `CancelRequest` for a session that is not running a statement does
  nothing, as in PostgreSQL.

**Timeouts**, with PostgreSQL's semantics and SQLSTATEs:

| Timeout | Ends | Error | Default |
|---|---|---|---|
| Idle in transaction | a session idle inside a transaction: it is rolled back and the connection closed | `FATAL 25P03` | 60 s (`--idle-in-transaction-timeout-ms 60000`, `0` = off) |
| Statement | a statement running past it | 57014, as a cancel | off (`--statement-timeout-ms 0`) |
| Transaction | a transaction open past it in total | `FATAL 25P04` | off (`--transaction-timeout-ms 0`) |
| Idle session | a session idle outside a transaction | `FATAL 57P05` | off (`--idle-session-timeout-ms 0`) |

- **Idle in transaction is on by default.** An open transaction holds a
  snapshot (*Bounds*, *Snapshots*), and its share of the memory cap.
  PostgreSQL leaves it off by default; this difference is listed with the
  others.
- **The transaction timeout is off by default.** A long transaction that is
  busy, such as a report, is legitimate.
- **The statement timeout is the work budget's deadline,** which the runtime
  already checks. It fails as a cancel does.
- **The idle-session timeout** matters little while a session lives as long
  as its connection, since `--max-connections` bounds idle sessions. With a
  session-aware transport (*Later: a session-aware transport*) it becomes
  the session's lifetime.

Each session has one libuv timer, restarted when its state changes (idle,
idle in transaction, running). When it fires, the loop marks the session
and, if the session is idle, schedules it onto a worker so its transaction
is rolled back, the `FATAL` is sent, and the connection closes. No lock
and no wait.

**Shutdown.** The loop stops accepting. Running messages finish, and their
replies go out. Queued messages are answered with `FATAL 57P01`
(`admin_shutdown`), and open transactions are rolled back. Output keeps
draining under today's rule: a client still reading gets every byte, and
one that takes no bytes for ten seconds is dropped. Then the server calls
`Database::close()`.

## Bounds

None of them waits. A bound past its limit refuses, with the SQLSTATE
PostgreSQL uses.
- **Sessions.** `--max-connections`, as today: `FATAL 53300` past it.
- **Workers.** A fixed pool (`--workers`, by default four times the cores,
  minimum 4; *Decisions*, 6).
- **A session's input.** At most 64 MiB of unrun messages. Past it, the loop
  stops reading the socket until the session catches up. This is TCP
  backpressure, not an error.
- **A connection's output.** At most 64 MiB unsent. Past it, the session is
  not scheduled for its next turn until the client reads.
- **Memory across sessions.** Each session can hold a transaction buffer
  (64 MiB, `TransactionOptions::max_bytes`) and run a statement with its
  memory budget (64 MiB, `planner_design.md`). With many sessions that is
  more than the machine has, so a global cap (`--max-memory-bytes`, by
  default half the machine's RAM; *Decisions*, 9) bounds the sum across
  sessions and fails with `53200` (`out_of_memory`). The planner design
  left this cap to this one.
- **Snapshots.** A transaction holds its snapshot from its first statement
  to its commit or rollback; an autocommit statement holds one while it
  runs, and a prepared statement none. Taking one is O(1), but holding one
  is not:
  - it pins the key directory's version, so every node later writes replace
    stays alive until it is released. The cost grows with what the rest of
    the database writes meanwhile, up to a whole copy of the key directory
    (`docs/bytecask_design.md`, *Read path*: about 1.1 GB at 20 M
    keys, measured on the MariaDB plugin);
  - it keeps the data files it references from vacuum.

  The global cap can't count this. The pinned nodes are the engine's,
  shared by every snapshot of the same age, and not any one session's. The
  idle-in-transaction timeout bounds the case where a snapshot is pinned
  for nothing; exposing `keydir_nodes_parked`, `keydir_versions_live` and
  the oldest open snapshot's age is tracked with the server's operational
  counters (#496).

## Prepared statements and portals

Named prepared statements and portals are part of the session, as the
protocol defines them, and portals still end with their transaction. The
compiled plans they name come from `Database::prepare`'s cache, which is
shared by every session and keyed by statement text. #549 bounds that
cache. With sessions it should be a reference-counted handle a session
holds, so the cache can evict a plan no session uses while a session's
named statement keeps its own.

## TLS and authentication

Neither is in the server (owner, 2026-10-24). A terminating proxy in front
of it owns both, and the server listens on loopback and trusts it. It
already warns when `--listen` names an address that isn't a loopback one.
- **TLS.** The server answers `SSLRequest` with `N`, as today. A client on
  libpq 17 or later can use direct TLS (`sslnegotiation=direct`, ALPN
  `postgresql`), which any TLS terminator handles. An older client needs a
  proxy that speaks PostgreSQL's in-band negotiation, such as Envoy's
  PostgreSQL filter or pgbouncer.
- **Authentication.** Behind a proxy every connection comes from the
  proxy, so the server can't tell clients apart, and trusting the proxy is
  the model. Client certificates checked by the proxy fit it. Password or
  SCRAM authentication in the server would also need the client's identity
  passed through (the PROXY protocol header), and is not planned.

## Later: a session-aware transport

A request names its session, and a session outlives any one connection: a
client can reconnect and continue. That needs a session id and secret (the
cancellation key above), an owner, and an idle timeout, because an idle
open transaction holds a snapshot, and a snapshot keeps data files from
vacuum. The connection-to-session map then goes.

## Tests

- **Nothing a client sees changes.** `tests/server/test_wire.py` and the
  proof machine's wire target pass unchanged. That is the main proof that
  the rewrite keeps the protocol.
- **Sessions move between threads.** With two workers and many sessions,
  each session's replies stay in its message order, and a session's
  messages run on more than one worker over a run. Under ThreadSanitizer.
- **Nobody holds a worker.**
  - A client that stops reading a large result holds no worker: with one
    worker, another session's statement still runs.
  - A long pipeline from one session doesn't starve another.
- **Cancellation.** A `CancelRequest` with the right key fails a long
  statement with 57014, and one with a wrong key or on an idle session does
  nothing.
- **Timeouts.** Each one ends what it names with its SQLSTATE, an
  idle-in-transaction timeout rolls back, and a session that stays busy is
  never timed out as idle.
- **Shutdown.** Today's shutdown tests carry over: the slow reader gets
  every row and then 57P01, and the stalled one is dropped after ten
  seconds.
- **The HammerDB gate and the in-process harness**, before and after, at 1,
  16 and 48 users, with the numbers in the PR that switches the server.

## Slices

1. **The model at parity (built).** The libuv loop, the session structure, the
   per-session queue and the worker pool. The protocol and everything
   observable are unchanged, and `test_wire.py` passes as it is. The thread
   per connection is removed in the same PR.
2. **Cancellation and timeouts.** `BackendKeyData`, `CancelRequest`, 57014,
   and the four timeouts.
3. **Bounds.** The input and output bounds, and the global memory cap.

## Proposed `CONTRACT.md` changes

- **Added:** a `CancelRequest` with the session's key fails its running
  statement with 57014, and cancels nothing else.
- **Added:** each timeout ends what it names with PostgreSQL's SQLSTATE; an
  idle-in-transaction timeout rolls the transaction back.
- **Added:** a statement or transaction that would pass the server's memory
  cap fails with 53200 (`out_of_memory`).
- **Narrowed:** "A `CancelRequest` closes the connection with nothing done"
  goes.
- **Unchanged:** startup, the block status, transaction control, the text
  forms and shutdown, as `server_design.md` states them.

## Decisions

The owner's, in the discussions of 2026-10-24 and 2026-10-25:
1. A session is a mutable data structure, decoupled from the connection and
   from the thread that runs it: since nothing waits, nothing requires
   coupling them.
2. The transport stays PostgreSQL's wire protocol. The server maps
   connections to sessions and runs their requests as tasks on a worker
   pool. A session-aware transport may remove the mapping later.
3. Commits run on the worker. The sync modes are the durability trade-off:
   no reads are held back, and there are no separate commit threads.
4. Non-blocking I/O through a framework: libuv, not Asio, with plain
   callbacks rather than coroutines.
5. No TLS and no authentication in the server: a terminating proxy in front
   owns both, and the server listens on loopback and trusts it.
6. The worker pool's size is four times the cores by default (minimum 4) and
   configurable (`--workers`): a worker waits on `fdatasync` under
   `AT_EVERY_COMMIT`, and waits for the client's next statement between the
   statements of an active burst, so the pool is larger than the cores.
   Measured on the 48-user gate (48 warehouses, 2 + 5 min): `AT_EVERY_COMMIT`
   57,109 NOPM against 39,530 with 48 workers, and `AT_INTERVAL` (1,000 ms)
   150,208 NOPM at 48 users and 146,169 at 16.
7. A global memory cap (`--max-memory-bytes`, by default half the machine's
   RAM) bounds the bytes every session's transaction buffers and statement
   budgets hold; passing it fails with `53200` (`out_of_memory`) and never
   waits.
8. Cancellation and timeouts come in slice 2, after slice 1 proves parity.
9. libuv comes from xmake's `libuv` package on `sql_layer_server`.
10. The idle-in-transaction timeout is 60 seconds by default
    (`--idle-in-transaction-timeout-ms 60000`), and `0` turns it off.
    Reporting the engine's parked key-directory counters is tracked with
    #496.
