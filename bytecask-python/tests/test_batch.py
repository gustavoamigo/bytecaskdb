import bytecaskdb as bc


def test_batch_put(db):
    plan = bc.WritePlan()
    plan.put(b"k1", b"v1")
    plan.put(b"k2", b"v2")
    plan.put(b"k3", b"v3")
    db.apply_batch(plan)

    assert db.get(b"k1") == b"v1"
    assert db.get(b"k2") == b"v2"
    assert db.get(b"k3") == b"v3"


def test_batch_put_and_del(db):
    db.put(b"existing", b"old")

    plan = bc.WritePlan()
    plan.put(b"new_key", b"new_val")
    plan.del_(b"existing")
    db.apply_batch(plan)

    assert db.get(b"new_key") == b"new_val"
    assert db.get(b"existing") is None


def test_batch_consumed_after_apply(db):
    plan = bc.WritePlan()
    plan.put(b"k1", b"v1")
    db.apply_batch(plan)

    import pytest
    with pytest.raises(RuntimeError, match="already consumed"):
        db.apply_batch(plan)


def test_batch_nosync(db):
    opts = bc.WriteOptions()
    opts.sync = False
    plan = bc.WritePlan()
    plan.put(b"k1", b"v1")
    db.apply_batch(plan, opts)
    assert db.get(b"k1") == b"v1"
