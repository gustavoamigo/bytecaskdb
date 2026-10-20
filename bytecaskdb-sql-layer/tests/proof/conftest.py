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

        self.schema = "proof_" + uuid.uuid4().hex[:12]
        # No server-side prepared statements: a table dropped and re-created
        # with other types would fail a cached plan (0A000).
        self.conn = psycopg.connect(dsn, autocommit=True, prepare_threshold=None)
        with self.conn.cursor() as cur:
            cur.execute(f"CREATE SCHEMA {self.schema}")
            cur.execute(f"SET search_path TO {self.schema}")

    def close(self) -> None:
        with self.conn.cursor() as cur:
            cur.execute(f"DROP SCHEMA {self.schema} CASCADE")
        self.conn.close()


@pytest.fixture
def dsn() -> str:
    return oracle_dsn()
