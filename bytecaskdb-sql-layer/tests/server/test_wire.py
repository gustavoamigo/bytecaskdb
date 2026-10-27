# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""sql_layer_server's protocol (docs/server_design.md, *Tests*): the flows
psycopg does not produce, sent as raw messages, and the session's block
status, statement by statement. The text forms are checked against
PostgreSQL when BYTECASK_SQL_ORACLE names one.

Run: BYTECASK_SQL_SERVER=build/.../sql_layer_server python3 -m pytest bytecaskdb-sql-layer/tests/server
"""

from __future__ import annotations

import decimal
import os
import shutil
import socket
import struct
import signal
import subprocess
import tempfile
import threading
import time

import pytest

from pgserver import Server, server_binary

PROTOCOL_30 = 196608


@pytest.fixture
def server():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        yield s
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def cstr(s: str) -> bytes:
    return s.encode() + b"\0"


def message(kind: bytes, body: bytes = b"") -> bytes:
    return kind + struct.pack("!i", len(body) + 4) + body


def startup_packet(code: int, params: dict[str, str] | None = None) -> bytes:
    body = struct.pack("!i", code)
    if params is not None:
        body += b"".join(cstr(k) + cstr(v) for k, v in params.items()) + b"\0"
    return struct.pack("!i", len(body) + 4) + body


def query(sql: str) -> bytes:
    return message(b"Q", cstr(sql))


def parse(sql: str, name: str = "") -> bytes:
    return message(b"P", cstr(name) + cstr(sql) + struct.pack("!h", 0))


def bind(params: list[str | None], statement: str = "", portal: str = "", formats: list[int] = ()) -> bytes:
    body = cstr(portal) + cstr(statement) + struct.pack("!h", len(formats))
    body += b"".join(struct.pack("!h", f) for f in formats)
    body += struct.pack("!h", len(params))
    for p in params:
        body += struct.pack("!i", -1) if p is None else struct.pack("!i", len(p.encode())) + p.encode()
    return message(b"B", body + struct.pack("!h", 0))


def describe(kind: str, name: str = "") -> bytes:
    return message(b"D", kind.encode() + cstr(name))


def execute(portal: str = "", max_rows: int = 0) -> bytes:
    return message(b"E", cstr(portal) + struct.pack("!i", max_rows))


SYNC = message(b"S")
FLUSH = message(b"H")
TERMINATE = message(b"X")


class Client:
    """One raw connection: sends bytes, reads whole messages."""

    def __init__(self, server: Server):
        self.sock = socket.create_connection(("127.0.0.1", server.port), timeout=30)
        self.buf = b""

    def send(self, *parts: bytes) -> None:
        self.sock.sendall(b"".join(parts))

    def read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("the server closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def byte(self) -> bytes:
        return self.read_exact(1)

    def message(self) -> tuple[bytes, bytes]:
        kind = self.read_exact(1)
        (length,) = struct.unpack("!i", self.read_exact(4))
        return kind, self.read_exact(length - 4)

    def until_ready(self) -> list[tuple[bytes, bytes]]:
        """Every message up to and including ReadyForQuery."""
        out = []
        while True:
            m = self.message()
            out.append(m)
            if m[0] == b"Z":
                return out

    def start(self, database: str = "proof", **extra: str) -> list[tuple[bytes, bytes]]:
        self.send(startup_packet(PROTOCOL_30, {"user": "wire", "database": database, **extra}))
        return self.until_ready()

    def closed(self) -> bool:
        try:
            self.sock.settimeout(5)
            return self.sock.recv(1) == b""
        except ConnectionResetError:
            return True

    def close(self) -> None:
        self.sock.close()


def fields(body: bytes) -> dict[str, str]:
    """An ErrorResponse's or NoticeResponse's fields by code."""
    out = {}
    while body and body[0] != 0:
        code, rest = chr(body[0]), body[1:]
        value, _, body = rest.partition(b"\0")
        out[code] = value.decode()
    return out


def kinds(messages) -> bytes:
    return b"".join(k for k, _ in messages)


def states(messages, kind: bytes = b"E") -> list[str]:
    return [fields(b)["C"] for k, b in messages if k == kind]


def status(messages) -> str:
    """ReadyForQuery's transaction status: I, T or E."""
    assert messages[-1][0] == b"Z"
    return messages[-1][1].decode()


def run(c: Client, sql: str, params: list[str | None] = ()) -> list[tuple[bytes, bytes]]:
    """One statement as PQexecParams sends it."""
    c.send(parse(sql), bind(list(params)), describe("P"), execute(), SYNC)
    return c.until_ready()


def data(messages) -> list[list[str | None]]:
    """DataRows' columns as text."""
    rows = []
    for k, b in messages:
        if k != b"D":
            continue
        (n,) = struct.unpack("!h", b[:2])
        b, row = b[2:], []
        for _ in range(n):
            (length,) = struct.unpack("!i", b[:4])
            b = b[4:]
            if length < 0:
                row.append(None)
            else:
                row.append(b[:length].decode())
                b = b[length:]
        rows.append(row)
    return rows


def connect(server: Server) -> Client:
    c = Client(server)
    assert status(c.start()) == "I"
    return c


def table(c: Client) -> None:
    c.send(query("CREATE TABLE t (k INTEGER PRIMARY KEY, v BIGINT)"))
    out = c.until_ready()
    assert states(out) == [], out


def keys(c: Client) -> list[str]:
    rows = []
    for k in ("1", "2", "3"):
        rows += [r[0] for r in data(run(c, "SELECT k FROM t WHERE k = $1", [k]))]
    return rows


# ---------------------------------------------------------------- startup


def test_ssl_and_gssenc_requests_are_answered_n(server):
    c = Client(server)
    c.send(startup_packet(80877103))
    assert c.byte() == b"N"
    c.send(startup_packet(80877104))
    assert c.byte() == b"N"
    out = c.start()
    assert status(out) == "I"
    params = dict(b.split(b"\0")[:2] for k, b in out if k == b"S")
    assert params[b"server_version"] == b"17.0"
    assert params[b"client_encoding"] == b"UTF8"
    assert params[b"standard_conforming_strings"] == b"on"


def test_a_newer_minor_version_is_negotiated_down_to_3_0(server):
    c = Client(server)
    c.send(startup_packet(PROTOCOL_30 + 2, {"user": "wire", "database": "proof", "_pq_.thing": "1"}))
    out = c.until_ready()
    assert out[0][0] == b"v"
    minor, count = struct.unpack("!ii", out[0][1][:8])
    assert (minor, count) == (0, 1) and b"_pq_.thing" in out[0][1]
    assert status(out) == "I"


@pytest.mark.parametrize(
    "packet, state",
    [
        (startup_packet(PROTOCOL_30, {"user": "wire", "database": "other"}), "3D000"),
        (startup_packet(PROTOCOL_30, {"user": "wire", "database": "proof", "search_path": "x"}), "42704"),
        (startup_packet(PROTOCOL_30, {"user": "wire", "database": "proof", "client_encoding": "LATIN1"}), "22023"),
        (startup_packet(PROTOCOL_30, {"database": "proof"}), "28000"),
        (startup_packet(2 << 16, {"user": "wire"}), "0A000"),
    ],
)
def test_startup_refusals_are_fatal(server, packet, state):
    c = Client(server)
    c.send(packet)
    kind, body = c.message()
    assert kind == b"E"
    f = fields(body)
    assert (f["S"], f["C"]) == ("FATAL", state)
    assert c.closed()


@pytest.mark.parametrize("encoding, name", [("UTF8", "UTF8"), ("utf-8", "UTF8"), ("UNICODE", "UTF8"),
                                            ("SQL_ASCII", "SQL_ASCII"), ("sql-ascii", "SQL_ASCII")])
def test_utf8_its_alias_and_sql_ascii_are_the_client_encodings_taken(server, encoding, name):
    """psql in a C locale asks for SQL_ASCII, which PostgreSQL converts nothing for."""
    out = Client(server).start(client_encoding=encoding)
    assert status(out) == "I"
    assert (b"S", b"client_encoding\0" + name.encode() + b"\0") in out


def test_the_default_database_name_is_the_directory_s_last_component():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory + "/", database=None)
    try:
        assert status(Client(s).start(database=os.path.basename(directory))) == "I"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_the_connection_past_the_limit_is_refused_53300():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, max_connections=1)
    try:
        first = connect(s)
        second = Client(s)
        kind, body = second.message()
        assert kind == b"E" and fields(body)["C"] == "53300"
        first.close()
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


# ---------------------------------------------------------------- simple query


def test_simple_query_refusals_and_the_empty_query(server):
    c = connect(server)
    table(c)
    c.send(query("SELECT k FROM t WHERE k = $1"))
    assert states(c.until_ready()) == ["42P02"]
    c.send(query("SELECT 1; SELECT 2"))
    assert states(c.until_ready()) == ["0A000"]
    c.send(query("SET search_path TO x"))
    assert states(c.until_ready()) == ["0A000"]
    c.send(query("BEGIN ISOLATION LEVEL SERIALIZABLE"))
    assert states(c.until_ready()) == ["0A000"]
    c.send(query("  ;  "))
    assert kinds(c.until_ready()) == b"IZ"
    c.send(query("DROP TABLE t;"))
    assert [b for k, b in c.until_ready() if k == b"C"] == [b"DROP TABLE\0"]


def test_set_client_encoding_is_the_one_set_taken(server):
    """pgtcl's pg_connect calls PQsetClientEncoding, which sends it."""
    c = connect(server)
    for sql, name in (("set client_encoding to 'UTF8'", "UTF8"), ("SET client_encoding = 'utf-8'", "UTF8"),
                      ("SET SESSION client_encoding TO UTF8", "UTF8"), ("SET client_encoding TO UNICODE", "UTF8"),
                      ("SET client_encoding TO 'SQL_ASCII'", "SQL_ASCII")):
        c.send(query(sql))
        out = c.until_ready()
        assert [b for k, b in out if k == b"C"] == [b"SET\0"], (sql, out)
        assert (b"S", b"client_encoding\0" + name.encode() + b"\0") in out
    c.send(query("SET client_encoding TO 'LATIN1'"))
    assert states(c.until_ready()) == ["22023"]
    c.send(query("SET datestyle TO 'ISO'"))
    assert states(c.until_ready()) == ["0A000"]


def test_a_syntax_error_carries_its_position_in_characters(server):
    c = connect(server)
    c.send(query("SELECT é FROM"))
    out = c.until_ready()
    f = fields(out[0][1])
    assert f["C"] == "42601" and int(f["P"]) > 0


def test_a_binder_error_carries_its_sqlstate_on_the_wire(server):
    c = connect(server)
    c.send(query("CREATE TABLE bound_err (k BIGINT PRIMARY KEY)"))
    c.until_ready()
    for sql, state in (
        ("SELECT k FROM no_such_table WHERE k = $1", "42P01"),
        ("SELECT no_such_column FROM bound_err WHERE k = $1", "42703"),
        ("SELECT k FROM bound_err WHERE k = $2", "42P18"),
    ):
        c.send(query(sql))
        out = c.until_ready()
        f = fields(out[0][1])
        assert f["C"] == state and int(f["P"]) > 0, (sql, f)


# ---------------------------------------------------------------- extended query


def test_an_error_discards_messages_up_to_sync(server):
    c = connect(server)
    c.send(parse("SELECT FROM WHERE"), bind([]), describe("P"), execute(), SYNC)
    assert kinds(c.until_ready()) == b"EZ"
    table(c)  # the session goes on


def test_binary_format_and_a_row_limit_are_refused(server):
    c = connect(server)
    table(c)
    c.send(parse("SELECT v FROM t WHERE k = $1"), bind(["1"], formats=[1]), SYNC)
    assert states(c.until_ready()) == ["0A000"]
    c.send(parse("SELECT v FROM t WHERE k = $1"), bind(["1"]), execute(max_rows=1), SYNC)
    assert states(c.until_ready()) == ["0A000"]


def test_describe_statement_gives_parameter_types_and_columns(server):
    c = connect(server)
    c.send(query("CREATE TABLE d (k SMALLINT PRIMARY KEY, n NUMERIC(6, 2), s VARCHAR(8))"))
    c.until_ready()
    c.send(parse("SELECT n, s FROM d WHERE k = $1", "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    n, oid = struct.unpack("!hi", out[1][1])
    assert (n, oid) == (1, 21)
    row = out[2][1]
    assert row.startswith(b"\0\2n\0")
    _, _, oid, length, modifier, _ = struct.unpack("!ihihih", row[4:22])
    assert (oid, length, modifier) == (1700, -1, ((6 << 16) | 2) + 4)
    c.send(parse("INSERT INTO d VALUES ($1, $2, $3)", "i"), describe("S", "i"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tnZ"


def test_a_parameter_its_type_does_not_take_is_22p02(server):
    c = connect(server)
    table(c)
    assert states(run(c, "SELECT v FROM t WHERE k = $1", ["one"])) == ["22P02"]
    assert states(run(c, "SELECT v FROM t WHERE k = $1", ["99999999999"])) == ["22003"]


# ---------------------------------------------------------------- the block status


def test_terminate_and_a_dropped_connection_roll_back_an_open_transaction(server):
    c = connect(server)
    table(c)
    for end in (lambda x: x.send(TERMINATE), lambda x: x.close()):
        d = connect(server)
        d.send(query("BEGIN"))
        assert status(d.until_ready()) == "T"
        assert status(run(d, "INSERT INTO t VALUES ($1, $2)", ["1", "1"])) == "T"
        end(d)
        d.close()
        assert keys(c) == []


def test_an_error_inside_begin_fails_the_block_until_its_end(server):
    c = connect(server)
    table(c)
    for failing in (
        ("SELECT nope FROM t WHERE k = $1", ["1"]),  # fails to bind
        ("SELECT v FROM t WHERE k = $1", ["not a number"]),  # a parameter that does not parse
    ):
        c.send(query("BEGIN"))
        c.until_ready()
        assert status(run(c, "INSERT INTO t VALUES ($1, $2)", ["1", "1"])) == "T"
        out = run(c, *failing)
        assert states(out) and status(out) == "E"
        out = run(c, "INSERT INTO t VALUES ($1, $2)", ["2", "2"])
        assert states(out) == ["25P02"] and status(out) == "E"
        c.send(query("BEGIN"))
        assert states(c.until_ready()) == ["25P02"]
        c.send(query("COMMIT"))
        out = c.until_ready()
        assert [b for k, b in out if k == b"C"] == [b"ROLLBACK\0"] and status(out) == "I"
        assert keys(c) == []


def test_warnings_for_begin_inside_a_block_and_commit_outside_one(server):
    c = connect(server)
    c.send(query("COMMIT"))
    out = c.until_ready()
    assert states(out, b"N") == ["25P01"] and [b for k, b in out if k == b"C"] == [b"COMMIT\0"]
    c.send(query("ROLLBACK"))
    assert states(c.until_ready(), b"N") == ["25P01"]
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("BEGIN"))
    out = c.until_ready()
    assert states(out, b"N") == ["25001"] and status(out) == "T"
    c.send(query("ROLLBACK"))
    assert status(c.until_ready()) == "I"


def test_executes_before_one_sync_are_one_implicit_transaction(server):
    c = connect(server)
    table(c)
    insert = "INSERT INTO t VALUES ($1, $2)"
    # Two that succeed: committed together at Sync.
    c.send(parse(insert), bind(["1", "1"]), execute(), bind(["2", "2"]), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    assert keys(c) == ["1", "2"]
    # The second fails: neither is committed, and Sync reports I.
    c.send(parse(insert), bind(["3", "3"]), execute(), bind(["1", "1"]), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == ["23505"] and status(out) == "I"
    assert keys(c) == ["1", "2"]


def test_a_query_inside_an_extended_block_joins_its_transaction_and_commits_it(server):
    c = connect(server)
    table(c)
    insert = "INSERT INTO t VALUES ($1, $2)"
    # A Query takes no parameters, so the layer runs none but DDL and the
    # control words in one; an empty one ends the block the same way.
    c.send(parse(insert), bind(["1", "1"]), execute(), query(""))
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    assert kinds(out) == b"12CIZ"
    c.send(SYNC)
    assert status(c.until_ready()) == "I"
    assert keys(c) == ["1"]
    # The Query fails: the Execute before it is rolled back with it.
    for failing, state in (("SELECT k FROM t WHERE k = 1", "42601"), ("CREATE TABLE w (k BIGINT PRIMARY KEY)", "0A000")):
        c.send(parse(insert), bind(["2", "2"]), execute(), query(failing))
        out = c.until_ready()
        assert states(out) == [state] and status(out) == "I", failing
        c.send(SYNC)
        c.until_ready()
        assert keys(c) == ["1"]


def test_execute_flush_sync_is_one_statement(server):
    c = connect(server)
    c.send(parse("CREATE TABLE u (k BIGINT PRIMARY KEY)"), bind([]), execute(), FLUSH, SYNC)
    out = c.until_ready()
    assert states(out) == [] and [b for k, b in out if k == b"C"] == [b"CREATE TABLE\0"]
    # A client that waits after Flush gets the reply before it sends Sync.
    c.send(parse("INSERT INTO u VALUES ($1)"), bind(["1"]), execute(), FLUSH)
    assert kinds([c.message() for _ in range(3)]) == b"12C"
    c.send(SYNC)
    assert status(c.until_ready()) == "I"
    assert data(run(c, "SELECT k FROM u WHERE k = $1", ["1"])) == [["1"]]


def test_portals_end_with_their_transaction(server):
    c = connect(server)
    table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(parse("SELECT k FROM t WHERE k = $1", "s"), bind(["1"], "s", "p"), SYNC)
    c.until_ready()
    c.send(execute("p"), SYNC)  # inside BEGIN a portal outlives Sync
    assert states(c.until_ready()) == []
    c.send(query("COMMIT"))
    c.until_ready()
    c.send(execute("p"), SYNC)
    assert states(c.until_ready()) == ["34000"]
    c.send(bind(["1"], "s", "p"), execute("p"), SYNC)  # the name binds again in a new transaction
    assert states(c.until_ready()) == []
    c.send(execute("p"), SYNC)  # outside BEGIN, the Sync ended its transaction
    assert states(c.until_ready()) == ["34000"]


def test_a_bad_message_length_is_fatal_and_nothing_after_it_runs(server):
    c = connect(server)
    table(c)
    c.send(parse("INSERT INTO t VALUES ($1, $2)"), bind(["1", "1"]), execute(),
           b"Q" + struct.pack("!i", 2), query("INSERT INTO t VALUES (2, 2)"), SYNC)
    out = []
    while not out or out[-1][0] != b"E":
        out.append(c.message())
    f = fields(out[-1][1])
    assert (f["S"], f["C"]) == ("FATAL", "08P01")
    assert c.closed()
    assert keys(connect(server)) == []


def test_ddl_runs_alone_and_is_refused_beside_another_statement(server):
    c = connect(server)
    table(c)
    c.send(parse("CREATE TABLE u (k BIGINT PRIMARY KEY)"), bind([]), execute(), SYNC)
    out = c.until_ready()
    assert [b for k, b in out if k == b"C"] == [b"CREATE TABLE\0"]
    c.send(parse("INSERT INTO t VALUES ($1, $2)"), bind(["3", "3"]), execute(),
           parse("CREATE TABLE w (k BIGINT PRIMARY KEY)"), bind([]), execute(), SYNC)
    assert states(c.until_ready()) == ["0A000"]
    assert keys(c) == []
    c.send(parse("CREATE TABLE w (k BIGINT PRIMARY KEY)"), bind([]), execute(),
           parse("INSERT INTO t VALUES ($1, $2)"), bind(["3", "3"]), execute(), SYNC)
    assert states(c.until_ready()) == ["0A000"]
    c.send(query("DROP TABLE w"))
    assert states(c.until_ready()) == ["42P01"]  # never created


def test_shutdown_sends_57p01_to_an_idle_connection():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = connect(s)
        c.send(query("BEGIN"))
        c.until_ready()
        assert s.stop() == 0
        kind, body = c.message()
        assert kind == b"E" and fields(body)["C"] == "57P01"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


BIG_ROWS = 20_000


def big_table(c: Client) -> None:
    """40 MB: more than both sockets' buffers hold."""
    c.send(query("CREATE TABLE big (k INTEGER PRIMARY KEY, v VARCHAR(2000))"))
    assert states(c.until_ready()) == []
    value = "x" * 2000
    for batch in range(BIG_ROWS // 2000):
        rows = range(batch * 2000, (batch + 1) * 2000)
        c.send(query("BEGIN"), parse("INSERT INTO big VALUES ($1, $2)"),
               *[m for k in rows for m in (bind([str(k), value]), execute())], SYNC, query("COMMIT"))
        for _ in range(3):
            assert states(c.until_ready()) == []


def test_a_stop_drops_a_client_that_stopped_reading():
    """Its send waits on a full socket; the stop still ends and closes the database."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = connect(s)
        big_table(c)
        c.send(parse("SELECT k, v FROM big WHERE k >= $1"), bind(["0"]), execute(), SYNC)  # and never read
        time.sleep(1)
        started = time.monotonic()
        assert s.stop() == 0
        assert time.monotonic() - started < 30  # ten seconds without progress, then dropped
        again = Server(directory)
        try:
            assert data(run(connect(again), "SELECT k FROM big WHERE k = $1", ["19999"])) == [["19999"]]
        finally:
            again.stop()
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_a_stop_lets_a_client_still_reading_take_its_whole_reply():
    """A result that takes seconds to read, begun before the stop: the client
    gets every row, CommandComplete and ReadyForQuery, then 57P01. A second
    SIGTERM while the stop runs changes nothing, and the server exits 0."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = connect(s)
        big_table(c)
        c.send(parse("SELECT k, v FROM big WHERE k >= $1"), bind(["0"]), execute(), SYNC)
        assert c.message()[0] == b"1"
        started = time.monotonic()
        s.proc.send_signal(signal.SIGTERM)
        out = []
        while not out or out[-1][0] != b"Z":
            out.append(c.message())
            if len(out) == 400:
                s.proc.send_signal(signal.SIGTERM)  # the stop is still running
            if len(out) % 200 == 0:
                time.sleep(0.03)  # a slow reader: the reply takes seconds
        assert time.monotonic() - started > 2
        assert kinds(out).count(b"D") == BIG_ROWS
        assert [b for k, b in out if k == b"C"] == [f"SELECT {BIG_ROWS}\0".encode()]
        kind, body = c.message()
        assert kind == b"E" and fields(body)["C"] == "57P01"
        assert s.stop() == 0
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)



def test_a_slow_reader_holds_no_worker():
    """With one worker, a client that stops reading a 40 MB result does not
    hold the worker: another session's statements still run (docs/session_design.md,
    *Tests*)."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--workers", "1"])
    try:
        slow = connect(s)
        fast = connect(s)
        big_table(slow)
        slow.send(parse("SELECT k, v FROM big WHERE k >= $1"), bind(["0"]), execute(), SYNC)
        assert slow.message()[0] == b"1"  # the worker has run the query; 40 MB sits unsent
        for k in ("0", "42", "19999"):
            assert data(run(fast, "SELECT k FROM big WHERE k = $1", [k])) == [[k]]
        fast.close()
        slow.close()
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_a_long_pipeline_does_not_starve_another_session():
    """With one worker, a session goes behind the others after each
    ReadyForQuery so a long pipeline cannot starve another client."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--workers", "1"])
    try:
        pipeliner = connect(s)
        other = connect(s)
        table(pipeliner)
        turns = 50
        pipeliner.send(
            parse("INSERT INTO t VALUES ($1, $2)"),
            *[m for k in range(turns) for m in (bind([str(k), str(k)]), execute(), SYNC)],
        )
        assert status(pipeliner.until_ready()) == "I"
        assert data(run(other, "SELECT k FROM t WHERE k = $1", ["0"])) == [["0"]]
        for _ in range(turns - 1):
            assert status(pipeliner.until_ready()) == "I"
        assert data(run(other, "SELECT k FROM t WHERE k = $1", [str(turns - 1)])) == [[str(turns - 1)]]
        other.close()
        pipeliner.close()
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)

# ---------------------------------------------------------------- text forms


# Each type with values in the forms the server reads, and some it must
# refuse as PostgreSQL does. Each is written through both and read back: the
# text, byte for byte, and the SQLSTATE of a refusal, must be PostgreSQL's.
TEXT_FORMS = [
    ("integer", ["0", " 42 ", "-2147483648", "+7", "2147483648", "1.5", "x"]),
    ("bigint", ["9223372036854775807", "-9223372036854775808", "9223372036854775808"]),
    ("smallint", ["32767", "-32769"]),
    ("numeric(6,2)", ["0", "1.005", "-1.005", "2.5", "1234.5678", "9999.995", "1e2", ".5", "-0", "x"]),
    ("double precision", ["0.1", "1e15", "1e16", "123456789012345", "1e-4", "1e-5", "-0", "Infinity", "-inf", "NaN",
                          "5e-324", "1e400", "1.7976931348623157e308", "x",
                          # Shortest digits that read back only by rounding a tie to even: one more digit.
                          "1e23", "26745696395518848"]),
    ("real", ["0.1", "1e6", "123456", "1e-4", "1e-5", "3.4028235e38", "1e39", "1e-46", "9e9", "3e10"]),
    ("boolean", ["t", "TRUE", "yes", "on", "1", "f", "off", "no", "0", "o", "maybe"]),
    ("bytea", ["\\x", "\\xDEADbeef", "\\x0 1", "abc", "a\\\\b", "\\101\\102", "\\xf", "\\9"]),
    ("text", ["", "héllo", "line\nbreak"]),
    ("varchar(3)", ["abc", "abcd"]),
    ("date", ["2026-10-22", "0001-01-01", "0001-12-31 BC", "4714-11-24 BC", "2024-02-29", "2023-02-29", "x"]),
    ("time", ["00:00", "12:34:56", "23:59:59.999999", "24:00:00", "10:00:00.5", "25:00", "x"]),
    ("timestamp", ["2026-10-22 10:11:12", "2026-10-22T10:11:12.5", "2026-10-22", "1999-12-31 23:59:59.000001",
                   "0044-03-15 12:00:00 BC", "2026-13-01", "x"]),
]


@pytest.mark.parametrize("sql_type, forms", TEXT_FORMS, ids=[t for t, _ in TEXT_FORMS])
def test_text_forms_match_postgresql(server, sql_type, forms):
    import psycopg

    dsn = os.environ.get("BYTECASK_SQL_ORACLE")
    if not dsn:
        pytest.skip("BYTECASK_SQL_ORACLE is not set: no PostgreSQL to compare with")
    pg = psycopg.connect(dsn, autocommit=True, prepare_threshold=None)
    ours = psycopg.connect(server.dsn(), autocommit=True, prepare_threshold=None)
    name = "forms_" + sql_type.split("(")[0].replace(" ", "_")
    try:
        pg.execute(f"DROP TABLE IF EXISTS {name}")
        for conn in (pg, ours):
            conn.execute(f"CREATE TABLE {name} (k BIGINT PRIMARY KEY, v {sql_type})")

        def outcome(conn, k, form):
            try:
                conn.execute(f"INSERT INTO {name} VALUES (%t, %t)", (k, form))
            except psycopg.Error as e:
                return ("refused", e.sqlstate)
            cur = conn.execute(f"SELECT v FROM {name} WHERE k = %t", (k,))
            return ("text", cur.pgresult.get_value(0, 0), cur.pgresult.ftype(0))

        for k, form in enumerate(forms):
            assert outcome(ours, k, form) == outcome(pg, k, form), f"{sql_type} {form!r}"
    finally:
        pg.execute(f"DROP TABLE IF EXISTS {name}")
        pg.close()
        ours.close()


def test_a_compared_decimal_finer_than_its_scale_and_nan_are_refused(server):
    """Known differences (docs/server_design.md): PostgreSQL compares such a
    value unrounded and stores NaN; a scaled integer holds neither."""
    import psycopg

    with psycopg.connect(server.dsn(), autocommit=True, prepare_threshold=None) as c:
        c.execute("CREATE TABLE m (k NUMERIC(6, 2) PRIMARY KEY, v NUMERIC(6, 2))")
        c.execute("INSERT INTO m VALUES (%t, %t)", (decimal.Decimal("1.00"), decimal.Decimal("1.005")))
        assert c.execute("SELECT v FROM m WHERE k = %t", ("1.00",)).fetchall() == [(decimal.Decimal("1.01"),)]
        for sql, params in (("SELECT v FROM m WHERE k = %t", ("1.005",)),
                            ("UPDATE m SET v = v + %t WHERE k = %t", ("1.005", "1.00"))):
            with pytest.raises(psycopg.Error) as e:
                c.execute(sql, params)
            assert e.value.sqlstate == "22P02", e.value
        with pytest.raises(psycopg.Error) as e:
            c.execute("INSERT INTO m VALUES (%t, %t)", ("2.00", "NaN"))
        assert e.value.sqlstate == "22P02"


@pytest.mark.parametrize("flags", [
    ["--sync", "AT_EVERY_COMMIT"],
    ["--sync", "at_interval", "--sync-interval-ms", "50"],
    ["--sync", "AT_FILE_ROTATION"],
    ["--io-backend", "buffer_pool", "--buffer-pool-size", str(256 << 20), "--buffer-pool-direct-io", "on"],
    ["--io-backend", "MMAP"],
    ["--statement-memory-bytes", str(1 << 20)],
])
def test_sync_modes_and_io_backends_serve_and_keep_commits(flags):
    """--sync names the plugin's bytecaskdb_sync modes and --io-backend its
    backends (docs/server_design.md, *Durability* and *Reads*). Under each,
    rows written are read back, and are there again after SIGTERM and a
    restart with the same flags."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_sync_")
    try:
        s = Server(directory, extra_args=flags)
        try:
            c = connect(s)
            table(c)
            for k in ("1", "2", "3"):
                assert states(run(c, "INSERT INTO t VALUES ($1, $2)", [k, "10"])) == []
            assert keys(c) == ["1", "2", "3"]
            c.close()
        finally:
            assert s.stop() == 0
        s = Server(directory, extra_args=flags)
        try:
            c = connect(s)
            assert keys(c) == ["1", "2", "3"]
            c.close()
        finally:
            assert s.stop() == 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_statement_memory_bytes_is_every_statement_s_budget():
    """--statement-memory-bytes is DatabaseOptions::statement_memory_bytes
    (docs/server_design.md, *Memory*): a statement whose result rows pass it
    fails with 54000, and one within it runs."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_mem_")
    try:
        s = Server(directory, extra_args=["--statement-memory-bytes", "4096"])
        try:
            c = connect(s)
            table(c)
            for k in range(1, 201):
                assert states(run(c, "INSERT INTO t VALUES ($1, $2)", [str(k), "1"])) == []
            assert states(run(c, "SELECT k, v FROM t WHERE k >= $1", ["1"])) == ["54000"]
            assert len(data(run(c, "SELECT k, v FROM t WHERE k >= $1", ["191"]))) == 10
            c.close()
        finally:
            assert s.stop() == 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


@pytest.mark.parametrize("flags, says", [
    (["--sync", "SOMETIMES"], "--sync"),
    (["--sync-interval-ms", "0"], "sync_interval_ms is 1 to 60000"),
    (["--sync-interval-ms", "60001"], "sync_interval_ms is 1 to 60000"),
    (["--sync-interval-ms", "soon"], "--sync-interval-ms"),
    (["--io-backend", "tape"], "--io-backend"),
    (["--buffer-pool-size", "big"], "--buffer-pool-size"),
    (["--buffer-pool-direct-io", "maybe"], "--buffer-pool-direct-io"),
    (["--io-backend", "buffer_pool", "--buffer-pool-size", "1"], "buffer"),
    (["--statement-memory-bytes", "lots"], "--statement-memory-bytes"),
    (["--statement-memory-bytes", "0"], "statement_memory_bytes is at least 1"),
    (["--workers", "0"], "--workers"),
    (["--workers", "many"], "--workers"),
    (["--statement-timeout-ms", "soon"], "--statement-timeout-ms"),
    (["--idle-in-transaction-timeout-ms", "-1"], "--idle-in-transaction-timeout-ms"),
    (["--transaction-timeout-ms", "1.5"], "--transaction-timeout-ms"),
    (["--idle-session-timeout-ms", ""], "--idle-session-timeout-ms"),
    (["--max-memory-bytes", "0"], "--max-memory-bytes"),
    (["--max-memory-bytes", "half"], "--max-memory-bytes"),
])
def test_sync_and_io_flags_are_validated(flags, says):
    binary = server_binary()
    if not binary:
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_sync_")
    try:
        r = subprocess.run([binary, "--dir", directory, *flags], capture_output=True, text=True, timeout=60)
        assert r.returncode != 0
        assert says in r.stderr, r.stderr
        assert "ready on" not in r.stdout
    finally:
        shutil.rmtree(directory, ignore_errors=True)


# ------------------------------------------------- cancellation and timeouts
# docs/session_design.md, *Lifetime, cancellation and shutdown*.

CANCEL_REQUEST = 80877102
SLOW = "SELECT count(*) FROM a JOIN b ON a.v = b.v"  # every row with every row: 400 million pairs, seconds


def slow_tables(c: Client) -> None:
    """Two tables of 20,000 rows whose join on v pairs every row with every row."""
    for t in ("a", "b"):
        c.send(query(f"CREATE TABLE {t} (k INTEGER PRIMARY KEY, v BIGINT)"))
        assert states(c.until_ready()) == []
        c.send(parse(f"INSERT INTO {t} VALUES ($1, $2)"),
               *[m for k in range(20_000) for m in (bind([str(k), "0"]), execute())], SYNC)
        assert states(c.until_ready()) == []


def backend_key(messages) -> tuple[int, int]:
    [body] = [b for k, b in messages if k == b"K"]
    return struct.unpack("!ii", body)


def cancel(server: Server, key: tuple[int, int]) -> None:
    """A CancelRequest on a connection of its own, as libpq sends it: the
    server closes that connection without a reply."""
    with socket.create_connection(("127.0.0.1", server.port), timeout=30) as s:
        s.sendall(struct.pack("!iiii", 16, CANCEL_REQUEST, *key))
        assert s.recv(1) == b""


def until_ready_or_fatal(c: Client) -> list[tuple[bytes, bytes]]:
    """Messages up to ReadyForQuery, or up to a FATAL, after which there is none."""
    out = []
    while True:
        m = c.message()
        out.append(m)
        if m[0] == b"Z" or (m[0] == b"E" and fields(m[1])["S"] == "FATAL"):
            return out


def fatal(messages) -> dict[str, str]:
    kind, body = messages[-1]
    assert kind == b"E", messages
    f = fields(body)
    assert f["S"] == "FATAL", f
    return f


def test_startup_sends_backend_key_data_once_and_distinct_per_session():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        first, second = Client(s).start(), Client(s).start()
        for out in (first, second):
            assert kinds(out).count(b"K") == 1
            assert kinds(out).index(b"K") < kinds(out).index(b"Z")
        assert backend_key(first)[0] != backend_key(second)[0]
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_a_cancel_request_fails_the_running_statement_with_57014_and_the_session_goes_on():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = Client(s)
        key = backend_key(c.start())
        slow_tables(c)

        # A wrong secret cancels nothing: only the right key, sent later, does.
        started = time.monotonic()
        c.send(query(SLOW))
        time.sleep(0.2)
        cancel(s, (key[0], key[1] ^ 1))
        time.sleep(0.6)
        cancel(s, key)
        out = c.until_ready()
        assert states(out) == ["57014"]
        assert [fields(b)["M"] for k, b in out if k == b"E"] == ["canceling statement due to user request"]
        assert 0.8 <= time.monotonic() - started < 10
        assert status(out) == "I"
        assert data(run(c, "SELECT k FROM a WHERE k = $1", ["7"])) == [["7"]]

        # Inside a block the cancelled statement fails the block, as any error.
        c.send(query("BEGIN"))
        assert status(c.until_ready()) == "T"
        c.send(query(SLOW))
        time.sleep(0.3)
        cancel(s, key)
        out = c.until_ready()
        assert states(out) == ["57014"] and status(out) == "E"
        c.send(query("ROLLBACK"))
        assert status(c.until_ready()) == "I"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_a_cancel_request_for_an_idle_session_cancels_nothing():
    """It is not kept for the session's next statement."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = Client(s)
        key = backend_key(c.start())
        slow_tables(c)
        cancel(s, key)
        time.sleep(0.2)
        c.send(query("SELECT count(*) FROM a"))
        out = c.until_ready()
        assert states(out) == [] and data(out) == [["20000"]]
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_the_statement_timeout_fails_a_statement_with_57014_and_the_session_goes_on():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--statement-timeout-ms", "300"])
    try:
        c = connect(s)
        slow_tables(c)
        started = time.monotonic()
        c.send(query(SLOW))
        out = c.until_ready()
        assert states(out) == ["57014"]
        assert [fields(b)["M"] for k, b in out if k == b"E"] == ["canceling statement due to statement timeout"]
        assert 0.3 <= time.monotonic() - started < 10
        assert data(run(c, "SELECT k FROM a WHERE k = $1", ["7"])) == [["7"]]
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_the_idle_in_transaction_timeout_rolls_back_and_closes_but_spares_a_busy_transaction():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--idle-in-transaction-timeout-ms", "300"])
    try:
        c = connect(s)
        table(c)
        c.send(query("BEGIN"))
        assert status(c.until_ready()) == "T"
        assert states(run(c, "INSERT INTO t VALUES ($1, $2)", ["1", "1"])) == []
        for _ in range(8):  # 100 ms apart: never idle for 300 ms, though open for longer
            time.sleep(0.1)
            assert status(run(c, "SELECT k FROM t WHERE k = $1", ["1"])) == "T"
        time.sleep(1)
        f = fatal([c.message()])
        assert f["C"] == "25P03"
        assert f["M"] == "terminating connection due to idle-in-transaction timeout"
        assert c.closed()
        assert keys(connect(s)) == []  # rolled back
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_the_transaction_timeout_ends_a_busy_transaction():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--transaction-timeout-ms", "500"])
    try:
        c = connect(s)
        table(c)
        c.send(query("BEGIN"))
        assert status(c.until_ready()) == "T"
        started = time.monotonic()
        assert states(run(c, "INSERT INTO t VALUES ($1, $2)", ["1", "1"])) == []
        out = []
        while time.monotonic() - started < 5:  # busy: a statement every 50 ms
            time.sleep(0.05)
            c.send(parse("SELECT k FROM t WHERE k = $1"), bind(["1"]), execute(), SYNC)
            out = until_ready_or_fatal(c)
            if out[-1][0] != b"Z":
                break
        f = fatal(out)
        assert f["C"] == "25P04"
        assert f["M"] == "terminating connection due to transaction timeout"
        assert 0.5 <= time.monotonic() - started < 5
        assert c.closed()
        assert keys(connect(s)) == []  # rolled back
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_the_idle_session_timeout_closes_an_idle_session_but_not_one_in_a_transaction():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--idle-session-timeout-ms", "300"])
    try:
        idle = connect(s)
        in_block = connect(s)
        table(in_block)
        in_block.send(query("BEGIN"))
        assert status(in_block.until_ready()) == "T"
        time.sleep(1)
        f = fatal([idle.message()])
        assert f["C"] == "57P05"
        assert f["M"] == "terminating connection due to idle-session timeout"
        assert idle.closed()
        assert status(run(in_block, "SELECT k FROM t WHERE k = $1", ["1"])) == "T"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


# ------------------------------------------------------------------ bounds
# docs/session_design.md, *Bounds*.

def rss_bytes(pid: int) -> int:
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    raise AssertionError("no VmRSS")


def test_a_client_that_takes_no_replies_holds_its_pipeline_back():
    """3,000 pipelined statements of 20 KB, each replying 100 KB (60 MB in,
    300 MB out), from a client that reads nothing for three seconds: past
    64 MiB unsent the session runs nothing more, so the server reads nothing
    more, the client's send stops part-way, and the server's memory stays far
    under the 300 MB. Then the client reads, and every reply arrives."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    try:
        c = connect(s)
        c.send(query("CREATE TABLE p (k INTEGER PRIMARY KEY, v VARCHAR(1000))"))
        assert states(c.until_ready()) == []
        c.send(query("BEGIN"), parse("INSERT INTO p VALUES ($1, $2)"),
               *[m for k in range(100) for m in (bind([str(k), "x" * 1000]), execute())], SYNC, query("COMMIT"))
        for _ in range(3):
            assert states(c.until_ready()) == []
        before = rss_bytes(s.proc.pid)

        statements = 3_000
        one = query("SELECT k, v FROM p" + " " * 20_000)
        sender = threading.Thread(target=c.send, args=(one * statements,), daemon=True)
        sender.start()
        time.sleep(3)
        assert sender.is_alive()  # the server stopped reading
        grown = rss_bytes(s.proc.pid) - before
        assert grown < 160 << 20, grown

        replies = completes = 0
        while replies < statements:
            kind, body = c.message()
            if kind == b"C":
                assert body == b"SELECT 100\0"
                completes += 1
            elif kind == b"Z":
                replies += 1
            else:
                assert kind in (b"T", b"D"), (kind, body)
        assert completes == statements
        sender.join(timeout=30)
        assert not sender.is_alive()
        assert data(run(c, "SELECT k FROM p WHERE k = $1", ["7"])) == [["7"]]
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_max_memory_bytes_bounds_every_statement_and_transaction_together():
    """--max-memory-bytes is DatabaseOptions::max_memory_bytes: a statement
    that would pass it fails with 53200 and fails its block; one within it
    runs."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_mem_")
    s = Server(directory, extra_args=["--max-memory-bytes", "4096"])
    try:
        c = connect(s)
        table(c)
        for k in range(1, 201):
            assert states(run(c, "INSERT INTO t VALUES ($1, $2)", [str(k), "1"])) == []
        out = run(c, "SELECT k, v FROM t WHERE k >= $1", ["1"])
        assert states(out) == ["53200"]
        error = fields(next(b for k, b in out if k == b"E"))
        assert error["M"] == ("out of memory: the statements and transactions in progress "
                              "would hold more than the memory cap of 4096 bytes")
        assert len(data(run(c, "SELECT k, v FROM t WHERE k >= $1", ["191"]))) == 10
        c.send(query("BEGIN"))
        assert status(c.until_ready()) == "T"
        out = run(c, "SELECT k, v FROM t WHERE k >= $1", ["1"])
        assert states(out) == ["53200"] and status(out) == "E"
        c.send(query("ROLLBACK"))
        assert status(c.until_ready()) == "I"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)
