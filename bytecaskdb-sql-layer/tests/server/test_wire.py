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
import resource
import select
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


def fields_tag(messages) -> str:
    """CommandComplete's tag."""
    return next(b for k, b in messages if k == b"C").rstrip(b"\0").decode()


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
        # Past the limit the startup packet is still read, so that a cancel
        # gets through, and a startup is refused.
        second = Client(s)
        second.send(startup_packet(PROTOCOL_30, {"user": "wire", "database": "proof"}))
        kind, body = second.message()
        assert kind == b"E" and fields(body)["C"] == "53300"
        assert second.closed()
        # Past the 16 connections taken in for a cancel, one is refused at once.
        waiting = [Client(s) for _ in range(16)]
        for c in waiting:
            c.send(startup_packet(80877103))  # SSLRequest
            assert c.byte() == b"N"  # each is taken in: the server answered it
        past = Client(s)
        kind, body = past.message()
        assert kind == b"E" and fields(body)["C"] == "53300"
        for c in waiting:
            c.close()
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
    c.send(query("SHOW search_path"))
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


def test_an_error_carries_postgresqls_detail_and_hint(server):
    c = connect(server)
    for sql in ("CREATE TABLE detail_t (k BIGINT PRIMARY KEY)", "CREATE VIEW detail_v AS SELECT DISTINCT k FROM detail_t"):
        c.send(query(sql))
        c.until_ready()
    c.send(query("INSERT INTO detail_v VALUES (1)"))
    f = fields(c.until_ready()[0][1])
    assert f["C"] == "55000" and f["M"] == 'cannot insert into view "detail_v"', f
    assert f["D"] == "Views containing DISTINCT are not automatically updatable.", f
    assert f["H"].startswith("To enable inserting into the view"), f
    c.send(query("SELECT k FROM detail_t WHERE nope = 1"))
    f = fields(c.until_ready()[0][1])
    assert "D" not in f and "H" not in f, f


def test_a_runtime_error_sends_its_sqlstate_and_its_hint_and_context_only_where_present(server):
    # The SQLSTATE comes from the error the runtime raised, the same as
    # PostgreSQL 17's; a runtime error without a HINT or CONTEXT sends no H
    # or W field, and one with a HINT sends it as H.
    c = connect(server)
    for sql in (
        "CREATE TABLE rt_err (k BIGINT PRIMARY KEY, z INTEGER, s TEXT)",
        "INSERT INTO rt_err VALUES (1, 0, 'abc')",
    ):
        c.send(query(sql))
        c.until_ready()
    for sql, state, message in (
        ("SELECT k / z FROM rt_err", "22012", "division by zero"),
        ("SELECT substring(s FROM 1 FOR z - 1) FROM rt_err", "22011", "negative substring length not allowed"),
        ("SELECT random(k + 1, k) FROM rt_err", "22023", "lower bound must be less than or equal to upper bound"),
    ):
        c.send(query(sql))
        f = fields(c.until_ready()[0][1])
        assert f["C"] == state and f["M"] == message, (sql, f)
        assert "D" not in f and "H" not in f and "W" not in f, (sql, f)
    c.send(
        query(
            "INSERT INTO rt_err VALUES (2, 0, 'a'), (2, 0, 'b') "
            "ON CONFLICT (k) DO UPDATE SET s = excluded.s"
        )
    )
    f = fields(c.until_ready()[0][1])
    assert f["C"] == "21000", f
    assert f["H"].startswith("Ensure that no rows proposed for insertion"), f
    assert "W" not in f, f


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


def row_description(body: bytes) -> list[tuple[str, int]]:
    """Each RowDescription field's name and type OID."""
    (n,) = struct.unpack("!h", body[:2])
    body, out = body[2:], []
    for _ in range(n):
        name, body = body.split(b"\0", 1)
        _, _, oid, _, _, _ = struct.unpack("!ihihih", body[:18])
        out.append((name.decode(), oid))
        body = body[18:]
    return out


def test_literals_are_no_parameters_and_their_columns_have_postgresqls_types(server):
    c = connect(server)
    c.send(query("CREATE TABLE d (k SMALLINT PRIMARY KEY, s TEXT)"))
    c.until_ready()
    sql = "SELECT k, 'x', 2.50, 5, NULL FROM d WHERE k = $1 AND s = 'y'"
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert struct.unpack("!hi", out[1][1]) == (1, 21)  # $1 alone, a SMALLINT: the literals are no parameters
    assert row_description(out[2][1]) == [("k", 21), ("?column?", 25), ("?column?", 1700), ("?column?", 23),
                                          ("?column?", 25)]
    assert states(run(c, "INSERT INTO d VALUES (7, 'y')")) == []
    assert data(run(c, sql, ["7"])) == [["7", "x", "2.50", "5", None]]
    assert data(run(c, "SELECT -2.E-2, 1 + 1.5, TRUE")) == [["-0.02", "2.5", "t"]]
    assert states(run(c, "SELECT 2147483647 + 1")) == ["22003"]
    # A cast is named by its type; a comparison of constants is a boolean.
    sql = "SELECT DATE '2016-03-26', 3 < 5, CAST('5' AS INTEGER)"
    c.send(parse(sql, "c"), describe("S", "c"), SYNC)
    assert row_description(c.until_ready()[2][1]) == [("date", 1082), ("?column?", 16), ("int4", 23)]
    assert data(run(c, sql)) == [["2016-03-26", "t", "5"]]


def test_the_jsonb_accessors_describe_as_postgresql_17(server):
    """jsonb's accessors (docs/jsonb_design.md, stage 3): -> and #> give
    jsonb (3802), ->> and jsonb_typeof text (25); a key's $n is text, a
    path's $n text[] (1009), as PostgreSQL 17 describes them."""
    c = connect(server)
    c.send(query("CREATE TABLE jw (k INTEGER PRIMARY KEY, v jsonb)"))
    c.until_ready()
    sql = "SELECT v -> $1, v ->> 'a', v #> $2, jsonb_typeof(v), (v -> 0)::int FROM jw WHERE v #>> ARRAY[$3] = $4"
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert struct.unpack("!hiiii", out[1][1]) == (4, 25, 1009, 25, 25)
    assert row_description(out[2][1]) == [("?column?", 3802), ("?column?", 25), ("?column?", 3802), ("jsonb_typeof", 25),
                                          ("int4", 23)]
    assert states(run(c, "INSERT INTO jw VALUES (1, '{\"a\": [1, 2], \"b\": {\"c\": \"x\"}}')")) == []
    assert data(run(c, sql, ["a", "{a,1}", "b", '{"c": "x"}'])) == [['[1, 2]', '[1, 2]', '2', 'object', None]]
    assert states(run(c, sql, ["a", "{a", "b", "x"])) == ["22P02"]
    assert states(run(c, "SELECT (v -> 'a')::int FROM jw")) == ["22023"]


def test_a_cast_of_an_expression_is_described_and_named_as_postgresql_17(server):
    """A cast of any expression (#904 stage C1): described as its target
    type, numeric(5,2) with its typmod; $1::text::int describes $1 as text
    (25) and the result as int4 (23); each named by FigureColname, as
    PostgreSQL 17 does; a per-row error with no position."""
    c = connect(server)
    c.send(query("CREATE TABLE cw (k INTEGER PRIMARY KEY, s TEXT, j jsonb)"))
    c.until_ready()
    sql = ("SELECT s::int, (s || 'x')::text, (j->>'n')::bigint, upper(s)::text, (k + 1)::numeric(5,2), $1::text::int "
           "FROM cw WHERE s::int > $2")
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert struct.unpack("!hii", out[1][1]) == (2, 25, 23)
    assert row_description(out[2][1]) == [("s", 23), ("text", 25), ("int8", 20), ("upper", 25), ("numeric", 1700),
                                          ("int4", 23)]
    body = out[2][1][2:]
    mods = []
    for _ in range(6):
        _, body = body.split(b"\0", 1)
        mods.append(struct.unpack("!ihihih", body[:18])[4])
        body = body[18:]
    assert mods[4] == (5 << 16 | 2) + 4  # numeric(5,2)'s typmod
    assert states(run(c, "INSERT INTO cw VALUES (1, '7', '{\"n\": \"12\"}'), (2, 'x', NULL)")) == []
    assert data(run(c, sql + " AND k = 1", ["0x1F", "1"])) == [["7", "7x", "12", "7", "2.00", "31"]]
    assert states(run(c, sql, ["1", "0"])) == ["22P02"]
    assert states(run(c, "SELECT $1::text::int", ["zz"])) == ["22P02"]


def test_text_min_and_max_describe_as_postgresql_17(server):
    """MIN and MAX of text (#904, stage M1): of a VARCHAR text (25), of a
    CHAR(n) column bpchar of no length (1042, typmod -1), its value padded
    to the column's n, as PostgreSQL 17 describes and returns them."""
    c = connect(server)
    c.send(query("CREATE TABLE mw (k INTEGER PRIMARY KEY, g INTEGER, v VARCHAR(10), ch CHAR(4))"))
    c.until_ready()
    sql = "SELECT g, max(v), min(ch) FROM mw WHERE k > $1 GROUP BY g"
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert row_description(out[2][1]) == [("g", 23), ("max", 25), ("min", 1042)]
    body = out[2][1][2:]
    mods = []
    for _ in range(3):
        _, body = body.split(b"\0", 1)
        mods.append(struct.unpack("!ihihih", body[:18])[4])
        body = body[18:]
    assert mods == [-1, -1, -1]
    assert states(run(c, "INSERT INTO mw VALUES (1, 1, 'b', 'b'), (2, 1, 'a', 'ab'), (3, 1, NULL, NULL)")) == []
    assert data(run(c, sql, ["0"])) == [["1", "b", "ab  "]]
    assert data(run(c, "SELECT max(v), min(ch) FROM mw WHERE k > $1", ["5"])) == [[None, None]]


def test_a_cast_of_a_cast_parameter_is_named_by_the_outer_type(server):
    """PostgreSQL's FigureColname: a cast's name is its operand's when that
    names it, and an inner cast's is weak, so $1::jsonb::int is int4."""
    c = connect(server)
    sql = "SELECT $1::jsonb::int, ($1::jsonb)::numeric, CAST($1::jsonb AS bigint), (CAST($1 AS jsonb))::int, $1::jsonb"
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert row_description(out[2][1]) == [("int4", 23), ("numeric", 1700), ("int8", 20), ("int4", 23), ("jsonb", 3802)]
    assert data(run(c, sql, ["7"])) == [["7", "7", "7", "7", "7"]]


def test_a_parameter_cast_in_a_select_without_from_takes_the_cast_s_type(server):
    c = connect(server)
    sql = "SELECT $1::integer, $1::integer + 1"
    c.send(parse(sql, "q"), describe("S", "q"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"
    assert struct.unpack("!hi", out[1][1]) == (1, 23)  # $1 is an INTEGER, as its cast types it
    assert row_description(out[2][1]) == [("int4", 23), ("?column?", 23)]
    assert data(run(c, sql, ["41"])) == [["41", "42"]]
    assert data(run(c, sql, [None])) == [[None, None]]
    assert states(run(c, sql, ["2147483647"])) == ["22003"]  # the sum is past INTEGER
    assert states(run(c, sql, ["2147483648"])) == ["22003"]  # $1 is
    assert states(run(c, sql, ["x"])) == ["22P02"]


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
    for failing, state in (("SELECT k FROM t WHERE v SIMILAR TO 'a%'", "42601"),
                           ("CREATE TABLE t (k BIGINT PRIMARY KEY)", "42P07")):
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


def test_ddl_in_an_implicit_block_is_one_transaction_with_its_statements(server):
    """docs/transaction_design.md, stage 4: Executes before one Sync, and a
    Simple Query sent inside that pipeline, form one transaction with their
    DDL, as PostgreSQL's implicit block does: all of it commits, or none."""
    c = connect(server)
    table(c)
    c.send(parse("CREATE TABLE u (k BIGINT PRIMARY KEY)"), bind([]), execute(), SYNC)
    out = c.until_ready()
    assert [b for k, b in out if k == b"C"] == [b"CREATE TABLE\0"]
    # DDL by Execute, beside DML before and after it: committed together at Sync.
    c.send(parse("INSERT INTO t VALUES ($1, $2)"), bind(["1", "1"]), execute(),
           parse("CREATE TABLE w (k BIGINT PRIMARY KEY)"), bind([]), execute(),
           parse("INSERT INTO w VALUES ($1)"), bind(["7"]), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    assert keys(c) == ["1"]
    assert data(q(c, "SELECT k FROM w")) == [["7"]]
    # It fails half way: neither the DDL nor the rows around it are written.
    c.send(parse("CREATE TABLE w2 (k BIGINT PRIMARY KEY)"), bind([]), execute(),
           parse("INSERT INTO w2 VALUES ($1)"), bind(["1"]), execute(),
           parse("INSERT INTO t VALUES ($1, $2)"), bind(["2", "2"]), execute(),
           parse("INSERT INTO t VALUES ($1, $2)"), bind(["1", "1"]), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == ["23505"] and status(out) == "I"
    assert keys(c) == ["1"]
    assert states(q(c, "SELECT k FROM w2")) == ["42P01"]
    # DDL by a Simple Query inside the pipeline: it joins the block, which
    # commits when the Query ends; a failing one writes nothing of the block.
    c.send(parse("INSERT INTO t VALUES ($1, $2)"), bind(["2", "2"]), execute(),
           query("CREATE TABLE w3 AS SELECT k FROM t"))
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    c.send(SYNC)
    c.until_ready()
    assert data(q(c, "SELECT k FROM w3 ORDER BY k")) == [["1"], ["2"]]
    c.send(parse("INSERT INTO t VALUES ($1, $2)"), bind(["3", "3"]), execute(),
           query("CREATE TABLE w4 AS SELECT k / 0 AS z FROM t"))
    out = c.until_ready()
    assert states(out) == ["22012"] and status(out) == "I"
    c.send(SYNC)
    c.until_ready()
    assert keys(c) == ["1", "2"]
    assert states(q(c, "SELECT z FROM w4")) == ["42P01"]


def test_create_table_as_in_a_begin_block_is_a_statement_of_the_block(server):
    """docs/transaction_design.md, stage 4: CREATE TABLE AS in a block reads
    the block's own writes and tables; ROLLBACK undoes it, COMMIT publishes
    the table and its rows. Its query's parameters are the statement's, by
    Execute as by PQexecParams."""
    c, other = connect(server), connect(server)
    table(c)
    q(c, "INSERT INTO t VALUES (1, 10)")
    for end in ("ROLLBACK", "COMMIT"):
        q(c, "BEGIN")
        q(c, "INSERT INTO t VALUES (2, 20)")
        q(c, "CREATE TABLE src (k INTEGER PRIMARY KEY)")
        q(c, "INSERT INTO src VALUES (3)")
        out = q(c, "CREATE TABLE copied AS SELECT k FROM t UNION ALL SELECT k FROM src")
        assert states(out) == [] and fields_tag(out) == "SELECT 3", end
        c.send(parse("CREATE TABLE picked AS SELECT k, v FROM t WHERE v > $1"), bind(["15"]), execute(), SYNC)
        out = c.until_ready()
        assert states(out) == [] and fields_tag(out) == "SELECT 1" and status(out) == "T", end
        assert data(q(c, "SELECT k FROM copied ORDER BY k")) == [["1"], ["2"], ["3"]]
        assert states(q(other, "SELECT k FROM copied")) == ["42P01"]  # uncommitted: not seen
        assert status(q(c, end)) == "I"
    assert data(q(other, "SELECT k FROM copied ORDER BY k")) == [["1"], ["2"], ["3"]]
    assert data(q(other, "SELECT k, v FROM picked")) == [["2", "20"]]
    # A taken name is 42P07 and fails the block.
    q(c, "BEGIN")
    out = q(c, "CREATE TABLE copied AS SELECT 1 AS k")
    assert states(out) == ["42P07"] and status(out) == "E"
    q(c, "ROLLBACK")


def test_create_table_as_whose_constant_fails_to_fold_lists_its_parameters_in_a_block(server):
    """CONTRACT, the CREATE TABLE AS row: its parameters are the statement's
    whether or not a constant fails to fold, in a BEGIN block and in an
    implicit block as outside one. Describe lists int4; Bind takes the
    parameter, and Execute is the folding error, 22012, as in PostgreSQL 17."""
    c = connect(server)
    sql = "CREATE TABLE {} AS SELECT 1/0 AS z, $1::int AS y"
    for block, name in (("none", "fold_a"), ("begin", "fold_b"), ("implicit", "fold_c")):
        if block == "begin":
            assert status(q(c, "BEGIN")) == "T"
        head = parse("SELECT 1") + bind([]) + execute() if block == "implicit" else b""
        c.send(head + parse(sql.format(name), name) + describe("S", name) + bind(["3"], name) + execute() + SYNC)
        out = c.until_ready()
        described = next(b for k, b in out if k == b"t")
        assert struct.unpack("!hi", described) == (1, 23), block
        assert states(out) == ["22012"], block
        if block == "begin":
            assert status(out) == "E"
            q(c, "ROLLBACK")
        assert states(q(c, f"SELECT * FROM {name}")) == ["42P01"], block
    # WITH NO DATA of such a query: a Bind of no parameters is PostgreSQL's
    # 08P01; with its parameter, the layer's refusal.
    q(c, "BEGIN")
    c.send(parse(sql.format("fold_d") + " WITH NO DATA", "n") + bind([], "n") + SYNC)
    assert states(c.until_ready()) == ["08P01"]
    q(c, "ROLLBACK")
    q(c, "BEGIN")
    c.send(parse(sql.format("fold_d") + " WITH NO DATA", "m") + bind(["3"], "m") + execute() + SYNC)
    assert states(c.until_ready()) == ["0A000"]
    q(c, "ROLLBACK")


def test_create_and_drop_procedure_have_their_command_tags(server):
    c = connect(server)
    c.send(query("CREATE PROCEDURE p(a INTEGER, INOUT b TEXT) LANGUAGE plpgsql AS $$ BEGIN b := 'x'; END $$"))
    assert [b for k, b in c.until_ready() if k == b"C"] == [b"CREATE PROCEDURE\0"]
    c.send(parse("CALL p(1, NULL)"), SYNC)  # a simple Query only
    assert states(c.until_ready()) == ["0A000"]
    c.send(query("DROP PROCEDURE p(INTEGER, TEXT)"))
    assert [b for k, b in c.until_ready() if k == b"C"] == [b"DROP PROCEDURE\0"]
    c.send(query("DROP PROCEDURE p"))
    assert states(c.until_ready()) == ["42883"]
    # The ';' of a dollar-quoted body, tagged or not, ends no statement; one after it does.
    c.send(query("CREATE PROCEDURE q() LANGUAGE plpgsql AS $body$ BEGIN NULL; NULL; END $body$; ; "))
    assert [b for k, b in c.until_ready() if k == b"C"] == [b"CREATE PROCEDURE\0"]
    c.send(query("CREATE PROCEDURE r() LANGUAGE plpgsql AS $$ BEGIN NULL; END $$; DROP PROCEDURE q"))
    assert states(c.until_ready()) == ["0A000"]  # two statements: the simple protocol takes one


def test_a_ddl_that_if_exists_skips_sends_postgresqls_notice_before_its_tag(server):
    c = connect(server)
    for sql in ("CREATE TABLE skipped (id INTEGER PRIMARY KEY)", "CREATE VIEW skipped_v AS SELECT id FROM skipped"):
        c.send(query(sql))
        assert states(c.until_ready()) == []
    cases = [
        # Taken by a table or a view: skipped before the definition is read.
        ("CREATE TABLE IF NOT EXISTS skipped (a INTEGER, a INTEGER)", "CREATE TABLE", "42P07",
         'relation "skipped" already exists, skipping'),
        ("CREATE TABLE IF NOT EXISTS skipped_v (a INTEGER)", "CREATE TABLE", "42P07",
         'relation "skipped_v" already exists, skipping'),
        ("DROP TABLE IF EXISTS nope", "DROP TABLE", "00000", 'table "nope" does not exist, skipping'),
        ("DROP INDEX IF EXISTS nope", "DROP INDEX", "00000", 'index "nope" does not exist, skipping'),
        ("DROP VIEW IF EXISTS nope", "DROP VIEW", "00000", 'view "nope" does not exist, skipping'),
        ("DROP PROCEDURE IF EXISTS nope(INTEGER, TEXT)", "DROP PROCEDURE", "00000",
         "procedure nope(pg_catalog.int4,text) does not exist, skipping"),
    ]
    for sql, tag, state, text in cases:
        for message in (query(sql), parse(sql) + bind([]) + execute() + SYNC):
            c.send(message)
            got = c.until_ready()
            kinds = [k for k, _ in got]
            assert b"N" in kinds and kinds.index(b"N") < kinds.index(b"C"), sql
            notice = fields(next(b for k, b in got if k == b"N"))
            assert (notice["S"], notice["C"], notice["M"]) == ("NOTICE", state, text)
            assert fields_tag(got) == tag
    # A DDL that changes the catalog sends none.
    c.send(query("CREATE TABLE IF NOT EXISTS fresh (id INTEGER PRIMARY KEY)"))
    assert b"N" not in [k for k, _ in c.until_ready()]


def test_drop_schema_cascade_sends_postgresqls_notice_with_its_detail(server):
    # As PostgreSQL 17: one NOTICE, its DETAIL a line per object dropped, before the tag.
    c = connect(server)
    for sql in ("CREATE SCHEMA wire_cs", "CREATE TABLE wire_cs.t (a INTEGER)", "CREATE VIEW wire_cs_v AS SELECT a FROM wire_cs.t"):
        c.send(query(sql))
        assert states(c.until_ready()) == []
    c.send(query("DROP SCHEMA wire_cs CASCADE"))
    got = c.until_ready()
    kinds = [k for k, _ in got]
    assert kinds.count(b"N") == 1 and kinds.index(b"N") < kinds.index(b"C")
    notice = fields(next(b for k, b in got if k == b"N"))
    assert (notice["S"], notice["C"], notice["M"]) == ("NOTICE", "00000", "drop cascades to 2 other objects")
    assert notice["D"] == "drop cascades to table wire_cs.t\ndrop cascades to view wire_cs_v"
    assert fields_tag(got) == "DROP SCHEMA"


def test_insert_select_answers_insert_0_n_and_takes_its_querys_parameters(server):
    c = connect(server)
    for sql in ("CREATE TABLE ins_src (id INTEGER PRIMARY KEY, v TEXT)", "INSERT INTO ins_src VALUES (1, 'a'), (2, 'b')",
                "CREATE TABLE ins_dst (id INTEGER, v TEXT)"):
        c.send(query(sql))
        c.until_ready()
    c.send(query("INSERT INTO ins_dst SELECT * FROM ins_src"))
    assert fields_tag(c.until_ready()) == "INSERT 0 2"
    c.send(parse("INSERT INTO ins_dst (v, id) SELECT $1, id FROM ins_src WHERE id = $2") + bind(["z", "2"]) + execute() + SYNC)
    assert fields_tag(c.until_ready()) == "INSERT 0 1"
    c.send(query("INSERT INTO ins_dst SELECT * FROM ins_src WHERE id > 5"))
    assert fields_tag(c.until_ready()) == "INSERT 0 0"
    assert data(run(c, "SELECT id, v FROM ins_dst WHERE v = $1", ["z"])) == [["2", "z"]]


def test_create_table_as_and_select_into_answer_postgresqls_tags_with_no_row_description(server):
    c = connect(server)
    c.send(query("CREATE TABLE ctas_src (id INTEGER PRIMARY KEY, v TEXT)"))
    c.until_ready()
    c.send(query("INSERT INTO ctas_src VALUES (1, 'a'), (2, 'b')"))
    c.until_ready()
    cases = [
        ("CREATE TABLE ctas_a AS SELECT * FROM ctas_src", "SELECT 2"),
        ("SELECT v INTO ctas_b FROM ctas_src WHERE id = 1", "SELECT 1"),
        ("CREATE TABLE ctas_c AS SELECT id FROM ctas_src WITH NO DATA", "CREATE TABLE AS"),
    ]
    for sql, tag in cases:
        c.send(query(sql))
        got = c.until_ready()
        assert states(got) == [], sql
        assert b"T" not in [k for k, _ in got], sql
        assert fields_tag(got) == tag, sql
    # IF NOT EXISTS of a name taken: PostgreSQL's notice, then CREATE TABLE AS.
    for message in (query("CREATE TABLE IF NOT EXISTS ctas_a AS SELECT 1"),
                    parse("CREATE TABLE IF NOT EXISTS ctas_a AS SELECT $1::int") + bind(["5"]) + execute() + SYNC):
        c.send(message)
        got = c.until_ready()
        notice = fields(next(b for k, b in got if k == b"N"))
        assert (notice["C"], notice["M"]) == ("42P07", 'relation "ctas_a" already exists, skipping')
        assert fields_tag(got) == "CREATE TABLE AS"
    # Its query's parameters are the statement's.
    c.send(parse("CREATE TABLE ctas_d AS SELECT v FROM ctas_src WHERE id = $1") + bind(["2"]) + execute() + SYNC)
    assert fields_tag(c.until_ready()) == "SELECT 1"
    assert data(run(c, "SELECT v FROM ctas_d", [])) == [["b"]]
    c.send(query("CREATE TABLE ctas_e AS SELECT $1::int"))
    assert states(c.until_ready()) == ["42P02"]
    # A read-only block names the statement as written.
    for sql, name in (("SELECT 1 INTO ctas_f", "SELECT INTO"), ("CREATE TABLE ctas_f AS SELECT 1", "CREATE TABLE AS")):
        c.send(query("BEGIN READ ONLY"))
        c.until_ready()
        c.send(query(sql))
        got = c.until_ready()
        error = fields(next(b for k, b in got if k == b"E"))
        assert (error["C"], error["M"]) == ("25006", f"cannot execute {name} in a read-only transaction")
        c.send(query("ROLLBACK"))
        c.until_ready()


def test_call_as_hammerdb_sends_it_returns_the_out_row_and_the_tag_call(server):
    c = connect(server)
    for sql in ("CREATE TABLE stock (id INTEGER PRIMARY KEY, qty INTEGER, at TIMESTAMP)",
                "INSERT INTO stock VALUES (1, 10, NULL)",
                """CREATE PROCEDURE take(k INTEGER, n INTEGER, INOUT left_qty INTEGER, INOUT amount NUMERIC(6,2),
                     INOUT note VARCHAR(8), stamp TIMESTAMP) LANGUAGE plpgsql AS $$
                   BEGIN
                     UPDATE stock SET qty = qty - n WHERE id = k;
                     UPDATE stock SET at = stamp WHERE id = k;
                     SELECT qty INTO left_qty FROM stock WHERE id = k;
                     note := 'done';
                   END $$"""):
        c.send(query(sql))
        assert states(c.until_ready()) == [], sql
    # HammerDB's form: literal arguments, '' and 0.0 for the INOUT ones, and the timestamp.
    c.send(query("call take(1,3,0,0.0,'',TO_TIMESTAMP('20261103120530','YYYYMMDDHH24MISS')::timestamp without time zone)"))
    out = c.until_ready()
    assert states(out) == [], out
    assert [(n, oid) for n, oid in row_description(next(b for k, b in out if k == b"T"))] == [
        ("left_qty", 23), ("amount", 1700), ("note", 1043)]
    assert data(out) == [["7", "0.00", "done"]]
    assert [b for k, b in out if k == b"C"] == [b"CALL\0"]
    assert data(run(c, "SELECT qty, at FROM stock WHERE id = $1", ["1"])) == [["7", "2026-11-03 12:05:30"]]
    # Inside a block, the procedure's writes are the block's.
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("CALL take(1, 1, NULL, NULL, NULL, NULL)"))
    assert data(c.until_ready()) == [["6", None, "done"]]
    c.send(query("ROLLBACK"))
    c.until_ready()
    assert data(run(c, "SELECT qty FROM stock WHERE id = $1", ["1"])) == [["7"]]
    # Its errors: no such procedure, the wrong count, a failed statement (the block's 25P02 after).
    c.send(query("CALL nope(1)"))
    assert states(c.until_ready()) == ["42883"]
    c.send(query("CALL take(1)"))
    assert states(c.until_ready()) == ["42883"]
    c.send(query("CALL take(1, 1, 0, 0, 'toolongvalue', NULL)"))
    assert states(c.until_ready()) == ["22001"]


def test_the_monitor_s_queries_answer(server):
    c = connect(server)
    for sql in ("CREATE TABLE district (d_id INTEGER, d_w_id INTEGER, d_next_o_id INTEGER, PRIMARY KEY (d_w_id, d_id))",
                "CREATE TABLE warehouse (w_id INTEGER PRIMARY KEY)",
                "INSERT INTO district VALUES (1, 1, 3001)", "INSERT INTO district VALUES (2, 1, 3005)",
                "INSERT INTO warehouse VALUES (1)", "INSERT INTO warehouse VALUES (2)"):
        c.send(query(sql))
        assert states(c.until_ready()) == [], sql
    c.send(query("select sum(xact_commit + xact_rollback) from pg_stat_database"))
    out = c.until_ready()
    assert row_description(next(b for k, b in out if k == b"T")) == [("sum", 1700)]
    before = int(data(out)[0][0])
    assert before >= 6  # the six statements above, each its own transaction
    c.send(query("INSERT INTO warehouse VALUES (2)"))  # 23505: rolled back, and counted
    c.until_ready()
    c.send(query("SELECT SUM(xact_commit+xact_rollback) FROM pg_stat_database;"))
    assert int(data(c.until_ready())[0][0]) == before + 1
    for sql, value in (("select sum(d_next_o_id) from district", "6006"), ("select max(w_id) from warehouse", "2"),
                       ("select max(d_id) from district", "2")):
        c.send(query(sql))
        assert data(c.until_ready()) == [[value]], sql


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


def test_a_thread_that_fails_to_start_is_an_error_not_an_abort(tmp_path):
    """Each thread the server starts, failed in turn, as at a thread limit: the
    engine's, the workers', the vacuum thread and the loops'. Failing a thread
    past the last one the server starts leaves it serving."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    compiler = shutil.which("cc") or shutil.which("clang")
    if not compiler:
        pytest.skip("no C compiler for the shim")
    shim = tmp_path / "fail_thread.so"
    source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fail_thread.c")
    subprocess.run([compiler, "-shared", "-fPIC", "-o", str(shim), source, "-ldl"], check=True)

    refused = 0
    for nth in range(1, 16):
        directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
        env = dict(os.environ, LD_PRELOAD=str(shim), BYTECASK_FAIL_THREAD=str(nth))
        proc = subprocess.Popen([server_binary(), "--dir", directory, "--listen", "127.0.0.1:0", "--workers", "4"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        try:
            output = []
            while True:
                ready, _, _ = select.select([proc.stdout], [], [], 60)
                assert ready, f"thread {nth}: neither ready nor exited within 60 s"
                line = proc.stdout.readline()
                output.append(line)
                if not line or "ready on" in line:
                    break
            serving = "ready on" in output[-1]
            if serving:
                proc.send_signal(signal.SIGTERM)
            output.extend(proc.stdout)
            status = proc.wait(timeout=60)
            assert status == (0 if serving else 1), f"thread {nth}: exit {status}: {''.join(output)}"
            if not serving:
                refused += 1
                assert "Resource temporarily unavailable" in "".join(output)
        finally:
            proc.kill()
            proc.wait()
            shutil.rmtree(directory, ignore_errors=True)
    assert refused >= 6  # at least the workers, the vacuum thread and one loop's


WIDE_ROWS = 32
WIDE_ROW = "x" * (1 << 20)


def overwrite_wide_rows(c: Client, rounds: int) -> None:
    """32 MiB a round, each over the last: 64 MiB is one data file."""
    c.send(query("CREATE TABLE wide (k INTEGER PRIMARY KEY, v TEXT)"))
    assert states(c.until_ready()) == []
    for k in range(WIDE_ROWS):
        c.send(query(f"INSERT INTO wide VALUES ({k}, '{WIDE_ROW}')"))
        assert states(c.until_ready()) == []
    for _ in range(rounds - 1):
        for k in range(WIDE_ROWS):
            c.send(query(f"UPDATE wide SET v = '{WIDE_ROW}' WHERE k = {k}"))
            assert states(c.until_ready()) == []


def data_bytes(directory: str) -> int:
    total = 0
    for name in os.listdir(directory):
        if name.endswith(".data"):
            try:
                total += os.path.getsize(os.path.join(directory, name))
            except FileNotFoundError:  # vacuum unlinked it since the listing
                pass
    return total


def test_background_vacuum_is_on_by_default_and_gives_the_disk_back():
    """192 MiB written, 32 MiB of it live, and SIGTERM still stops it cleanly."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--sync", "AT_FILE_ROTATION", "--vacuum-busy-interval-ms", "10",
                                      "--vacuum-idle-interval-ms", "100"])
    try:
        c = connect(s)
        overwrite_wide_rows(c, rounds=6)
        deadline = time.monotonic() + 60
        while data_bytes(directory) > 128 << 20 and time.monotonic() < deadline:
            time.sleep(0.05)
        assert data_bytes(directory) <= 128 << 20
        assert s.stop() == 0
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_vacuum_off_leaves_the_disk_as_written():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--sync", "AT_FILE_ROTATION", "--vacuum", "off", "--vacuum-busy-interval-ms", "10",
                                      "--vacuum-idle-interval-ms", "100"])
    try:
        c = connect(s)
        overwrite_wide_rows(c, rounds=6)
        time.sleep(1)
        assert data_bytes(directory) >= 190 << 20
        assert s.stop() == 0
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def oldest_data_file(directory: str) -> str:
    return min(name for name in os.listdir(directory) if name.endswith(".data"))


@pytest.mark.parametrize("threshold,reclaimed", [(None, False), ("0.5", True)])
def test_the_default_vacuum_threshold_leaves_a_file_under_two_thirds_dead(threshold, reclaimed):
    """64 MiB written, then 40 of those rows again: the first file is sealed and
    about 63% dead. The default threshold (0.7) keeps it; 0.5 reclaims it."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    args = ["--sync", "AT_FILE_ROTATION", "--vacuum-busy-interval-ms", "10", "--vacuum-idle-interval-ms", "100"]
    if threshold is not None:
        args += ["--vacuum-fragmentation-threshold", threshold]
    s = Server(directory, extra_args=args)
    try:
        c = connect(s)
        c.send(query("CREATE TABLE wide (k INTEGER PRIMARY KEY, v TEXT)"))
        assert states(c.until_ready()) == []
        for k in range(2 * WIDE_ROWS):
            c.send(query(f"INSERT INTO wide VALUES ({k}, '{WIDE_ROW}')"))
            assert states(c.until_ready()) == []
        first = oldest_data_file(directory)
        for k in range(40):
            c.send(query(f"UPDATE wide SET v = '{WIDE_ROW}' WHERE k = {k}"))
            assert states(c.until_ready()) == []
        # A pass every 100 ms: 3 s is about 30 passes over the sealed file.
        deadline = time.monotonic() + (60 if reclaimed else 3)
        while os.path.exists(os.path.join(directory, first)) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert os.path.exists(os.path.join(directory, first)) != reclaimed
        assert s.stop() == 0
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


@pytest.mark.parametrize("flag,value", [
    ("--vacuum", "maybe"),
    ("--vacuum-fragmentation-threshold", "1.5"),
    ("--vacuum-fragmentation-threshold", "-0.1"),
    ("--vacuum-fragmentation-threshold", "nan"),
    ("--vacuum-fragmentation-threshold", "half"),
    ("--vacuum-busy-interval-ms", "9"),
    ("--vacuum-busy-interval-ms", "3600001"),
    ("--vacuum-idle-interval-ms", "99"),
    ("--vacuum-idle-interval-ms", "86400001"),
])
def test_a_bad_vacuum_flag_exits_before_the_server_listens(flag, value):
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    try:
        done = subprocess.run([server_binary(), "--dir", directory, "--listen", "127.0.0.1:0", flag, value],
                              capture_output=True, text=True, timeout=60)
        assert done.returncode != 0
        assert flag in done.stderr
        assert "ready on" not in done.stdout
    finally:
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
    ("char(3)", ["abc", "ab", "", "  a", "ab  ", "abcd", "abc   ", "abcd  ", "é", "日本語", "日本語 "]),
    ("date", ["2026-10-22", "0001-01-01", "0001-12-31 BC", "4714-11-24 BC", "2024-02-29", "2023-02-29", "x"]),
    ("time", ["00:00", "12:34:56", "23:59:59.999999", "24:00:00", "10:00:00.5", "25:00", "x",
              # Past six digits the fraction rounds to even; second 60 is the next minute's first.
              "12:00:00.1234565", "12:00:00.1234575", "12:00:00.0000015", "12:00:00.9999996", "12:00:00.",
              "12:30:60", "12:30:60.5", "23:59:60", "12:30:61", "24:00:00.0000005", "24:00:00.0000006", "24:01:00"]),
    ("timestamp", ["2026-10-22 10:11:12", "2026-10-22T10:11:12.5", "2026-10-22", "1999-12-31 23:59:59.000001",
                   "0044-03-15 12:00:00 BC", "2026-13-01", "x",
                   "2026-10-25 12:00:00.1234565", "2026-10-25 23:59:59.9999995", "2026-12-31 23:59:60",
                   "2026-10-25 24:00:00", "2026-10-25 24:00:00.5", "2026-10-25 23:59:60.5"]),
    ("timestamptz", ["2026-10-27 10:00:00+02", "2026-10-27 10:00:00Z", "2026-10-27 10:00:00", "2026-10-27T10:00:00.5-04:30",
                     "2026-10-27 10:00:00+0530", "2026-10-27 10:00:00 +02:00", "2026-10-27 10:00:00+05:30:15",
                     "2026-10-27", "0044-03-15 12:00:00+01 BC", "0001-01-01 00:00:00+00 BC",
                     "2026-10-27 10:00:00+15", "2026-10-27 10:00:00+16", "2026-10-27 10:00:00+", "2026-13-01 00:00:00+01",
                     "294276-12-31 23:00:00-02", "4714-11-24 00:00:00+00 BC", "4714-11-23 23:00:00+00 BC", "x"]),
    ("time with time zone", ["12:34:56+02", "12:34:56", "00:00Z", "24:00:00-15:59:59", "23:59:59.999999+15:59:59",
                             "10:00:00.5-04:30", "12:00+0530", "12:00 +02:00", "12:00+05:30:15", "12:00:00.1234565+01",
                             "12:30:60+01", "2026-10-27 12:00-02", "2026-10-27T12:00-02", "2026-10-27T12:00", "12:00+15", "12:00+16", "12:00+", "24:00:01", "25:00",
                             "x"]),
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
        assert e.value.sqlstate == "22003"  # #904 C0: numeric_in reads NaN, which a DECIMAL cannot hold
        assert e.value.diag.message_detail == "A field with precision 6, scale 2 cannot hold NaN."


STATS = "select * from pg_stat_bytecask"
POOL_COUNTERS = {"bytecask.pool_" + n for n in (
    "hits", "misses", "fills", "evictions", "frames_total", "frames_resident", "frames_released", "direct_io_fallbacks")}


def counters(c: Client) -> dict[str, int]:
    c.send(query(STATS))
    messages = c.until_ready()
    assert states(messages) == []
    rows = data(messages)
    assert [name for name, _ in rows] == sorted(name for name, _ in rows)
    return {name: int(value) for name, value in rows}


def test_pg_stat_bytecask_answers_over_both_protocols(server):
    """One row per engine counter, name and value, in order of name
    (docs/server_design.md, *Reading the counters*)."""
    c = connect(server)
    table(c)
    for sql in (STATS, "SELECT  *\n FROM PG_STAT_BYTECASK ;"):
        c.send(query(sql))
        messages = c.until_ready()
        assert kinds(messages)[:2] == b"TD" and status(messages) == "I"
        assert row_description(next(b for k, b in messages if k == b"T")) == [("name", 25), ("value", 20)]
        rows = data(messages)
        assert POOL_COUNTERS <= {name for name, _ in rows}
        assert fields_tag(messages) == f"SELECT {len(rows)}"
    messages = run(c, STATS)
    assert states(messages) == []
    assert row_description(next(b for k, b in messages if k == b"T")) == [("name", 25), ("value", 20)]
    assert POOL_COUNTERS <= {name for name, _ in data(messages)}
    # Describe of the statement, before anything runs: no parameters, the same columns.
    c.send(parse(STATS, "s"), describe("S", "s"), SYNC)
    messages = c.until_ready()
    assert (b"t", struct.pack("!h", 0)) in messages
    assert row_description(next(b for k, b in messages if k == b"T")) == [("name", 25), ("value", 20)]
    # Any other text is a query of a relation that does not exist.
    for sql in ("select name from pg_stat_bytecask", "select * from pg_stat_bytecask where value > 0",
                'select * from "pg_stat_bytecask"', "select * from pg_stat_bytecask_pool"):
        c.send(query(sql))
        assert states(c.until_ready()) == ["42P01"], sql
    # A table of the name: the session still answers this text, any other reaches the table.
    c.send(query("CREATE TABLE pg_stat_bytecask (id BIGINT PRIMARY KEY)"))
    assert states(c.until_ready()) == []
    c.send(query("select id from pg_stat_bytecask"))
    assert fields_tag(c.until_ready()) == "SELECT 0"
    assert "bytecask.pool_hits" in counters(c)
    c.close()


def test_pg_stat_bytecask_in_a_block_starts_nothing_and_a_failed_one_refuses_it(server):
    c = connect(server)
    table(c)
    assert states(run(c, "INSERT INTO t VALUES ($1, $2)", ["1", "10"])) == []
    c.send(query("BEGIN"))
    assert status(c.until_ready()) == "T"
    before = counters(c)["bytecask.bytes_written"]
    # Live, not the block's snapshot: another session's commit shows.
    other = connect(server)
    assert states(run(other, "INSERT INTO t VALUES ($1, $2)", ["2", "20"])) == []
    other.close()
    assert counters(c)["bytecask.bytes_written"] > before
    c.send(query("SELECT nosuch FROM t"))
    assert status(c.until_ready()) == "E"
    c.send(query(STATS))
    messages = c.until_ready()
    assert states(messages) == ["25P02"] and data(messages) == []
    c.send(parse(STATS), SYNC)
    assert states(c.until_ready()) == ["25P02"]
    c.send(query("ROLLBACK"))
    c.until_ready()
    # A statement prepared before the block failed is refused at Bind.
    c.send(parse(STATS, "s"), SYNC)
    c.until_ready()
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("SELECT nosuch FROM t"))
    c.until_ready()
    c.send(bind([], "s"), execute(), SYNC)
    assert states(c.until_ready()) == ["25P02"]
    c.close()


@pytest.mark.parametrize("flags", [[], ["--io-backend", "pread"], ["--io-backend", "mmap"]])
def test_pg_stat_bytecask_s_pool_rows_are_zero_with_no_pool(flags):
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_stats_")
    try:
        s = Server(directory, extra_args=flags)
        try:
            c = connect(s)
            table(c)
            assert states(run(c, "INSERT INTO t VALUES ($1, $2)", ["1", "10"])) == []
            assert keys(c) == ["1"]
            got = counters(c)
            assert {n: got[n] for n in POOL_COUNTERS} == dict.fromkeys(POOL_COUNTERS, 0)
            assert got["bytecask.bytes_written"] > 0
            c.close()
        finally:
            assert s.stop() == 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def test_pg_stat_bytecask_s_pool_rows_move_with_the_pool_while_another_session_writes():
    """A sampler reads the pool's counters during a load, without stopping it."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    flags = ["--io-backend", "buffer_pool", "--buffer-pool-size", str(256 << 20)]
    directory = tempfile.mkdtemp(prefix="bytecask_sql_stats_")
    try:
        s = Server(directory, extra_args=flags)
        try:
            c = connect(s)
            table(c)
            for k in ("1", "2", "3"):
                assert states(run(c, "INSERT INTO t VALUES ($1, $2)", [k, "10"])) == []
            c.close()
        finally:
            assert s.stop() == 0
        s = Server(directory, extra_args=flags)
        try:
            sampler = connect(s)
            first = counters(sampler)
            assert first["bytecask.pool_frames_total"] > 0
            done = threading.Event()
            errors: list[str] = []

            def load():
                w = connect(s)
                k = 100
                while not done.is_set():
                    errors.extend(states(run(w, "INSERT INTO t VALUES ($1, $2)", [str(k), "10"])))
                    assert keys(w)
                    k += 1
                w.close()

            writer = threading.Thread(target=load)
            writer.start()
            try:
                samples = []
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    samples.append(counters(sampler))
                    if (len(samples) >= 20 and samples[-1]["bytecask.bytes_written"] > first["bytecask.bytes_written"]
                            and samples[-1]["bytecask.pool_hits"] + samples[-1]["bytecask.pool_misses"]
                            > first["bytecask.pool_hits"] + first["bytecask.pool_misses"]):
                        break
                    time.sleep(0.01)
            finally:
                done.set()
                writer.join()
            assert errors == []
            last = samples[-1]
            assert last["bytecask.bytes_written"] > first["bytecask.bytes_written"]
            assert last["bytecask.pool_hits"] + last["bytecask.pool_misses"] > first["bytecask.pool_hits"] + first["bytecask.pool_misses"]
            for name in ("hits", "misses", "fills", "evictions"):
                seen = [x[f"bytecask.pool_{name}"] for x in samples]
                assert seen == sorted(seen), name
            sampler.close()
        finally:
            assert s.stop() == 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


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
    (["--startup-timeout-ms", "later"], "--startup-timeout-ms"),
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


def test_a_cancel_request_is_taken_when_the_server_is_at_its_connection_limit():
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, max_connections=1)
    try:
        c = Client(s)
        key = backend_key(c.start())
        slow_tables(c)
        c.send(query(SLOW))
        time.sleep(0.3)
        cancel(s, key)
        out = c.until_ready()
        assert states(out) == ["57014"] and status(out) == "I"
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def cpu_seconds(pid: int) -> float:
    """The process's user and system CPU time so far."""
    with open(f"/proc/{pid}/stat") as f:
        fields_ = f.read().rsplit(")", 1)[1].split()
    return (int(fields_[11]) + int(fields_[12])) / os.sysconf("SC_CLK_TCK")


@pytest.mark.skipif(not hasattr(resource, "prlimit"), reason="needs resource.prlimit (Linux)")
def test_out_of_file_descriptors_the_listener_waits_instead_of_spinning():
    """accept4 failing with EMFILE leaves the connection in the backlog: the
    server stops accepting for a while rather than retry it in a loop, and
    takes it once a descriptor is free."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory)
    pid = s.proc.pid
    old = resource.prlimit(pid, resource.RLIMIT_NOFILE)
    try:
        resource.prlimit(pid, resource.RLIMIT_NOFILE, (len(os.listdir(f"/proc/{pid}/fd")), old[1]))
        waiting = Client(s)  # in the backlog: the server has no descriptor for it
        waiting.sock.sendall(startup_packet(PROTOCOL_30, {"user": "wire", "database": "proof"}))
        time.sleep(0.3)
        before = cpu_seconds(pid)
        time.sleep(1.0)
        assert cpu_seconds(pid) - before < 0.3  # a spinning listener takes the whole second
        resource.prlimit(pid, resource.RLIMIT_NOFILE, old)
        assert status(waiting.until_ready()) == "I"
        waiting.close()
    finally:
        resource.prlimit(pid, resource.RLIMIT_NOFILE, old)
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
        # One sendall per statement: a socket's timeout bounds a whole
        # sendall, so one for all 60 MB would time out on a slow build.
        sender = threading.Thread(target=lambda: [c.send(one) for _ in range(statements)], daemon=True)
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


def test_the_startup_timeout_closes_a_connection_that_never_finishes_its_startup():
    """As PostgreSQL's authentication_timeout: counted from the connection,
    closed with no reply, and its place under --max-connections freed."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, max_connections=1, extra_args=["--startup-timeout-ms", "400"])
    try:
        # Taken before the connect: the server counts from its accept, which
        # can come before Client() returns.
        started = time.monotonic()
        silent = Client(s)  # takes the one place, and sends nothing
        assert silent.sock.recv(1) == b""  # closed, with no reply
        assert 0.4 <= time.monotonic() - started < 10
        # An SSLRequest answered, then nothing: the timeout still counts from the connection.
        started = time.monotonic()
        trickle = Client(s)
        trickle.send(startup_packet(80877103))
        assert trickle.byte() == b"N"
        assert trickle.sock.recv(1) == b""
        assert 0.4 <= time.monotonic() - started < 10
        # The place is free again, and a session that started is not ended by it.
        c = connect(s)
        time.sleep(0.8)
        c.send(query("BEGIN"))
        assert status(c.until_ready()) == "T"
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


# ---------------------------------------------------------------- cursors (docs/cursor_design.md)


def tags(messages) -> list[str]:
    return [b.rstrip(b"\0").decode() for k, b in messages if k == b"C"]


def cursor_table(c: Client) -> None:
    table(c)
    for k in range(1, 6):
        assert states(run(c, "INSERT INTO t VALUES ($1, $2)", [str(k), str(k * 10)])) == []


def test_simple_query_cursor_statements_and_their_tags(server):
    c = connect(server)
    cursor_table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c CURSOR FOR SELECT k, v FROM t ORDER BY k"))
    out = c.until_ready()
    assert kinds(out) == b"CZ" and tags(out) == ["DECLARE CURSOR"] and status(out) == "T"
    c.send(query("FETCH 2 FROM c"))
    out = c.until_ready()
    assert kinds(out) == b"TDDCZ" and tags(out) == ["FETCH 2"]
    assert row_description(out[0][1]) == [("k", 23), ("v", 20)]
    assert data(out) == [["1", "10"], ["2", "20"]]
    c.send(query("MOVE FORWARD 1 IN c"))
    out = c.until_ready()
    assert kinds(out) == b"CZ" and tags(out) == ["MOVE 1"]
    c.send(query("FETCH ALL c"))
    out = c.until_ready()
    assert data(out) == [["4", "40"], ["5", "50"]] and tags(out) == ["FETCH 2"]
    c.send(query("FETCH c"))
    out = c.until_ready()  # past the end: a description, no row
    assert kinds(out) == b"TCZ" and tags(out) == ["FETCH 0"]
    c.send(query("CLOSE c"))
    assert tags(c.until_ready()) == ["CLOSE CURSOR"]
    c.send(query("CLOSE ALL"))
    assert tags(c.until_ready()) == ["CLOSE CURSOR ALL"]
    c.send(query("COMMIT"))
    c.until_ready()


def test_cursor_errors_and_the_failed_block(server):
    c = connect(server)
    cursor_table(c)
    # Outside a block a DECLARE is a hold cursor (PostgreSQL says 25P01: a known difference).
    c.send(query("DECLARE c CURSOR FOR SELECT k FROM t"))
    out = c.until_ready()
    assert states(out) == [] and tags(out) == ["DECLARE CURSOR"] and status(out) == "I"
    c.send(query("CLOSE c"))
    assert tags(c.until_ready()) == ["CLOSE CURSOR"]
    c.send(query("FETCH c"))
    assert states(c.until_ready()) == ["34000"]
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c CURSOR WITH HOLD FOR SELECT k FROM t FOR UPDATE"))
    out = c.until_ready()
    assert states(out) == ["0A000"] and status(out) == "E"  # as PostgreSQL's
    for sql, state in [("FETCH c", "25P02"), ("DECLARE d CURSOR FOR SELECT k FROM t", "25P02"), ("CLOSE ALL", "25P02")]:
        c.send(query(sql))
        assert states(c.until_ready()) == [state], sql
    c.send(query("ROLLBACK"))
    assert status(c.until_ready()) == "I"
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c CURSOR FOR SELECT k FROM t"))
    c.until_ready()
    for sql, state in [("DECLARE c CURSOR FOR SELECT k FROM t", "42P03")]:
        c.send(query(sql))
        assert states(c.until_ready()) == [state], sql
    c.send(query("ROLLBACK"))
    c.until_ready()
    # A backward fetch on NO SCROLL, BINARY, an unknown name.
    for declare, sql, state in [
        ("DECLARE c NO SCROLL CURSOR FOR SELECT k FROM t", "FETCH PRIOR FROM c", "55000"),
        ("DECLARE c NO SCROLL CURSOR FOR SELECT k FROM t", "FETCH LAST FROM c", "55000"),
        ("DECLARE c BINARY CURSOR FOR SELECT k FROM t", "SELECT 1", "0A000"),
        ("DECLARE c SCROLL CURSOR FOR SELECT k FROM t FOR UPDATE", "SELECT 1", "0A000"),
        ("DECLARE c CURSOR FOR SELECT k FROM t", "CLOSE nothing", "34000"),
        ("DECLARE c CURSOR FOR SELECT k FROM t", "FETCH c INTO t", "42601"),
        ("DECLARE c CURSOR FOR SELECT k FROM nope", "SELECT 1", "42P01"),
    ]:
        c.send(query("BEGIN"))
        c.until_ready()
        c.send(query(declare))
        out = c.until_ready()
        if states(out):  # the DECLARE is the statement that is refused
            assert states(out) == [state], declare
        else:
            c.send(query(sql))
            assert states(c.until_ready()) == [state], sql
        c.send(query("ROLLBACK"))
        c.until_ready()


def test_a_cancel_request_and_the_statement_timeout_stop_a_fetch():
    """A FETCH runs the cursor's query: a cancel or the statement timeout stops
    it with 57014, as any statement."""
    if not server_binary():
        pytest.skip("BYTECASK_SQL_SERVER is not set: no server to test")
    directory = tempfile.mkdtemp(prefix="bytecask_sql_wire_")
    s = Server(directory, extra_args=["--statement-timeout-ms", "500"])
    try:
        c = Client(s)
        key = backend_key(c.start())
        slow_tables(c)
        # Inside a block, cancelled: the block fails, as for any statement.
        for sql in ("BEGIN", f"DECLARE c CURSOR FOR {SLOW}"):
            c.send(query(sql))
            assert states(c.until_ready()) == []
        c.send(query("FETCH ALL FROM c"))
        time.sleep(0.2)
        cancel(s, key)
        out = c.until_ready()
        assert states(out) == ["57014"] and status(out) == "E"
        assert [fields(b)["M"] for k, b in out if k == b"E"] == ["canceling statement due to user request"]
        c.send(query("ROLLBACK"))
        assert status(c.until_ready()) == "I"
        # A hold cursor outside a block, timed out: the session goes on.
        c.send(query(f"DECLARE h CURSOR WITH HOLD FOR {SLOW}"))
        assert states(c.until_ready()) == []
        started = time.monotonic()
        c.send(query("FETCH ALL FROM h"))
        out = c.until_ready()
        assert states(out) == ["57014"] and status(out) == "I"
        assert [fields(b)["M"] for k, b in out if k == b"E"] == ["canceling statement due to statement timeout"]
        assert 0.5 <= time.monotonic() - started < 10
        assert data(run(c, "SELECT k FROM a WHERE k = $1", ["7"])) == [["7"]]
    finally:
        s.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_every_fetch_direction_and_its_command_tag(server):
    c = connect(server)
    cursor_table(c)  # k = 1 .. 5
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c SCROLL CURSOR FOR SELECT k FROM t ORDER BY k"))
    c.until_ready()
    # (statement, rows, tag): recorded from PostgreSQL 16 and kept in step with it.
    for sql, rows, tag in [
        ("FETCH LAST FROM c", [["5"]], "FETCH 1"),
        ("FETCH PRIOR c", [["4"]], "FETCH 1"),
        ("FETCH BACKWARD 2 FROM c", [["3"], ["2"]], "FETCH 2"),
        ("FETCH NEXT IN c", [["3"]], "FETCH 1"),
        ("FETCH ABSOLUTE 1 FROM c", [["1"]], "FETCH 1"),
        ("FETCH PRIOR FROM c", [], "FETCH 0"),
        ("FETCH PRIOR FROM c", [], "FETCH 0"),
        ("FETCH RELATIVE 3 FROM c", [["3"]], "FETCH 1"),
        ("FETCH 0 FROM c", [["3"]], "FETCH 1"),
        ("FETCH FORWARD 9 FROM c", [["4"], ["5"]], "FETCH 2"),
        ("FETCH 0 FROM c", [], "FETCH 0"),
        ("FETCH BACKWARD ALL FROM c", [["5"], ["4"], ["3"], ["2"], ["1"]], "FETCH 5"),
        ("FETCH -2 FROM c", [], "FETCH 0"),
        ("FETCH ABSOLUTE -2 FROM c", [["4"]], "FETCH 1"),
        ("FETCH ABSOLUTE 9 FROM c", [], "FETCH 0"),
        ("FETCH FIRST FROM c", [["1"]], "FETCH 1"),
    ]:
        c.send(query(sql))
        out = c.until_ready()
        assert states(out) == [] and data(out) == rows and tags(out) == [tag], sql
        assert kinds(out) == (b"TCZ" if not rows else b"T" + b"D" * len(rows) + b"CZ"), sql
    for sql, tag in [
        ("MOVE LAST IN c", "MOVE 1"),
        ("MOVE BACKWARD ALL IN c", "MOVE 4"),
        ("MOVE FORWARD ALL IN c", "MOVE 5"),
        ("MOVE ABSOLUTE 0 IN c", "MOVE 0"),
        ("MOVE RELATIVE 0 IN c", "MOVE 0"),
        ("MOVE ABSOLUTE 2 IN c", "MOVE 1"),
        ("MOVE 0 IN c", "MOVE 1"),
    ]:
        c.send(query(sql))
        out = c.until_ready()
        assert kinds(out) == b"CZ" and tags(out) == [tag], sql
    # Extended protocol: a FETCH in any direction describes as the cursor's columns.
    c.send(parse("FETCH BACKWARD 2 FROM c", "b"), describe("S", "b"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ" and row_description(out[2][1]) == [("k", 23)]
    c.send(bind([], "b", "pb"), execute("pb"), SYNC)
    out = c.until_ready()
    assert data(out) == [["1"]] and tags(out) == ["FETCH 1"]  # from row 2: one row, then the start
    c.send(parse("FETCH ABSOLUTE -1 FROM c", "l"), bind([], "l", "pl"), describe("P", "pl"), execute("pl"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"12TDCZ" and data(out) == [["5"]]
    c.send(query("COMMIT"))
    c.until_ready()


def test_no_scroll_refusals_fail_the_block_with_55000(server):
    c = connect(server)
    cursor_table(c)
    for first, refused in [
        ("FETCH 2 FROM n", "FETCH PRIOR FROM n"),
        ("FETCH 2 FROM n", "FETCH 0 FROM n"),
        ("FETCH 2 FROM n", "FETCH ABSOLUTE 1 FROM n"),
        ("FETCH 2 FROM n", "MOVE BACKWARD 1 IN n"),
        ("FETCH 2 FROM n", "FETCH LAST FROM n"),
    ]:
        c.send(query("BEGIN"))
        c.until_ready()
        c.send(query("DECLARE n NO SCROLL CURSOR FOR SELECT k FROM t ORDER BY k"))
        c.until_ready()
        c.send(query(first))
        assert data(c.until_ready()) == [["1"], ["2"]]
        c.send(query(refused))
        out = c.until_ready()
        assert states(out) == ["55000"] and status(out) == "E", refused
        c.send(query("FETCH NEXT FROM n"))
        assert states(c.until_ready()) == ["25P02"]
        c.send(query("ROLLBACK"))
        c.until_ready()
    # Going forward is not refused, and a hold cursor keeps its declaration.
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE n NO SCROLL CURSOR WITH HOLD FOR SELECT k FROM t ORDER BY k"))
    c.until_ready()
    c.send(query("COMMIT"))
    c.until_ready()
    c.send(query("FETCH ABSOLUTE 4 FROM n"))
    assert data(c.until_ready()) == [["4"]]
    c.send(query("FETCH PRIOR FROM n"))
    assert states(c.until_ready()) == ["55000"]
    c.send(query("CLOSE n"))
    c.until_ready()


def test_a_cursor_does_not_see_later_writes_and_ends_with_its_transaction(server):
    c = connect(server)
    cursor_table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c CURSOR FOR SELECT k FROM t ORDER BY k"))
    c.until_ready()
    c.send(query("DELETE FROM t"))
    c.until_ready()
    c.send(query("INSERT INTO t VALUES (9, 90)"))
    c.until_ready()
    c.send(query("FETCH ALL c"))
    assert data(c.until_ready()) == [["1"], ["2"], ["3"], ["4"], ["5"]]
    c.send(query("COMMIT"))
    c.until_ready()
    c.send(query("FETCH c"))
    assert states(c.until_ready()) == ["34000"]
    # Another connection's cursor is its own: the same name is free.
    other = connect(server)
    other.send(query("BEGIN"))
    other.until_ready()
    other.send(query("DECLARE c CURSOR FOR SELECT k FROM t"))
    assert states(other.until_ready()) == []
    other.close()  # a disconnect with a cursor open rolls the block back
    again = connect(server)
    assert data(run(again, "SELECT k FROM t")) == [["9"]]


def test_extended_cursor_statements_across_syncs(server):
    c = connect(server)
    cursor_table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    # DECLARE with a parameter, through Parse, Bind and Execute.
    out = run(c, "DECLARE c CURSOR FOR SELECT k, v FROM t WHERE k > $1 ORDER BY k", ["1"])
    assert kinds(out) == b"12nCZ" and tags(out) == ["DECLARE CURSOR"]
    # Describing a FETCH before the cursor exists gives NoData; a portal on an existing one, its columns.
    c.send(parse("FETCH 2 FROM c", "f"), describe("S", "f"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tTZ"  # the cursor exists by now
    assert row_description(out[2][1]) == [("k", 23), ("v", 20)]
    c.send(bind([], "f", "p"), describe("P", "p"), execute("p"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"2TDDCZ" and data(out) == [["2", "20"], ["3", "30"]] and tags(out) == ["FETCH 2"]
    c.send(execute("p"), SYNC)  # the same portal again: the next two
    out = c.until_ready()
    assert data(out) == [["4", "40"], ["5", "50"]]
    c.send(parse("MOVE c", "m"), bind([], "m", "pm"), describe("P", "pm"), execute("pm"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"12nCZ" and tags(out) == ["MOVE 0"]
    # A portal may not take a cursor's name, and a cursor not a portal's.
    c.send(parse("SELECT 1", "s"), bind([], "s", "c"), SYNC)
    assert states(c.until_ready()) == ["42P03"]
    c.send(query("ROLLBACK"))
    c.until_ready()
    # Outside a block the one statement is autocommit: a DECLARE is a hold cursor.
    out = run(c, "DECLARE c CURSOR FOR SELECT k FROM t ORDER BY k")
    assert states(out) == [] and status(out) == "I"
    out = run(c, "FETCH 2 FROM c")
    assert states(out) == [] and data(out) == [["1"], ["2"]]
    assert states(run(c, "CLOSE c")) == []
    out = run(c, "FETCH c")
    assert states(out) == ["34000"]


def test_where_current_of_over_simple_and_extended_protocol(server):
    c = connect(server)
    cursor_table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c CURSOR FOR SELECT k FROM t ORDER BY k"))
    c.until_ready()
    # Never fetched: no row to change.
    c.send(query("UPDATE t SET v = 0 WHERE CURRENT OF c"))
    assert states(c.until_ready()) == ["24000"]
    c.send(query("ROLLBACK"))
    c.until_ready()
    c.send(query("BEGIN"))
    c.until_ready()
    c.send(query("DECLARE c SCROLL CURSOR FOR SELECT k FROM t ORDER BY k"))
    c.until_ready()
    c.send(query("FETCH 2 c"))
    c.until_ready()
    c.send(query("UPDATE t SET v = 99 WHERE CURRENT OF c"))
    out = c.until_ready()
    assert tags(out) == ["UPDATE 1"] and states(out) == []
    # Extended protocol: the cursor's key is no parameter of the statement.
    c.send(parse("DELETE FROM t WHERE CURRENT OF c", "d"), describe("S", "d"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tnZ"  # ParameterDescription with none, NoData
    c.send(bind([], "d", "pd"), execute("pd"), SYNC)
    out = c.until_ready()
    assert tags(out) == ["DELETE 1"]
    c.send(parse("UPDATE t SET v = $1 WHERE CURRENT OF c", "u"), describe("S", "u"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tnZ"
    c.send(bind(["7"], "u", "pu"), execute("pu"), SYNC)
    assert tags(c.until_ready()) == ["UPDATE 0"]  # the row is gone: its key finds nothing
    c.send(query("FETCH PRIOR c"))
    c.until_ready()
    c.send(query("UPDATE t SET v = 7 WHERE CURRENT OF c"))
    assert tags(c.until_ready()) == ["UPDATE 1"]
    c.send(query("SELECT k, v FROM t ORDER BY k"))
    assert data(c.until_ready()) == [["1", "7"], ["3", "30"], ["4", "40"], ["5", "50"]]
    # An unknown cursor is 34000, and the error fails the block.
    c.send(query("DELETE FROM t WHERE CURRENT OF nope"))
    assert states(c.until_ready()) == ["34000"]
    c.send(query("SELECT 1"))
    assert states(c.until_ready()) == ["25P02"]
    c.send(query("ROLLBACK"))
    c.until_ready()
    # Outside a block the cursor is a hold cursor, held from a previous transaction.
    assert states(run(c, "DECLARE h CURSOR FOR SELECT k FROM t ORDER BY k")) == []
    assert states(run(c, "FETCH h")) == []
    out = run(c, "DELETE FROM t WHERE CURRENT OF h")
    assert states(out) == ["24000"]


def test_hold_cursor_outlives_commit_and_dies_with_rollback(server):
    c = connect(server)
    cursor_table(c)

    def send(sql):
        c.send(query(sql))
        return c.until_ready()

    # WITH HOLD in a block survives its COMMIT and shows the view at DECLARE.
    send("BEGIN")
    send("DECLARE h CURSOR WITH HOLD FOR SELECT k FROM t ORDER BY k")
    send("INSERT INTO t VALUES (6, 60)")
    out = send("COMMIT")
    assert tags(out) == ["COMMIT"] and status(out) == "I"
    assert data(send("FETCH 2 FROM h")) == [["1"], ["2"]]
    assert states(send("INSERT INTO t VALUES (7, 70)")) == []
    assert data(send("FETCH ALL FROM h")) == [["3"], ["4"], ["5"]]
    # A cursor declared in a transaction that rolls back goes with it; the earlier hold cursor stays.
    send("BEGIN")
    send("DECLARE g CURSOR WITH HOLD FOR SELECT k FROM t ORDER BY k")
    send("ROLLBACK")
    assert states(send("FETCH g")) == ["34000"]
    send("BEGIN")
    assert states(send("FETCH h")) == [] and send("COMMIT")
    # It is a name in the session's namespace until closed.
    assert states(send("DECLARE h CURSOR FOR SELECT k FROM t")) == ["42P03"]
    assert tags(send("CLOSE h")) == ["CLOSE CURSOR"]
    assert states(send("FETCH h")) == ["34000"]
    # Another connection does not see it.
    other = connect(server)
    send("DECLARE mine CURSOR FOR SELECT k FROM t")
    other.send(query("FETCH mine"))
    assert states(other.until_ready()) == ["34000"]


def test_extended_cursor_in_a_failed_block(server):
    c = connect(server)
    cursor_table(c)
    c.send(query("BEGIN"))
    c.until_ready()
    assert states(run(c, "DECLARE c CURSOR FOR SELECT k FROM t")) == []
    assert states(run(c, "DECLARE c CURSOR FOR SELECT k FROM t")) == ["42P03"]
    out = run(c, "FETCH c")
    assert states(out) == ["25P02"] and status(out) == "E"
    out = run(c, "CLOSE c")
    assert states(out) == ["25P02"]
    c.send(query("ROLLBACK"))
    out = c.until_ready()
    assert status(out) == "I"
    out = run(c, "FETCH c")
    assert states(out) == ["34000"]


# ---------------------------------------------------------------- savepoints


def test_savepoints_over_simple_and_extended_query(server):
    """SAVEPOINT, ROLLBACK TO and RELEASE, as PostgreSQL 17 answers them, sent
    as simple Queries and as Parse/Bind/Execute (docs/transaction_design.md,
    *Savepoints*)."""
    c = connect(server)
    table(c)
    for via in ("simple", "extended"):
        def say(sql):
            if via == "simple":
                c.send(query(sql))
                return c.until_ready()
            return run(c, sql)

        # Outside a block: 25P01, an error.
        for sql, what in (("SAVEPOINT s", "SAVEPOINT"), ("RELEASE s", "RELEASE SAVEPOINT"),
                          ("ROLLBACK TO s", "ROLLBACK TO SAVEPOINT")):
            out = say(sql)
            assert states(out) == ["25P01"] and status(out) == "I", (via, sql)
            assert fields(next(b for k, b in out if k == b"E"))["M"] == what + " can only be used in transaction blocks"
        assert tags(say("BEGIN")) == ["BEGIN"]
        assert tags(say("INSERT INTO t VALUES (1, 1)")) == ["INSERT 0 1"]
        assert tags(say("SAVEPOINT a")) == ["SAVEPOINT"]
        assert tags(say("INSERT INTO t VALUES (2, 2)")) == ["INSERT 0 1"]
        out = say("ROLLBACK TO SAVEPOINT a")
        assert tags(out) == ["ROLLBACK"] and status(out) == "T"
        assert keys(c) == ["1"]
        # A failed statement fails the block; ROLLBACK TO the savepoint ends that.
        out = say("INSERT INTO t VALUES (1, 1)")
        assert states(out) == ["23505"] and status(out) == "E"
        for sql in ("SAVEPOINT b", "RELEASE a", "SELECT k FROM t"):
            assert states(say(sql)) == ["25P02"], (via, sql)
        out = say("ROLLBACK TO nope")
        assert states(out) == ["3B001"] and status(out) == "E"
        out = say('ROLLBACK WORK TO "a"')
        assert tags(out) == ["ROLLBACK"] and status(out) == "T"
        assert tags(say("SAVEPOINT b")) == ["SAVEPOINT"]
        assert tags(say("INSERT INTO t VALUES (3, 3)")) == ["INSERT 0 1"]
        assert tags(say("RELEASE SAVEPOINT b")) == ["RELEASE"]
        # An unknown name in a healthy block fails it, as in PostgreSQL.
        out = say("RELEASE b")
        assert states(out) == ["3B001"] and status(out) == "E"
        assert tags(say("ROLLBACK TO a")) == ["ROLLBACK"]
        assert tags(say("INSERT INTO t VALUES (2, 2)")) == ["INSERT 0 1"]
        out = say("COMMIT")
        assert tags(out) == ["COMMIT"] and status(out) == "I"
        assert keys(c) == ["1", "2"]
        c.send(query("DELETE FROM t"))
        c.until_ready()


def test_savepoints_and_the_transaction_modes(server):
    c = connect(server)
    table(c)

    def say(sql):
        c.send(query(sql))
        return c.until_ready()

    # Only a change of level is refused, as PostgreSQL's check_transaction_isolation does:
    # the block's own level (READ COMMITTED by default) is taken in a subtransaction
    # and after a query.
    say("BEGIN")
    say("SAVEPOINT s")
    assert tags(say("SET TRANSACTION ISOLATION LEVEL READ COMMITTED")) == ["SET"]
    out = say("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ")
    assert states(out) == ["25001"]
    assert fields(next(b for k, b in out if k == b"E"))["M"] == (
        "SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction")
    say("ROLLBACK")
    say("BEGIN ISOLATION LEVEL REPEATABLE READ")
    say("SAVEPOINT s")
    assert tags(say("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ")) == ["SET"]
    assert states(say("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED")) == ["25001"]
    say("ROLLBACK")
    say("BEGIN")
    say("SELECT 1")
    assert tags(say("SET TRANSACTION ISOLATION LEVEL READ COMMITTED")) == ["SET"]
    out = say("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED")
    assert states(out) == ["25001"]
    assert fields(next(b for k, b in out if k == b"E"))["M"] == (
        "SET TRANSACTION ISOLATION LEVEL must be called before any query")
    say("ROLLBACK")
    # READ WRITE inside a savepoint of a read-only block is 25001, as in PostgreSQL 17.
    say("BEGIN READ ONLY")
    say("SAVEPOINT s")
    out = say("SET TRANSACTION READ WRITE")
    assert states(out) == ["25001"]
    assert fields(next(b for k, b in out if k == b"E"))["M"] == (
        "cannot set transaction read-write mode inside a read-only transaction")
    say("ROLLBACK")
    # READ ONLY set after a savepoint is undone by ROLLBACK TO and by RELEASE.
    for end in ("ROLLBACK TO s", "RELEASE s"):
        say("BEGIN")
        say("SAVEPOINT s")
        assert tags(say("SET TRANSACTION READ ONLY")) == ["SET"]
        assert states(say("INSERT INTO t VALUES (1, 1)")) == ["25006"]
        say("ROLLBACK TO s")
        assert tags(say("SET TRANSACTION READ ONLY")) == ["SET"]
        assert states(say(end)) == []
        assert tags(say("INSERT INTO t VALUES (1, 1)")) == ["INSERT 0 1"], end
        say("ROLLBACK")


def test_rollback_to_closes_the_cursors_declared_after_the_savepoint(server):
    c = connect(server)
    cursor_table(c)

    def say(sql):
        c.send(query(sql))
        return c.until_ready()

    say("BEGIN")
    say("DECLARE before CURSOR FOR SELECT k FROM t ORDER BY k")
    say("DECLARE bad CURSOR FOR SELECT 1 / (k - k) FROM t")
    say("SAVEPOINT s")
    say("DECLARE after CURSOR FOR SELECT k FROM t ORDER BY k")
    assert data(say("FETCH before")) == [["1"]]
    out = say("FETCH bad")
    assert states(out) == ["22012"] and status(out) == "E"
    assert tags(say("ROLLBACK TO s")) == ["ROLLBACK"]
    assert data(say("FETCH before")) == [["2"]]  # where it stood
    assert states(say("FETCH after")) == ["34000"]  # an error: it fails the block again
    say("ROLLBACK TO s")
    out = say("FETCH bad")
    assert states(out) == ["55000"] and status(out) == "E"
    assert fields(next(b for k, b in out if k == b"E"))["M"] == 'portal "bad" cannot be run'
    say("ROLLBACK TO s")
    assert tags(say("CLOSE bad")) == ["CLOSE CURSOR"]
    assert states(say("FETCH bad")) == ["34000"]
    say("ROLLBACK")


# ------------------------------------------- plans bound again after a DDL


def q(c: Client, sql: str) -> list[tuple[bytes, bytes]]:
    c.send(query(sql))
    return c.until_ready()


def columns(messages) -> list[str]:
    return [name for name, _ in row_description(next(b for k, b in messages if k == b"T"))]


def test_a_simple_query_returns_the_new_column_and_a_prepared_statement_is_0a000(server):
    """docs/transaction_design.md, *A transaction binds only against its own
    catalog*: by text a statement returns its catalog's columns; a statement
    Parsed before a DDL that changed them, named or unnamed, described or
    not, and a bound portal are 0A000, as in PostgreSQL 17."""
    c = connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    c.send(parse("SELECT * FROM r", "named"), describe("S", "named"), SYNC)
    assert states(c.until_ready()) == []
    c.send(parse("SELECT * FROM r", "never"), SYNC)
    c.until_ready()
    c.send(parse("SELECT * FROM r"), bind([], portal="p"), FLUSH)
    c.send(SYNC)
    c.until_ready()
    other = connect(server)
    assert states(q(other, "ALTER TABLE r ADD COLUMN b INTEGER")) == []
    # By text: the new column, never 0A000.
    out = q(c, "SELECT * FROM r")
    assert states(out) == [] and columns(out) == ["k", "a", "b"]
    for name in ("named", "never"):
        c.send(bind([], name), execute(), SYNC)
        assert states(c.until_ready()) == ["0A000"]
        c.send(describe("S", name), SYNC)
        assert states(c.until_ready()) == ["0A000"]
    c.send(parse("SELECT * FROM r", "fresh"), bind([], "fresh"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and data(out) == [["1", "10", None]]


def test_parse_in_a_block_binds_against_the_block_s_catalog(server):
    """A Parse in a block fixes the columns of the block's pin, and a Bind
    after COMMIT, in autocommit, is 0A000 when they differ at the live
    catalog. A name created after the pin is 40001 at Parse, and fails the
    block; its retry runs."""
    c, other = connect(server), connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    q(c, "BEGIN")
    assert data(q(c, "SELECT k FROM r")) == [["1"]]  # pins the block's catalog
    assert states(q(other, "ALTER TABLE r ADD COLUMN b INTEGER DEFAULT 7")) == []
    c.send(parse("SELECT * FROM r", "in_block"), describe("S", "in_block"), bind([], "in_block"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and columns(out) == ["k", "a"] and data(out) == [["1", "10"]]
    out = q(c, "SELECT * FROM r")
    assert columns(out) == ["k", "a"]
    assert status(q(c, "COMMIT")) == "I"
    c.send(bind([], "in_block"), execute(), SYNC)
    assert states(c.until_ready()) == ["0A000"]
    # A name created after the block's pin.
    q(c, "BEGIN")
    q(c, "SELECT k FROM r")
    assert states(q(other, "CREATE TABLE z (k INTEGER PRIMARY KEY)")) == []
    c.send(parse("SELECT k FROM z", "z"), SYNC)
    out = c.until_ready()
    assert states(out) == ["40001"] and status(out) == "E"
    q(c, "ROLLBACK")
    q(c, "BEGIN")
    c.send(parse("SELECT k FROM z", "z"), bind([], "z"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "T"
    q(c, "COMMIT")


def test_a_portal_bound_in_a_block_before_a_ddl_runs_at_the_pin(server):
    c, other = connect(server), connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    q(c, "BEGIN")
    c.send(parse("SELECT * FROM r", "s"), bind([], "s", "p"), SYNC)
    assert states(c.until_ready()) == []
    assert states(q(other, "ALTER TABLE r ADD COLUMN b INTEGER")) == []
    assert states(q(other, "INSERT INTO r VALUES (2, 20, 2)")) == []
    c.send(describe("P", "p"), execute("p"), SYNC)
    out = c.until_ready()
    assert states(out) == [] and columns(out) == ["k", "a"] and data(out) == [["1", "10"]]
    q(c, "COMMIT")


def test_session_functions_are_the_startup_user_and_the_served_database_as_names(server):
    """CURRENT_USER and SESSION_USER are the startup packet's user, CURRENT_CATALOG
    the --database name, CURRENT_SCHEMA public; each a name (OID 19), as a NAME
    column is."""
    c = connect(server)
    sql = "SELECT CURRENT_USER, SESSION_USER, CURRENT_CATALOG, CURRENT_SCHEMA"
    c.send(parse(sql, "s"), describe("S", "s"), SYNC)
    assert row_description(c.until_ready()[2][1]) == [("current_user", 19), ("session_user", 19),
                                                      ("current_catalog", 19), ("current_schema", 19)]
    assert data(run(c, sql)) == [["wire", "wire", "proof", "public"]]
    q(c, "CREATE TABLE n (k NAME PRIMARY KEY, u NAME DEFAULT CURRENT_USER, i INTEGER)")
    q(c, "INSERT INTO n (k, i) VALUES ('x', 1)")
    # A $n compared with a NAME is typed name, and cut to 63 bytes.
    c.send(parse("SELECT u FROM n WHERE k = $1", "p"), describe("S", "p"), SYNC)
    assert struct.unpack("!hi", c.until_ready()[1][1]) == (1, 19)
    assert data(run(c, "SELECT u FROM n WHERE k = $1", ["x"])) == [["wire"]]
    long = "é" + "a" * 70  # 72 bytes: the cut keeps 63
    run(c, "INSERT INTO n (k, i) VALUES ($1, 2)", [long])
    assert data(run(c, "SELECT octet_length(k), i FROM n WHERE k = $1", [long])) == [["63", "2"]]
    # A session-function DEFAULT inside a BEGIN block is read per row.
    q(c, "BEGIN")
    q(c, "INSERT INTO n (k, i) VALUES ('y', 3)")
    assert data(run(c, "SELECT u FROM n WHERE k = $1", ["y"])) == [["wire"]]
    q(c, "COMMIT")
    assert data(run(c, "SELECT u, i FROM n WHERE k = $1", ["y"])) == [["wire", "3"]]


def test_a_parsed_statement_keeps_its_parameter_types(server):
    """$1 fixed as int4 at Parse: a Bind after the column became a DATE is
    42883, as PostgreSQL's revalidation reports it, while a Simple Query of
    the text binds afresh."""
    c = connect(server)
    q(c, "CREATE TABLE r (a INTEGER PRIMARY KEY)")
    c.send(parse("SELECT 1 FROM r WHERE a = $1", "s"), SYNC)
    c.until_ready()
    q(c, "DROP TABLE r")
    q(c, "CREATE TABLE r (a DATE PRIMARY KEY)")
    c.send(bind(["7"], "s"), execute(), SYNC)
    assert states(c.until_ready()) == ["42883"]
    c.send(describe("S", "s"), SYNC)
    assert states(c.until_ready()) == ["42883"]
    out = run(c, "SELECT 1 FROM r WHERE a = $1", ["2026-01-01"])
    assert states(out) == []


def test_an_implicit_block_binds_against_the_catalog_of_its_first_execute(server):
    c, other = connect(server), connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    # Two Executes before one Sync: PostgreSQL's implicit block, begun by the first.
    c.send(parse("SELECT k FROM r"), bind([]), execute(), FLUSH)
    assert kinds([c.message() for _ in range(4)]) == b"12DC"
    assert states(q(other, "ALTER TABLE r ADD COLUMN b INTEGER")) == []
    c.send(parse("SELECT * FROM r"), bind([]), describe("P"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and columns(out) == ["k", "a"] and data(out) == [["1", "10"]]
    out = q(c, "SELECT * FROM r")
    assert columns(out) == ["k", "a", "b"]


def test_a_ddl_parsed_in_a_block_and_never_executed_leaves_it_open(server):
    """docs/transaction_design.md, stage 0's tests: Parse of a DDL in a block
    is no error and the block stays open; a Parsed CREATE TABLE AS executed
    there runs in the block (stage 4), and ROLLBACK undoes it."""
    c = connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "BEGIN")
    q(c, "SELECT 1")
    c.send(parse("CREATE TABLE r2 (k INTEGER PRIMARY KEY)", "ddl"), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "T"
    c.send(parse("ALTER TABLE r ADD COLUMN b INTEGER", "alter"), parse("DROP TABLE r", "drop"), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "T"
    c.send(parse("CREATE TABLE r3 AS SELECT k FROM r", "ctas"), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "T"
    c.send(bind([], "ctas"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and fields_tag(out) == "SELECT 0" and status(out) == "T"
    q(c, "ROLLBACK")
    assert columns(q(c, "SELECT * FROM r")) == ["k", "a"]
    assert states(q(c, "SELECT k FROM r3")) == ["42P01"]


def test_ddl_in_a_begin_block_runs_in_the_block_and_rollback_undoes_it(server):
    """docs/transaction_design.md, stage 3: DDL in a BEGIN block, by Simple
    Query and by Execute, is a statement of the block. Its statements see
    the new table; ROLLBACK undoes all of it, and a statement Parsed on it
    is 42P01 at its next Bind, as PostgreSQL's revalidation reports it."""
    c, other = connect(server), connect(server)
    for create in ("simple", "extended"):
        q(c, "BEGIN")
        ddl = "CREATE TABLE in_block (k INTEGER PRIMARY KEY, v TEXT)"
        if create == "simple":
            out = q(c, ddl)
        else:
            c.send(parse(ddl), bind([]), execute(), SYNC)
            out = c.until_ready()
        assert states(out) == [] and fields_tag(out) == "CREATE TABLE" and status(out) == "T", create
        c.send(parse("INSERT INTO in_block VALUES ($1, $2)", "ins"), bind(["1", "a"], "ins"), execute(), SYNC)
        assert states(c.until_ready()) == []
        c.send(parse("SELECT k, v FROM in_block WHERE k = $1", "sel"), describe("S", "sel"),
               bind(["1"], "sel"), describe("P"), execute(), SYNC)
        out = c.until_ready()
        assert states(out) == [] and columns(out) == ["k", "v"] and data(out) == [["1", "a"]]
        assert states(q(other, "SELECT k FROM in_block")) == ["42P01"]  # uncommitted: not seen
        assert status(q(c, "ROLLBACK")) == "I"
        for name in ("sel", "ins"):
            c.send(bind(["1"] if name == "sel" else ["2", "b"], name), execute(), SYNC)
            assert states(c.until_ready()) == ["42P01"], name
        assert states(q(c, "SELECT k FROM in_block")) == ["42P01"]
        c.send(close_statement("ins"), close_statement("sel"), SYNC)
        c.until_ready()
    # COMMIT publishes the table with its rows.
    q(c, "BEGIN")
    q(c, "CREATE TABLE in_block (k INTEGER PRIMARY KEY, v TEXT)")
    q(c, "INSERT INTO in_block VALUES (1, 'a')")
    q(c, "CREATE INDEX in_block_v ON in_block (v)")
    assert status(q(c, "COMMIT")) == "I"
    assert data(q(other, "SELECT k, v FROM in_block WHERE v = 'a'")) == [["1", "a"]]
    # A DDL in a failed block is 25P02, and in a READ ONLY block 25006, as PostgreSQL's.
    q(c, "BEGIN")
    out = q(c, "CREATE TABLE in_block (k INTEGER PRIMARY KEY)")
    assert states(out) == ["42P07"] and status(out) == "E"
    out = q(c, "DROP TABLE in_block")
    assert states(out) == ["25P02"] and status(out) == "E"
    # Only a syntax error comes before 25P02, not what parse analysis refuses (42P16).
    assert states(q(c, "CREATE TABLE w (a INTEGER PRIMARY KEY, PRIMARY KEY (a))")) == ["25P02"]
    assert states(q(c, "CREATE TABL w (a INTEGER)")) == ["42601"]
    q(c, "ROLLBACK")
    q(c, "BEGIN READ ONLY")
    out = q(c, "CREATE TABLE read_only (k INTEGER PRIMARY KEY)")
    assert states(out) == ["25006"] and status(out) == "E"
    q(c, "ROLLBACK")
    assert states(q(c, "SELECT * FROM read_only")) == ["42P01"]


def close_statement(name: str) -> bytes:
    return message(b"C", b"S" + cstr(name))


def close_portal(name: str) -> bytes:
    return message(b"C", b"P" + cstr(name))


def test_a_statement_parsed_after_the_block_s_ddl_keeps_its_columns_after_rollback(server):
    """A Parse after ADD COLUMN in a block fixes the block's columns. After
    ROLLBACK, its Bind is 0A000 in autocommit, in a new block at the same
    catalog, and in one pinned after another session's ADD COLUMN, and again
    on retry: as PostgreSQL 17."""
    c, other = connect(server), connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    q(c, "BEGIN")
    q(c, "ALTER TABLE r ADD COLUMN b INTEGER DEFAULT 3")
    c.send(parse("SELECT * FROM r", "s"), describe("S", "s"), bind([], "s"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and columns(out) == ["k", "a", "b"] and data(out) == [["1", "10", "3"]]
    q(c, "ROLLBACK")
    for _ in range(2):
        c.send(bind([], "s"), execute(), SYNC)
        assert states(c.until_ready()) == ["0A000"]
    for _ in range(2):
        q(c, "BEGIN")
        q(c, "SELECT 1")
        c.send(bind([], "s"), execute(), SYNC)
        out = c.until_ready()
        assert states(out) == ["0A000"] and status(out) == "E"
        q(c, "ROLLBACK")
    assert states(q(other, "ALTER TABLE r ADD COLUMN c INTEGER")) == []
    for _ in range(2):
        q(c, "BEGIN")
        q(c, "SELECT 1")
        c.send(bind([], "s"), execute(), SYNC)
        out = c.until_ready()
        assert states(out) == ["0A000"] and status(out) == "E"
        q(c, "ROLLBACK")


def test_a_ddl_on_what_a_portal_of_the_block_reads_is_55006(server):
    """As PostgreSQL's CheckTableNotInUse: a SELECT portal bound in the block,
    named, executed or not, holds what its plan reads until it is closed, the block
    ends, or a ROLLBACK TO drops it. A Simple Query takes the unnamed portal
    for itself, so the unnamed one never refuses a Simple Query's DDL."""
    c = connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    in_use = "because it is being used by active queries in this session"
    for executed in (False, True):
        for ddl, message_ in (("DROP TABLE r", 'cannot DROP TABLE "r"'),
                              ("CREATE INDEX r_a ON r (a)", 'cannot CREATE INDEX "r"'),
                              ("ALTER TABLE r ADD COLUMN b INTEGER", 'cannot ALTER TABLE "r"')):
            q(c, "BEGIN")
            c.send(parse("SELECT * FROM r", "s"), bind([], "s", "p"), *([execute("p")] if executed else []), SYNC)
            assert states(c.until_ready()) == []
            out = q(c, ddl)
            assert states(out) == ["55006"] and status(out) == "E", ddl
            assert fields(next(b for k, b in out if k == b"E"))["M"] == f"{message_} {in_use}"
            q(c, "ROLLBACK")
            c.send(close_statement("s"), SYNC)
            c.until_ready()
    # A closed portal, the unnamed one before a Simple Query, a portal bound
    # since a savepoint rolled back to, and one on another table hold nothing.
    q(c, "CREATE TABLE u (k INTEGER PRIMARY KEY)")
    q(c, "BEGIN")
    c.send(parse("SELECT * FROM r", "s"), bind([], "s", "p"), close_portal("p"), bind([], "s"), execute(), SYNC)
    assert states(c.until_ready()) == []
    q(c, "SAVEPOINT a")
    c.send(parse("SELECT k FROM u", "su"), bind([], "su", "pu"), bind([], "s", "p2"), SYNC)
    assert states(c.until_ready()) == []
    assert states(q(c, "CREATE INDEX u_k ON u (k)")) == ["55006"]
    q(c, "ROLLBACK TO a")
    c.send(execute("p2"), SYNC)
    assert states(c.until_ready()) == ["34000"]  # gone with the savepoint, as in PostgreSQL
    q(c, "ROLLBACK TO a")
    c.send(bind([], "su", "pu"), SYNC)
    assert states(c.until_ready()) == []
    out = q(c, "ALTER TABLE r ADD COLUMN b INTEGER")
    assert states(out) == [] and status(out) == "T"
    # A RELEASE hands the savepoint's portals to the block: a ROLLBACK TO a
    # later savepoint keeps pu, which still holds u.
    q(c, "RELEASE a")
    q(c, "SAVEPOINT b")
    q(c, "ROLLBACK TO b")
    assert states(q(c, "DROP TABLE u")) == ["55006"]
    q(c, "ROLLBACK")
    assert columns(q(c, "SELECT * FROM r")) == ["k", "a"]


def test_a_ddl_on_the_table_of_a_dml_portal_of_the_block_runs(server):
    """As PostgreSQL 17: only a SELECT portal opens its relations at Bind
    (PortalStart). An INSERT, UPDATE or DELETE portal, with RETURNING or
    not, executed or not, holds nothing, so a DROP TABLE, ALTER TABLE or
    CREATE INDEX on its table runs."""
    c = connect(server)
    q(c, "CREATE TABLE d (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO d VALUES (1, 10)")
    for dml in ("INSERT INTO d VALUES (2, 20)", "INSERT INTO d VALUES (2, 20) RETURNING k",
                "UPDATE d SET a = 11 WHERE k = 1", "DELETE FROM d WHERE k = 1"):
        for executed in (False, True):
            for ddl in ("DROP TABLE d", "ALTER TABLE d ADD COLUMN b INTEGER", "CREATE INDEX d_a ON d (a)"):
                q(c, "BEGIN")
                c.send(parse(dml, "s"), bind([], "s", "p"), *([execute("p")] if executed else []), SYNC)
                assert states(c.until_ready()) == [], dml
                out = q(c, ddl)
                assert states(out) == [] and status(out) == "T", (dml, executed, ddl)
                q(c, "ROLLBACK")
                c.send(close_statement("s"), SYNC)
                c.until_ready()
    assert data(q(c, "SELECT k, a FROM d")) == [["1", "10"]]


def test_savepoints_across_ddl_in_a_block(server):
    """ROLLBACK TO undoes the DDL since the savepoint and keeps the rows
    before it; RELEASE keeps it; a failed DDL is reopened by ROLLBACK TO."""
    c = connect(server)
    q(c, "BEGIN")
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    q(c, "INSERT INTO r VALUES (1, 10)")
    q(c, "SAVEPOINT s")
    q(c, "ALTER TABLE r ADD COLUMN b INTEGER DEFAULT 5")
    q(c, "CREATE TABLE u (k INTEGER PRIMARY KEY)")
    assert columns(q(c, "SELECT * FROM r")) == ["k", "a", "b"]
    out = q(c, "ROLLBACK TO s")
    assert states(out) == [] and status(out) == "T"
    out = q(c, "SELECT * FROM r")
    assert columns(out) == ["k", "a"] and data(out) == [["1", "10"]]
    assert states(q(c, "SELECT k FROM u")) == ["42P01"]
    q(c, "ROLLBACK TO s")
    out = q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY)")
    assert states(out) == ["42P07"] and status(out) == "E"
    assert states(q(c, "ROLLBACK TO s")) == []
    q(c, "SAVEPOINT t")
    q(c, "CREATE INDEX r_a ON r (a)")
    q(c, "RELEASE t")
    assert status(q(c, "COMMIT")) == "I"
    out = q(c, "SELECT k, a FROM r WHERE a = 10")
    assert data(out) == [["1", "10"]]
    assert states(q(c, "DROP INDEX r_a")) == []


def test_a_block_that_ran_ddl_is_40001_once_another_ddl_commits(server):
    """docs/transaction_design.md, *Catalog snapshots and isolation levels
    with DDL*: a known difference from PostgreSQL, which runs it. The
    block's next statement is 40001; ROLLBACK TO a savepoint before its DDL
    reopens it, and nothing of the DDL is written."""
    c, other = connect(server), connect(server)
    q(c, "BEGIN")
    q(c, "SAVEPOINT s")
    q(c, "CREATE TABLE x (k INTEGER PRIMARY KEY)")
    assert states(q(other, "CREATE TABLE y (k INTEGER PRIMARY KEY)")) == []
    out = q(c, "SELECT 1")
    assert states(out) == ["40001"] and status(out) == "E"
    out = q(c, "ROLLBACK TO s")
    assert states(out) == [] and status(out) == "T"
    assert data(q(c, "SELECT 1")) == [["1"]]
    assert fields_tag(q(c, "COMMIT")) == "COMMIT"
    assert states(q(c, "SELECT k FROM x")) == ["42P01"]


def test_a_ddl_is_bound_at_execute_and_a_query_at_parse(server):
    """docs/transaction_design.md, stage 1, as PostgreSQL 17: Parse and
    Describe of a DDL naming a table that does not exist are no error, its
    Execute is 42P01, and once the table exists the same statement runs.
    A query's binder error is still Parse's."""
    c = connect(server)
    c.send(parse("CREATE INDEX x_a ON x (a)", "ix"), describe("S", "ix"), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    c.send(bind([], "ix"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == ["42P01"] and status(out) == "I"
    q(c, "CREATE TABLE x (k INTEGER PRIMARY KEY, a INTEGER)")
    c.send(bind([], "ix"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "I"
    c.send(parse("SELECT a FROM nosuch", "sel"), SYNC)
    assert states(c.until_ready()) == ["42P01"]


def test_a_parsed_int4_parameter_assigned_to_a_column_now_bigint_is_range_checked_as_int4(server):
    """PostgreSQL's Bind reads the value as the type fixed at Parse: 22003
    for 5000000000 sent for an int4 $n, whatever column it is assigned to."""
    c = connect(server)
    q(c, "CREATE TABLE r (k INTEGER PRIMARY KEY, a INTEGER)")
    c.send(parse("INSERT INTO r VALUES ($1, $2)", "ins"), parse("UPDATE r SET a = $1 WHERE k = 1", "upd"), SYNC)
    c.until_ready()
    q(c, "DROP TABLE r")
    q(c, "CREATE TABLE r (k BIGINT PRIMARY KEY, a BIGINT)")
    q(c, "INSERT INTO r VALUES (1, 1)")
    c.send(bind(["5000000000", "7"], "ins"), execute(), SYNC)
    assert states(c.until_ready()) == ["22003"]
    c.send(bind(["5000000000"], "upd"), execute(), SYNC)
    assert states(c.until_ready()) == ["22003"]
    q(c, "BEGIN")
    c.send(bind(["6000000000", "7"], "ins"), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == ["22003"] and status(out) == "E"
    q(c, "ROLLBACK")
    assert data(q(c, "SELECT k, a FROM r")) == [["1", "1"]]


# ------------------------------------------------- SERIALIZABLE and the modes


def say_both(c: Client, sql: str) -> list[list[tuple[bytes, bytes]]]:
    """One statement as a simple Query, then as an extended one."""
    c.send(query(sql))
    simple = c.until_ready()
    c.send(parse(sql), bind([]), describe("P"), execute(), SYNC)
    return [simple, c.until_ready()]


def error_of(messages) -> dict[str, str]:
    return next(fields(b) for k, b in messages if k == b"E")


def test_serializable_isolation_and_the_transaction_defaults_over_simple_and_extended_query(server):
    """docs/transaction_design.md, *Choosing the level: SET, SHOW, BEGIN and
    the embedded API*: each SQLSTATE and value as PostgreSQL 17 gives it."""
    c = connect(server)
    table(c)
    # SHOW of each parameter outside a block: the defaults.
    for name, value in (("transaction_isolation", "read committed"), ("transaction_read_only", "off"),
                        ("transaction_deferrable", "off"), ("default_transaction_isolation", "read committed"),
                        ("default_transaction_read_only", "off"), ("default_transaction_deferrable", "off")):
        for out in say_both(c, f"SHOW {name}"):
            assert data(out) == [[value]] and fields_tag(out) == "SHOW", (name, out)
            assert columns(out) == [name]
    assert data(q(c, "SHOW TRANSACTION ISOLATION LEVEL")) == [["read committed"]]
    for out in say_both(c, "SET default_transaction_isolation = 'serializable'"):
        assert fields_tag(out) == "SET" and states(out) == []
    assert data(q(c, "SHOW transaction_isolation")) == [["serializable"]]
    # Values: any case, an identifier for one word, a quoted string for two.
    for sql in ("SET default_transaction_isolation TO 'REPEATABLE READ'", "SET default_transaction_isolation = serializable",
                'SET default_transaction_isolation = "read committed"'):
        assert states(q(c, sql)) == [], sql
    out = q(c, "SET default_transaction_isolation = repeatable read")
    assert states(out) == ["42601"] and error_of(out)["M"] == 'syntax error at or near "read"'
    for out in say_both(c, "SET default_transaction_isolation = 'bogus'"):
        e = error_of(out)
        assert e["C"] == "22023" and e["M"] == 'invalid value for parameter "default_transaction_isolation": "bogus"'
        assert e["H"] == "Available values: serializable, repeatable read, read committed, read uncommitted."
    out = q(c, "SET default_transaction_read_only = bogus")
    assert error_of(out)["M"] == 'parameter "default_transaction_read_only" requires a Boolean value'
    out = q(c, "SET default_transaction_isolation = 'serializable', 'read committed'")
    assert error_of(out)["M"] == "SET default_transaction_isolation takes only one argument"
    # SET TO DEFAULT and RESET restore PostgreSQL's defaults.
    assert fields_tag(q(c, "SET default_transaction_isolation TO DEFAULT")) == "SET"
    assert data(q(c, "SHOW default_transaction_isolation")) == [["read committed"]]
    q(c, "SET default_transaction_isolation = 'serializable'")
    assert fields_tag(q(c, "RESET default_transaction_isolation")) == "RESET"
    assert data(q(c, "SHOW default_transaction_isolation")) == [["read committed"]]
    # transaction_isolation outside a block is read and changes nothing; it cannot be reset.
    assert fields_tag(q(c, "SET transaction_isolation = 'serializable'")) == "SET"
    assert data(q(c, "SHOW transaction_isolation")) == [["read committed"]]
    for sql in ("RESET transaction_isolation", "SET transaction_isolation TO DEFAULT"):
        out = q(c, sql)
        assert states(out) == ["0A000"] and error_of(out)["M"] == 'parameter "transaction_isolation" cannot be reset'
    q(c, "BEGIN")
    out = q(c, "RESET transaction_isolation")
    assert states(out) == ["0A000"] and status(out) == "E"  # it aborts the block, as any error does
    q(c, "ROLLBACK")
    # SET LOCAL outside a block warns and changes nothing.
    for out in say_both(c, "SET LOCAL default_transaction_isolation = 'serializable'"):
        assert states(out, b"N") == ["25P01"] and fields_tag(out) == "SET"
    assert data(q(c, "SHOW default_transaction_isolation")) == [["read committed"]]
    # SET SESSION CHARACTERISTICS sets the defaults; RESET ALL resets them.
    q(c, "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY, DEFERRABLE")
    assert [data(q(c, f"SHOW {n}")) for n in ("transaction_isolation", "transaction_read_only", "transaction_deferrable")] == [
        [["serializable"]], [["on"]], [["on"]]]
    for out in say_both(c, "INSERT INTO t VALUES (1, 1)"):
        assert states(out) == ["25006"] and error_of(out)["M"] == "cannot execute INSERT in a read-only transaction"
    q(c, "BEGIN")
    assert data(q(c, "SHOW transaction_read_only")) == [["on"]]
    out = q(c, "INSERT INTO t VALUES (1, 1)")
    assert states(out) == ["25006"] and status(out) == "E"
    q(c, "ROLLBACK")
    assert fields_tag(q(c, "RESET ALL")) == "RESET"
    assert [data(q(c, f"SHOW {n}")) for n in ("default_transaction_isolation", "default_transaction_read_only",
                                               "default_transaction_deferrable")] == [[["read committed"]], [["off"]], [["off"]]]
    assert states(q(c, "INSERT INTO t VALUES (1, 1)")) == []


def test_the_transaction_defaults_are_transactional(server):
    """A SET of a default in a block is undone by ROLLBACK, by the ROLLBACK
    of a failed block and by ROLLBACK TO, and kept by COMMIT and RELEASE;
    SET LOCAL lasts to the block's end; the open block's level is unchanged."""
    c = connect(server)

    def default():
        return data(q(c, "SHOW default_transaction_isolation"))[0][0]

    q(c, "BEGIN")
    q(c, "SET default_transaction_isolation = 'serializable'")
    assert data(q(c, "SHOW transaction_isolation")) == [["read committed"]]
    assert default() == "serializable"
    q(c, "ROLLBACK")
    assert default() == "read committed"
    for end in ("ROLLBACK", "COMMIT"):
        q(c, "BEGIN")
        q(c, "SET default_transaction_isolation = 'serializable'")
        assert status(q(c, "SELECT 1/0")) == "E"
        q(c, end)
        assert default() == "read committed", end
    q(c, "BEGIN")
    q(c, "SAVEPOINT s")
    q(c, "SET default_transaction_isolation = 'serializable'")
    q(c, "ROLLBACK TO s")
    assert default() == "read committed"
    q(c, "SET default_transaction_isolation = 'repeatable read'")
    q(c, "RELEASE s")
    assert fields_tag(q(c, "COMMIT")) == "COMMIT"
    assert default() == "repeatable read"
    q(c, "BEGIN")
    q(c, "SET LOCAL default_transaction_isolation = 'serializable'")
    assert default() == "serializable"
    q(c, "COMMIT")
    assert default() == "repeatable read"
    q(c, "BEGIN")
    q(c, "SET LOCAL default_transaction_isolation = 'serializable'")
    q(c, "SET default_transaction_isolation = 'read uncommitted'")  # a SET overrides the block's SET LOCAL
    q(c, "COMMIT")
    assert default() == "read uncommitted"
    # The default reaches the next block.
    q(c, "SET default_transaction_isolation = 'serializable'")
    q(c, "BEGIN")
    assert data(q(c, "SHOW transaction_isolation")) == [["serializable"]]
    q(c, "COMMIT")


def test_the_25001_rules_of_isolation_read_write_and_deferrable(server):
    c = connect(server)
    table(c)

    def refused(out, message):
        e = error_of(out)
        assert (e["C"], e["M"]) == ("25001", message), out

    # DEFERRABLE: in a savepoint first, after a query even unchanged.
    q(c, "BEGIN")
    q(c, "SAVEPOINT s")
    refused(q(c, "SET TRANSACTION NOT DEFERRABLE"), "SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction")
    q(c, "ROLLBACK")
    q(c, "BEGIN")
    q(c, "SELECT 1")
    refused(q(c, "SET TRANSACTION NOT DEFERRABLE"), "SET TRANSACTION [NOT] DEFERRABLE must be called before any query")
    q(c, "ROLLBACK")
    q(c, "BEGIN")
    assert fields_tag(q(c, "SET TRANSACTION DEFERRABLE")) == "SET"
    assert data(q(c, "SHOW transaction_deferrable")) == [["on"]]
    q(c, "ROLLBACK")
    # The level through transaction_isolation, under the same rules as SET TRANSACTION.
    q(c, "BEGIN")
    assert fields_tag(q(c, "SET transaction_isolation = 'serializable'")) == "SET"
    assert data(q(c, "SHOW transaction_isolation")) == [["serializable"]]
    q(c, "SELECT 1")
    assert fields_tag(q(c, "SET transaction_isolation = 'serializable'")) == "SET"  # unchanged: taken
    refused(q(c, "SET transaction_isolation = 'read committed'"), "SET TRANSACTION ISOLATION LEVEL must be called before any query")
    q(c, "ROLLBACK")
    # BEGIN in a block warns, then sets its modes as SET TRANSACTION does.
    q(c, "BEGIN ISOLATION LEVEL SERIALIZABLE")
    q(c, "SELECT 1")
    out = q(c, "BEGIN ISOLATION LEVEL READ COMMITTED")
    assert states(out, b"N") == ["25001"] and states(out) == ["25001"] and status(out) == "E"
    q(c, "ROLLBACK")
    # BEGIN after a statement of an extended block (the design's
    # "SELECT 1; BEGIN ISOLATION LEVEL SERIALIZABLE" in one Query, which the
    # server refuses as several statements): 25001, and the block is rolled back.
    c.send(parse("SELECT 1"), bind([]), execute(), parse("BEGIN ISOLATION LEVEL SERIALIZABLE"), bind([]), execute(), SYNC)
    out = c.until_ready()
    refused(out, "SET TRANSACTION ISOLATION LEVEL must be called before any query")
    assert status(out) == "I"
    c.send(parse("SELECT 1"), bind([]), execute(), parse("BEGIN DEFERRABLE"), bind([]), execute(), SYNC)
    out = c.until_ready()
    refused(out, "SET TRANSACTION [NOT] DEFERRABLE must be called before any query")
    assert status(out) == "I"
    c.send(parse("SELECT 1"), bind([]), execute(), parse("BEGIN READ ONLY"), bind([]), execute(), SYNC)
    out = c.until_ready()
    assert states(out) == [] and status(out) == "T"
    assert data(q(c, "SHOW transaction_read_only")) == [["on"]]
    assert states(q(c, "INSERT INTO t VALUES (9, 9)")) == ["25006"]
    q(c, "ROLLBACK")


def two_clients(server) -> tuple[Client, Client]:
    a, b = connect(server), connect(server)
    q(a, "CREATE TABLE doc (id INTEGER PRIMARY KEY, on_call INTEGER)")
    q(a, "INSERT INTO doc VALUES (1, 1), (2, 1)")
    return a, b


def test_write_skew_between_serializable_blocks_is_40001_at_the_second_commit(server):
    """PostgreSQL 17 gives the same 40001, message and hint (its DETAIL, a
    reason code, is not sent)."""
    a, b = two_clients(server)
    for begin in ("BEGIN ISOLATION LEVEL SERIALIZABLE", "START TRANSACTION ISOLATION LEVEL SERIALIZABLE",
                  "BEGIN; SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"):
        q(a, "UPDATE doc SET on_call = 1")
        for c in (a, b):
            for sql in begin.split("; "):
                assert states(q(c, sql)) == [], sql
            assert data(q(c, "SELECT count(*) FROM doc WHERE on_call = 1")) == [["2"]]
        q(a, "UPDATE doc SET on_call = 0 WHERE id = 1")
        q(b, "UPDATE doc SET on_call = 0 WHERE id = 2")
        assert fields_tag(q(a, "COMMIT")) == "COMMIT"
        out = q(b, "COMMIT")
        e = error_of(out)
        assert e["C"] == "40001" and status(out) == "I", begin
        assert e["M"] == "could not serialize access due to read/write dependencies among transactions"
        assert e["H"] == "The transaction might succeed if retried."
        assert data(q(a, "SELECT id, on_call FROM doc ORDER BY id")) == [["1", "0"], ["2", "1"]]
    # The same at the default level commits both: snapshot isolation.
    q(a, "UPDATE doc SET on_call = 1")
    for c in (a, b):
        q(c, "BEGIN ISOLATION LEVEL REPEATABLE READ")
        q(c, "SELECT count(*) FROM doc WHERE on_call = 1")
    q(a, "UPDATE doc SET on_call = 0 WHERE id = 1")
    q(b, "UPDATE doc SET on_call = 0 WHERE id = 2")
    assert fields_tag(q(a, "COMMIT")) == "COMMIT" and fields_tag(q(b, "COMMIT")) == "COMMIT"


def test_a_phantom_in_a_read_range_is_40001_and_disjoint_ranges_commit(server):
    """The empty-range phantom is 40001, as in PostgreSQL 17. Disjoint
    ranges commit here; PostgreSQL 17, whose predicate locks on a small
    table cover pages, may fail one (a documented difference)."""
    a, b = connect(server), connect(server)
    q(a, "CREATE TABLE r (k INTEGER PRIMARY KEY)")
    for (lo_a, lo_b), (ins_a, ins_b), outcome in (((1, 1), (5, 6), ["COMMIT", "40001"]),
                                                  ((100, 200), (205, 105), ["COMMIT", "40001"]),
                                                  ((300, 400), (301, 401), ["COMMIT", "COMMIT"])):
        for c, lo in ((a, lo_a), (b, lo_b)):
            q(c, "BEGIN ISOLATION LEVEL SERIALIZABLE")
            assert data(q(c, f"SELECT k FROM r WHERE k BETWEEN {lo} AND {lo + 10}")) == []
        q(a, f"INSERT INTO r VALUES ({ins_a})")
        q(b, f"INSERT INTO r VALUES ({ins_b})")
        got = []
        for c in (a, b):
            out = q(c, "COMMIT")
            got.append(states(out)[0] if states(out) else fields_tag(out))
        assert got == outcome, (lo_a, lo_b)


def test_a_read_in_a_read_only_savepoint_is_recorded_and_fails_the_commit(server):
    """docs/transaction_design.md, stage 2: READ ONLY set inside a savepoint
    is the session's; the transaction keeps recording, so the read made
    under it still fails the COMMIT on skew."""
    a, b = two_clients(server)
    q(a, "BEGIN ISOLATION LEVEL SERIALIZABLE")
    q(a, "SAVEPOINT s")
    q(a, "SET TRANSACTION READ ONLY")
    assert data(q(a, "SELECT count(*) FROM doc WHERE on_call = 1")) == [["2"]]
    assert states(q(a, "UPDATE doc SET on_call = 0 WHERE id = 1")) == ["25006"]
    q(a, "ROLLBACK TO s")
    q(a, "RELEASE s")
    assert data(q(a, "SHOW transaction_read_only")) == [["off"]]
    q(b, "UPDATE doc SET on_call = 0 WHERE id = 2")  # a concurrent commit changes what a read
    assert states(q(a, "UPDATE doc SET on_call = 0 WHERE id = 1")) == []
    out = q(a, "COMMIT")
    assert states(out) == ["40001"] and status(out) == "I"
    # A top-level READ ONLY reaches the transaction: it records nothing and commits.
    q(a, "BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY")
    assert data(q(a, "SELECT count(*) FROM doc WHERE on_call = 1")) == [["1"]]
    q(b, "UPDATE doc SET on_call = 1")
    assert fields_tag(q(a, "COMMIT")) == "COMMIT"


def test_serializable_and_the_modes_match_postgresql(server):
    """The SET, SHOW, RESET and BEGIN forms above, statement by statement,
    against PostgreSQL 17: the SQLSTATE, tag, rows and block status."""
    import psycopg  # noqa: F401
    from psycopg import pq

    dsn = os.environ.get("BYTECASK_SQL_ORACLE")
    if not dsn:
        pytest.skip("BYTECASK_SQL_ORACLE is not set: no PostgreSQL to compare with")
    script = [
        "SHOW transaction_isolation", "SHOW default_transaction_read_only", "SET default_transaction_isolation = 'serializable'",
        "SHOW transaction_isolation", "SET default_transaction_isolation = repeatable read",
        "SET default_transaction_isolation = 'bogus'", "SET default_transaction_isolation TO DEFAULT",
        "SET default_transaction_read_only = 'of'", "SET default_transaction_read_only = 'o'",
        "SET default_transaction_read_only = 1", "SHOW default_transaction_read_only", "RESET default_transaction_read_only",
        "SET LOCAL default_transaction_isolation = 'serializable'", "SET transaction_isolation = 'serializable'",
        "RESET transaction_isolation", "SET transaction_deferrable = on",
        "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE, DEFERRABLE",
        "SHOW transaction_deferrable", "BEGIN", "SHOW transaction_isolation", "SET transaction_isolation = 'read committed'",
        "SHOW transaction_isolation", "SET default_transaction_isolation = 'repeatable read'", "SAVEPOINT s",
        "SET TRANSACTION DEFERRABLE", "ROLLBACK TO s", "SHOW default_transaction_isolation", "SELECT 1",
        "SET TRANSACTION NOT DEFERRABLE", "ROLLBACK", "SHOW default_transaction_isolation",
        "BEGIN ISOLATION LEVEL SERIALIZABLE", "SET LOCAL default_transaction_isolation = 'read committed'",
        "SHOW default_transaction_isolation", "SELECT 1", "BEGIN ISOLATION LEVEL SERIALIZABLE", "SET transaction_read_only = on",
        "SHOW transaction_read_only", "SET transaction_read_only = off", "ROLLBACK", "SHOW default_transaction_isolation",
        "SET default_transaction_read_only = on", "BEGIN READ WRITE", "SHOW transaction_read_only", "ROLLBACK", "RESET ALL",
        "SHOW default_transaction_deferrable",
        # A mode written twice is checked value by value, in order.
        "BEGIN", "SELECT 1", "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE ISOLATION LEVEL READ COMMITTED", "ROLLBACK",
        "BEGIN", "SELECT 1", "SET TRANSACTION READ ONLY, READ WRITE", "ROLLBACK",
        "BEGIN", "SAVEPOINT s", "SET TRANSACTION READ ONLY READ WRITE", "ROLLBACK",
        "BEGIN ISOLATION LEVEL SERIALIZABLE ISOLATION LEVEL READ COMMITTED", "SHOW transaction_isolation", "ROLLBACK",
        # A comma only between two modes: anywhere else 42601, and nothing changes.
        "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY,", "SHOW default_transaction_read_only",
        "SET SESSION CHARACTERISTICS AS TRANSACTION , READ ONLY", "SET TRANSACTION READ ONLY,",
        "START TRANSACTION READ ONLY,,READ WRITE", "BEGIN ISOLATION LEVEL SERIALIZABLE,", "BEGIN WORK, READ ONLY",
        "BEGIN ISOLATION LEVEL", "BEGIN NOT", "BEGIN read foo", "SHOW transaction_isolation",
        # A parameter's name in any case, a quoted one too; errors name it as written.
        'SHOW "Transaction_Isolation"', 'SET "Default_Transaction_Isolation" = serializable',
        "SHOW default_transaction_isolation", 'SET "Default_Transaction_Isolation" = bogus',
        'RESET "Default_Transaction_Isolation"', 'RESET "Transaction_Isolation"', 'SET "Transaction_Read_Only" = maybe',
        'SHOW "CLIENT_ENCODING"',
    ]

    def outcome(conn, sql):
        r = conn.exec_(sql.encode())
        st = r.error_field(pq.DiagnosticField.SQLSTATE)
        rows = [[r.get_value(i, j) for j in range(r.nfields)] for i in range(r.ntuples)]
        names = [r.fname(j) for j in range(r.nfields)]
        message = r.error_field(pq.DiagnosticField.MESSAGE_PRIMARY)
        return (st.decode() if st else (r.command_status or b"").decode(), message, names, rows, conn.transaction_status)

    pg = pq.PGconn.connect(dsn.encode())
    ours = pq.PGconn.connect(server.dsn().encode())
    try:
        for sql in script:
            assert outcome(ours, sql) == outcome(pg, sql), sql
    finally:
        pg.exec_(b"ROLLBACK")
        pg.exec_(b"RESET ALL")
        pg.finish()
        ours.finish()


def test_a_set_first_in_an_extended_query_block_runs_in_that_block(server):
    """A SET of a default, SET SESSION CHARACTERISTICS or SET TRANSACTION
    first of several statements before Sync runs in the implicit block, as
    PostgreSQL 17's does: the block runs with the defaults it found, and a
    failure of the block undoes the SET."""
    c = connect(server)
    table(c)

    def pipeline(*sqls):
        msgs = []
        for sql in sqls:
            msgs += [parse(sql), bind([]), execute()]
        c.send(*msgs, SYNC)
        return c.until_ready()

    def show(name):
        return data(q(c, f"SHOW {name}"))[0][0]

    q(c, "SET default_transaction_read_only = on")
    out = pipeline("SET default_transaction_read_only = off", "INSERT INTO t VALUES (1, 1)")
    assert states(out) == ["25006"] and status(out) == "I"
    assert show("default_transaction_read_only") == "on"  # undone with the block
    for first in ("RESET default_transaction_read_only", "RESET ALL"):
        assert states(pipeline(first, "INSERT INTO t VALUES (1, 1)")) == ["25006"], first
        assert show("default_transaction_read_only") == "on", first
    q(c, "RESET ALL")
    assert data(q(c, "SELECT count(*) FROM t")) == [["0"]]
    # The block keeps the level it began with; the SET lasts once it commits.
    out = pipeline("SET default_transaction_isolation = 'serializable'", "SHOW transaction_isolation")
    assert states(out) == [] and data(out) == [["read committed"]]
    assert show("default_transaction_isolation") == "serializable"
    q(c, "RESET ALL")
    assert states(pipeline("SET default_transaction_isolation = 'serializable'", "SELECT 1/0")) == ["22012"]
    assert show("default_transaction_isolation") == "read committed"
    out = pipeline("SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY", "INSERT INTO t VALUES (2, 2)")
    assert states(out) == [] and show("default_transaction_read_only") == "on"
    # BEGIN after it makes the block explicit with the modes it began with.
    out = pipeline("SET default_transaction_read_only = off", "BEGIN", "SELECT 1")
    assert states(out) == [] and status(out) == "T"
    assert show("transaction_read_only") == "on"
    q(c, "ROLLBACK")
    assert show("default_transaction_read_only") == "on"
    q(c, "RESET ALL")
    # SET TRANSACTION and SET transaction_read_only apply to the block they start.
    for first in ("SET TRANSACTION READ ONLY", "SET transaction_read_only = on"):
        out = pipeline(first, "INSERT INTO t VALUES (3, 3)")
        assert states(out) == ["25006"], first
    # SET LOCAL lasts to the block's end.
    out = pipeline("SET LOCAL default_transaction_read_only = on", "SHOW default_transaction_read_only")
    assert data(out) == [["on"]] and show("default_transaction_read_only") == "off"
    # Alone before Sync, a SET runs by itself, as before.
    assert states(pipeline("SET default_transaction_read_only = on")) == []
    assert show("default_transaction_read_only") == "on"
    q(c, "RESET ALL")
    assert data(q(c, "SELECT k FROM t ORDER BY k")) == [["2"]]


def test_jsonb_columns_and_parameters_are_oid_3802_in_text_format(server):
    # docs/jsonb_design.md, *Wire OIDs and formats for jsonb and json*: a
    # jsonb column and a parameter typed jsonb are OID 3802; values travel
    # as jsonb_out's text, a parameter read as jsonb_in reads it.
    c = connect(server)
    c.send(query("CREATE TABLE wj (k INTEGER PRIMARY KEY, j jsonb)"))
    c.until_ready()
    c.send(parse("INSERT INTO wj (k, j) VALUES ($1, $2)", "i"), describe("S", "i"), SYNC)
    out = c.until_ready()
    assert kinds(out) == b"1tnZ"
    n, *oids = struct.unpack("!hii", out[1][1])
    assert (n, oids) == (2, [23, 3802])
    c.send(bind(["1", '{"b": 1,  "a": [1, 2.50]}'], statement="i"), execute(), SYNC)
    assert kinds(c.until_ready()) == b"2CZ"
    c.send(query("SELECT k, j, j::text FROM wj"))
    out = c.until_ready()
    assert row_description(out[0][1]) == [("k", 23), ("j", 3802), ("j", 25)]
    assert data_row(out[1][1]) == [b"1", b'{"a": [1, 2.50], "b": 1}', b'{"a": [1, 2.50], "b": 1}']
    c.send(parse("SELECT $1::jsonb", "s"), describe("S", "s"), SYNC)
    out = c.until_ready()
    assert struct.unpack("!hi", out[1][1]) == (1, 3802)
    assert row_description(out[2][1]) == [("jsonb", 3802)]


def test_a_jsonb_input_error_sends_its_detail_and_context(server):
    # 22P02 with PostgreSQL 17's DETAIL and CONTEXT as D and W, from a
    # literal read at prepare and from a parameter read at execution.
    c = connect(server)
    c.send(query("CREATE TABLE wj_err (k INTEGER PRIMARY KEY, j jsonb)"))
    c.until_ready()
    c.send(query("INSERT INTO wj_err VALUES (1, '{\"a\":1 \"b\":2}')"))
    f = fields(c.until_ready()[0][1])
    assert f["C"] == "22P02" and f["M"] == "invalid input syntax for type json", f
    assert f["D"] == 'Expected "," or "}", but found ""b"".', f
    assert f["W"] == 'JSON data, line 1: {"a":1 "b"...', f
    c.send(parse("INSERT INTO wj_err VALUES ($1, $2)"), bind(["1", '"\\u0000"']), execute(), SYNC)
    f = fields(next(b for k, b in c.until_ready() if k == b"E"))
    assert f["C"] == "22P05" and f["M"] == "unsupported Unicode escape sequence", f
    assert f["D"] == "\\u0000 cannot be converted to text.", f
    assert f["W"] == 'JSON data, line 1: "\\u0000...', f


def data_row(body: bytes) -> list[bytes | None]:
    """A DataRow's values, None for NULL."""
    (n,) = struct.unpack("!h", body[:2])
    body, out = body[2:], []
    for _ in range(n):
        (length,) = struct.unpack("!i", body[:4])
        body = body[4:]
        if length < 0:
            out.append(None)
            continue
        out.append(body[:length])
        body = body[length:]
    return out


def test_a_text_parameter_is_read_by_postgresql_17s_input_functions(server):
    """#904 stage C0: int2in/int4in/int8in's prefixes, underscores and
    range message, float4in's, and a DECIMAL's infinity, each as PG 17."""
    c = connect(server)
    c.send(query("CREATE TABLE c0_wire (k INTEGER PRIMARY KEY, s SMALLINT, b BIGINT, r REAL, d NUMERIC(3,0))"))
    assert states(c.until_ready()) == []
    insert = "INSERT INTO c0_wire VALUES ($1, $2, $3, $4, $5)"
    assert states(run(c, insert, ["0x1F", " 1_000 ", "-0b11", "0.1", "1"])) == []
    assert data(run(c, "SELECT k, s, b FROM c0_wire WHERE k = $1", ["0o37"])) == [["31", "1000", "-3"]]

    def error(params):
        f = fields(next(b for k, b in run(c, insert, params) if k == b"E"))
        return f["C"], f["M"], f.get("D")

    assert error(["2", "0x8000", "1", "1", "1"]) == ("22003", 'value "0x8000" is out of range for type smallint', None)
    assert error(["3000000000", "1", "1", "1", "1"]) == (
        "22003", 'value "3000000000" is out of range for type integer', None)
    assert error(["1_", "1", "1", "1", "1"]) == ("22P02", 'invalid input syntax for type integer: "1_"', None)
    assert error(["2", "1", "1", "1e39", "1"]) == ("22003", '"1e39" is out of range for type real', None)
    assert error(["2", "1", "1", " 1e39 ", "1"]) == ("22003", '"1e39" is out of range for type real', None)
    assert error(["2", "1", "1", "1e39x", "1"]) == ("22003", '"1e39" is out of range for type real', None)
    assert error(["2", "1", "1", "x", "1"]) == ("22P02", 'invalid input syntax for type real: "x"', None)
    assert error(["2", "1", "1", "1", "-Infinity"]) == (
        "22003", "numeric field overflow", "A field with precision 3, scale 0 cannot hold an infinite value.")
