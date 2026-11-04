#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Prints pg_stat_bytecask as name,value lines (docs/server_design.md, *Reading
the counters*), for run-gate.sh. The standard library only: the gate's machines
have python3 and need not have psql.

Usage: read_counters.py PORT USER DATABASE
"""

from __future__ import annotations

import socket
import struct
import sys


def message(kind: bytes, body: bytes) -> bytes:
    return kind + struct.pack("!i", len(body) + 4) + body


def read_exact(s: socket.socket, n: int) -> bytes:
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise OSError("the server closed the connection")
        out += chunk
    return out


def read_message(s: socket.socket) -> tuple[bytes, bytes]:
    kind = read_exact(s, 1)
    (length,) = struct.unpack("!i", read_exact(s, 4))
    return kind, read_exact(s, length - 4)


def until_ready(s: socket.socket) -> list[tuple[bytes, bytes]]:
    messages = []
    while True:
        kind, body = read_message(s)
        if kind == b"E":
            raise OSError(body.decode(errors="replace"))
        if kind == b"Z":
            return messages
        messages.append((kind, body))


def row(body: bytes) -> list[str]:
    (n,) = struct.unpack("!h", body[:2])
    at, out = 2, []
    for _ in range(n):
        (length,) = struct.unpack("!i", body[at:at + 4])
        at += 4
        out.append(body[at:at + length].decode())
        at += length
    return out


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    port, user, database = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
            params = f"user\0{user}\0database\0{database}\0\0".encode()
            s.sendall(struct.pack("!ii", len(params) + 8, 3 << 16) + params)
            until_ready(s)
            s.sendall(message(b"Q", b"select * from pg_stat_bytecask\0"))
            rows = [row(body) for kind, body in until_ready(s) if kind == b"D"]
            s.sendall(message(b"X", b""))
    except OSError as e:
        print(f"read_counters.py: {e}", file=sys.stderr)
        return 1
    for name, value in rows:
        print(f"{name},{value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
