# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""The proof tests' fixtures (#515).

BYTECASK_SQL_ORACLE names the PostgreSQL to compare with, as a libpq
connection string ("host=localhost user=postgres password=... dbname=postgres").
Unset, the tests that need it skip. Each test gets a database directory and
a PostgreSQL schema of its own.

Hypothesis profiles: "ci" is derandomized (a fixed seed) with a bounded
budget; "dev", the default, explores. Pick one with HYPOTHESIS_PROFILE.
"""

from __future__ import annotations

import os
import uuid

import pytest
from hypothesis import HealthCheck, settings

settings.register_profile(
    "ci",
    derandomize=True,
    database=None,
    max_examples=40,
    stateful_step_count=40,
    deadline=None,
    suppress_health_check=[HealthCheck.too_slow],
)
settings.register_profile(
    "dev",
    max_examples=100,
    stateful_step_count=50,
    deadline=None,
    suppress_health_check=[HealthCheck.too_slow],
)
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "dev"))

ORACLE_ENV = "BYTECASK_SQL_ORACLE"


def oracle_dsn() -> str:
    dsn = os.environ.get(ORACLE_ENV)
    if not dsn:
        pytest.skip(f"{ORACLE_ENV} is not set: no PostgreSQL to compare with")
    return dsn


class Oracle:
    """A PostgreSQL connection in autocommit, confined to a fresh schema."""

    def __init__(self, dsn: str):
        import psycopg

        import oracle as o

        self.schema = "proof_" + uuid.uuid4().hex[:12]
        # No server-side prepared statements: a table dropped and re-created
        # with other types would fail a cached plan (0A000).
        self.conn = o.jsonb_as_text(psycopg.connect(dsn, autocommit=True, prepare_threshold=None))
        with self.conn.cursor() as cur:
            cur.execute(f"CREATE SCHEMA {self.schema}")
            cur.execute(f"SET search_path TO {self.schema}")

    def close(self) -> None:
        with self.conn.cursor() as cur:
            cur.execute(f"DROP SCHEMA {self.schema} CASCADE")
            # A schema of the machine's own (test_sql_machine's schema rule).
            cur.execute(f"DROP SCHEMA IF EXISTS {self.schema}_s CASCADE")
        self.conn.close()


@pytest.fixture
def dsn() -> str:
    return oracle_dsn()


C_DATABASE = "bytecask_proof_c"


@pytest.fixture
def c_dsn(dsn: str) -> str:
    """The oracle in a database whose default collation is C. jsonb compares
    its strings and keys under the database's default collation, which
    COLLATE cannot change, and the layer's is bytewise: a test of jsonb's
    order needs PostgreSQL in a C database (the postgres image's default is
    en_US.utf8). The database is made from template0 when the oracle's own
    is not C, and kept for later runs."""
    import psycopg
    from psycopg.conninfo import make_conninfo

    with psycopg.connect(dsn, autocommit=True) as conn:
        # The default collation's order itself: datcollate reads C.UTF-8 in
        # a database whose provider is ICU.
        (bytewise,) = conn.execute("""SELECT '"B"'::jsonb < '"a"'::jsonb AND 'Z' < 'a' AND 'a' < 'ä'""").fetchone()
        if bytewise:
            return dsn
        if conn.execute("SELECT 1 FROM pg_database WHERE datname = %s", [C_DATABASE]).fetchone() is None:
            try:
                conn.execute(f"CREATE DATABASE {C_DATABASE} TEMPLATE template0 ENCODING 'UTF8' LOCALE_PROVIDER libc LOCALE 'C'")
            except psycopg.errors.DuplicateDatabase:
                pass
    return make_conninfo(dsn, dbname=C_DATABASE)
