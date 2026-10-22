# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""sql_layer_server as a test runs it (docs/server_design.md, *Tests*).

BYTECASK_SQL_SERVER names the binary. Unset, the tests that need it skip,
and the proof machine runs without its wire target.
"""

from __future__ import annotations

import os
import select
import signal
import socket
import subprocess
import threading

SERVER_ENV = "BYTECASK_SQL_SERVER"


def server_binary() -> str | None:
    return os.environ.get(SERVER_ENV) or None


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """A server on a directory of its own, on a free loopback port."""

    def __init__(self, directory: str, database: str | None = "proof", max_connections: int | None = None):
        """`database` None leaves the name to the server's default."""
        binary = server_binary()
        assert binary, f"{SERVER_ENV} is not set"
        self.directory = directory
        self.database = database
        self.port = free_port()
        args = [binary, "--dir", directory, "--listen", f"127.0.0.1:{self.port}"]
        if database is not None:
            args += ["--database", database]
        if max_connections is not None:
            args += ["--max-connections", str(max_connections)]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.output = []
        # Ready once it prints so; a server that exits first fails here.
        while True:
            ready, _, _ = select.select([self.proc.stdout], [], [], 60)
            assert ready, "sql_layer_server did not start within 60 s"
            line = self.proc.stdout.readline()
            assert line, f"sql_layer_server exited: {''.join(self.output)}"
            self.output.append(line)
            if "ready on" in line:
                break
        # Drained from here on, so its output never fills the pipe and blocks it.
        self._drain = threading.Thread(target=lambda: self.output.extend(self.proc.stdout), daemon=True)
        self._drain.start()

    def dsn(self, user: str = "proof") -> str:
        return (f"host=127.0.0.1 port={self.port} user={user} dbname={self.database} "
                "sslmode=disable gssencmode=disable")

    def stop(self) -> int:
        """SIGTERM, as the run script stops it; its exit status."""
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        status = self.proc.wait(timeout=60)
        self._drain.join(timeout=10)
        return status
