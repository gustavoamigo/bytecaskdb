"""Tests that verify every former segfault vector now raises a Python exception.

Covers:
  1. Consumed snapshot — every method raises RuntimeError after WritePlan(snap)
  2. Consumed WritePlan — every method raises RuntimeError after apply_batch
  3. Iterator outliving snapshot — keep_alive prevents GC of parent
  4. Iterator outliving DB — keep_alive prevents GC of parent
  5. Pythonic wrapper transaction — deferred commit keeps reads live
"""

import gc

import pytest
import bytecaskdb._bytecaskdb as bc

import sys
from bytecaskdb import ext


# ── Helpers ──────────────────────────────────────────────────────────────────

def _consume_snapshot(db):
    """Return a snapshot that has been consumed by a WritePlan."""
    db.put(b"k", b"v")
    snap = db.snapshot()
    bc.WritePlan(snap)  # consumes the snapshot
    return snap


def _consume_plan(db):
    """Return a WritePlan that has been consumed by apply_batch."""
    plan = bc.WritePlan()
    plan.put(b"x", b"y")
    db.apply_batch(plan)
    return plan


def _consume_snapshot_plan(db):
    """Return a snapshot-backed WritePlan that has been consumed."""
    db.put(b"k", b"v")
    snap = db.snapshot()
    plan = bc.WritePlan(snap)
    plan.put(b"k", b"v2")
    db.apply_batch(plan)
    return plan


# ═══════════════════════════════════════════════════════════════════════════════
# 1. Consumed Snapshot
# ═══════════════════════════════════════════════════════════════════════════════

class TestConsumedSnapshot:
    """After WritePlan(snap) moves the C++ snapshot, every method must raise."""

    def test_get_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.get(b"k")

    def test_contains_key_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.contains_key(b"k")

    def test_iter_from_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.iter_from()

    def test_keys_from_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.keys_from()

    def test_riter_from_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.riter_from()

    def test_rkeys_from_raises(self, db):
        snap = _consume_snapshot(db)
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.rkeys_from()

    def test_context_manager_raises(self, db):
        snap = _consume_snapshot(db)
        # __enter__ should not crash — it just returns self
        # __exit__ is a no-op.  The key safety is that methods inside raise.
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            snap.get(b"k")

    def test_double_consume_raises(self, db):
        db.put(b"k", b"v")
        snap = db.snapshot()
        bc.WritePlan(snap)  # first consume
        with pytest.raises(RuntimeError, match="Snapshot already consumed"):
            bc.WritePlan(snap)  # second consume


# ═══════════════════════════════════════════════════════════════════════════════
# 2. Consumed WritePlan
# ═══════════════════════════════════════════════════════════════════════════════

class TestConsumedWritePlan:
    """After apply_batch(plan) consumes the plan, every method must raise."""

    def test_put_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.put(b"a", b"b")

    def test_del_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.del_(b"a")

    def test_del_range_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.del_range(b"a", b"z")

    def test_ensure_present_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.ensure_present(b"a")

    def test_ensure_absent_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.ensure_absent(b"a")

    def test_ensure_unchanged_raises(self, db):
        plan = _consume_snapshot_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.ensure_unchanged(b"a")

    def test_ensure_range_unchanged_raises(self, db):
        plan = _consume_snapshot_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            plan.ensure_range_unchanged(b"a", b"z")

    def test_has_snapshot_raises(self, db):
        plan = _consume_plan(db)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            _ = plan.has_snapshot

    def test_double_apply_raises(self, db):
        plan = bc.WritePlan()
        plan.put(b"a", b"b")
        db.apply_batch(plan)
        with pytest.raises(RuntimeError, match="WritePlan already consumed"):
            db.apply_batch(plan)


# ═══════════════════════════════════════════════════════════════════════════════
# 3. Iterator outliving snapshot (keep_alive)
# ═══════════════════════════════════════════════════════════════════════════════

class TestSnapshotIteratorKeepAlive:
    """Iterators created from a snapshot must survive deletion of the
    Python snapshot reference.  nb::keep_alive prevents GC of the parent."""

    def test_iter_survives(self, populated_db):
        snap = populated_db.snapshot()
        it = snap.iter_from()
        del snap
        gc.collect()
        items = list(it)
        assert len(items) == 5

    def test_keys_survives(self, populated_db):
        snap = populated_db.snapshot()
        it = snap.keys_from()
        del snap
        gc.collect()
        keys = list(it)
        assert len(keys) == 5

    def test_riter_survives(self, populated_db):
        snap = populated_db.snapshot()
        it = snap.riter_from()
        del snap
        gc.collect()
        items = list(it)
        assert len(items) == 5

    def test_rkeys_survives(self, populated_db):
        snap = populated_db.snapshot()
        it = snap.rkeys_from()
        del snap
        gc.collect()
        keys = list(it)
        assert len(keys) == 5


# ═══════════════════════════════════════════════════════════════════════════════
# 4. Iterator outliving DB (keep_alive)
# ═══════════════════════════════════════════════════════════════════════════════

class TestDBIteratorKeepAlive:
    """Iterators created from a DB must survive deletion of the Python
    DB reference.  nb::keep_alive prevents GC of the parent."""

    @staticmethod
    def _make_db(tmp_path):
        db = bc.DB.open(str(tmp_path / "keepalive_db"))
        opts = bc.WriteOptions()
        opts.sync = False
        for i in range(5):
            db.put(f"k{i:03d}".encode(), b"v", opts)
        return db

    def test_iter_survives(self, tmp_path):
        db = self._make_db(tmp_path)
        it = db.iter_from()
        del db
        gc.collect()
        items = list(it)
        assert len(items) == 5

    def test_keys_survives(self, tmp_path):
        db = self._make_db(tmp_path)
        it = db.keys_from()
        del db
        gc.collect()
        keys = list(it)
        assert len(keys) == 5

    def test_riter_survives(self, tmp_path):
        db = self._make_db(tmp_path)
        it = db.riter_from()
        del db
        gc.collect()
        items = list(it)
        assert len(items) == 5

    def test_rkeys_survives(self, tmp_path):
        db = self._make_db(tmp_path)
        it = db.rkeys_from()
        del db
        gc.collect()
        keys = list(it)
        assert len(keys) == 5


# ═══════════════════════════════════════════════════════════════════════════════
# 5. Pythonic wrapper transaction safety (bytecaskdb_ext)
# ═══════════════════════════════════════════════════════════════════════════════

class TestExtTransactionSafety:
    """The deferred-commit _Transaction must keep reads live throughout
    the block and apply staged writes at commit."""

    def test_reads_live_throughout(self, tmp_path):
        db = ext.DB.open(str(tmp_path / "ext_txn"))
        db[b"stock"] = b"10"

        with db.transaction() as txn:
            # Read at start
            assert txn[b"stock"] == b"10"
            # Stage a write
            txn[b"stock"] = b"9"
            # Read again — sees staged write (RYOW)
            assert txn[b"stock"] == b"9"

        assert db[b"stock"] == b"9"

    def test_commit_applies_writes(self, tmp_path):
        db = ext.DB.open(str(tmp_path / "ext_txn2"))
        db[b"a"] = b"1"
        db[b"b"] = b"2"
        db[b"c"] = b"3"

        with db.transaction() as txn:
            txn[b"a"] = b"10"
            txn.delete(b"b")
            txn.put(b"d", b"4")

        assert db[b"a"] == b"10"
        assert b"b" not in db
        assert db[b"c"] == b"3"
        assert db[b"d"] == b"4"

    def test_conflict_raises(self, tmp_path):
        db = ext.DB.open(str(tmp_path / "ext_txn3"))
        db[b"stock"] = b"10"

        with pytest.raises(ext.ConflictError):
            with db.transaction() as txn:
                val = int(txn[b"stock"])
                # Concurrent write outside the transaction
                db[b"stock"] = b"0"
                txn[b"stock"] = str(val - 1).encode()

    def test_ryow_put_then_get(self, tmp_path):
        """Point read sees a staged put."""
        db = ext.DB.open(str(tmp_path / "ryow1"))
        with db.transaction() as txn:
            txn[b"k"] = b"v1"
            assert txn[b"k"] == b"v1"
            assert txn.get(b"k") == b"v1"
            assert b"k" in txn

    def test_ryow_put_overwrite(self, tmp_path):
        """Successive puts to the same key — last write wins."""
        db = ext.DB.open(str(tmp_path / "ryow2"))
        db[b"k"] = b"old"
        with db.transaction() as txn:
            txn[b"k"] = b"v1"
            txn[b"k"] = b"v2"
            assert txn[b"k"] == b"v2"

    def test_ryow_delete_then_get(self, tmp_path):
        """Deleted key raises KeyError / returns default."""
        db = ext.DB.open(str(tmp_path / "ryow3"))
        db[b"k"] = b"v"
        with db.transaction() as txn:
            txn.delete(b"k")
            assert txn.get(b"k") is None
            assert txn.get(b"k", b"fallback") == b"fallback"
            assert b"k" not in txn
            with pytest.raises(KeyError):
                _ = txn[b"k"]

    def test_ryow_delete_then_put(self, tmp_path):
        """Delete followed by put — key is live again."""
        db = ext.DB.open(str(tmp_path / "ryow4"))
        db[b"k"] = b"old"
        with db.transaction() as txn:
            txn.delete(b"k")
            assert b"k" not in txn
            txn[b"k"] = b"new"
            assert txn[b"k"] == b"new"

    def test_ryow_delete_range(self, tmp_path):
        """Range delete hides keys from point reads."""
        db = ext.DB.open(str(tmp_path / "ryow5"))
        db[b"a"] = b"1"
        db[b"b"] = b"2"
        db[b"c"] = b"3"
        with db.transaction() as txn:
            txn.delete_range(b"a", b"c")
            assert b"a" not in txn
            assert b"b" not in txn
            assert txn[b"c"] == b"3"  # c is outside [a, c)

    def test_ryow_put_after_range_delete(self, tmp_path):
        """Put into a range-deleted region makes the key visible again."""
        db = ext.DB.open(str(tmp_path / "ryow6"))
        db[b"b"] = b"old"
        with db.transaction() as txn:
            txn.delete_range(b"a", b"d")
            assert b"b" not in txn
            txn[b"b"] = b"new"
            assert txn[b"b"] == b"new"


# ═══════════════════════════════════════════════════════════════════════════════
# 7. Reference leak tests — verify context managers release internal state
# ═══════════════════════════════════════════════════════════════════════════════


class TestReferenceLeak:
    """Verify that transaction and batch context managers don't pin
    internal objects (snapshot, write plan, buffers) after __exit__."""

    def test_transaction_releases_state(self, tmp_path):
        """Internal references cleared after normal commit."""
        db = ext.DB.open(str(tmp_path / "leak1"))
        db[b"k"] = b"v"

        ctx = db.transaction()
        txn = ctx.__enter__()
        txn[b"k"] = b"v2"
        ctx.__exit__(None, None, None)

        # _TransactionContext should have cleared _txn
        assert ctx._txn is None
        # _Transaction should have cleared its internals in _commit
        assert txn._raw_snap is None
        assert txn._snap is None
        assert len(txn._ops) == 0
        assert len(txn._pending) == 0
        assert len(txn._guards) == 0
        assert len(txn._range_dels) == 0

    def test_transaction_conflict_releases_state(self, tmp_path):
        """Internal references cleared even on conflict."""
        db = ext.DB.open(str(tmp_path / "leak2"))
        db[b"k"] = b"v"

        ctx = db.transaction()
        txn = ctx.__enter__()
        db[b"k"] = b"conflict"
        txn[b"k"] = b"v2"

        try:
            ctx.__exit__(None, None, None)
        except ext.ConflictError:
            pass

        assert ctx._txn is None
        assert txn._raw_snap is None

    def test_transaction_exception_releases_state(self, tmp_path):
        """Internal references cleared when the with-block raises."""
        db = ext.DB.open(str(tmp_path / "leak3"))
        db[b"k"] = b"v"

        ctx = db.transaction()
        txn = ctx.__enter__()
        txn[b"k"] = b"v2"

        # Simulate an exception in the with-block (commit skipped)
        ctx.__exit__(ValueError, ValueError("boom"), None)

        assert ctx._txn is None

    def test_batch_releases_state(self, tmp_path):
        """Internal references cleared after batch exits."""
        db = ext.DB.open(str(tmp_path / "leak4"))

        ctx = db.batch()
        batch = ctx.__enter__()
        batch[b"k"] = b"v"
        ctx.__exit__(None, None, None)

        assert ctx._batch is None
