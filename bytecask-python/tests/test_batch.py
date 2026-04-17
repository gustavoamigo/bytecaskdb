import bytecaskdb as bc


def test_batch_put(db):
    batch = bc.Batch()
    batch.put(b"k1", b"v1")
    batch.put(b"k2", b"v2")
    batch.put(b"k3", b"v3")
    db.apply_batch(batch)

    assert db.get(b"k1") == b"v1"
    assert db.get(b"k2") == b"v2"
    assert db.get(b"k3") == b"v3"


def test_batch_put_and_del(db):
    db.put(b"existing", b"old")

    batch = bc.Batch()
    batch.put(b"new_key", b"new_val")
    batch.del_(b"existing")
    db.apply_batch(batch)

    assert db.get(b"new_key") == b"new_val"
    assert db.get(b"existing") is None


def test_batch_consumed_after_apply(db):
    batch = bc.Batch()
    batch.put(b"k1", b"v1")
    db.apply_batch(batch)

    import pytest
    with pytest.raises(RuntimeError, match="already consumed"):
        db.apply_batch(batch)


def test_batch_nosync(db):
    opts = bc.WriteOptions()
    opts.sync = False
    batch = bc.Batch()
    batch.put(b"k1", b"v1")
    db.apply_batch(batch, opts)
    assert db.get(b"k1") == b"v1"
