import pytest
import bytecaskdb
import bytecaskdb._bytecaskdb as bc
from bytecaskdb._bytecaskdb import DB, WriteOptions


def test_db_closed_is_value_error():
    assert issubclass(bc.DbClosed, ValueError)
    assert bytecaskdb.DbClosed is bc.DbClosed


def test_close_is_idempotent_and_later_operations_raise(tmp_path):
    db = DB.open(str(tmp_path / "db"))
    opts = WriteOptions()
    opts.sync = False
    db.put(b"k", b"v", opts)
    snap = db.snapshot()
    db.close()
    db.close()
    with pytest.raises(bc.DbClosed):
        db.get(b"k")
    with pytest.raises(bc.DbClosed):
        db.put(b"k", b"v")
    with pytest.raises(bc.DbClosed):
        db.stats()
    assert db.is_degraded is False
    # A snapshot taken before the close stays readable.
    assert snap.get(b"k") == b"v"


def test_close_releases_the_lock_and_keeps_unsynced_writes(tmp_path):
    path = str(tmp_path / "db")
    db = DB.open(path)
    opts = WriteOptions()
    opts.sync = False
    db.put(b"k", b"v", opts)
    db.close()
    # The first handle is still alive; the reopen succeeds only if the
    # close released the directory lock.
    reopened = DB.open(path)
    assert reopened.get(b"k") == b"v"
    reopened.close()


def test_context_manager_closes(tmp_path):
    path = str(tmp_path / "db")
    with DB.open(path) as db:
        db.put(b"k", b"v")
    with pytest.raises(bc.DbClosed):
        db.get(b"k")
    with bytecaskdb.DB.open(path) as wrapped:
        assert wrapped[b"k"] == b"v"
    with pytest.raises(bc.DbClosed):
        wrapped.get(b"k")
