# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
"""Degraded mode through the binding (#291).

A write the disk refuses degrades the engine. RLIMIT_FSIZE set to the size of
the active data file gives a real refusal with no fault-injection build: the
file is zero-filled ahead of the write cursor, so writes inside it succeed and
the one that has to extend it fails with EFBIG.
"""

import contextlib
import os
import resource
import signal

import pytest

import bytecaskdb as bc
from bytecaskdb import ext


@contextlib.contextmanager
def file_size_limit(limit):
    """Caps the size of any file this process writes. SIGXFSZ is ignored, so
    the write past the limit fails with EFBIG rather than killing the test."""
    saved = resource.getrlimit(resource.RLIMIT_FSIZE)
    handler = signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (limit, saved[1]))
    try:
        yield
    finally:
        resource.setrlimit(resource.RLIMIT_FSIZE, saved)
        signal.signal(signal.SIGXFSZ, handler)


def degrade(db, path):
    """Writes past the end of the active data file under a limit at its size.
    Returns the error the failed write raised."""
    size = max(
        os.path.getsize(path / name) for name in os.listdir(path)
        if name.endswith(".data"))
    with file_size_limit(size):
        with pytest.raises(OSError) as failed:
            for i in range(size // (1 << 20) + 2):
                db.put(b"big%d" % i, b"x" * (1 << 20))
    return failed.value


@pytest.fixture
def degraded(tmp_path):
    path = tmp_path / "db"
    db = ext.DB.open(str(path))
    db.put(b"a", b"1")
    db.put(b"b", b"2")
    error = degrade(db, path)
    yield db, error
    if db.is_degraded:
        db.resume()
    db.close()


def test_a_refused_write_degrades_the_engine(degraded):
    db, error = degraded
    # The write that failed reports the system call's error.
    assert not isinstance(error, bc.ByteCaskError)
    assert db.is_degraded
    assert "resume()" in db.degraded_reason


def test_writes_on_a_degraded_engine_raise_a_bytecask_error(degraded):
    db, _ = degraded
    with pytest.raises(bc.DbDegraded) as put:
        db.put(b"c", b"3")
    assert isinstance(put.value, bc.ByteCaskError)
    assert isinstance(put.value, RuntimeError)
    with pytest.raises(bc.ByteCaskError):
        db.delete(b"a")
    with pytest.raises(bc.ByteCaskError):
        with db.batch() as batch:
            batch.put(b"c", b"3")
    with pytest.raises(bc.ByteCaskError):
        with db.transaction() as txn:
            txn.put(b"c", b"3")


def test_reads_go_on_while_degraded(degraded):
    db, _ = degraded
    assert db.get(b"a") == b"1"
    assert b"b" in db
    assert [k for k in db.keys() if not k.startswith(b"big")] == [b"a", b"b"]
    with db.snapshot() as snap:
        assert snap.get(b"b") == b"2"


def test_resume_clears_the_degraded_state(degraded):
    db, _ = degraded
    db.resume()
    assert not db.is_degraded
    assert db.degraded_reason == ""
    db.put(b"c", b"3")
    assert db.get(b"c") == b"3"
    assert db.get(b"a") == b"1"


def test_the_engine_errors_share_one_base():
    for error in (bc.DbDegraded, bc.DbFollowerMode, bc.DbClosed,
                  bc.ConflictError):
        assert issubclass(error, bc.ByteCaskError)
    assert bc.ByteCaskError is bc._bytecaskdb.ByteCaskError
    assert not issubclass(OSError, bc.ByteCaskError)
