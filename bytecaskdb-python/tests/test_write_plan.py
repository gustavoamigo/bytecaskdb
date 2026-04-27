import pytest
import bytecaskdb._bytecaskdb as bc


def test_write_plan_no_conflict(db):
    db.put(b"stock", b"100")

    snap = db.snapshot()
    plan = bc.WritePlan(snap)
    plan.put(b"stock", b"99")
    assert db.apply_batch(plan) is True
    assert db.get(b"stock") == b"99"


def test_write_plan_conflict(db):
    db.put(b"stock", b"100")

    snap = db.snapshot()
    # Concurrent write
    db.put(b"stock", b"50")

    plan = bc.WritePlan(snap)
    plan.put(b"stock", b"99")
    assert db.apply_batch(plan) is False
    # Original concurrent write survives
    assert db.get(b"stock") == b"50"


def test_ensure_present(db):
    plan = bc.WritePlan()
    plan.ensure_present(b"missing")
    plan.put(b"other", b"val")
    assert db.apply_batch(plan) is False


def test_ensure_absent(db):
    db.put(b"exists", b"val")
    plan = bc.WritePlan()
    plan.ensure_absent(b"exists")
    plan.put(b"other", b"val")
    assert db.apply_batch(plan) is False


def test_ensure_absent_succeeds(db):
    plan = bc.WritePlan()
    plan.ensure_absent(b"missing")
    plan.put(b"new_key", b"val")
    assert db.apply_batch(plan) is True
    assert db.get(b"new_key") == b"val"


def test_ensure_unchanged(db):
    db.put(b"price", b"10")
    snap = db.snapshot()

    # Concurrent price change
    db.put(b"price", b"20")

    plan = bc.WritePlan(snap)
    plan.ensure_unchanged(b"price")
    plan.put(b"order", b"total_10")
    assert db.apply_batch(plan) is False


def test_ensure_unchanged_succeeds(db):
    db.put(b"price", b"10")
    snap = db.snapshot()

    plan = bc.WritePlan(snap)
    plan.ensure_unchanged(b"price")
    plan.put(b"order", b"total_10")
    assert db.apply_batch(plan) is True
    assert db.get(b"order") == b"total_10"


def test_ensure_range_unchanged(db):
    db.put(b"item:1", b"a")
    db.put(b"item:2", b"b")
    snap = db.snapshot()

    # Concurrent change within range
    db.put(b"item:1", b"changed")

    plan = bc.WritePlan(snap)
    plan.ensure_range_unchanged(b"item:", b"item:\xff")
    plan.put(b"summary", b"total")
    assert db.apply_batch(plan) is False


def test_ensure_unchanged_without_snapshot_raises(db):
    plan = bc.WritePlan()
    with pytest.raises(ValueError, match="requires a snapshot"):
        plan.ensure_unchanged(b"key")


def test_consumed_plan_raises(db):
    plan = bc.WritePlan()
    plan.put(b"k", b"v")
    db.apply_batch(plan)

    with pytest.raises(RuntimeError, match="already consumed"):
        db.apply_batch(plan)


def test_has_snapshot_property(db):
    plan_no_snap = bc.WritePlan()
    assert plan_no_snap.has_snapshot is False

    snap = db.snapshot()
    plan_with_snap = bc.WritePlan(snap)
    assert plan_with_snap.has_snapshot is True
